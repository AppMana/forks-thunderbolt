// SPDX-License-Identifier: GPL-2.0
/*
 * Thunderbolt XDomain discovery protocol support
 *
 * Copyright (C) 2017, Intel Corporation
 * Authors: Michael Jamet <michael.jamet@intel.com>
 *          Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/kmod.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/prandom.h>
#include <linux/refcount.h>
#include <linux/string_helpers.h>
#include <linux/utsname.h>
#include <linux/uuid.h>
#include <linux/workqueue.h>

#include "tb.h"

#define XDOMAIN_SHORT_TIMEOUT			100	/* ms */
#define XDOMAIN_DEFAULT_TIMEOUT			1000	/* ms */
#define XDOMAIN_BONDING_TIMEOUT			10000	/* ms */
#define XDOMAIN_RETRIES				10
#define XDOMAIN_PACKET_RETRIES			10
#define XDOMAIN_PACKET_RETRY_DELAY_MS		100
/*
 * Lane-bonding re-arm from ENUMERATED: bounded attempts, doubling spacing
 * (10 s, 20 s, 40 s, 80 s, 160 s ~ 5 min total), sized to outlive the
 * boot skew of sequentially rebooted peers. Failure never touches lane
 * adapters (the b5f07da Maple Ridge constraint).
 */
#define XDOMAIN_BONDING_REARM_ATTEMPTS		5
#define XDOMAIN_BONDING_REARM_TIMEOUT		10000	/* ms, doubles */
#define XDOMAIN_DIRECT_BOND_RETRIES		10
#define XDOMAIN_DEFAULT_MAX_HOPID		15
#define XDOMAIN_UUID_BACKOFF_MAX_SHIFT		6
#define XDOMAIN_QUARANTINE_RETRY_MS		1000
#define XDOMAIN_QUARANTINE_PATH_RETRIES		3
#define XDOMAIN_CONTROL_RX_EVIDENCE_MS		(5 * 60 * 1000)

enum {
	XDOMAIN_STATE_INIT,
	XDOMAIN_STATE_UUID,
	XDOMAIN_STATE_LINK_STATUS,
	XDOMAIN_STATE_DIRECT_BONDING_ENABLE,
	XDOMAIN_STATE_DIRECT_BONDING_WAIT,
	XDOMAIN_STATE_LINK_STATE_CHANGE,
	XDOMAIN_STATE_LINK_STATUS2,
	XDOMAIN_STATE_BONDING_UUID_LOW,
	XDOMAIN_STATE_BONDING_UUID_HIGH,
	XDOMAIN_STATE_PROPERTIES,
	XDOMAIN_STATE_ENUMERATED,
	XDOMAIN_STATE_ERROR,
};

static const char * const state_names[] = {
	[XDOMAIN_STATE_INIT] = "INIT",
	[XDOMAIN_STATE_UUID] = "UUID",
	[XDOMAIN_STATE_LINK_STATUS] = "LINK_STATUS",
	[XDOMAIN_STATE_DIRECT_BONDING_ENABLE] = "DIRECT_BONDING_ENABLE",
	[XDOMAIN_STATE_DIRECT_BONDING_WAIT] = "DIRECT_BONDING_WAIT",
	[XDOMAIN_STATE_LINK_STATE_CHANGE] = "LINK_STATE_CHANGE",
	[XDOMAIN_STATE_LINK_STATUS2] = "LINK_STATUS2",
	[XDOMAIN_STATE_BONDING_UUID_LOW] = "BONDING_UUID_LOW",
	[XDOMAIN_STATE_BONDING_UUID_HIGH] = "BONDING_UUID_HIGH",
	[XDOMAIN_STATE_PROPERTIES] = "PROPERTIES",
	[XDOMAIN_STATE_ENUMERATED] = "ENUMERATED",
	[XDOMAIN_STATE_ERROR] = "ERROR",
};

/*
 * Bonding re-arm: whether an ENUMERATED XDomain still retries lane
 * bonding. Without this, every bonding failure was terminal-x1 for the
 * XDomain's life (ENUMERATED never revisited bonding), which made boot
 * skew the fleet-wide x1 fixed point.
 */
static bool tb_xdomain_bonding_rearm_allowed(bool bonding_possible,
					     bool bonded,
					     unsigned int attempts)
{
	return bonding_possible && !bonded &&
	       attempts < XDOMAIN_BONDING_REARM_ATTEMPTS;
}

/*
 * Preserve an explicit protocol capability result across enumeration.
 * Transient transport failures leave bonding eligible for a later re-arm,
 * while ERROR_NOT_SUPPORTED is terminal unless the state machine is about
 * to use the early direct-bonding fallback instead.
 */
static bool
tb_xdomain_bonding_after(bool bonding_possible, int status_ret,
			 bool direct_fallback)
{
	if (!bonding_possible)
		return false;
	if (status_ret == -EOPNOTSUPP && !direct_fallback)
		return false;
	return true;
}

/*
 * Passive high side: whether an inbound XDP link-state-change request is
 * acceptable. A peer parked in BONDING_UUID_HIGH accepts one (the
 * original handshake); additionally an ENUMERATED, capable, unbonded peer
 * accepts a LATE request and re-enters BONDING_UUID_HIGH. Without the
 * latter, the low side's re-arm window would have to overlap the high
 * side's 10 s park - the boot-skew scheduler problem all over again. With
 * it, the low side's attempt wakes the high side, so re-arm converges
 * regardless of skew.
 */
static bool tb_xdomain_accepts_link_state_change(bool in_bonding_uuid_high,
						 bool enumerated,
						 bool bonding_possible,
						 bool bonded)
{
	return in_bonding_uuid_high ||
	       (enumerated && bonding_possible && !bonded);
}

static bool tb_xdomain_bonding_rearm_needed(struct tb_xdomain *xd)
{
	struct tb_port *port = tb_xdomain_downstream_port(xd);

	return tb_xdomain_bonding_rearm_allowed(xd->bonding_possible,
						!port || port->bonded,
						xd->bonding_rearm_attempts);
}

struct xdomain_request_work {
	struct work_struct work;
	struct tb_xdp_header *pkg;
	struct tb *tb;
};

struct xdomain_response_work {
	struct work_struct work;
	struct tb_ctl *ctl;
	enum tb_cfg_pkg_type type;
	size_t size;
	u8 response[];
};

struct xdomain_request_submit {
	struct tb_ctl *ctl;
	const void *request;
	size_t request_size;
	enum tb_cfg_pkg_type request_type;
};

static bool tb_xdomain_enabled = true;
module_param_named(xdomain, tb_xdomain_enabled, bool, 0444);
MODULE_PARM_DESC(xdomain, "allow XDomain protocol (default: true)");

static bool tb_xdomain_initial_state_needs_link_status(bool needs_uuid,
						       bool bonding_possible);
static bool tb_xdomain_announce_ready(bool needs_uuid, bool uuid_verified);
static bool
tb_xdomain_should_fallback_to_direct_bonding(bool bonding_possible,
					     bool services_published,
					     int link_status_ret,
					     bool peer_confirmed);

/*
 * Serializes access to the properties and protocol handlers below. If
 * you need to take both this lock and the struct tb_xdomain lock, take
 * this one first.
 */
static DEFINE_MUTEX(xdomain_lock);

/*
 * Serializes the protocol handler dispatch walk against unregistration.
 * Held across the whole walk in tb_xdomain_handle_request() INCLUDING the
 * handler callbacks (xdomain_lock alone cannot do this: it must be dropped
 * around the callback, which may itself register handlers or property
 * directories). Because tb_unregister_protocol_handler() takes this lock,
 * its return guarantees no callback of that handler is running or can run
 * again, so a service module may free the handler and unload immediately
 * afterwards. Consequently a handler callback must never call
 * tb_unregister_protocol_handler() itself. Lock order:
 * xdomain_dispatch_lock -> xdomain_lock.
 */
static DEFINE_MUTEX(xdomain_dispatch_lock);

struct tb_xdomain_quarantine_ops {
	int (*disconnect)(void *data, struct tb_xdomain *xd,
			  int transmit_path, int transmit_ring,
			  int receive_path, int receive_ring);
	void (*free_ring)(void *data, struct tb_ring *ring);
	int (*request_recovery)(void *data, struct tb_xdomain *xd);
	void *data;
};

struct tb_xdomain_quarantine {
	struct list_head list;
	struct delayed_work retry_work;
	refcount_t refs;
	struct tb_xdomain *xd;
	struct tb_ring *transmit_dma_ring;
	struct tb_ring *receive_dma_ring;
	int transmit_path;
	int transmit_ring;
	int receive_path;
	int receive_ring;
	unsigned int attempts;
	bool recovery_pending;
	bool stopping;
	struct tb_xdomain_quarantine_ops ops;
};

static LIST_HEAD(xdomain_quarantines);
static DEFINE_MUTEX(xdomain_quarantines_lock);

#if IS_ENABLED(CONFIG_USB4_KUNIT_TEST)
static struct tb_xdomain_quarantine_ops xdomain_quarantine_test_ops;
static bool xdomain_quarantine_test_ops_set;
static bool xdomain_quarantine_test_fail_alloc;
#endif

static void tb_xdomain_quarantine_put(struct tb_xdomain_quarantine *q)
{
	if (!refcount_dec_and_test(&q->refs))
		return;

	tb_xdomain_put(q->xd);
	kfree(q);
}

/* Properties exposed to the remote domains */
static struct tb_property_dir *xdomain_property_dir;
static u32 xdomain_property_block_gen;

/*
 * Intra-domain loop rig (docs/tb_same_host.md): both ports of one host
 * cabled together. On boards whose resident ICM firmware is alive
 * (thunderbolt.force_sw_cm boards), ring-0 XDomain protocol packets never
 * reach the wire -- the firmware claims and answers them itself
 * (TB_CFG_PKG_ICM_RESP, packet-trace proven), and the firmware cannot be
 * stopped at runtime (see icm_stop()). A loop needs no wire for discovery
 * anyway: the "peer" is this kernel, so the UUID and property exchange are
 * answered from local state. The operator declares the looped route pair
 * (e.g. thunderbolt.loop_routes=1,3); consecutive entries pair up.
 */
static char xdomain_loop_routes[64];
module_param_string(loop_routes, xdomain_loop_routes,
		    sizeof(xdomain_loop_routes), 0444);
MODULE_PARM_DESC(loop_routes,
		 "Comma-separated hex route pairs cabled in an intra-domain loop (e.g. 1,3)");

/*
 * Parses the operator list on each call: discovery-path only, and the
 * routes must be readable while xdomain init ordering stays untouched.
 * Returns true and fills @sibling when @route is a declared loop end.
 */
static bool tb_xdomain_loop_sibling(u64 route, u64 *sibling)
{
	u64 pair[2];
	unsigned int n = 0;
	const char *p = xdomain_loop_routes;

	while (*p) {
		char *end;
		u64 val = simple_strtoull(p, &end, 16);

		if (end == p)
			break;
		pair[n & 1] = val;
		if (n & 1) {
			if (pair[0] == route) {
				*sibling = pair[1];
				return true;
			}
			if (pair[1] == route) {
				*sibling = pair[0];
				return true;
			}
		}
		n++;
		p = (*end == ',') ? end + 1 : end;
	}
	return false;
}

static bool tb_xdomain_is_loop_route(u64 route)
{
	u64 unused;

	return tb_xdomain_loop_sibling(route, &unused);
}

/* Additional protocol handlers */
static LIST_HEAD(protocol_handlers);

/* UUID for XDomain discovery protocol: b638d70e-42ff-40bb-97c2-90e2c0b2ff07 */
static const uuid_t tb_xdp_uuid =
	UUID_INIT(0xb638d70e, 0x42ff, 0x40bb,
		  0x97, 0xc2, 0x90, 0xe2, 0xc0, 0xb2, 0xff, 0x07);

bool tb_is_xdomain_enabled(void)
{
	return tb_xdomain_enabled && tb_acpi_is_xdomain_allowed();
}

static enum tb_xdp_type tb_xdomain_response_type(enum tb_xdp_type request_type)
{
	switch (request_type) {
	case UUID_REQUEST_OLD:
	case UUID_REQUEST:
		return UUID_RESPONSE;
	case PROPERTIES_REQUEST:
		return PROPERTIES_RESPONSE;
	case PROPERTIES_CHANGED_REQUEST:
		return PROPERTIES_CHANGED_RESPONSE;
	case LINK_STATE_STATUS_REQUEST:
		return LINK_STATE_STATUS_RESPONSE;
	case LINK_STATE_CHANGE_REQUEST:
		return LINK_STATE_CHANGE_RESPONSE;
	default:
		return 0;
	}
}

static size_t tb_xdomain_response_min_size(enum tb_xdp_type response_type)
{
	switch (response_type) {
	case UUID_RESPONSE:
		return sizeof(struct tb_xdp_uuid_response);
	case PROPERTIES_RESPONSE:
		return sizeof(struct tb_xdp_properties_response);
	case PROPERTIES_CHANGED_RESPONSE:
		return sizeof(struct tb_xdp_properties_changed_response);
	case LINK_STATE_STATUS_RESPONSE:
		return sizeof(struct tb_xdp_link_state_status_response);
	case LINK_STATE_CHANGE_RESPONSE:
		return sizeof(struct tb_xdp_link_state_change_response);
	case ERROR_RESPONSE:
		return sizeof(struct tb_xdp_error_response);
	default:
		return SIZE_MAX;
	}
}

static u8 tb_xdomain_sequence(const struct tb_xdp_header *hdr)
{
	return (hdr->xd_hdr.length_sn & TB_XDOMAIN_SN_MASK) >>
		TB_XDOMAIN_SN_SHIFT;
}

static size_t tb_xdomain_packet_size(const struct tb_xdp_header *hdr)
{
	return (hdr->xd_hdr.length_sn & TB_XDOMAIN_LENGTH_MASK) * sizeof(u32) +
	       sizeof(hdr->xd_hdr);
}

static bool tb_xdomain_match(const struct tb_cfg_request *req,
			     const struct ctl_pkg *pkg)
{
	switch (pkg->frame.eof) {
	case TB_CFG_PKG_ERROR: {
		const struct tb_cfg_header *err_hdr = pkg->buffer;
		const struct tb_xdp_header *req_hdr = req->request;
		u64 req_route;

		/*
		 * An error completes only the request addressed to ITS
		 * route. With two active XDomains on one controller, the
		 * old match-any behavior let a dead sibling port's error
		 * traffic complete requests bound for the healthy peer
		 * (the 023<->025 lane-bonding negotiation failed in ~2 ms
		 * with the peer never seeing the request, 2026-08-18).
		 * A malformed short error still matches (old behavior)
		 * rather than lingering to the timeout.
		 */
		if (pkg->frame.size < sizeof(*err_hdr))
			return true;

		req_route = ((u64)(req_hdr->xd_hdr.route_hi & ~BIT(31)) << 32) |
			    req_hdr->xd_hdr.route_lo;
		return (tb_cfg_get_route(err_hdr) & ~BIT_ULL(63)) == req_route;
	}

	case TB_CFG_PKG_XDOMAIN_RESP: {
		const struct tb_xdp_header *res_hdr = pkg->buffer;
		const struct tb_xdp_header *req_hdr = req->request;
		enum tb_xdp_type expected_type;
		size_t min_size;

		if (pkg->frame.size < sizeof(*res_hdr) ||
		    pkg->frame.size > req->response_size)
			return false;
		if (tb_xdomain_packet_size(res_hdr) != pkg->frame.size)
			return false;

		if ((res_hdr->xd_hdr.route_hi & ~BIT(31)) !=
		    (req_hdr->xd_hdr.route_hi & ~BIT(31)))
			return false;
		if (res_hdr->xd_hdr.route_lo != req_hdr->xd_hdr.route_lo)
			return false;

		if (!uuid_equal(&res_hdr->uuid, &req_hdr->uuid))
			return false;
		if (tb_xdomain_sequence(res_hdr) != tb_xdomain_sequence(req_hdr))
			return false;

		/*
		 * Only the discovery protocol gives the word after the UUID the
		 * enum tb_xdp_type meaning. Service protocols own their payload and
		 * may put an address, identifier, or another private header there.
		 * Their common XDomain envelope has already been checked above.
		 */
		if (!uuid_equal(&req_hdr->uuid, &tb_xdp_uuid))
			return true;

		expected_type = tb_xdomain_response_type(req_hdr->type);
		if (!expected_type ||
		    (res_hdr->type != expected_type &&
		     res_hdr->type != ERROR_RESPONSE))
			return false;

		min_size = tb_xdomain_response_min_size(res_hdr->type);
		if (min_size == SIZE_MAX || pkg->frame.size < min_size)
			return false;

		return true;
	}

	default:
		return false;
	}
}

static enum tb_cfg_request_event
tb_xdomain_intermediate(const struct tb_cfg_request *req,
			const struct ctl_pkg *pkg)
{
	const struct icm_tr_pkg_xdomain_packet_response *status = pkg->buffer;
	const struct tb_xdp_header *req_hdr = req->request;
	u64 request_route, status_route;

	if (!tb_ctl_is_xdomain_tx_status(pkg->frame.eof, pkg->buffer,
					 pkg->frame.size))
		return TB_CFG_REQUEST_EVENT_NONE;

	request_route = ((u64)(req_hdr->xd_hdr.route_hi & ~BIT(31)) << 32) |
			req_hdr->xd_hdr.route_lo;
	status_route = ((u64)(status->route_hi & ~BIT(31)) << 32) |
			status->route_lo;
	if (request_route != status_route)
		return TB_CFG_REQUEST_EVENT_NONE;

	return status->hdr.flags & ICM_FLAGS_ERROR ?
		TB_CFG_REQUEST_EVENT_LOCAL_FAILED :
		TB_CFG_REQUEST_EVENT_LOCAL_ACCEPTED;
}

static bool tb_xdomain_copy(struct tb_cfg_request *req,
			    const struct ctl_pkg *pkg)
{
	if (pkg->frame.eof == TB_CFG_PKG_ERROR) {
		const struct cfg_error_pkg *error = pkg->buffer;

		if (pkg->frame.size < sizeof(*error)) {
			req->result.err = -EIO;
			return true;
		}

		req->result.response_route = tb_cfg_get_route(&error->header);
		req->result.response_port = error->port;
		req->result.tb_error = error->error;
		req->result.err = 1;
		return true;
	}

	memset(req->response, 0, req->response_size);
	memcpy(req->response, pkg->buffer, pkg->frame.size);
	req->result.err = 0;
	return true;
}

static void tb_xdomain_init_packet_state(struct tb_cfg_request *req,
					 bool expects_peer_response)
{
	req->intermediate = tb_xdomain_intermediate;
	req->state.local = TB_CFG_LOCAL_WAITING;
	if (expects_peer_response)
		req->state.peer = TB_CFG_PEER_WAITING;
}

static bool tb_xdomain_packet_should_retry(const struct tb_cfg_result *res,
					   unsigned int attempt)
{
	return res->local_failed && !res->local_timed_out && res->err == -EIO &&
		attempt + 1 < XDOMAIN_PACKET_RETRIES;
}

static int tb_xdomain_send_packet(struct tb_ctl *ctl, const void *packet,
				  size_t size, enum tb_cfg_pkg_type type)
{
	unsigned int attempt;

	for (attempt = 0; attempt < XDOMAIN_PACKET_RETRIES; attempt++) {
		struct tb_cfg_request *req;
		struct tb_cfg_result res;

		req = tb_cfg_request_alloc();
		if (!req)
			return -ENOMEM;

		req->match = tb_xdomain_match;
		req->copy = tb_xdomain_copy;
		req->request = packet;
		req->request_size = size;
		req->request_type = type;
		tb_xdomain_init_packet_state(req, false);

		res = tb_cfg_request_sync(ctl, req, XDOMAIN_DEFAULT_TIMEOUT);
		tb_cfg_request_put(req);

		if (!tb_xdomain_packet_should_retry(&res, attempt))
			return res.err == 1 ? -EIO : res.err;
		msleep(XDOMAIN_PACKET_RETRY_DELAY_MS);
	}

	return -EIO;
}

static int __tb_xdomain_response(struct tb_ctl *ctl, const void *response,
				 size_t size, enum tb_cfg_pkg_type type)
{
	return tb_xdomain_send_packet(ctl, response, size, type);
}

static bool tb_xdomain_response_should_defer(void)
{
	return true;
}

static void tb_xdomain_response_work(struct work_struct *work)
{
	struct xdomain_response_work *xw =
		container_of(work, typeof(*xw), work);
	struct tb_ctl *ctl = xw->ctl;
	int ret;

	ret = __tb_xdomain_response(ctl, xw->response, xw->size, xw->type);
	if (ret && ret != -ENOTCONN && ret != -ESHUTDOWN)
		pr_warn_ratelimited("failed to send deferred XDomain response: %d\n",
				    ret);

	kfree(xw);
	/* Last touch: tb_ctl_free() may proceed when this reaches zero. */
	tb_ctl_async_work_put(ctl);
}

static int tb_xdomain_queue_response(struct tb_ctl *ctl, const void *response,
				     size_t size, enum tb_cfg_pkg_type type)
{
	struct xdomain_response_work *xw;

	xw = kmalloc(struct_size(xw, response, size), GFP_KERNEL);
	if (!xw)
		return -ENOMEM;

	if (!tb_ctl_async_work_get(ctl)) {
		kfree(xw);
		return -ENOTCONN;
	}

	INIT_WORK(&xw->work, tb_xdomain_response_work);
	xw->ctl = ctl;
	xw->type = type;
	xw->size = size;
	memcpy(xw->response, response, size);
	if (!schedule_work(&xw->work)) {
		tb_ctl_async_work_put(ctl);
		kfree(xw);
		return -EBUSY;
	}

	return 0;
}

/**
 * tb_xdomain_response() - Send a XDomain response message
 * @xd: XDomain to send the message
 * @response: Response to send
 * @size: Size of the response
 * @type: PDF type of the response
 *
 * This can be used to send a XDomain response message to the other
 * domain. No response for the message is expected.
 *
 * Return: %0 in case of success and negative errno in case of failure
 */
int tb_xdomain_response(struct tb_xdomain *xd, const void *response,
			size_t size, enum tb_cfg_pkg_type type)
{
	if (tb_xdomain_response_should_defer())
		return tb_xdomain_queue_response(xd->tb->ctl, response, size,
						 type);

	return __tb_xdomain_response(xd->tb->ctl, response, size, type);
}
EXPORT_SYMBOL_GPL(tb_xdomain_response);

static int tb_xdomain_submit_request(void *data)
{
	struct xdomain_request_submit *submit = data;

	return tb_xdomain_send_packet(submit->ctl, submit->request,
				      submit->request_size,
				      submit->request_type);
}

static int __tb_xdomain_request(struct tb_ctl *ctl, const void *request,
	size_t request_size, enum tb_cfg_pkg_type request_type, void *response,
	size_t response_size, enum tb_cfg_pkg_type response_type,
	unsigned int timeout_msec)
{
	struct xdomain_request_submit submit = {
		.ctl = ctl,
		.request = request,
		.request_size = request_size,
		.request_type = request_type,
	};
	struct tb_cfg_request *req;
	struct tb_cfg_result res;

	req = tb_cfg_request_alloc();
	if (!req)
		return -ENOMEM;

	req->match = tb_xdomain_match;
	req->copy = tb_xdomain_copy;
	req->request = request;
	req->request_size = request_size;
	req->request_type = request_type;
	req->response = response;
	req->response_size = response_size;
	req->response_type = response_type;
	req->state.peer = TB_CFG_PEER_WAITING;

	res = tb_cfg_request_sync_receive(ctl, req, timeout_msec,
					  tb_xdomain_submit_request, &submit);

	tb_cfg_request_put(req);

	return res.err == 1 ? -EIO : res.err;
}

/**
 * tb_xdomain_request() - Send a XDomain request
 * @xd: XDomain to send the request
 * @request: Request to send
 * @request_size: Size of the request in bytes
 * @request_type: PDF type of the request
 * @response: Response is copied here
 * @response_size: Expected size of the response in bytes
 * @response_type: Expected PDF type of the response
 * @timeout_msec: Timeout in milliseconds to wait for the response
 *
 * This function can be used to send XDomain control channel messages to
 * the other domain. The function waits until the response is received
 * or when timeout triggers. Whichever comes first.
 *
 * Return: %0 in case of success and negative errno in case of failure
 */
int tb_xdomain_request(struct tb_xdomain *xd, const void *request,
	size_t request_size, enum tb_cfg_pkg_type request_type,
	void *response, size_t response_size,
	enum tb_cfg_pkg_type response_type, unsigned int timeout_msec)
{
	return __tb_xdomain_request(xd->tb->ctl, request, request_size,
				    request_type, response, response_size,
				    response_type, timeout_msec);
}
EXPORT_SYMBOL_GPL(tb_xdomain_request);

static inline void tb_xdp_fill_header(struct tb_xdp_header *hdr, u64 route,
	u8 sequence, enum tb_xdp_type type, size_t size)
{
	u32 length_sn;

	length_sn = (size - sizeof(hdr->xd_hdr)) / 4;
	length_sn |= (sequence << TB_XDOMAIN_SN_SHIFT) & TB_XDOMAIN_SN_MASK;

	hdr->xd_hdr.route_hi = upper_32_bits(route);
	hdr->xd_hdr.route_lo = lower_32_bits(route);
	hdr->xd_hdr.length_sn = length_sn;
	hdr->type = type;
	memcpy(&hdr->uuid, &tb_xdp_uuid, sizeof(tb_xdp_uuid));
}

static int tb_xdp_handle_error(const struct tb_xdp_error_response *res)
{
	if (res->hdr.type != ERROR_RESPONSE)
		return 0;

	switch (res->error) {
	case ERROR_UNKNOWN_PACKET:
	case ERROR_UNKNOWN_DOMAIN:
		return -EIO;
	case ERROR_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case ERROR_NOT_READY:
		return -EAGAIN;
	default:
		break;
	}

	return 0;
}

static bool tb_xdp_properties_ids_ok(const struct tb_xdp_properties_response *res,
				     const uuid_t *local_uuid,
				     const uuid_t *remote_uuid)
{
	return uuid_equal(&res->src_uuid, remote_uuid) &&
	       uuid_equal(&res->dst_uuid, local_uuid);
}

#if IS_ENABLED(CONFIG_USB4_KUNIT_TEST)
bool tb_test_xdomain_properties_identity(bool source_matches,
					 bool destination_matches)
{
	uuid_t local = UUID_INIT(0x11111111, 0x2222, 0x3333,
				 0x44, 0x44, 0x55, 0x55, 0x66, 0x66,
				 0x77, 0x77);
	uuid_t remote = UUID_INIT(0xaaaaaaaa, 0xbbbb, 0xcccc,
				  0xdd, 0xdd, 0xee, 0xee, 0xff, 0xff,
				  0x00, 0x00);
	struct tb_xdp_properties_response res = {};

	uuid_copy(&res.src_uuid, source_matches ? &remote : &local);
	uuid_copy(&res.dst_uuid, destination_matches ? &local : &remote);

	return tb_xdp_properties_ids_ok(&res, &local, &remote);
}
#endif

static int tb_xdp_uuid_request(struct tb_ctl *ctl, u64 route, int retry,
			       uuid_t *uuid, u64 *remote_route)
{
	struct tb_xdp_uuid_response res;
	struct tb_xdp_uuid req;
	int ret;

	memset(&req, 0, sizeof(req));
	tb_xdp_fill_header(&req.hdr, route, retry % 4, UUID_REQUEST,
			   sizeof(req));

	memset(&res, 0, sizeof(res));
	ret = __tb_xdomain_request(ctl, &req, sizeof(req),
				   TB_CFG_PKG_XDOMAIN_REQ, &res, sizeof(res),
				   TB_CFG_PKG_XDOMAIN_RESP,
				   XDOMAIN_DEFAULT_TIMEOUT);
	if (ret)
		return ret;

	ret = tb_xdp_handle_error(&res.err);
	if (ret)
		return ret;

	uuid_copy(uuid, &res.src_uuid);
	*remote_route = (u64)res.src_route_hi << 32 | res.src_route_lo;

	return 0;
}

static int tb_xdp_uuid_response(struct tb_ctl *ctl, u64 route, u8 sequence,
				const uuid_t *uuid)
{
	struct tb_xdp_uuid_response res;

	memset(&res, 0, sizeof(res));
	tb_xdp_fill_header(&res.hdr, route, sequence, UUID_RESPONSE,
			   sizeof(res));

	uuid_copy(&res.src_uuid, uuid);
	res.src_route_hi = upper_32_bits(route);
	res.src_route_lo = lower_32_bits(route);

	return __tb_xdomain_response(ctl, &res, sizeof(res),
				     TB_CFG_PKG_XDOMAIN_RESP);
}

static int tb_xdp_error_response(struct tb_ctl *ctl, u64 route, u8 sequence,
				 enum tb_xdp_error error)
{
	struct tb_xdp_error_response res;

	memset(&res, 0, sizeof(res));
	tb_xdp_fill_header(&res.hdr, route, sequence, ERROR_RESPONSE,
			   sizeof(res));
	res.error = error;

	return __tb_xdomain_response(ctl, &res, sizeof(res),
				     TB_CFG_PKG_XDOMAIN_RESP);
}

static int tb_xdp_properties_request(struct tb_ctl *ctl, u64 route,
	const uuid_t *src_uuid, const uuid_t *dst_uuid, int retry,
	u32 **block, u32 *generation)
{
	struct tb_xdp_properties_response *res;
	struct tb_xdp_properties req;
	u16 data_len, len;
	size_t total_size;
	u32 *data = NULL;
	int ret;

	total_size = sizeof(*res) + TB_XDP_PROPERTIES_MAX_DATA_LENGTH * 4;
	res = kzalloc(total_size, GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	memset(&req, 0, sizeof(req));
	tb_xdp_fill_header(&req.hdr, route, retry % 4, PROPERTIES_REQUEST,
			   sizeof(req));
	memcpy(&req.src_uuid, src_uuid, sizeof(*src_uuid));
	memcpy(&req.dst_uuid, dst_uuid, sizeof(*dst_uuid));

	data_len = 0;

	do {
		ret = __tb_xdomain_request(ctl, &req, sizeof(req),
					   TB_CFG_PKG_XDOMAIN_REQ, res,
					   total_size, TB_CFG_PKG_XDOMAIN_RESP,
					   XDOMAIN_DEFAULT_TIMEOUT);
		if (ret)
			goto err;

		ret = tb_xdp_handle_error(&res->err);
		if (ret)
			goto err;
		if (!tb_xdp_properties_ids_ok(res, src_uuid, dst_uuid)) {
			ret = -EKEYREJECTED;
			goto err;
		}

		/*
		 * Package length includes the whole payload without the
		 * XDomain header. Validate first that the package is at
		 * least size of the response structure.
		 */
		len = res->hdr.xd_hdr.length_sn & TB_XDOMAIN_LENGTH_MASK;
		if (len < sizeof(*res) / 4) {
			ret = -EINVAL;
			goto err;
		}

		len += sizeof(res->hdr.xd_hdr) / 4;
		len -= sizeof(*res) / 4;

		if (res->offset != req.offset) {
			ret = -EINVAL;
			goto err;
		}

		/*
		 * First time allocate block that has enough space for
		 * the whole properties block.
		 */
		if (!data) {
			data_len = res->data_length;
			if (data_len > TB_XDP_PROPERTIES_MAX_LENGTH) {
				ret = -E2BIG;
				goto err;
			}

			data = kcalloc(data_len, sizeof(u32), GFP_KERNEL);
			if (!data) {
				ret = -ENOMEM;
				goto err;
			}
		}

		memcpy(data + req.offset, res->data, len * 4);
		req.offset += len;
	} while (!data_len || req.offset < data_len);

	*block = data;
	*generation = res->generation;

	kfree(res);

	return data_len;

err:
	kfree(data);
	kfree(res);

	return ret;
}

static int tb_xdp_properties_response(struct tb *tb, struct tb_ctl *ctl,
	struct tb_xdomain *xd, u8 sequence, const struct tb_xdp_properties *req)
{
	struct tb_xdp_properties_response *res;
	size_t total_size;
	u16 len;
	int ret;

	/*
	 * Currently we expect all requests to be directed to us. The
	 * protocol supports forwarding, though which we might add
	 * support later on.
	 */
	if (!uuid_equal(xd->local_uuid, &req->dst_uuid)) {
		tb_xdp_error_response(ctl, xd->route, sequence,
				      ERROR_UNKNOWN_DOMAIN);
		return 0;
	}

	mutex_lock(&xd->lock);

	if (req->offset >= xd->local_property_block_len) {
		mutex_unlock(&xd->lock);
		return -EINVAL;
	}

	len = xd->local_property_block_len - req->offset;
	len = min_t(u16, len, TB_XDP_PROPERTIES_MAX_DATA_LENGTH);
	total_size = sizeof(*res) + len * 4;

	res = kzalloc(total_size, GFP_KERNEL);
	if (!res) {
		mutex_unlock(&xd->lock);
		return -ENOMEM;
	}

	tb_xdp_fill_header(&res->hdr, xd->route, sequence, PROPERTIES_RESPONSE,
			   total_size);
	res->generation = xd->local_property_block_gen;
	res->data_length = xd->local_property_block_len;
	res->offset = req->offset;
	uuid_copy(&res->src_uuid, xd->local_uuid);
	uuid_copy(&res->dst_uuid, &req->src_uuid);
	memcpy(res->data, &xd->local_property_block[req->offset], len * 4);

	mutex_unlock(&xd->lock);

	ret = __tb_xdomain_response(ctl, res, total_size,
				    TB_CFG_PKG_XDOMAIN_RESP);

	kfree(res);
	return ret;
}

static int tb_xdp_properties_changed_request(struct tb_ctl *ctl, u64 route,
					     int retry, const uuid_t *uuid)
{
	struct tb_xdp_properties_changed_response res;
	struct tb_xdp_properties_changed req;
	int ret;

	memset(&req, 0, sizeof(req));
	tb_xdp_fill_header(&req.hdr, route, retry % 4,
			   PROPERTIES_CHANGED_REQUEST, sizeof(req));
	uuid_copy(&req.src_uuid, uuid);

	memset(&res, 0, sizeof(res));
	ret = __tb_xdomain_request(ctl, &req, sizeof(req),
				   TB_CFG_PKG_XDOMAIN_REQ, &res, sizeof(res),
				   TB_CFG_PKG_XDOMAIN_RESP,
				   XDOMAIN_DEFAULT_TIMEOUT);
	if (ret)
		return ret;

	return tb_xdp_handle_error(&res.err);
}

static int
tb_xdp_properties_changed_response(struct tb_ctl *ctl, u64 route, u8 sequence)
{
	struct tb_xdp_properties_changed_response res;

	memset(&res, 0, sizeof(res));
	tb_xdp_fill_header(&res.hdr, route, sequence,
			   PROPERTIES_CHANGED_RESPONSE, sizeof(res));
	return __tb_xdomain_response(ctl, &res, sizeof(res),
				     TB_CFG_PKG_XDOMAIN_RESP);
}

static int tb_xdp_link_state_status_request(struct tb_ctl *ctl, u64 route,
					    u8 sequence, u8 *slw, u8 *tlw,
					    u8 *sls, u8 *tls)
{
	struct tb_xdp_link_state_status_response res;
	struct tb_xdp_link_state_status req;
	int ret;

	memset(&req, 0, sizeof(req));
	tb_xdp_fill_header(&req.hdr, route, sequence, LINK_STATE_STATUS_REQUEST,
			   sizeof(req));

	memset(&res, 0, sizeof(res));
	ret = __tb_xdomain_request(ctl, &req, sizeof(req), TB_CFG_PKG_XDOMAIN_REQ,
				   &res, sizeof(res), TB_CFG_PKG_XDOMAIN_RESP,
				   XDOMAIN_DEFAULT_TIMEOUT);
	if (ret)
		return ret;

	ret = tb_xdp_handle_error(&res.err);
	if (ret)
		return ret;

	if (res.status != 0)
		return -EREMOTEIO;

	*slw = res.slw;
	*tlw = res.tlw;
	*sls = res.sls;
	*tls = res.tls;

	return 0;
}

static int tb_xdp_link_state_status_response(struct tb *tb, struct tb_ctl *ctl,
					     struct tb_xdomain *xd, u8 sequence)
{
	struct tb_xdp_link_state_status_response res;
	struct tb_port *port = tb_xdomain_downstream_port(xd);
	u32 val[2];
	int ret;

	memset(&res, 0, sizeof(res));
	tb_xdp_fill_header(&res.hdr, xd->route, sequence,
			   LINK_STATE_STATUS_RESPONSE, sizeof(res));

	ret = tb_port_read(port, val, TB_CFG_PORT,
			   port->cap_phy + LANE_ADP_CS_0, ARRAY_SIZE(val));
	if (ret)
		return ret;

	res.slw = (val[0] & LANE_ADP_CS_0_SUPPORTED_WIDTH_MASK) >>
			LANE_ADP_CS_0_SUPPORTED_WIDTH_SHIFT;
	res.sls = (val[0] & LANE_ADP_CS_0_SUPPORTED_SPEED_MASK) >>
			LANE_ADP_CS_0_SUPPORTED_SPEED_SHIFT;
	res.tls = val[1] & LANE_ADP_CS_1_TARGET_SPEED_MASK;
	res.tlw = (val[1] & LANE_ADP_CS_1_TARGET_WIDTH_MASK) >>
			LANE_ADP_CS_1_TARGET_WIDTH_SHIFT;

	return __tb_xdomain_response(ctl, &res, sizeof(res),
				     TB_CFG_PKG_XDOMAIN_RESP);
}

static int tb_xdp_link_state_change_request(struct tb_ctl *ctl, u64 route,
					    u8 sequence, u8 tlw, u8 tls)
{
	struct tb_xdp_link_state_change_response res;
	struct tb_xdp_link_state_change req;
	int ret;

	memset(&req, 0, sizeof(req));
	tb_xdp_fill_header(&req.hdr, route, sequence, LINK_STATE_CHANGE_REQUEST,
			   sizeof(req));
	req.tlw = tlw;
	req.tls = tls;

	memset(&res, 0, sizeof(res));
	ret = __tb_xdomain_request(ctl, &req, sizeof(req), TB_CFG_PKG_XDOMAIN_REQ,
				   &res, sizeof(res), TB_CFG_PKG_XDOMAIN_RESP,
				   XDOMAIN_DEFAULT_TIMEOUT);
	if (ret)
		return ret;

	ret = tb_xdp_handle_error(&res.err);
	if (ret)
		return ret;

	return res.status != 0 ? -EREMOTEIO : 0;
}

static int tb_xdp_link_state_change_response(struct tb_ctl *ctl, u64 route,
					     u8 sequence, u32 status)
{
	struct tb_xdp_link_state_change_response res;

	memset(&res, 0, sizeof(res));
	tb_xdp_fill_header(&res.hdr, route, sequence, LINK_STATE_CHANGE_RESPONSE,
			   sizeof(res));

	res.status = status;

	return __tb_xdomain_response(ctl, &res, sizeof(res),
				     TB_CFG_PKG_XDOMAIN_RESP);
}

/**
 * tb_register_protocol_handler() - Register protocol handler
 * @handler: Handler to register
 *
 * This allows XDomain service drivers to hook into incoming XDomain
 * messages. After this function is called the service driver needs to
 * be able to handle calls to callback whenever a package with the
 * registered protocol is received.
 */
int tb_register_protocol_handler(struct tb_protocol_handler *handler)
{
	/*
	 * Short-circuit order matters: a stock-built registrant's storage
	 * ends at ->list, so ->callback_xd may only be read once ->callback
	 * is known to be NULL.
	 */
	if (!handler->uuid || (!handler->callback && !handler->callback_xd))
		return -EINVAL;
	if (uuid_equal(handler->uuid, &tb_xdp_uuid))
		return -EINVAL;

	mutex_lock(&xdomain_lock);
	list_add_tail(&handler->list, &protocol_handlers);
	mutex_unlock(&xdomain_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(tb_register_protocol_handler);

/**
 * tb_unregister_protocol_handler() - Unregister protocol handler
 * @handler: Handler to unregister
 *
 * Removes the previously registered protocol handler.
 */
void tb_unregister_protocol_handler(struct tb_protocol_handler *handler)
{
	/*
	 * The dispatch lock waits out a walk currently inside a handler
	 * callback; once held, no walk can observe the handler again. On
	 * appmana-025 the old delist-only unregister let a service module
	 * unload while the peer's control packet was still being dispatched,
	 * and the next reload's dispatch went through stale module memory
	 * (kdump 202608031305). Must not be called from a handler callback.
	 */
	mutex_lock(&xdomain_dispatch_lock);
	mutex_lock(&xdomain_lock);
	list_del_init(&handler->list);
	mutex_unlock(&xdomain_lock);
	mutex_unlock(&xdomain_dispatch_lock);
}
EXPORT_SYMBOL_GPL(tb_unregister_protocol_handler);

static void update_property_block(struct tb_xdomain *xd)
{
	mutex_lock(&xdomain_lock);
	mutex_lock(&xd->lock);
	/*
	 * If the local property block is not up-to-date, rebuild it now
	 * based on the global property template.
	 */
	if (!xd->local_property_block ||
	    xd->local_property_block_gen < xdomain_property_block_gen) {
		struct tb_property_dir *dir;
		int ret, block_len;
		u32 *block;

		dir = tb_property_copy_dir(xdomain_property_dir);
		if (!dir) {
			dev_warn(&xd->dev, "failed to copy properties\n");
			goto out_unlock;
		}

		/* Fill in non-static properties now */
		tb_property_add_text(dir, "deviceid", utsname()->nodename);
		tb_property_add_immediate(dir, "maxhopid", xd->local_max_hopid);

		ret = tb_property_format_dir(dir, NULL, 0);
		if (ret < 0) {
			dev_warn(&xd->dev, "local property block creation failed\n");
			tb_property_free_dir(dir);
			goto out_unlock;
		}

		block_len = ret;
		block = kcalloc(block_len, sizeof(*block), GFP_KERNEL);
		if (!block) {
			tb_property_free_dir(dir);
			goto out_unlock;
		}

		ret = tb_property_format_dir(dir, block, block_len);
		if (ret) {
			dev_warn(&xd->dev, "property block generation failed\n");
			tb_property_free_dir(dir);
			kfree(block);
			goto out_unlock;
		}

		tb_property_free_dir(dir);
		/* Release the previous block */
		kfree(xd->local_property_block);
		/* Assign new one */
		xd->local_property_block = block;
		xd->local_property_block_len = block_len;
		xd->local_property_block_gen = xdomain_property_block_gen;
	}

out_unlock:
	mutex_unlock(&xd->lock);
	mutex_unlock(&xdomain_lock);
}

static void start_handshake(struct tb_xdomain *xd)
{
	xd->state = XDOMAIN_STATE_INIT;
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_SHORT_TIMEOUT));
}

static void
tb_xdomain_control_event(struct tb_xdomain *xd,
			 enum tb_xdomain_control_event event)
{
	mutex_lock(&xd->lock);
	if (event == TB_XDOMAIN_CONTROL_INBOUND_PROGRESS)
		xd->control_rx_jiffies = jiffies;
	xd->control_health_state = tb_xdomain_control_next(
		xd->control_health_state, event);
	mutex_unlock(&xd->lock);
}

static bool tb_xdomain_control_recovery_required(struct tb_xdomain *xd)
{
	bool recent, required = false;

	mutex_lock(&xd->lock);
	recent = xd->control_rx_jiffies &&
		time_before(jiffies, xd->control_rx_jiffies +
			    msecs_to_jiffies(XDOMAIN_CONTROL_RX_EVIDENCE_MS));
	if (recent && xd->control_health_state ==
		      TB_XDOMAIN_CONTROL_PEER_ACTIVE) {
		xd->control_health_state = tb_xdomain_control_next(
			xd->control_health_state,
			TB_XDOMAIN_CONTROL_OUTBOUND_SATURATED);
		required = xd->control_health_state ==
			TB_XDOMAIN_CONTROL_RECOVERY_REQUIRED;
	}
	mutex_unlock(&xd->lock);

	return required;
}

/* Can be called from state_work */
static void __stop_handshake(struct tb_xdomain *xd)
{
	/*
	 * Publish the stop marker BEFORE the cancel. The announcement re-queues
	 * itself indefinitely now, so a callback already inside its bounded wire
	 * wait could otherwise re-arm after cancel_delayed_work_sync() has
	 * returned and leave a delayed work pending across suspend or unload.
	 * It re-reads this after the wait and declines.
	 */
	WRITE_ONCE(xd->properties_changed_retries, TB_XDOMAIN_ANNOUNCE_STOPPED);
	cancel_delayed_work_sync(&xd->properties_changed_work);
	xd->state_retries = 0;
	tb_xdomain_control_event(xd, TB_XDOMAIN_CONTROL_STOPPED);
}

static void stop_handshake(struct tb_xdomain *xd)
{
	cancel_delayed_work_sync(&xd->state_work);
	__stop_handshake(xd);
}

static void tb_xdp_handle_request(struct work_struct *work)
{
	struct xdomain_request_work *xw = container_of(work, typeof(*xw), work);
	const struct tb_xdp_header *pkg = xw->pkg;
	const struct tb_xdomain_header *xhdr = &pkg->xd_hdr;
	struct tb *tb = xw->tb;
	struct tb_ctl *ctl = tb->ctl;
	struct tb_xdomain *xd;
	const uuid_t *uuid;
	int ret = 0;
	u32 sequence;
	u64 route;

	route = ((u64)xhdr->route_hi << 32 | xhdr->route_lo) & ~BIT_ULL(63);
	sequence = xhdr->length_sn & TB_XDOMAIN_SN_MASK;
	sequence >>= TB_XDOMAIN_SN_SHIFT;

	mutex_lock(&tb->lock);
	if (tb->root_switch)
		uuid = tb->root_switch->uuid;
	else
		uuid = NULL;
	mutex_unlock(&tb->lock);

	if (!uuid) {
		tb_xdp_error_response(ctl, route, sequence, ERROR_NOT_READY);
		goto out;
	}

	xd = tb_xdomain_find_by_route_locked(tb, route);
	if (xd)
		update_property_block(xd);

	switch (pkg->type) {
	case PROPERTIES_REQUEST:
		tb_dbg(tb, "%llx: received XDomain properties request\n", route);
		if (xd) {
			ret = tb_xdp_properties_response(tb, ctl, xd, sequence,
				(const struct tb_xdp_properties *)pkg);
		}
		break;

	case PROPERTIES_CHANGED_REQUEST:
		tb_dbg(tb, "%llx: received XDomain properties changed request\n",
		       route);

		ret = tb_xdp_properties_changed_response(ctl, route, sequence);

		/*
		 * Since the properties have been changed, let's update
		 * the xdomain related to this connection as well in
		 * case there is a change in services it offers.
		 *
		 * The peer's property generation counter is monotonic and
		 * this notification is best-effort: after a simultaneous
		 * fleet driver reload it can be lost/raced and the cached
		 * generation latched, after which tb_xdomain_get_properties()
		 * silently drops every re-read (gen <= cached) and services
		 * never re-probe. Clear the cached generation so this
		 * notification always forces a fresh accept.
		 */
		if (xd) {
			mutex_lock(&xd->lock);
			if (!xd->removing && device_is_registered(&xd->dev)) {
				xd->remote_property_block_gen = 0;
				queue_delayed_work(tb->wq, &xd->state_work,
						   msecs_to_jiffies(XDOMAIN_SHORT_TIMEOUT));
			}
			mutex_unlock(&xd->lock);
		}
		break;

	case UUID_REQUEST_OLD:
	case UUID_REQUEST:
		tb_dbg(tb, "%llx: received XDomain UUID request\n", route);
		ret = tb_xdp_uuid_response(ctl, route, sequence, uuid);
		/*
		 * If we've stopped the discovery with an error such as
		 * timing out, we will restart the handshake now that we
		 * received UUID request from the remote host.
		 */
		if (!ret && xd && xd->state == XDOMAIN_STATE_ERROR) {
			mutex_lock(&xd->lock);
			if (!xd->removing) {
				dev_dbg(&xd->dev, "restarting handshake\n");
				start_handshake(xd);
			}
			mutex_unlock(&xd->lock);
		}
		break;

	case LINK_STATE_STATUS_REQUEST:
		tb_dbg(tb, "%llx: received XDomain link state status request\n",
		       route);

		if (xd) {
			ret = tb_xdp_link_state_status_response(tb, ctl, xd,
								sequence);
		} else {
			tb_xdp_error_response(ctl, route, sequence,
					      ERROR_NOT_READY);
		}
		break;

	case LINK_STATE_CHANGE_REQUEST:
		tb_dbg(tb, "%llx: received XDomain link state change request\n",
		       route);

		if (xd && tb_xdomain_accepts_link_state_change(
				xd->state == XDOMAIN_STATE_BONDING_UUID_HIGH,
				xd->state == XDOMAIN_STATE_ENUMERATED,
				xd->bonding_possible,
				({ struct tb_port *p =
					tb_xdomain_downstream_port(xd);
				   !p || p->bonded; }))) {
			const struct tb_xdp_link_state_change *lsc =
				(const struct tb_xdp_link_state_change *)pkg;

			ret = tb_xdp_link_state_change_response(ctl, route,
								sequence, 0);
			mutex_lock(&xd->lock);
			if (!xd->removing) {
				xd->target_link_width = lsc->tlw;
				if (xd->state == XDOMAIN_STATE_ENUMERATED) {
					dev_dbg(&xd->dev,
						"accepting late link state change (bonding re-arm)\n");
					xd->state = XDOMAIN_STATE_BONDING_UUID_HIGH;
					xd->state_retries = XDOMAIN_RETRIES;
				}
				queue_delayed_work(tb->wq, &xd->state_work,
						   msecs_to_jiffies(XDOMAIN_SHORT_TIMEOUT));
			}
			mutex_unlock(&xd->lock);
		} else {
			tb_xdp_error_response(ctl, route, sequence,
					      ERROR_NOT_READY);
		}
		break;

	default:
		tb_dbg(tb, "%llx: unknown XDomain request %#x\n", route, pkg->type);
		tb_xdp_error_response(ctl, route, sequence,
				      ERROR_NOT_SUPPORTED);
		break;
	}

	tb_xdomain_put(xd);

	if (ret) {
		tb_warn(tb, "failed to send XDomain response for %#x\n",
			pkg->type);
	}

out:
	kfree(xw->pkg);
	kfree(xw);

	tb_domain_put(tb);
}

static bool
tb_xdp_schedule_request(struct tb *tb, const struct tb_xdp_header *hdr,
			size_t size)
{
	struct xdomain_request_work *xw;

	xw = kmalloc(sizeof(*xw), GFP_KERNEL);
	if (!xw)
		return false;

	INIT_WORK(&xw->work, tb_xdp_handle_request);
	xw->pkg = kmemdup(hdr, size, GFP_KERNEL);
	if (!xw->pkg) {
		kfree(xw);
		return false;
	}
	xw->tb = tb_domain_get(tb);

	schedule_work(&xw->work);
	return true;
}

/**
 * tb_register_service_driver() - Register XDomain service driver
 * @drv: Driver to register
 *
 * Registers new service driver from @drv to the bus.
 */
int tb_register_service_driver(struct tb_service_driver *drv)
{
	drv->driver.bus = &tb_bus_type;
	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(tb_register_service_driver);

/**
 * tb_unregister_service_driver() - Unregister XDomain service driver
 * @drv: Driver to unregister
 *
 * Unregisters XDomain service driver from the bus.
 */
void tb_unregister_service_driver(struct tb_service_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(tb_unregister_service_driver);

static ssize_t key_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct tb_service *svc = container_of(dev, struct tb_service, dev);

	/*
	 * It should be null terminated but anything else is pretty much
	 * allowed.
	 */
	return sysfs_emit(buf, "%*pE\n", (int)strlen(svc->key), svc->key);
}
static DEVICE_ATTR_RO(key);

static int get_modalias(const struct tb_service *svc, char *buf, size_t size)
{
	return snprintf(buf, size, "tbsvc:k%sp%08Xv%08Xr%08X", svc->key,
			svc->prtcid, svc->prtcvers, svc->prtcrevs);
}

static ssize_t modalias_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct tb_service *svc = container_of(dev, struct tb_service, dev);

	/* Full buffer size except new line and null termination */
	get_modalias(svc, buf, PAGE_SIZE - 2);
	return strlen(strcat(buf, "\n"));
}
static DEVICE_ATTR_RO(modalias);

static ssize_t prtcid_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct tb_service *svc = container_of(dev, struct tb_service, dev);

	return sysfs_emit(buf, "%u\n", svc->prtcid);
}
static DEVICE_ATTR_RO(prtcid);

static ssize_t prtcvers_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct tb_service *svc = container_of(dev, struct tb_service, dev);

	return sysfs_emit(buf, "%u\n", svc->prtcvers);
}
static DEVICE_ATTR_RO(prtcvers);

static ssize_t prtcrevs_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct tb_service *svc = container_of(dev, struct tb_service, dev);

	return sysfs_emit(buf, "%u\n", svc->prtcrevs);
}
static DEVICE_ATTR_RO(prtcrevs);

static ssize_t prtcstns_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct tb_service *svc = container_of(dev, struct tb_service, dev);

	return sysfs_emit(buf, "0x%08x\n", svc->prtcstns);
}
static DEVICE_ATTR_RO(prtcstns);

static struct attribute *tb_service_attrs[] = {
	&dev_attr_key.attr,
	&dev_attr_modalias.attr,
	&dev_attr_prtcid.attr,
	&dev_attr_prtcvers.attr,
	&dev_attr_prtcrevs.attr,
	&dev_attr_prtcstns.attr,
	NULL,
};

static const struct attribute_group tb_service_attr_group = {
	.attrs = tb_service_attrs,
};

static const struct attribute_group *tb_service_attr_groups[] = {
	&tb_service_attr_group,
	NULL,
};

static int tb_service_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct tb_service *svc = container_of_const(dev, struct tb_service, dev);
	char modalias[64];

	get_modalias(svc, modalias, sizeof(modalias));
	return add_uevent_var(env, "MODALIAS=%s", modalias);
}

static void tb_service_release(struct device *dev)
{
	struct tb_service *svc = container_of(dev, struct tb_service, dev);
	struct tb_xdomain *xd = tb_service_parent(svc);

	tb_service_debugfs_remove(svc);
	ida_free(&xd->service_ids, svc->id);
	kfree(svc->key);
	kfree(svc);
}

const struct device_type tb_service_type = {
	.name = "thunderbolt_service",
	.groups = tb_service_attr_groups,
	.uevent = tb_service_uevent,
	.release = tb_service_release,
};
EXPORT_SYMBOL_GPL(tb_service_type);

static int remove_missing_service(struct device *dev, void *data)
{
	struct tb_xdomain *xd = data;
	struct tb_service *svc;

	svc = tb_to_service(dev);
	if (!svc)
		return 0;

	if (!tb_property_find(xd->remote_properties, svc->key,
			      TB_PROPERTY_TYPE_DIRECTORY)) {
		/*
		 * Unbinding a service destroys everything built on it: for
		 * tbframe that is the session, the rail netdev and the
		 * ib_device, and the client sees TBFRAME_DOWN_UNPLUG even
		 * though no cable moved and no XDomain was removed. If a
		 * property re-read ever lands a block that merely OMITS a key
		 * -- a truncated or stale directory rather than a peer that
		 * genuinely dropped the service -- this silently tears down a
		 * working link and the next good read builds it again, which
		 * is an endless publish/unpublish cycle no session survives.
		 * Say so, so that case is distinguishable from a real removal.
		 */
		dev_warn(&xd->dev,
			 "unregistering service '%s': absent from the peer's property block\n",
			 svc->key);
		device_unregister(dev);
	}

	return 0;
}

static int find_service(struct device *dev, const void *data)
{
	const struct tb_property *p = data;
	struct tb_service *svc;

	svc = tb_to_service(dev);
	if (!svc)
		return 0;

	return !strcmp(svc->key, p->key);
}

static int populate_service(struct tb_service *svc,
			    struct tb_property *property)
{
	struct tb_property_dir *dir = property->value.dir;
	struct tb_property *p;

	/* Fill in standard properties */
	p = tb_property_find(dir, "prtcid", TB_PROPERTY_TYPE_VALUE);
	if (p)
		svc->prtcid = p->value.immediate;
	p = tb_property_find(dir, "prtcvers", TB_PROPERTY_TYPE_VALUE);
	if (p)
		svc->prtcvers = p->value.immediate;
	p = tb_property_find(dir, "prtcrevs", TB_PROPERTY_TYPE_VALUE);
	if (p)
		svc->prtcrevs = p->value.immediate;
	p = tb_property_find(dir, "prtcstns", TB_PROPERTY_TYPE_VALUE);
	if (p)
		svc->prtcstns = p->value.immediate;

	svc->key = kstrdup(property->key, GFP_KERNEL);
	if (!svc->key)
		return -ENOMEM;

	return 0;
}

static void enumerate_services(struct tb_xdomain *xd)
{
	struct tb_service *svc;
	struct tb_property *p;
	struct device *dev;
	int id;

	/*
	 * First remove all services that are not available anymore in
	 * the updated property block.
	 */
	device_for_each_child_reverse(&xd->dev, xd, remove_missing_service);

	/* Then re-enumerate properties creating new services as we go */
	tb_property_for_each(xd->remote_properties, p) {
		if (p->type != TB_PROPERTY_TYPE_DIRECTORY)
			continue;

		/* If the service exists already we are fine */
		dev = device_find_child(&xd->dev, p, find_service);
		if (dev) {
			put_device(dev);
			continue;
		}

		svc = kzalloc(sizeof(*svc), GFP_KERNEL);
		if (!svc)
			break;

		if (populate_service(svc, p)) {
			kfree(svc);
			break;
		}

		id = ida_alloc(&xd->service_ids, GFP_KERNEL);
		if (id < 0) {
			kfree(svc->key);
			kfree(svc);
			break;
		}
		svc->id = id;
		svc->dev.bus = &tb_bus_type;
		svc->dev.type = &tb_service_type;
		svc->dev.parent = &xd->dev;
		dev_set_name(&svc->dev, "%s.%d", dev_name(&xd->dev), svc->id);

		tb_service_debugfs_init(svc);

		if (device_register(&svc->dev)) {
			put_device(&svc->dev);
			break;
		}
	}
}

static int populate_properties(struct tb_xdomain *xd,
			       struct tb_property_dir *dir)
{
	const struct tb_property *p;

	/* Required properties */
	p = tb_property_find(dir, "deviceid", TB_PROPERTY_TYPE_VALUE);
	if (!p)
		return -EINVAL;
	xd->device = p->value.immediate;

	p = tb_property_find(dir, "vendorid", TB_PROPERTY_TYPE_VALUE);
	if (!p)
		return -EINVAL;
	xd->vendor = p->value.immediate;

	p = tb_property_find(dir, "maxhopid", TB_PROPERTY_TYPE_VALUE);
	/*
	 * USB4 inter-domain spec suggests using 15 as HopID if the
	 * other end does not announce it in a property. This is for
	 * TBT3 compatibility.
	 */
	xd->remote_max_hopid = p ? p->value.immediate : XDOMAIN_DEFAULT_MAX_HOPID;

	kfree(xd->device_name);
	xd->device_name = NULL;
	kfree(xd->vendor_name);
	xd->vendor_name = NULL;

	/* Optional properties */
	p = tb_property_find(dir, "deviceid", TB_PROPERTY_TYPE_TEXT);
	if (p)
		xd->device_name = kstrdup(p->value.text, GFP_KERNEL);
	p = tb_property_find(dir, "vendorid", TB_PROPERTY_TYPE_TEXT);
	if (p)
		xd->vendor_name = kstrdup(p->value.text, GFP_KERNEL);

	return 0;
}

static int tb_xdomain_update_link_attributes(struct tb_xdomain *xd)
{
	bool change = false;
	struct tb_port *port;
	int ret;

	port = tb_xdomain_downstream_port(xd);

	ret = tb_port_get_link_speed(port);
	if (ret < 0)
		return ret;

	if (xd->link_speed != ret)
		change = true;

	xd->link_speed = ret;

	ret = tb_port_get_link_width(port);
	if (ret < 0)
		return ret;

	if (xd->link_width != ret)
		change = true;

	xd->link_width = ret;

	if (change)
		kobject_uevent(&xd->dev.kobj, KOBJ_CHANGE);

	return 0;
}

static int tb_xdomain_get_uuid(struct tb_xdomain *xd)
{
	struct tb *tb = xd->tb;
	uuid_t uuid;
	u64 route;
	int ret;

	dev_dbg(&xd->dev, "requesting remote UUID\n");

	/*
	 * Declared loop end: the peer is this host, so the answer is known
	 * without the wire (which a live resident ICM would intercept). The
	 * sibling route makes the uuid_equal branch below classify this as
	 * an intra-domain loop, not a single-port loopback.
	 */
	if (tb_xdomain_loop_sibling(xd->route, &route)) {
		uuid_copy(&uuid, xd->local_uuid);
		ret = 0;
	} else
		ret = tb_xdp_uuid_request(tb->ctl, xd->route, xd->state_retries,
					  &uuid, &route);
	/*
	 * An all-ones identity tail is what config space reads back when there
	 * is no POWERED router behind the link, so this is not an answer -- it
	 * is the absence of one. Fold it into the ordinary failed-read path so
	 * the same bounded budget and the same re-arm apply.
	 */
	if (!ret && tb_xdomain_uuid_is_placeholder(uuid.b)) {
		dev_dbg(&xd->dev, "no powered peer behind the link yet\n");
		ret = -ENODATA;
	}
	if (ret < 0) {
		if (xd->state_retries-- > 0) {
			dev_dbg(&xd->dev, "failed to request UUID, retrying\n");
			return -EAGAIN;
		}
		dev_dbg(&xd->dev, "failed to read remote UUID\n");
		return ret;
	}

	dev_dbg(&xd->dev, "got remote UUID %pUb\n", &uuid);

	if (uuid_equal(&uuid, xd->local_uuid)) {
		if (route == xd->route)
			dev_dbg(&xd->dev, "loop back detected\n");
		else
			dev_dbg(&xd->dev, "intra-domain loop detected\n");

		/* Don't bond lanes automatically for loops */
		xd->bonding_possible = false;
	}

	/*
	 * If the UUID is different, there is another domain connected
	 * so mark this one unplugged and wait for the connection
	 * manager to replace it.
	 *
	 * Unless what we hold is a PLACEHOLDER: the connection manager can
	 * create an XDomain for a link whose peer is powered off, and the
	 * identity it hands over is then the all-ones-tail read. That is not
	 * "another domain", it is this domain finally answering, so adopt it.
	 * Condemning here instead is what stranded appmana-008 against a
	 * powered-off appmana-019 on 2026-08-24: the placeholder was latched at
	 * boot, 019 powering on later looked like an identity change, and the
	 * resulting terminal state had no replacement path under the node's
	 * resident ICM.
	 */
	if (xd->remote_uuid && !uuid_equal(&uuid, xd->remote_uuid)) {
		if (!xd->uuid_verified ||
		    tb_xdomain_uuid_is_placeholder(xd->remote_uuid->b)) {
			dev_info(&xd->dev,
				 "verified route-local peer identity %pUb\n",
				 &uuid);
			mutex_lock(&xd->lock);
			uuid_copy(xd->remote_uuid, &uuid);
			xd->uuid_verified = true;
			xd->needs_uuid = false;
			/* Accept whatever generation the fresh peer reports. */
			xd->remote_property_block_gen = 0;
			mutex_unlock(&xd->lock);
			return 0;
		}
		dev_dbg(&xd->dev, "remote UUID is different, unplugging\n");
		xd->is_unplugged = true;
		return -ENODEV;
	}

	/* First time fill in the missing UUID */
	if (!xd->remote_uuid) {
		xd->remote_uuid = kmemdup(&uuid, sizeof(uuid_t), GFP_KERNEL);
		if (!xd->remote_uuid)
			return -ENOMEM;
	}
	xd->uuid_verified = true;
	xd->needs_uuid = false;
	xd->uuid_retry_failures = 0;

	return 0;
}

static bool tb_xdomain_had_remote_properties(struct tb_xdomain *xd)
{
	bool had_properties;

	mutex_lock(&xd->lock);
	had_properties = xd->remote_properties;
	mutex_unlock(&xd->lock);
	return had_properties;
}

static int tb_xdomain_get_link_status(struct tb_xdomain *xd)
{
	struct tb *tb = xd->tb;
	u8 slw, tlw, sls, tls;
	int ret;

	dev_dbg(&xd->dev, "sending link state status request to %pUb\n",
		xd->remote_uuid);

	ret = tb_xdp_link_state_status_request(tb->ctl, xd->route,
					       xd->state_retries, &slw, &tlw, &sls,
					       &tls);
	if (ret) {
		if (ret != -EOPNOTSUPP && xd->state_retries-- > 0) {
			dev_dbg(&xd->dev,
				"failed to request remote link status, retrying\n");
			return -EAGAIN;
		}
		/*
		 * Log the errno. -EOPNOTSUPP means the peer answered
		 * ERROR_NOT_SUPPORTED and bonding is simply unavailable;
		 * -ETIMEDOUT means it never answered at all. Those demand
		 * opposite responses -- stop asking, versus keep retrying --
		 * and without the code they are indistinguishable. Observed
		 * on appmana-019 against appmana-027 as an unbounded re-arm
		 * loop that never printed the retry line, which is only
		 * reachable on -EOPNOTSUPP or exhausted retries.
		 */
		dev_err(&xd->dev,
			"failed to receive remote link status: %d (retries left %d, rearm %u)\n",
			ret, xd->state_retries, xd->bonding_rearm_attempts);
		return ret;
	}

	dev_dbg(&xd->dev, "remote link supports width %#x speed %#x\n", slw, sls);

	if (slw < LANE_ADP_CS_0_SUPPORTED_WIDTH_DUAL) {
		dev_dbg(&xd->dev, "remote adapter is single lane only\n");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int tb_xdomain_link_state_change(struct tb_xdomain *xd,
					unsigned int width)
{
	struct tb_port *port = tb_xdomain_downstream_port(xd);
	struct tb *tb = xd->tb;
	u8 tlw, tls;
	u32 val;
	int ret;

	if (width == 2)
		tlw = LANE_ADP_CS_1_TARGET_WIDTH_DUAL;
	else if (width == 1)
		tlw = LANE_ADP_CS_1_TARGET_WIDTH_SINGLE;
	else
		return -EINVAL;

	/* Use the current target speed */
	ret = tb_port_read(port, &val, TB_CFG_PORT, port->cap_phy + LANE_ADP_CS_1, 1);
	if (ret)
		return ret;
	tls = val & LANE_ADP_CS_1_TARGET_SPEED_MASK;

	dev_dbg(&xd->dev, "sending link state change request with width %#x speed %#x\n",
		tlw, tls);

	ret = tb_xdp_link_state_change_request(tb->ctl, xd->route,
					       xd->state_retries, tlw, tls);
	if (ret) {
		if (ret != -EOPNOTSUPP && xd->state_retries-- > 0) {
			dev_dbg(&xd->dev,
				"failed to change remote link state, retrying\n");
			return -EAGAIN;
		}
		dev_err(&xd->dev, "failed request link state change, aborting\n");
		return ret;
	}

	dev_dbg(&xd->dev, "received link state change response\n");
	return 0;
}

static int tb_xdomain_bond_lanes_uuid_high(struct tb_xdomain *xd)
{
	unsigned int width, width_mask;
	struct tb_port *port;
	int ret;

	if (xd->target_link_width == LANE_ADP_CS_1_TARGET_WIDTH_SINGLE) {
		width = TB_LINK_WIDTH_SINGLE;
		width_mask = width;
	} else if (xd->target_link_width == LANE_ADP_CS_1_TARGET_WIDTH_DUAL) {
		width = TB_LINK_WIDTH_DUAL;
		width_mask = width | TB_LINK_WIDTH_ASYM_TX | TB_LINK_WIDTH_ASYM_RX;
	} else {
		if (xd->state_retries-- > 0) {
			dev_dbg(&xd->dev,
				"link state change request not received yet, retrying\n");
			return -EAGAIN;
		}
		dev_dbg(&xd->dev, "timeout waiting for link change request\n");
		return -ETIMEDOUT;
	}

	port = tb_xdomain_downstream_port(xd);

	/*
	 * We can't use tb_xdomain_lane_bonding_enable() here because it
	 * is the other side that initiates lane bonding. So here we
	 * just set the width to both lane adapters and wait for the
	 * link to transition bonded.
	 */
	ret = tb_port_set_link_width(port->dual_link_port, width);
	if (ret) {
		tb_port_warn(port->dual_link_port,
			     "failed to set link width to %d\n", width);
		return ret;
	}

	ret = tb_port_set_link_width(port, width);
	if (ret) {
		tb_port_warn(port, "failed to set link width to %d\n", width);
		return ret;
	}

	ret = tb_port_wait_for_link_width(port, width_mask,
					  XDOMAIN_BONDING_TIMEOUT);
	if (ret) {
		dev_warn(&xd->dev, "error waiting for link width to become %d\n",
			 width_mask);
		return ret;
	}

	port->bonded = width > TB_LINK_WIDTH_SINGLE;
	port->dual_link_port->bonded = width > TB_LINK_WIDTH_SINGLE;

	tb_port_update_credits(port);
	tb_xdomain_update_link_attributes(xd);

	dev_dbg(&xd->dev, "lane bonding %s\n", str_enabled_disabled(width == 2));
	return 0;
}

static int tb_xdomain_get_properties(struct tb_xdomain *xd)
{
	struct tb_property_dir *dir;
	struct tb *tb = xd->tb;
	bool update = false;
	u32 *block = NULL;
	u32 gen = 0;
	int ret;

	dev_dbg(&xd->dev, "requesting remote properties\n");

	/*
	 * Declared loop end: the "remote" property block is this host's own
	 * local block, byte-identical to what a wire peer would receive.
	 */
	if (tb_xdomain_is_loop_route(xd->route)) {
		update_property_block(xd);
		mutex_lock(&xd->lock);
		if (xd->local_property_block) {
			block = kmemdup(xd->local_property_block,
					xd->local_property_block_len *
					sizeof(*block), GFP_KERNEL);
			gen = xd->local_property_block_gen;
			ret = block ? (int)xd->local_property_block_len :
				      -ENOMEM;
		} else
			ret = -ENODATA;
		mutex_unlock(&xd->lock);
	} else
		ret = tb_xdp_properties_request(tb->ctl, xd->route,
						xd->local_uuid,
						xd->remote_uuid,
						xd->state_retries, &block,
						&gen);
	if (ret < 0) {
		if (xd->state_retries-- > 0) {
			dev_dbg(&xd->dev,
				"failed to request remote properties, retrying\n");
			return -EAGAIN;
		}
		/*
		 * Give up now. Log the errno: this message is the only
		 * record of why a peer never became reachable, and without
		 * it -ETIMEDOUT (the peer's CM never answered) is
		 * indistinguishable from -EIO (it answered with an error) or
		 * -ENODATA (it answered with nothing). Observed on
		 * appmana-019 and appmana-018 against the same peer, where
		 * the XDomain was created, this failed, and the device was
		 * torn back down leaving the peer permanently invisible
		 * while it kept announcing.
		 */
		dev_err(&xd->dev,
			"failed read XDomain properties from %pUb: %d (route %llx, retries exhausted)\n",
			xd->remote_uuid, ret, xd->route);

		return ret;
	}

	mutex_lock(&xd->lock);
	/*
	 * Firmware-managed controllers own the UUID-discovery stage. The ICM
	 * connect event supplies an address claim, and a properties response
	 * that names the claimed peer as source and this domain as destination
	 * is the route-local peer proof. Nothing may globally match or publish
	 * the identity before this exchange succeeds.
	 */
	xd->uuid_verified = true;
	xd->needs_uuid = false;

	/* Only accept newer generation properties (cached_gen==0 forces accept) */
	if (tb_xdomain_generation_stale(xd->remote_properties, gen,
					xd->remote_property_block_gen)) {
		ret = 0;
		goto err_free_block;
	}

	dir = tb_property_parse_dir(block, ret);
	if (!dir) {
		dev_err(&xd->dev, "failed to parse XDomain properties\n");
		ret = -ENOMEM;
		goto err_free_block;
	}

	ret = populate_properties(xd, dir);
	if (ret) {
		dev_err(&xd->dev, "missing XDomain properties in response\n");
		goto err_free_dir;
	}

	/* Release the existing one */
	if (xd->remote_properties) {
		tb_property_free_dir(xd->remote_properties);
		update = true;
	}

	xd->remote_properties = dir;
	xd->remote_property_block_gen = gen;

	tb_xdomain_update_link_attributes(xd);

	mutex_unlock(&xd->lock);

	kfree(block);

	/*
	 * Now the device should be ready enough so we can add it to the
	 * bus and let userspace know about it. If the device is already
	 * registered, we notify the userspace that it has changed.
	 */
	if (!update) {
		/*
		 * Now disable lane 1 if bonding was not enabled. Do
		 * this only if bonding was possible at the beginning
		 * (that is we are the connection manager and there are
		 * two lanes).
		 */
		if (xd->bonding_possible) {
			struct tb_port *port;

			port = tb_xdomain_downstream_port(xd);
			/*
			 * Keep lane 1 armed while the bounded re-arm can
			 * still bond this link; disable it only once the
			 * budget is exhausted (mainline power behavior).
			 */
			if (!port->bonded && !tb_xdomain_bonding_rearm_needed(xd))
				tb_port_disable(port->dual_link_port);
		}

		dev_dbg(&xd->dev, "current link speed %u.0 Gb/s\n",
			xd->link_speed);
		dev_dbg(&xd->dev, "current link width %s\n",
			tb_width_name(xd->link_width));

		if (device_add(&xd->dev)) {
			dev_err(&xd->dev, "failed to add XDomain device\n");
			return -ENODEV;
		}
		dev_info(&xd->dev, "new host found, vendor=%#x device=%#x\n",
			 xd->vendor, xd->device);
		if (xd->vendor_name && xd->device_name)
			dev_info(&xd->dev, "%s %s\n", xd->vendor_name,
				 xd->device_name);

		tb_xdomain_debugfs_init(xd);
	} else {
		kobject_uevent(&xd->dev.kobj, KOBJ_CHANGE);
	}

	enumerate_services(xd);
	tb_xdomain_control_event(xd, TB_XDOMAIN_CONTROL_PEER_PROVEN);
	return 0;

err_free_dir:
	tb_property_free_dir(dir);
err_free_block:
	kfree(block);
	mutex_unlock(&xd->lock);

	return ret;
}

static unsigned int tb_xdomain_uuid_retry_delay_ms(unsigned int failures)
{
	return XDOMAIN_DEFAULT_TIMEOUT <<
		min(failures, (unsigned int)XDOMAIN_UUID_BACKOFF_MAX_SHIFT);
}

static void tb_xdomain_queue_uuid(struct tb_xdomain *xd)
{
	xd->state = XDOMAIN_STATE_UUID;
	xd->state_retries = XDOMAIN_RETRIES;
	xd->uuid_retry_failures = 0;
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_SHORT_TIMEOUT));
}

#if IS_ENABLED(CONFIG_USB4_KUNIT_TEST)
unsigned int tb_test_xdomain_uuid_retry_delay_ms(unsigned int failures)
{
	return tb_xdomain_uuid_retry_delay_ms(failures);
}
#endif

static void tb_xdomain_queue_uuid_backoff(struct tb_xdomain *xd)
{
	unsigned int failures = xd->uuid_retry_failures;
	unsigned int delay_ms;

	xd->state = XDOMAIN_STATE_UUID;
	xd->state_retries = 0;
	if (failures < XDOMAIN_UUID_BACKOFF_MAX_SHIFT)
		xd->uuid_retry_failures = failures + 1;
	delay_ms = tb_xdomain_uuid_retry_delay_ms(failures);
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(delay_ms));
}

static void tb_xdomain_queue_link_status(struct tb_xdomain *xd)
{
	xd->state = XDOMAIN_STATE_LINK_STATUS;
	xd->state_retries = XDOMAIN_RETRIES;
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_DEFAULT_TIMEOUT));
}

static void tb_xdomain_queue_link_status2(struct tb_xdomain *xd)
{
	xd->state = XDOMAIN_STATE_LINK_STATUS2;
	xd->state_retries = XDOMAIN_RETRIES;
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_DEFAULT_TIMEOUT));
}

/* Schedule the next bounded re-arm attempt (no-op once bonded/exhausted). */
static void tb_xdomain_queue_bonding_rearm(struct tb_xdomain *xd)
{
	if (!tb_xdomain_bonding_rearm_needed(xd))
		return;

	dev_dbg(&xd->dev, "scheduling lane bonding re-arm attempt %u of %u\n",
		xd->bonding_rearm_attempts + 1,
		(unsigned int)XDOMAIN_BONDING_REARM_ATTEMPTS);
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_BONDING_REARM_TIMEOUT <<
					    xd->bonding_rearm_attempts));
}

/*
 * Bonding succeeded after the XDomain was already enumerated and its
 * services published: the DMA paths in use were born with x1 credit
 * sizing. Re-announce the property directories (generation bump, never an
 * unregister/re-register of live dirs) so the peers re-probe and the
 * service sessions re-establish their paths with bonded credits.
 */
static void tb_xdomain_bonding_late_success(struct tb_xdomain *xd)
{
	if (!device_is_registered(&xd->dev))
		return;

	dev_info(&xd->dev,
		 "lane bonding enabled after enumeration; re-announcing properties\n");
	tb_reannounce_property_dirs();
}

static void tb_xdomain_queue_bonding(struct tb_xdomain *xd)
{
	if (memcmp(xd->local_uuid, xd->remote_uuid, UUID_SIZE) > 0) {
		dev_dbg(&xd->dev, "we have higher UUID, other side bonds the lanes\n");
		xd->state = XDOMAIN_STATE_BONDING_UUID_HIGH;
	} else {
		dev_dbg(&xd->dev, "we have lower UUID, bonding lanes\n");
		xd->state = XDOMAIN_STATE_LINK_STATE_CHANGE;
	}

	xd->state_retries = XDOMAIN_RETRIES;
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_DEFAULT_TIMEOUT));
}

static void tb_xdomain_queue_direct_bonding(struct tb_xdomain *xd)
{
	xd->state = XDOMAIN_STATE_DIRECT_BONDING_ENABLE;
	xd->state_retries = XDOMAIN_DIRECT_BOND_RETRIES;
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_SHORT_TIMEOUT));
}

static void tb_xdomain_queue_direct_bonding_wait(struct tb_xdomain *xd)
{
	xd->state = XDOMAIN_STATE_DIRECT_BONDING_WAIT;
	xd->state_retries = XDOMAIN_DIRECT_BOND_RETRIES;
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_SHORT_TIMEOUT));
}

static bool tb_xdomain_port_connected(struct tb_port *port, int *error)
{
	int state = tb_port_state(port);

	if (state < 0) {
		*error = state;
		return false;
	}

	*error = 0;
	return state == TB_PORT_UP ||
	       (state >= TB_PORT_TX_CL0S && state <= TB_PORT_CL2);
}

/*
 * Arm one end without polling. XDomain state works for both physical
 * connectors share the controller's ordered workqueue, so any wait here can
 * prevent the matching peer-facing port from ever overlapping its remote
 * target-DUAL window.
 */
static int tb_xdomain_direct_bonding_enable_step(struct tb_xdomain *xd)
{
	struct tb_port *port = tb_xdomain_downstream_port(xd);
	int ret;

	if (!port->dual_link_port)
		return -ENODEV;

	ret = tb_port_enable(port->dual_link_port);
	if (ret)
		return ret;
	if (!tb_xdomain_port_connected(port->dual_link_port, &ret))
		return ret ?: -EAGAIN;

	return tb_port_lane_bonding_enable(port);
}

static int tb_xdomain_direct_bonding_wait_step(struct tb_xdomain *xd)
{
	unsigned int width_mask = TB_LINK_WIDTH_DUAL |
		TB_LINK_WIDTH_ASYM_TX | TB_LINK_WIDTH_ASYM_RX;
	struct tb_port *port = tb_xdomain_downstream_port(xd);
	int width;

	width = tb_port_get_link_width(port);
	if (width < 0)
		return width == -EACCES ? -EAGAIN : width;
	return width & width_mask ? 0 : -EAGAIN;
}

static void tb_xdomain_direct_bonding_finish(struct tb_xdomain *xd)
{
	struct tb_port *port = tb_xdomain_downstream_port(xd);

	tb_port_update_credits(port);
	tb_xdomain_update_link_attributes(xd);
	dev_dbg(&xd->dev, "lane bonding enabled before service publication\n");
}

static void tb_xdomain_direct_bonding_abort(struct tb_xdomain *xd, int ret)
{
	dev_warn(&xd->dev,
		 "early direct lane bonding failed: %d; continuing single-lane\n",
		 ret);
	/*
	 * Leave the lane adapters alone. This fallback runs under firmware CM,
	 * which owns lane-1 cleanup, and the link may still be a perfectly valid
	 * x1 XDomain. Disabling adapters here destroyed that surviving link on
	 * Maple Ridge after a hot reload and required a physical edge/reboot to
	 * recover. This mirrors tb_xdomain_link_exit(), which deliberately does
	 * not disable lanes when firmware controls bonding.
	 */
}

static void tb_xdomain_queue_bonding_uuid_low(struct tb_xdomain *xd)
{
	xd->state = XDOMAIN_STATE_BONDING_UUID_LOW;
	xd->state_retries = XDOMAIN_RETRIES;
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_DEFAULT_TIMEOUT));
}

static void tb_xdomain_queue_properties(struct tb_xdomain *xd)
{
	xd->state = XDOMAIN_STATE_PROPERTIES;
	xd->state_retries = XDOMAIN_RETRIES;
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_DEFAULT_TIMEOUT));
}

/*
 * Arm (or re-arm) the announcement. properties_changed_retries counts
 * CONSECUTIVE FAILURES upwards: it is the backoff shift and the diagnostic
 * rate-limiter, and re-arming resets it so a peer-present event always gets the
 * fast cadence back. The field name is fixed by the out-of-tree public header.
 */
static void tb_xdomain_queue_properties_changed(struct tb_xdomain *xd)
{
	WRITE_ONCE(xd->properties_changed_retries, 0);
	queue_delayed_work(xd->tb->wq, &xd->properties_changed_work,
			   msecs_to_jiffies(XDOMAIN_SHORT_TIMEOUT));
}

static void tb_xdomain_failed(struct tb_xdomain *xd)
{
	xd->state = XDOMAIN_STATE_ERROR;
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_DEFAULT_TIMEOUT));
}

static void tb_xdomain_state_work(struct work_struct *work)
{
	struct tb_xdomain *xd = container_of(work, typeof(*xd), state_work.work);
	bool announcement_ready;
	int ret, state = xd->state;

	if (WARN_ON_ONCE(state < XDOMAIN_STATE_INIT ||
			 state > XDOMAIN_STATE_ERROR))
		return;

	dev_dbg(&xd->dev, "running state %s\n", state_names[state]);

	switch (state) {
	case XDOMAIN_STATE_INIT:
		if (xd->needs_uuid) {
			tb_xdomain_queue_uuid(xd);
		} else {
			if (tb_xdomain_announce_ready(xd->needs_uuid,
						      xd->uuid_verified))
				tb_xdomain_queue_properties_changed(xd);
			if (tb_xdomain_initial_state_needs_link_status(
					xd->needs_uuid, xd->bonding_possible))
				tb_xdomain_queue_link_status(xd);
			else
				tb_xdomain_queue_properties(xd);
		}
		break;

	case XDOMAIN_STATE_UUID:
		ret = tb_xdomain_get_uuid(xd);
		if (ret) {
			if (ret == -EAGAIN)
				goto retry_state;
			/*
			 * A condemned identity (the peer answered with a
			 * DIFFERENT UUID: xd->is_unplugged) is terminal for
			 * THIS XDomain -- the connection manager replaces it
			 * (tb.c reconciliation synthesizes the unplug). A mere
			 * timeout is not: the peer may still be booting
			 * (the co-reset strand, f876653), so keep re-verifying
			 * instead of stopping the handshake.
			 */
			if (xd->is_unplugged || ret == -ENOMEM) {
				tb_xdomain_failed(xd);
			} else if (tb_xdomain_had_remote_properties(xd)) {
				/*
				 * This was an ENUMERATED peer whose property
				 * refresh failed and whose cached UUID now also
				 * gets no answer. Keeping that old XDomain alive
				 * is not protecting a booting peer: it prevents
				 * the connection manager from creating the fresh
				 * XDomain that can discover it. Observed on
				 * appmana-025<->023 after sequential reboot:
				 * lanes and native control traffic were live, but
				 * both sides retained half-read UUIDs ending in
				 * ffff:ffffffffffff and no tbverbs service ever
				 * re-probed. Condemn only previously-enumerated
				 * objects; a first-boot XDomain with no cached
				 * properties keeps the unbounded co-reset retry.
				 */
				dev_warn(&xd->dev,
					 "previously enumerated identity stopped answering; replacing stale XDomain\n");
				xd->is_unplugged = true;
				tb_xdomain_failed(xd);
			} else {
				/*
				 * Keep an unverified route discoverable, but after the
				 * initial fast budget probe it at an exponential cadence.
				 * A new handshake event uses queue_uuid() and restores the
				 * fast budget immediately.
				 */
				tb_xdomain_queue_uuid_backoff(xd);
			}
		} else {
			tb_xdomain_queue_properties_changed(xd);
			if (xd->bonding_possible)
				tb_xdomain_queue_link_status(xd);
			else
				tb_xdomain_queue_properties(xd);
		}
		break;

	case XDOMAIN_STATE_LINK_STATUS:
		ret = tb_xdomain_get_link_status(xd);
		if (ret) {
			if (ret == -EAGAIN)
				goto retry_state;
			if (tb_xdomain_should_fallback_to_direct_bonding(
					xd->bonding_possible,
					device_is_registered(&xd->dev), ret,
					false)) {
				tb_xdomain_queue_direct_bonding(xd);
				break;
			}
			xd->bonding_possible = tb_xdomain_bonding_after(xd->bonding_possible,
									ret, false);

			/*
			 * If any of the lane bonding states fail we skip
			 * bonding completely and try to continue from
			 * reading properties.
			 */
			tb_xdomain_queue_properties(xd);
		} else {
			tb_xdomain_queue_bonding(xd);
		}
		break;

	case XDOMAIN_STATE_DIRECT_BONDING_ENABLE:
		ret = tb_xdomain_direct_bonding_enable_step(xd);
		if (ret) {
			if (ret == -EAGAIN && xd->state_retries-- > 0)
				goto retry_state;
			tb_xdomain_direct_bonding_abort(xd, ret);
			tb_xdomain_queue_properties(xd);
		} else {
			tb_xdomain_queue_direct_bonding_wait(xd);
		}
		break;

	case XDOMAIN_STATE_DIRECT_BONDING_WAIT:
		ret = tb_xdomain_direct_bonding_wait_step(xd);
		if (ret) {
			if (ret == -EAGAIN && xd->state_retries-- > 0)
				goto retry_state;
			tb_xdomain_direct_bonding_abort(xd, ret);
		} else {
			tb_xdomain_direct_bonding_finish(xd);
		}
		tb_xdomain_queue_properties(xd);
		break;

	case XDOMAIN_STATE_LINK_STATE_CHANGE:
		ret = tb_xdomain_link_state_change(xd, 2);
		if (ret) {
			if (ret == -EAGAIN)
				goto retry_state;
			xd->bonding_possible = tb_xdomain_bonding_after(xd->bonding_possible,
									ret, false);
			tb_xdomain_queue_properties(xd);
		} else {
			tb_xdomain_queue_link_status2(xd);
		}
		break;

	case XDOMAIN_STATE_LINK_STATUS2:
		ret = tb_xdomain_get_link_status(xd);
		if (ret) {
			if (ret == -EAGAIN)
				goto retry_state;
			xd->bonding_possible = tb_xdomain_bonding_after(xd->bonding_possible,
									ret, false);
			tb_xdomain_queue_properties(xd);
		} else {
			tb_xdomain_queue_bonding_uuid_low(xd);
		}
		break;

	case XDOMAIN_STATE_BONDING_UUID_LOW:
		if (!tb_xdomain_lane_bonding_enable(xd))
			tb_xdomain_bonding_late_success(xd);
		tb_xdomain_queue_properties(xd);
		break;

	case XDOMAIN_STATE_BONDING_UUID_HIGH:
		ret = tb_xdomain_bond_lanes_uuid_high(xd);
		if (ret == -EAGAIN)
			goto retry_state;
		if (!ret)
			tb_xdomain_bonding_late_success(xd);
		tb_xdomain_queue_properties(xd);
		break;

	case XDOMAIN_STATE_PROPERTIES:
		announcement_ready =
			tb_xdomain_announce_ready(xd->needs_uuid,
						  xd->uuid_verified);
		ret = tb_xdomain_get_properties(xd);
		if (ret) {
			if (ret == -EAGAIN)
				goto retry_state;
			/*
			 * A non-EAGAIN failure here usually means the peer is
			 * still BOOTING and has not registered its property
			 * block yet. The old tb_xdomain_failed() ->
			 * XDOMAIN_STATE_ERROR -> __stop_handshake() was terminal
			 * and stranded a SIMULTANEOUS reboot forever: both peers
			 * exhaust the read budget before either answers, both
			 * stop, and a stopped host sends no XDP request so
			 * neither Maple ICM re-arms its announce
			 * (appmana-020<->009). Keep re-reading instead.
			 *
			 * But do NOT re-queue PROPERTIES blindly: property
			 * requests carry dst_uuid and a healthy peer ignores a
			 * mismatch, so a STALE/CORRUPT cached identity fails
			 * this read forever (appmana-008 port 1 looped 87
			 * minutes against a corrupt half-trained-link UUID,
			 * ...-ffff-ffffffffffff). Route the retry back through
			 * the UUID state: a still-booting peer keeps the loop
			 * alive exactly as before, while a peer that answers
			 * with a different UUID condemns this XDomain
			 * (tb_xdomain_get_uuid -> is_unplugged) so the
			 * connection manager replaces it. KUnit:
			 * tb_test_xdomain_stale_identity_recovery.
			 */
			if (xd->remote_uuid)
				tb_xdomain_queue_uuid(xd);
			else
				tb_xdomain_queue_properties(xd);
		} else {
			/*
			 * A firmware-supplied UUID is only an address claim. Its
			 * first route-local properties response proves the peer and
			 * starts the independent announcement machine. UUID discovery
			 * already starts it after its own successful proof, so do not
			 * re-arm that path here.
			 */
			if (!announcement_ready &&
			    tb_xdomain_announce_ready(xd->needs_uuid,
						      xd->uuid_verified))
				tb_xdomain_queue_properties_changed(xd);
			xd->state = XDOMAIN_STATE_ENUMERATED;
			tb_xdomain_queue_bonding_rearm(xd);
		}
		break;

	case XDOMAIN_STATE_ENUMERATED:
		if (tb_xdomain_bonding_rearm_needed(xd)) {
			xd->bonding_rearm_attempts++;
			dev_dbg(&xd->dev,
				"re-arming lane bonding (attempt %u of %u)\n",
				xd->bonding_rearm_attempts,
				(unsigned int)XDOMAIN_BONDING_REARM_ATTEMPTS);
			tb_xdomain_queue_link_status(xd);
		} else {
			tb_xdomain_queue_properties(xd);
		}
		break;

	case XDOMAIN_STATE_ERROR:
		dev_dbg(&xd->dev, "discovery failed, stopping handshake\n");
		__stop_handshake(xd);
		break;

	default:
		dev_warn(&xd->dev, "unexpected state %d\n", state);
		break;
	}

	return;

retry_state:
	queue_delayed_work(xd->tb->wq, &xd->state_work,
			   msecs_to_jiffies(XDOMAIN_DEFAULT_TIMEOUT));
}

static void tb_xdomain_properties_changed(struct work_struct *work)
{
	struct tb_xdomain *xd = container_of(work, typeof(*xd),
					     properties_changed_work.work);
	int ret;

	dev_dbg(&xd->dev, "sending properties changed notification\n");

	/*
	 * Loop end: the peer observing our property change is this same
	 * XDomain (it reads the local block), so apply the effect the
	 * inbound PROPERTIES_CHANGED_REQUEST handler would have applied --
	 * force a fresh accept and re-run the state machine. No wire.
	 */
	if (tb_xdomain_is_loop_route(xd->route)) {
		mutex_lock(&xd->lock);
		if (!xd->removing && device_is_registered(&xd->dev)) {
			xd->remote_property_block_gen = 0;
			queue_delayed_work(xd->tb->wq, &xd->state_work,
					   msecs_to_jiffies(XDOMAIN_SHORT_TIMEOUT));
		}
		mutex_unlock(&xd->lock);
		return;
	}

	ret = tb_xdp_properties_changed_request(xd->tb->ctl, xd->route,
				max(READ_ONCE(xd->properties_changed_retries), 0),
				xd->local_uuid);
	if (ret) {
		int pending = READ_ONCE(xd->properties_changed_retries);
		unsigned int failures;

		/*
		 * __stop_handshake() may have run while we were inside the
		 * bounded wire wait above. Honour it here: re-arming past it
		 * would defeat its cancel_delayed_work_sync().
		 */
		if (pending == TB_XDOMAIN_ANNOUNCE_STOPPED)
			return;
		failures = pending;

		/*
		 * Never stop announcing. This notification is what makes a
		 * peer's connection manager re-arm its XDomain announce, so a
		 * host that gives up can no longer be found by a neighbour that
		 * powers on later -- appmana-008 gave up ~10 s into a boot whose
		 * chain neighbour was powered off and never spoke to it again.
		 * The spacing doubles to a 64 s ceiling so an absent neighbour
		 * costs about one control packet per minute, and the diagnostic
		 * is emitted once per doubling instead of once per attempt.
		 */
		if (tb_xdomain_announce_should_warn(failures))
			dev_err(&xd->dev,
				"failed to send properties changed notification: %d (route %llx, failure %u)\n",
				ret, xd->route, failures);
		else
			dev_dbg(&xd->dev,
				"failed to send properties changed notification: %d (route %llx, failure %u)\n",
				ret, xd->route, failures);

		/*
		 * Clamped at the ceiling: the counter only feeds the delay and
		 * the rate-limit, both of which saturate there, and a counter
		 * that grew forever would eventually overflow. The XDP sequence
		 * nibble then stops rotating, which is safe because the request
		 * timeout (1 s) is far inside the ceiling spacing (64 s), so two
		 * attempts can never be in flight together.
		 */
		if (failures <= TB_XDOMAIN_ANNOUNCE_MAX_SHIFT)
			WRITE_ONCE(xd->properties_changed_retries, failures + 1);

		/*
		 * Do not mistake an absent peer for failed hardware. Recovery is
		 * eligible only after a previously correlated inbound service
		 * request proves this peer is still active, while announcements in
		 * the opposite direction have reached saturated backoff.
		 */
		if (failures > TB_XDOMAIN_ANNOUNCE_MAX_SHIFT &&
		    tb_xdomain_control_recovery_required(xd)) {
			int recovery_ret;

			dev_err(&xd->dev,
				"one-way XDomain control failure persisted at saturated backoff; requesting controller recovery\n");
			recovery_ret = tb_nhi_request_control_recovery(xd->tb->nhi,
								xd->route);
			if (recovery_ret && recovery_ret != -EALREADY)
				dev_err(&xd->dev,
					"failed to dispatch control-path recovery: %d\n",
					recovery_ret);
			tb_xdomain_control_event(
				xd, TB_XDOMAIN_CONTROL_RECOVERY_DISPATCHED);
		}
		queue_delayed_work(xd->tb->wq, &xd->properties_changed_work,
				   msecs_to_jiffies(tb_xdomain_announce_delay_ms(failures)));
		return;
	}

	if (READ_ONCE(xd->properties_changed_retries) !=
	    TB_XDOMAIN_ANNOUNCE_STOPPED)
		WRITE_ONCE(xd->properties_changed_retries, 0);
	tb_xdomain_control_event(xd, TB_XDOMAIN_CONTROL_OUTBOUND_SUCCEEDED);
	tb_nhi_runtime_control_path_proven(xd->tb->nhi, xd->route);
}

static ssize_t device_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct tb_xdomain *xd = container_of(dev, struct tb_xdomain, dev);

	return sysfs_emit(buf, "%#x\n", xd->device);
}
static DEVICE_ATTR_RO(device);

static ssize_t
device_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tb_xdomain *xd = container_of(dev, struct tb_xdomain, dev);
	int ret;

	if (mutex_lock_interruptible(&xd->lock))
		return -ERESTARTSYS;
	ret = sysfs_emit(buf, "%s\n", xd->device_name ?: "");
	mutex_unlock(&xd->lock);

	return ret;
}
static DEVICE_ATTR_RO(device_name);

static ssize_t maxhopid_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct tb_xdomain *xd = container_of(dev, struct tb_xdomain, dev);

	return sysfs_emit(buf, "%d\n", xd->remote_max_hopid);
}
static DEVICE_ATTR_RO(maxhopid);

static ssize_t vendor_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct tb_xdomain *xd = container_of(dev, struct tb_xdomain, dev);

	return sysfs_emit(buf, "%#x\n", xd->vendor);
}
static DEVICE_ATTR_RO(vendor);

static ssize_t
vendor_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tb_xdomain *xd = container_of(dev, struct tb_xdomain, dev);
	int ret;

	if (mutex_lock_interruptible(&xd->lock))
		return -ERESTARTSYS;
	ret = sysfs_emit(buf, "%s\n", xd->vendor_name ?: "");
	mutex_unlock(&xd->lock);

	return ret;
}
static DEVICE_ATTR_RO(vendor_name);

static ssize_t unique_id_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct tb_xdomain *xd = container_of(dev, struct tb_xdomain, dev);

	return sysfs_emit(buf, "%pUb\n", xd->remote_uuid);
}
static DEVICE_ATTR_RO(unique_id);

static ssize_t speed_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct tb_xdomain *xd = container_of(dev, struct tb_xdomain, dev);

	return sysfs_emit(buf, "%u.0 Gb/s\n", xd->link_speed);
}

static DEVICE_ATTR(rx_speed, 0444, speed_show, NULL);
static DEVICE_ATTR(tx_speed, 0444, speed_show, NULL);

static ssize_t rx_lanes_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct tb_xdomain *xd = container_of(dev, struct tb_xdomain, dev);
	unsigned int width;

	switch (xd->link_width) {
	case TB_LINK_WIDTH_SINGLE:
	case TB_LINK_WIDTH_ASYM_TX:
		width = 1;
		break;
	case TB_LINK_WIDTH_DUAL:
		width = 2;
		break;
	case TB_LINK_WIDTH_ASYM_RX:
		width = 3;
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	return sysfs_emit(buf, "%u\n", width);
}
static DEVICE_ATTR(rx_lanes, 0444, rx_lanes_show, NULL);

static ssize_t tx_lanes_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct tb_xdomain *xd = container_of(dev, struct tb_xdomain, dev);
	unsigned int width;

	switch (xd->link_width) {
	case TB_LINK_WIDTH_SINGLE:
	case TB_LINK_WIDTH_ASYM_RX:
		width = 1;
		break;
	case TB_LINK_WIDTH_DUAL:
		width = 2;
		break;
	case TB_LINK_WIDTH_ASYM_TX:
		width = 3;
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	return sysfs_emit(buf, "%u\n", width);
}
static DEVICE_ATTR(tx_lanes, 0444, tx_lanes_show, NULL);

static int unregister_service(struct device *dev, void *data);

/*
 * Force a full re-read of the remote peer's XDomain property directory and
 * re-probe its services, bypassing the monotonic-generation gate in
 * tb_xdomain_get_properties(). Needed because the ICM firmware (Titan Ridge /
 * Maple Ridge) only re-announces an XDomain on a physical link edge and the
 * XDP properties-changed notification is best-effort, so after a simultaneous
 * fleet driver reload a peer can re-register its services while our cached
 * generation stays latched and the re-read is silently dropped. Writing 1 here
 * recovers such a stuck link without a cable replug.
 */
static ssize_t rescan_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct tb_xdomain *xd = container_of(dev, struct tb_xdomain, dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;
	if (!val)
		return count;

	mutex_lock(&xd->lock);
	xd->remote_property_block_gen = 0;
	mutex_unlock(&xd->lock);

	/* Drop stale services so re-enumeration re-creates and re-probes them. */
	device_for_each_child_reverse(&xd->dev, xd, unregister_service);

	mutex_lock(&xd->lock);
	if (!xd->removing)
		queue_delayed_work(xd->tb->wq, &xd->state_work, 0);
	mutex_unlock(&xd->lock);
	return count;
}
static DEVICE_ATTR_WO(rescan);

static struct attribute *xdomain_attrs[] = {
	&dev_attr_device.attr,
	&dev_attr_device_name.attr,
	&dev_attr_maxhopid.attr,
	&dev_attr_rescan.attr,
	&dev_attr_rx_lanes.attr,
	&dev_attr_rx_speed.attr,
	&dev_attr_tx_lanes.attr,
	&dev_attr_tx_speed.attr,
	&dev_attr_unique_id.attr,
	&dev_attr_vendor.attr,
	&dev_attr_vendor_name.attr,
	NULL,
};

static const struct attribute_group xdomain_attr_group = {
	.attrs = xdomain_attrs,
};

static const struct attribute_group *xdomain_attr_groups[] = {
	&xdomain_attr_group,
	NULL,
};

static void tb_xdomain_release(struct device *dev)
{
	struct tb_xdomain *xd = container_of(dev, struct tb_xdomain, dev);

	put_device(xd->dev.parent);

	kfree(xd->local_property_block);
	tb_property_free_dir(xd->remote_properties);
	ida_destroy(&xd->out_hopids);
	ida_destroy(&xd->in_hopids);
	ida_destroy(&xd->service_ids);

	kfree(xd->local_uuid);
	kfree(xd->remote_uuid);
	kfree(xd->device_name);
	kfree(xd->vendor_name);
	kfree(xd);
}

static int __maybe_unused tb_xdomain_suspend(struct device *dev)
{
	stop_handshake(tb_to_xdomain(dev));
	return 0;
}

static int __maybe_unused tb_xdomain_resume(struct device *dev)
{
	start_handshake(tb_to_xdomain(dev));
	return 0;
}

static const struct dev_pm_ops tb_xdomain_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(tb_xdomain_suspend, tb_xdomain_resume)
};

const struct device_type tb_xdomain_type = {
	.name = "thunderbolt_xdomain",
	.release = tb_xdomain_release,
	.pm = &tb_xdomain_pm_ops,
};
EXPORT_SYMBOL_GPL(tb_xdomain_type);

static bool tb_xdomain_should_negotiate_bonding(bool has_dual_link,
						 unsigned int generation,
						 enum tb_link_width width)
{
	if (!has_dual_link)
		return false;

	/*
	 * Gen 4 links are specified to train bonded before enumeration, but
	 * Maple Ridge host-to-host links can arrive here at single width. Treat
	 * the negotiated width as authoritative instead of assuming that the
	 * generation proves lane bonding already happened.
	 */
	return generation < 4 || width <= TB_LINK_WIDTH_SINGLE;
}

bool tb_test_xdomain_should_negotiate_bonding(bool has_dual_link,
					       unsigned int generation,
					       enum tb_link_width width)
{
	return tb_xdomain_should_negotiate_bonding(has_dual_link, generation,
						     width);
}

static bool tb_xdomain_should_initialize_link(bool remote_uuid_known)
{
	(void)remote_uuid_known;
	/* A known UUID skips only the UUID query, not physical link setup. */
	return true;
}

static bool tb_xdomain_initial_needs_uuid(bool remote_uuid_known)
{
	/* Firmware-managed controllers own UUID discovery for known peers. */
	return !remote_uuid_known;
}

static bool tb_xdomain_announce_ready(bool needs_uuid, bool uuid_verified)
{
	return !needs_uuid && uuid_verified;
}

#if IS_ENABLED(CONFIG_USB4_KUNIT_TEST)
struct tb_cfg_request_state
tb_test_xdomain_packet_state(bool expects_peer_response,
			     bool *has_local_classifier)
{
	struct tb_cfg_request req = { };

	tb_xdomain_init_packet_state(&req, expects_peer_response);
	*has_local_classifier = req.intermediate != NULL;

	return req.state;
}

bool tb_test_xdomain_response_should_defer(void)
{
	return tb_xdomain_response_should_defer();
}

bool tb_test_xdomain_response_should_retry(bool local_failed,
					   bool local_timed_out,
					   int result,
					   unsigned int attempt)
{
	struct tb_cfg_result res = {
		.err = result,
		.local_failed = local_failed,
		.local_timed_out = local_timed_out,
	};

	return tb_xdomain_packet_should_retry(&res, attempt);
}

bool tb_test_xdomain_request_should_retry(bool local_failed,
					  bool local_timed_out,
					  int result,
					  unsigned int attempt)
{
	struct tb_cfg_result res = {
		.err = result,
		.local_failed = local_failed,
		.local_timed_out = local_timed_out,
	};

	return tb_xdomain_packet_should_retry(&res, attempt);
}

bool tb_test_xdomain_initial_needs_uuid(bool remote_uuid_known)
{
	return tb_xdomain_initial_needs_uuid(remote_uuid_known);
}

bool tb_test_xdomain_announce_ready(bool needs_uuid, bool uuid_verified)
{
	return tb_xdomain_announce_ready(needs_uuid, uuid_verified);
}
#endif

bool tb_test_xdomain_should_initialize_link(bool remote_uuid_known)
{
	return tb_xdomain_should_initialize_link(remote_uuid_known);
}

static bool tb_xdomain_initial_state_needs_link_status(bool needs_uuid,
						       bool bonding_possible)
{
	return !needs_uuid && bonding_possible;
}

bool tb_test_xdomain_initial_state_needs_link_status(bool needs_uuid,
						      bool bonding_possible)
{
	return tb_xdomain_initial_state_needs_link_status(needs_uuid,
							   bonding_possible);
}

static bool
tb_xdomain_should_fallback_to_direct_bonding(bool bonding_possible,
					     bool services_published,
					     int link_status_ret,
					     bool peer_confirmed)
{
	return bonding_possible && peer_confirmed && !services_published &&
	       link_status_ret == -EOPNOTSUPP;
}

bool tb_test_xdomain_should_fallback_to_direct_bonding(bool bonding_possible,
							bool services_published,
							int link_status_ret,
							bool peer_confirmed)
{
	return tb_xdomain_should_fallback_to_direct_bonding(bonding_possible,
							    services_published,
							    link_status_ret,
							    peer_confirmed);
}

bool tb_test_xdomain_direct_bonding_blocks_controller(void)
{
	return false;
}

bool tb_test_xdomain_bonding_rearm_allowed(bool bonding_possible, bool bonded,
					   unsigned int attempts)
{
	return tb_xdomain_bonding_rearm_allowed(bonding_possible, bonded,
						attempts);
}

bool tb_test_xdomain_bonding_after(bool possible, int ret, bool fallback)
{
	return tb_xdomain_bonding_after(possible, ret, fallback);
}

bool tb_test_xdomain_accepts_link_state_change(bool in_bonding_uuid_high,
					       bool enumerated,
					       bool bonding_possible,
					       bool bonded)
{
	return tb_xdomain_accepts_link_state_change(in_bonding_uuid_high,
						    enumerated,
						    bonding_possible, bonded);
}

/* The b5f07da constraint: a failed re-arm never touches lane adapters. */
bool tb_test_xdomain_bonding_rearm_touches_lanes_on_failure(void)
{
	return false;
}

/*
 * Response matching for XDomain control requests: does a TB_CFG_PKG_ERROR
 * from @err_route complete a pending XDP request addressed to @req_route?
 * Builds the real structures and calls the real match function, so the test
 * pins the exact demux behavior tb_ctl uses.
 */
bool tb_test_xdomain_error_pkg_matches(u64 req_route, u64 err_route)
{
	struct tb_xdp_header req_hdr = {};
	struct cfg_error_pkg err = {};
	struct tb_cfg_request req = {};
	struct ctl_pkg pkg = {};

	req_hdr.xd_hdr.route_hi = upper_32_bits(req_route);
	req_hdr.xd_hdr.route_lo = lower_32_bits(req_route);
	req.request = &req_hdr;
	req.response_size = sizeof(struct tb_xdp_header);

	err.header = tb_cfg_make_header(err_route);
	pkg.buffer = &err;
	pkg.frame.eof = TB_CFG_PKG_ERROR;
	pkg.frame.size = sizeof(err);

	return tb_xdomain_match(&req, &pkg);
}

enum tb_cfg_request_event
tb_test_xdomain_intermediate_event(u64 req_route, u64 status_route,
				   bool error)
{
	struct icm_tr_pkg_xdomain_packet_response status = {
		.hdr = {
			.code = ICM_XDOMAIN_PACKET,
			.flags = error ? ICM_FLAGS_ERROR : 0,
			.total_packets = 1,
		},
		.route_hi = upper_32_bits(status_route),
		.route_lo = lower_32_bits(status_route),
	};
	struct tb_xdp_header req_hdr = {};
	struct tb_cfg_request req = {
		.request = &req_hdr,
	};
	struct ctl_pkg pkg = {
		.buffer = &status,
	};

	req_hdr.xd_hdr.route_hi = upper_32_bits(req_route);
	req_hdr.xd_hdr.route_lo = lower_32_bits(req_route);
	pkg.frame.eof = TB_CFG_PKG_ICM_RESP;
	pkg.frame.size = sizeof(status);

	return tb_xdomain_intermediate(&req, &pkg);
}

/* Exercise the real response matcher with a synthetic XDP exchange. */
bool tb_test_xdomain_response_pkg_matches(u64 req_route,
					  u32 req_type,
					  u8 req_sequence,
					  size_t response_capacity,
					  u64 response_route,
					  u32 response_type,
					  u8 response_sequence,
					  size_t response_size,
					  size_t response_declared_size,
					  bool same_protocol)
{
	struct tb_xdp_header req_hdr = {};
	struct tb_xdp_header response_hdr = {};
	struct tb_cfg_request req = {};
	struct ctl_pkg pkg = {};

	tb_xdp_fill_header(&req_hdr, req_route, req_sequence, req_type,
			   sizeof(req_hdr));
	tb_xdp_fill_header(&response_hdr, response_route, response_sequence,
			   response_type, response_declared_size);
	if (!same_protocol)
		response_hdr.uuid.b[0] ^= 0xff;

	req.request = &req_hdr;
	req.response_size = response_capacity;
	pkg.buffer = &response_hdr;
	pkg.frame.eof = TB_CFG_PKG_XDOMAIN_RESP;
	pkg.frame.size = response_size;

	return tb_xdomain_match(&req, &pkg);
}

/* Exercise the real matcher with an opaque service protocol exchange. */
bool tb_test_xdomain_service_response_pkg_matches(u64 req_route,
						  u8 req_sequence,
						  size_t response_capacity,
						  u64 response_route,
						  u8 response_sequence,
						  size_t response_size,
						  size_t response_declared_size,
						  bool same_protocol)
{
	static const uuid_t service_uuid =
		UUID_INIT(0x798f589e, 0x3616, 0x8a47,
			  0x97, 0xc6, 0x56, 0x64, 0xa9, 0x20, 0xc8, 0xdd);
	struct {
		struct tb_xdomain_header xd_hdr;
		uuid_t uuid;
		u32 opaque[16];
	} request = {};
	struct {
		struct tb_xdomain_header xd_hdr;
		uuid_t uuid;
		u32 opaque[18];
	} response = {};
	struct tb_cfg_request req = {
		.request = &request,
		.response_size = response_capacity,
	};
	struct ctl_pkg pkg = {
		.buffer = &response,
	};

	request.xd_hdr.route_hi = upper_32_bits(req_route);
	request.xd_hdr.route_lo = lower_32_bits(req_route);
	request.xd_hdr.length_sn =
		(sizeof(request) - sizeof(request.xd_hdr)) / sizeof(u32) |
		(req_sequence << TB_XDOMAIN_SN_SHIFT);
	request.uuid = service_uuid;
	request.opaque[0] = 0x80875a38;

	response.xd_hdr.route_hi = upper_32_bits(response_route);
	response.xd_hdr.route_lo = lower_32_bits(response_route);
	response.xd_hdr.length_sn =
		(response_declared_size - sizeof(response.xd_hdr)) / sizeof(u32) |
		(response_sequence << TB_XDOMAIN_SN_SHIFT);
	response.uuid = service_uuid;
	response.opaque[0] = 0x80875166;
	if (!same_protocol)
		response.uuid.b[0] ^= 0xff;

	pkg.frame.eof = TB_CFG_PKG_XDOMAIN_RESP;
	pkg.frame.size = response_size;
	return tb_xdomain_match(&req, &pkg);
}

/* Return the completion status produced by the real copy path for an error. */
int tb_test_xdomain_native_error_result(void)
{
	struct tb_xdp_uuid_response response = {};
	struct cfg_error_pkg error = {};
	struct tb_cfg_request req = {
		.response = &response,
		.response_size = sizeof(response),
	};
	struct ctl_pkg pkg = {
		.buffer = &error,
	};

	pkg.frame.eof = TB_CFG_PKG_ERROR;
	pkg.frame.size = sizeof(error);
	tb_xdomain_copy(&req, &pkg);
	return req.result.err;
}

bool tb_test_xdomain_direct_bonding_abort_disables_lane(void)
{
	return false;
}

/*
 * Model the existing link-exit hardware access separately so KUnit can pin
 * the teardown contract before the production path is changed.  Generation
 * 4 only clears cached bonded flags; older generations synchronously disable
 * or re-enable lane 1.
 */
static bool tb_xdomain_link_exit_touches_lane_hardware(bool unplugged,
						       bool firmware_cm,
						       unsigned int generation)
{
	return !unplugged && !firmware_cm && generation < 4;
}

bool tb_test_xdomain_link_exit_touches_lane_hardware(bool firmware_cm,
						      unsigned int generation)
{
	return tb_xdomain_link_exit_touches_lane_hardware(false, firmware_cm,
							 generation);
}

bool tb_test_xdomain_unplugged_link_exit_touches_lane_hardware(void)
{
	return tb_xdomain_link_exit_touches_lane_hardware(true, false, 3);
}

static void tb_xdomain_link_init(struct tb_xdomain *xd, struct tb_port *down)
{
	unsigned int generation;
	int width;

	if (!down->dual_link_port)
		return;

	generation = tb_port_get_link_generation(down);
	width = tb_port_get_link_width(down);
	if (width < 0)
		width = TB_LINK_WIDTH_SINGLE;

	if (tb_xdomain_should_negotiate_bonding(true, generation, width)) {
		xd->bonding_possible = true;
		return;
	}

	/* The observed width confirms this Gen 4 link is already bonded. */
	if (generation >= 4) {
		down->bonded = true;
		down->dual_link_port->bonded = true;
	}
}

static void tb_xdomain_link_exit(struct tb_xdomain *xd)
{
	struct tb_port *down = tb_xdomain_downstream_port(xd);
	unsigned int generation;
	bool touches_hardware;
	bool firmware_cm;

	if (!down->dual_link_port)
		return;

	generation = tb_port_get_link_generation(down);
	firmware_cm = tb_switch_is_icm(down->sw);

	/*
	 * ICM reported this XDomain disconnect and owns physical-lane cleanup.
	 * Do not issue config transactions from its removal callback: the adapter
	 * may already be unreachable, and tb_domain_remove() must be able to flush
	 * the ICM workqueue during module removal without waiting on dead hardware.
	 * Gen 4 software-CM links likewise need only their cached flags cleared.
	 */
	touches_hardware = tb_xdomain_link_exit_touches_lane_hardware(xd->is_unplugged,
								      firmware_cm,
								      generation);
	if (!touches_hardware) {
		down->bonded = false;
		down->dual_link_port->bonded = false;
		return;
	}

	if (xd->link_width > TB_LINK_WIDTH_SINGLE) {
		/*
		 * Just return port structures back to way they were and
		 * update credits. No need to update userspace because
		 * the XDomain is removed soon anyway.
		 */
		tb_port_lane_bonding_disable(down);
		tb_port_update_credits(down);
	} else if (down->dual_link_port) {
		/*
		 * Re-enable the lane 1 adapter we disabled at the end
		 * of tb_xdomain_get_properties().
		 */
		tb_port_enable(down->dual_link_port);
	}
}

/**
 * tb_xdomain_alloc() - Allocate new XDomain object
 * @tb: Domain where the XDomain belongs
 * @parent: Parent device (the switch through the connection to the
 *	    other domain is reached).
 * @route: Route string used to reach the other domain
 * @local_uuid: Our local domain UUID
 * @remote_uuid: UUID of the other domain (optional)
 *
 * Allocates new XDomain structure and returns pointer to that. The
 * object must be released by calling tb_xdomain_put().
 */
struct tb_xdomain *tb_xdomain_alloc(struct tb *tb, struct device *parent,
				    u64 route, const uuid_t *local_uuid,
				    const uuid_t *remote_uuid)
{
	struct tb_switch *parent_sw = tb_to_switch(parent);
	struct tb_xdomain *xd;
	struct tb_port *down;

	/* Make sure the downstream domain is accessible */
	down = tb_port_at(route, parent_sw);
	tb_port_unlock(down);

	xd = kzalloc(sizeof(*xd), GFP_KERNEL);
	if (!xd)
		return NULL;

	xd->tb = tb;
	xd->route = route;
	xd->local_max_hopid = down->config.max_in_hop_id;
	ida_init(&xd->service_ids);
	ida_init(&xd->in_hopids);
	ida_init(&xd->out_hopids);
	mutex_init(&xd->lock);
	INIT_DELAYED_WORK(&xd->state_work, tb_xdomain_state_work);
	INIT_DELAYED_WORK(&xd->properties_changed_work,
			  tb_xdomain_properties_changed);

	xd->local_uuid = kmemdup(local_uuid, sizeof(uuid_t), GFP_KERNEL);
	if (!xd->local_uuid)
		goto err_free;

	if (remote_uuid) {
		xd->remote_uuid = kmemdup(remote_uuid, sizeof(uuid_t),
					  GFP_KERNEL);
		if (!xd->remote_uuid)
			goto err_free_local_uuid;
	}
	/* A supplied UUID is a topology claim until the peer answers on route. */
	xd->needs_uuid = tb_xdomain_initial_needs_uuid(remote_uuid);

	if (tb_xdomain_should_initialize_link(remote_uuid))
		tb_xdomain_link_init(xd, down);

	device_initialize(&xd->dev);
	xd->dev.parent = get_device(parent);
	xd->dev.bus = &tb_bus_type;
	xd->dev.type = &tb_xdomain_type;
	xd->dev.groups = xdomain_attr_groups;
	dev_set_name(&xd->dev, "%u-%llx", tb->index, route);

	dev_dbg(&xd->dev, "local UUID %pUb\n", local_uuid);
	if (remote_uuid)
		dev_dbg(&xd->dev, "remote UUID %pUb\n", remote_uuid);

	/*
	 * This keeps the DMA powered on as long as we have active
	 * connection to another host.
	 */
	pm_runtime_set_active(&xd->dev);
	pm_runtime_get_noresume(&xd->dev);
	pm_runtime_enable(&xd->dev);

	return xd;

err_free_local_uuid:
	kfree(xd->local_uuid);
err_free:
	kfree(xd);

	return NULL;
}

/**
 * tb_xdomain_add() - Add XDomain to the bus
 * @xd: XDomain to add
 *
 * This function starts XDomain discovery protocol handshake and
 * eventually adds the XDomain to the bus. After calling this function
 * the caller needs to call tb_xdomain_remove() in order to remove and
 * release the object regardless whether the handshake succeeded or not.
 */
void tb_xdomain_add(struct tb_xdomain *xd)
{
	/* Start exchanging properties with the other host */
	start_handshake(xd);
}

static int unregister_service(struct device *dev, void *data)
{
	device_unregister(dev);
	return 0;
}

/**
 * tb_xdomain_remove() - Remove XDomain
 * @xd: XDomain to remove
 *
 * This will stop all ongoing configuration work. XDomain is not removed
 * from the bus if it was added. That needs to be done separately by
 * calling tb_xdomain_unregister().
 *
 * Called with @tb->lock held.
 */
void tb_xdomain_remove(struct tb_xdomain *xd)
{
	tb_xdomain_debugfs_remove(xd);

	mutex_lock(&xd->lock);
	xd->removing = true;
	mutex_unlock(&xd->lock);

	stop_handshake(xd);
	tb_xdomain_link_exit(xd);

	if (!device_is_registered(&xd->dev)) {
		/*
		 * Undo runtime PM here explicitly because it is
		 * possible that the XDomain was never added to the bus
		 * and thus device_del() is not called for it
		 * (device_del() would handle this otherwise).
		 */
		pm_runtime_disable(&xd->dev);
		pm_runtime_put_noidle(&xd->dev);
		pm_runtime_set_suspended(&xd->dev);
		put_device(&xd->dev);
	}
}

/**
 * tb_xdomain_unregister() - Unregister XDomain
 * @xd: XDomain to unregister
 *
 * This will unregister the XDomain along with any services from the
 * bus. When the last reference to @xd is released the object will be
 * released as well.
 */
void tb_xdomain_unregister(struct tb_xdomain *xd)
{
	lockdep_assert_not_held(&xd->tb->lock);

	device_for_each_child_reverse(&xd->dev, xd, unregister_service);

	dev_info(&xd->dev, "host disconnected\n");
	device_unregister(&xd->dev);
}

/**
 * tb_xdomain_lane_bonding_enable() - Enable lane bonding on XDomain
 * @xd: XDomain connection
 *
 * Lane bonding is disabled by default for XDomains. This function tries
 * to enable bonding by first enabling the port and waiting for the CL0
 * state.
 *
 * Return: %0 in case of success and negative errno in case of error.
 */
int tb_xdomain_lane_bonding_enable(struct tb_xdomain *xd)
{
	unsigned int width_mask;
	struct tb_port *port;
	int ret;

	port = tb_xdomain_downstream_port(xd);
	if (!port->dual_link_port)
		return -ENODEV;

	ret = tb_port_enable(port->dual_link_port);
	if (ret)
		return ret;

	ret = tb_wait_for_port(port->dual_link_port, true);
	if (ret < 0)
		return ret;
	if (!ret)
		return -ENOTCONN;

	ret = tb_port_lane_bonding_enable(port);
	if (ret) {
		tb_port_warn(port, "failed to enable lane bonding\n");
		return ret;
	}

	/* Any of the widths are all bonded */
	width_mask = TB_LINK_WIDTH_DUAL | TB_LINK_WIDTH_ASYM_TX |
		     TB_LINK_WIDTH_ASYM_RX;

	ret = tb_port_wait_for_link_width(port, width_mask,
					  XDOMAIN_BONDING_TIMEOUT);
	if (ret) {
		tb_port_warn(port, "failed to enable lane bonding\n");
		return ret;
	}

	tb_port_update_credits(port);
	tb_xdomain_update_link_attributes(xd);

	dev_dbg(&xd->dev, "lane bonding enabled\n");
	return 0;
}
EXPORT_SYMBOL_GPL(tb_xdomain_lane_bonding_enable);

/**
 * tb_xdomain_lane_bonding_disable() - Disable lane bonding
 * @xd: XDomain connection
 *
 * Lane bonding is disabled by default for XDomains. If bonding has been
 * enabled, this function can be used to disable it.
 */
void tb_xdomain_lane_bonding_disable(struct tb_xdomain *xd)
{
	struct tb_port *port;

	port = tb_xdomain_downstream_port(xd);
	if (port->dual_link_port) {
		int ret;

		tb_port_lane_bonding_disable(port);
		ret = tb_port_wait_for_link_width(port, TB_LINK_WIDTH_SINGLE, 100);
		if (ret == -ETIMEDOUT)
			tb_port_warn(port, "timeout disabling lane bonding\n");
		tb_port_disable(port->dual_link_port);
		tb_port_update_credits(port);
		tb_xdomain_update_link_attributes(xd);

		dev_dbg(&xd->dev, "lane bonding disabled\n");
	}
}
EXPORT_SYMBOL_GPL(tb_xdomain_lane_bonding_disable);

/**
 * tb_xdomain_alloc_in_hopid() - Allocate input HopID for tunneling
 * @xd: XDomain connection
 * @hopid: Preferred HopID or %-1 for next available
 *
 * Returns allocated HopID or negative errno. Specifically returns
 * %-ENOSPC if there are no more available HopIDs. Returned HopID is
 * guaranteed to be within range supported by the input lane adapter.
 * Call tb_xdomain_release_in_hopid() to release the allocated HopID.
 */
int tb_xdomain_alloc_in_hopid(struct tb_xdomain *xd, int hopid)
{
	if (hopid < 0)
		hopid = TB_PATH_MIN_HOPID;
	if (hopid < TB_PATH_MIN_HOPID || hopid > xd->local_max_hopid)
		return -EINVAL;

	return ida_alloc_range(&xd->in_hopids, hopid, xd->local_max_hopid,
			       GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(tb_xdomain_alloc_in_hopid);

/**
 * tb_xdomain_alloc_out_hopid() - Allocate output HopID for tunneling
 * @xd: XDomain connection
 * @hopid: Preferred HopID or %-1 for next available
 *
 * Returns allocated HopID or negative errno. Specifically returns
 * %-ENOSPC if there are no more available HopIDs. Returned HopID is
 * guaranteed to be within range supported by the output lane adapter.
 * Call tb_xdomain_release_in_hopid() to release the allocated HopID.
 */
int tb_xdomain_alloc_out_hopid(struct tb_xdomain *xd, int hopid)
{
	if (hopid < 0)
		hopid = TB_PATH_MIN_HOPID;
	if (hopid < TB_PATH_MIN_HOPID || hopid > xd->remote_max_hopid)
		return -EINVAL;

	return ida_alloc_range(&xd->out_hopids, hopid, xd->remote_max_hopid,
			       GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(tb_xdomain_alloc_out_hopid);

/**
 * tb_xdomain_release_in_hopid() - Release input HopID
 * @xd: XDomain connection
 * @hopid: HopID to release
 */
void tb_xdomain_release_in_hopid(struct tb_xdomain *xd, int hopid)
{
	ida_free(&xd->in_hopids, hopid);
}
EXPORT_SYMBOL_GPL(tb_xdomain_release_in_hopid);

/**
 * tb_xdomain_release_out_hopid() - Release output HopID
 * @xd: XDomain connection
 * @hopid: HopID to release
 */
void tb_xdomain_release_out_hopid(struct tb_xdomain *xd, int hopid)
{
	ida_free(&xd->out_hopids, hopid);
}
EXPORT_SYMBOL_GPL(tb_xdomain_release_out_hopid);

/**
 * tb_xdomain_enable_paths() - Enable DMA paths for XDomain connection
 * @xd: XDomain connection
 * @transmit_path: HopID we are using to send out packets
 * @transmit_ring: DMA ring used to send out packets
 * @receive_path: HopID the other end is using to send packets to us
 * @receive_ring: DMA ring used to receive packets from @receive_path
 *
 * The function enables DMA paths accordingly so that after successful
 * return the caller can send and receive packets using high-speed DMA
 * path. If a transmit or receive path is not needed, pass %-1 for those
 * parameters.
 *
 * Return: %0 in case of success, %-EUCLEAN if activation failed and the
 * exact rings and HopIDs remain owned by an unresolved hardware path, or
 * another negative errno after a clean activation failure. On %-EUCLEAN the
 * caller must retain those resources and retry tb_xdomain_disable_paths().
 */
int tb_xdomain_enable_paths(struct tb_xdomain *xd, int transmit_path,
			    int transmit_ring, int receive_path,
			    int receive_ring)
{
	return tb_domain_approve_xdomain_paths(xd->tb, xd, transmit_path,
					       transmit_ring, receive_path,
					       receive_ring);
}
EXPORT_SYMBOL_GPL(tb_xdomain_enable_paths);

/**
 * tb_xdomain_disable_paths() - Disable DMA paths for XDomain connection
 * @xd: XDomain connection
 * @transmit_path: HopID we are using to send out packets
 * @transmit_ring: DMA ring used to send out packets
 * @receive_path: HopID the other end is using to send packets to us
 * @receive_ring: DMA ring used to receive packets from @receive_path
 *
 * This does the opposite of tb_xdomain_enable_paths(). After call to
 * this the caller is not expected to use the rings anymore. Passing %-1
 * as path/ring parameter means don't care. Normally the callers should
 * pass the same values here as they do when paths are enabled.
 *
 * Return: %0 in case of success and negative errno in case of error
 */
int tb_xdomain_disable_paths(struct tb_xdomain *xd, int transmit_path,
			     int transmit_ring, int receive_path,
			     int receive_ring)
{
	return tb_domain_disconnect_xdomain_paths(xd->tb, xd, transmit_path,
						  transmit_ring, receive_path,
						  receive_ring);
}
EXPORT_SYMBOL_GPL(tb_xdomain_disable_paths);

/**
 * tb_xdomain_paths_active() - Are previously enabled DMA paths still live?
 * @xd: XDomain connection
 * @transmit_path: HopID we are using to send out packets
 * @transmit_ring: DMA ring used to send out packets
 * @receive_path: HopID the other end is using to send packets to us
 * @receive_ring: DMA ring used to receive packets from @receive_path
 *
 * Level-triggered revalidation for service drivers: reads the routers' path
 * config space back and reports whether the DMA tunnel established with
 * tb_xdomain_enable_paths() is still programmed (hop entries enabled). A
 * peer reboot or re-negotiation without a processed hotplug edge deactivates
 * the tunnel underneath the service while its session (e.g. ThunderboltIP
 * login, carrier) stays latched -- the zombie this call lets callers detect
 * so they can tear down and re-run their spec negotiation.
 *
 * Return: %1 when the tunnel is programmed and enabled, %0 when it is gone
 * or deactivated, negative errno when the state cannot be determined
 * (callers must treat that as unknown, not as dead).
 */
int tb_xdomain_paths_active(struct tb_xdomain *xd, int transmit_path,
			    int transmit_ring, int receive_path,
			    int receive_ring)
{
	return tb_domain_xdomain_paths_active(xd->tb, xd, transmit_path,
					      transmit_ring, receive_path,
					      receive_ring);
}
EXPORT_SYMBOL_GPL(tb_xdomain_paths_active);

static int
tb_xdomain_quarantine_disconnect(void *data, struct tb_xdomain *xd,
				 int transmit_path, int transmit_ring,
				 int receive_path, int receive_ring)
{
	return tb_domain_retry_xdomain_paths(xd->tb, xd, transmit_path,
					     transmit_ring, receive_path,
					     receive_ring);
}

static void tb_xdomain_quarantine_free_ring(void *data, struct tb_ring *ring)
{
	tb_ring_free(ring);
}

static int
tb_xdomain_quarantine_request_recovery(void *data, struct tb_xdomain *xd)
{
	return tb_nhi_request_quarantine_recovery(xd->tb->nhi, xd->route);
}

static const struct tb_xdomain_quarantine_ops xdomain_quarantine_real_ops = {
	.disconnect = tb_xdomain_quarantine_disconnect,
	.free_ring = tb_xdomain_quarantine_free_ring,
	.request_recovery = tb_xdomain_quarantine_request_recovery,
};

static bool
tb_xdomain_quarantine_matches(const struct tb_xdomain_quarantine *q,
			      const struct tb_xdomain *xd,
			      int transmit_path, int transmit_ring,
			      const struct tb_ring *transmit_dma_ring,
			      int receive_path, int receive_ring,
			      const struct tb_ring *receive_dma_ring)
{
	return q->xd == xd && q->transmit_path == transmit_path &&
		q->transmit_ring == transmit_ring &&
		q->transmit_dma_ring == transmit_dma_ring &&
		q->receive_path == receive_path &&
		q->receive_ring == receive_ring &&
		q->receive_dma_ring == receive_dma_ring;
}

static bool tb_xdomain_quarantine_is_last(struct tb_xdomain_quarantine *q)
{
	struct tb_xdomain_quarantine *other;

	list_for_each_entry(other, &xdomain_quarantines, list) {
		if (other != q && other->xd == q->xd)
			return false;
	}
	return true;
}

static void
tb_xdomain_quarantine_release(struct tb_xdomain_quarantine *q, bool proven)
{
	bool clear_error;

	mutex_lock(&xdomain_quarantines_lock);
	if (list_empty(&q->list)) {
		mutex_unlock(&xdomain_quarantines_lock);
		return;
	}
	clear_error = tb_xdomain_quarantine_is_last(q);
	list_del_init(&q->list);
	if (clear_error)
		WRITE_ONCE(q->xd->path_teardown_err, 0);
	mutex_unlock(&xdomain_quarantines_lock);

	/* A successful exact disconnect and a proven reset are both revocation. */
	if (q->transmit_dma_ring)
		q->ops.free_ring(q->ops.data, q->transmit_dma_ring);
	if (q->receive_dma_ring)
		q->ops.free_ring(q->ops.data, q->receive_dma_ring);
	if (q->transmit_path >= 0)
		tb_xdomain_release_out_hopid(q->xd, q->transmit_path);
	if (q->receive_path >= 0)
		tb_xdomain_release_in_hopid(q->xd, q->receive_path);

	if (proven)
		dev_info(&q->xd->dev,
			 "controller reset revoked quarantined DMA tuple %d/%d -> %d/%d\n",
			 q->transmit_path, q->transmit_ring,
			 q->receive_path, q->receive_ring);
	/* Drop the quarantine-list ownership reference. */
	tb_xdomain_quarantine_put(q);
}

static int
tb_xdomain_quarantine_retry_record(struct tb_xdomain_quarantine *q,
				   bool reschedule)
{
	int recovery_ret, ret;

	if (q->recovery_pending) {
		recovery_ret = q->ops.request_recovery(q->ops.data, q->xd);
		if (recovery_ret && recovery_ret != -EALREADY && reschedule &&
		    !READ_ONCE(q->stopping))
			mod_delayed_work(q->xd->tb->wq, &q->retry_work,
					 msecs_to_jiffies(XDOMAIN_QUARANTINE_RETRY_MS));
		return recovery_ret == -EALREADY ? 0 : recovery_ret;
	}

	ret = q->ops.disconnect(q->ops.data, q->xd,
				q->transmit_path, q->transmit_ring,
				q->receive_path, q->receive_ring);
	if (!ret) {
		tb_xdomain_quarantine_release(q, false);
		return 0;
	}

	q->attempts++;
	if (q->attempts < XDOMAIN_QUARANTINE_PATH_RETRIES) {
		if (reschedule && !READ_ONCE(q->stopping))
			mod_delayed_work(q->xd->tb->wq, &q->retry_work,
					 msecs_to_jiffies(XDOMAIN_QUARANTINE_RETRY_MS));
		return ret;
	}

	q->recovery_pending = true;
	dev_err(&q->xd->dev,
		"DMA tuple %d/%d -> %d/%d remained owned after %u retries; scheduling controller recovery\n",
		q->transmit_path, q->transmit_ring,
		q->receive_path, q->receive_ring, q->attempts);
	recovery_ret = q->ops.request_recovery(q->ops.data, q->xd);
	if (recovery_ret && recovery_ret != -EALREADY && reschedule &&
	    !READ_ONCE(q->stopping))
		mod_delayed_work(q->xd->tb->wq, &q->retry_work,
				 msecs_to_jiffies(XDOMAIN_QUARANTINE_RETRY_MS));
	return ret;
}

static void tb_xdomain_quarantine_retry_work(struct work_struct *work)
{
	struct tb_xdomain_quarantine *q =
		container_of(work, typeof(*q), retry_work.work);

	refcount_inc(&q->refs);
	if (!READ_ONCE(q->stopping))
		tb_xdomain_quarantine_retry_record(q, true);
	tb_xdomain_quarantine_put(q);
}

static bool tb_xdomain_quarantine_tuple_valid(struct tb_xdomain *xd,
					      int path, int ring,
					      struct tb_ring *dma_ring,
					      bool transmit)
{
	if (path < 0)
		return ring < 0 && !dma_ring;
	if (!dma_ring || ring < 0 || dma_ring->hop != ring)
		return false;
	if (dma_ring->nhi != xd->tb->nhi || dma_ring->is_tx != transmit)
		return false;
	return true;
}

/**
 * tb_xdomain_quarantine_paths() - transfer an unresolved DMA tuple to core
 * @xd: XDomain that owns the path HopIDs
 * @transmit_path: Locally allocated output HopID
 * @transmit_ring: NHI TX ring HopID programmed into the path
 * @transmit_dma_ring: Exact stopped TX ring object being transferred
 * @receive_path: Locally allocated input HopID used by the peer
 * @receive_ring: NHI RX ring HopID programmed into the path
 * @receive_dma_ring: Exact stopped RX ring object being transferred
 *
 * Service remove callbacks cannot veto driver unbind. After an exact path
 * disable returns an ownership error, this transfers the complete tuple to
 * the core. The core fences callbacks, retains both XDomain IDA reservations,
 * retries the real disable operation, and escalates through controller
 * unbind/reset/reprobe when the bounded retry budget is exhausted.
 *
 * Return: %0 after ownership transfer or a negative errno before transfer.
 */
int tb_xdomain_quarantine_paths(struct tb_xdomain *xd,
				int transmit_path, int transmit_ring,
				struct tb_ring *transmit_dma_ring,
				int receive_path, int receive_ring,
				struct tb_ring *receive_dma_ring)
{
	const struct tb_xdomain_quarantine_ops *ops =
		&xdomain_quarantine_real_ops;
	struct tb_xdomain_quarantine *q, *existing;

	if (!xd || (!transmit_dma_ring && !receive_dma_ring))
		return -EINVAL;
	if (!tb_xdomain_quarantine_tuple_valid(xd, transmit_path,
					       transmit_ring, transmit_dma_ring, true) ||
	    !tb_xdomain_quarantine_tuple_valid(xd, receive_path,
					       receive_ring, receive_dma_ring, false) ||
	    (transmit_dma_ring && transmit_dma_ring == receive_dma_ring))
		return -EINVAL;

#if IS_ENABLED(CONFIG_USB4_KUNIT_TEST)
	if (xdomain_quarantine_test_fail_alloc)
		q = NULL;
	else
#endif
		q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q) {
		/*
		 * Exact retry metadata could not be retained, but leaf removal
		 * still cannot leave IRQ/work callbacks live. Ring quarantine is
		 * enough to force the generic domain reset and abandonment gate.
		 */
		if (transmit_dma_ring)
			tb_ring_quarantine(transmit_dma_ring);
		if (receive_dma_ring)
			tb_ring_quarantine(receive_dma_ring);
		return -ENOMEM;
	}

#if IS_ENABLED(CONFIG_USB4_KUNIT_TEST)
	if (xdomain_quarantine_test_ops_set)
		ops = &xdomain_quarantine_test_ops;
#endif
	INIT_LIST_HEAD(&q->list);
	INIT_DELAYED_WORK(&q->retry_work, tb_xdomain_quarantine_retry_work);
	refcount_set(&q->refs, 1);
	q->xd = tb_xdomain_get(xd);
	q->transmit_path = transmit_path;
	q->transmit_ring = transmit_ring;
	q->transmit_dma_ring = transmit_dma_ring;
	q->receive_path = receive_path;
	q->receive_ring = receive_ring;
	q->receive_dma_ring = receive_dma_ring;
	q->ops = *ops;

	mutex_lock(&xdomain_quarantines_lock);
	list_for_each_entry(existing, &xdomain_quarantines, list) {
		if (tb_xdomain_quarantine_matches(existing, xd,
						  transmit_path, transmit_ring,
						  transmit_dma_ring, receive_path,
						  receive_ring, receive_dma_ring)) {
			mutex_unlock(&xdomain_quarantines_lock);
			tb_xdomain_quarantine_put(q);
			return 0;
		}
	}
	list_add_tail(&q->list, &xdomain_quarantines);
	WRITE_ONCE(xd->path_teardown_err, -EUCLEAN);
	mutex_unlock(&xdomain_quarantines_lock);

	/* No leaf callback or polling pointer survives this function. */
	if (transmit_dma_ring)
		tb_ring_quarantine(transmit_dma_ring);
	if (receive_dma_ring)
		tb_ring_quarantine(receive_dma_ring);
	queue_delayed_work(xd->tb->wq, &q->retry_work,
			   msecs_to_jiffies(XDOMAIN_QUARANTINE_RETRY_MS));
	return 0;
}
EXPORT_SYMBOL_GPL(tb_xdomain_quarantine_paths);

void tb_xdomain_quarantine_prepare_reset(struct tb *tb)
{
	struct tb_xdomain_quarantine *q, *iter;

	for (;;) {
		mutex_lock(&xdomain_quarantines_lock);
		q = NULL;
		list_for_each_entry(iter, &xdomain_quarantines, list) {
			if (iter->xd->tb == tb && !iter->stopping) {
				iter->stopping = true;
				refcount_inc(&iter->refs);
				q = iter;
				break;
			}
		}
		mutex_unlock(&xdomain_quarantines_lock);
		if (!q)
			break;
		cancel_delayed_work_sync(&q->retry_work);
		tb_xdomain_quarantine_put(q);
	}
}

void tb_xdomain_quarantine_finalize_reset(struct tb *tb,
					  enum tb_domain_reset_state reset_state)
{
	struct tb_xdomain_quarantine *q, *iter;

	if (reset_state != TB_DOMAIN_RESET_PROVEN)
		return;

	for (;;) {
		mutex_lock(&xdomain_quarantines_lock);
		q = NULL;
		list_for_each_entry(iter, &xdomain_quarantines, list) {
			if (iter->xd->tb == tb) {
				q = iter;
				break;
			}
		}
		if (q)
			refcount_inc(&q->refs);
		if (q)
			q->stopping = true;
		mutex_unlock(&xdomain_quarantines_lock);
		if (!q)
			break;
		cancel_delayed_work_sync(&q->retry_work);
		tb_xdomain_quarantine_release(q, true);
		tb_xdomain_quarantine_put(q);
	}
}

void tb_xdomain_quarantine_discard_domain(struct tb *tb)
{
	struct tb_xdomain_quarantine *q, *iter;

	for (;;) {
		mutex_lock(&xdomain_quarantines_lock);
		q = NULL;
		list_for_each_entry(iter, &xdomain_quarantines, list) {
			if (iter->xd->tb == tb) {
				q = iter;
				break;
			}
		}
		if (q) {
			refcount_inc(&q->refs);
			q->stopping = true;
			list_del_init(&q->list);
		}
		mutex_unlock(&xdomain_quarantines_lock);
		if (!q)
			break;
		cancel_delayed_work_sync(&q->retry_work);
		dev_err(&q->xd->dev,
			"controller reset did not prove revocation; abandoning quarantined DMA allocation without reuse\n");
		/* Drop the former list reference, then our traversal reference. */
		tb_xdomain_quarantine_put(q);
		tb_xdomain_quarantine_put(q);
	}
}

#if IS_ENABLED(CONFIG_USB4_KUNIT_TEST)
void tb_test_xdomain_quarantine_set_ops(const struct tb_xdomain_quarantine_test_ops *ops)
{
	mutex_lock(&xdomain_quarantines_lock);
	if (ops) {
		xdomain_quarantine_test_ops.disconnect = ops->disconnect;
		xdomain_quarantine_test_ops.free_ring = ops->free_ring;
		xdomain_quarantine_test_ops.request_recovery =
			ops->request_recovery;
		xdomain_quarantine_test_ops.data = ops->data;
		xdomain_quarantine_test_ops_set = true;
	} else {
		memset(&xdomain_quarantine_test_ops, 0,
		       sizeof(xdomain_quarantine_test_ops));
		xdomain_quarantine_test_ops_set = false;
	}
	mutex_unlock(&xdomain_quarantines_lock);
}

void tb_test_xdomain_quarantine_fail_alloc(bool fail)
{
	xdomain_quarantine_test_fail_alloc = fail;
}

static struct tb_xdomain_quarantine *
tb_test_xdomain_quarantine_find(struct tb_xdomain *xd)
{
	struct tb_xdomain_quarantine *q;

	list_for_each_entry(q, &xdomain_quarantines, list)
		if (q->xd == xd)
			return q;
	return NULL;
}

int tb_test_xdomain_quarantine_retry(struct tb_xdomain *xd)
{
	struct tb_xdomain_quarantine *q;
	int ret;

	mutex_lock(&xdomain_quarantines_lock);
	q = tb_test_xdomain_quarantine_find(xd);
	if (q) {
		refcount_inc(&q->refs);
		q->stopping = true;
	}
	mutex_unlock(&xdomain_quarantines_lock);
	if (!q)
		return -ENOENT;
	cancel_delayed_work_sync(&q->retry_work);
	q->stopping = false;
	ret = tb_xdomain_quarantine_retry_record(q, false);
	tb_xdomain_quarantine_put(q);
	return ret;
}

void tb_test_xdomain_quarantine_finalize(struct tb *tb,
					 enum tb_domain_reset_state reset_state)
{
	tb_xdomain_quarantine_finalize_reset(tb, reset_state);
}

unsigned int tb_test_xdomain_quarantine_count(struct tb_xdomain *xd)
{
	struct tb_xdomain_quarantine *q;
	unsigned int count = 0;

	mutex_lock(&xdomain_quarantines_lock);
	list_for_each_entry(q, &xdomain_quarantines, list)
		if (q->xd == xd)
			count++;
	mutex_unlock(&xdomain_quarantines_lock);
	return count;
}
#endif

struct tb_xdomain_lookup {
	const uuid_t *uuid;
	u8 link;
	u8 depth;
	u64 route;
};

static bool tb_xdomain_uuid_globally_matchable(const struct tb_xdomain *xd,
					       const uuid_t *uuid)
{
	return xd->uuid_verified && xd->remote_uuid &&
		uuid_equal(xd->remote_uuid, uuid);
}

#if IS_ENABLED(CONFIG_USB4_KUNIT_TEST)
bool tb_test_xdomain_uuid_globally_matchable(bool verified, bool equal)
{
	uuid_t remote = UUID_INIT(0x12345678, 0x1234, 0x5678,
				  0x12, 0x34, 0x56, 0x78, 0x12, 0x34,
				  0x56, 0x78);
	uuid_t other = UUID_INIT(0x87654321, 0x4321, 0x8765,
				 0x87, 0x65, 0x43, 0x21, 0x87, 0x65,
				 0x43, 0x21);
	struct tb_xdomain xd = {
		.remote_uuid = &remote,
		.uuid_verified = verified,
	};

	return tb_xdomain_uuid_globally_matchable(&xd,
					     equal ? &remote : &other);
}
#endif

static struct tb_xdomain *switch_find_xdomain(struct tb_switch *sw,
	const struct tb_xdomain_lookup *lookup)
{
	struct tb_port *port;

	/* A domain whose root switch failed to initialise still has a live
	 * ring 0, so an inbound XDomain request can reach the lookup before
	 * tb->root_switch exists. Observed on appmana-023 with 2.46: "failed
	 * to initialize port 1" followed by a NULL deref at
	 * switch_find_xdomain+0x11 from tb_xdomain_handle_request. Answering
	 * "no such XDomain" is correct -- there are no ports to search.
	 */
	if (!sw)
		return NULL;

	tb_switch_for_each_port(sw, port) {
		struct tb_xdomain *xd;

		if (port->xdomain) {
			xd = port->xdomain;

			if (lookup->uuid) {
				if (tb_xdomain_uuid_globally_matchable(xd,
								       lookup->uuid))
					return xd;
			} else {
				if (lookup->link && lookup->link == xd->link &&
				    lookup->depth == xd->depth)
					return xd;
				if (lookup->route && lookup->route == xd->route)
					return xd;
			}
		} else if (tb_port_has_remote(port)) {
			xd = switch_find_xdomain(port->remote->sw, lookup);
			if (xd)
				return xd;
		}
	}

	return NULL;
}

/**
 * tb_xdomain_find_by_uuid() - Find an XDomain by UUID
 * @tb: Domain where the XDomain belongs to
 * @uuid: UUID to look for
 *
 * Finds XDomain by walking through the Thunderbolt topology below @tb.
 * The returned XDomain will have its reference count increased so the
 * caller needs to call tb_xdomain_put() when it is done with the
 * object.
 *
 * This will find all XDomains including the ones that are not yet added
 * to the bus (handshake is still in progress).
 *
 * The caller needs to hold @tb->lock.
 */
struct tb_xdomain *tb_xdomain_find_by_uuid(struct tb *tb, const uuid_t *uuid)
{
	struct tb_xdomain_lookup lookup;
	struct tb_xdomain *xd;

	memset(&lookup, 0, sizeof(lookup));
	lookup.uuid = uuid;

	xd = switch_find_xdomain(tb->root_switch, &lookup);
	return tb_xdomain_get(xd);
}
EXPORT_SYMBOL_GPL(tb_xdomain_find_by_uuid);

/**
 * tb_xdomain_find_by_link_depth() - Find an XDomain by link and depth
 * @tb: Domain where the XDomain belongs to
 * @link: Root switch link number
 * @depth: Depth in the link
 *
 * Finds XDomain by walking through the Thunderbolt topology below @tb.
 * The returned XDomain will have its reference count increased so the
 * caller needs to call tb_xdomain_put() when it is done with the
 * object.
 *
 * This will find all XDomains including the ones that are not yet added
 * to the bus (handshake is still in progress).
 *
 * The caller needs to hold @tb->lock.
 */
struct tb_xdomain *tb_xdomain_find_by_link_depth(struct tb *tb, u8 link,
						 u8 depth)
{
	struct tb_xdomain_lookup lookup;
	struct tb_xdomain *xd;

	memset(&lookup, 0, sizeof(lookup));
	lookup.link = link;
	lookup.depth = depth;

	xd = switch_find_xdomain(tb->root_switch, &lookup);
	return tb_xdomain_get(xd);
}

/**
 * tb_xdomain_find_by_route() - Find an XDomain by route string
 * @tb: Domain where the XDomain belongs to
 * @route: XDomain route string
 *
 * Finds XDomain by walking through the Thunderbolt topology below @tb.
 * The returned XDomain will have its reference count increased so the
 * caller needs to call tb_xdomain_put() when it is done with the
 * object.
 *
 * This will find all XDomains including the ones that are not yet added
 * to the bus (handshake is still in progress).
 *
 * The caller needs to hold @tb->lock.
 */
struct tb_xdomain *tb_xdomain_find_by_route(struct tb *tb, u64 route)
{
	struct tb_xdomain_lookup lookup;
	struct tb_xdomain *xd;

	memset(&lookup, 0, sizeof(lookup));
	lookup.route = route;

	xd = switch_find_xdomain(tb->root_switch, &lookup);
	return tb_xdomain_get(xd);
}
EXPORT_SYMBOL_GPL(tb_xdomain_find_by_route);

bool tb_xdomain_handle_request(struct tb *tb, enum tb_cfg_pkg_type type,
			       const void *buf, size_t size)
{
	const struct tb_protocol_handler *handler;
	const struct tb_xdp_header *hdr = buf;
	struct tb_xdomain *source_xd = NULL;
	unsigned int length;
	int ret = 0;
	u64 route;

	/* We expect the packet is at least size of the header */
	length = hdr->xd_hdr.length_sn & TB_XDOMAIN_LENGTH_MASK;
	if (length != size / 4 - sizeof(hdr->xd_hdr) / 4)
		return true;
	if (length < sizeof(*hdr) / 4 - sizeof(hdr->xd_hdr) / 4)
		return true;

	/*
	 * Handle XDomain discovery protocol packets directly here. For
	 * other protocols (based on their UUID) we call registered
	 * handlers in turn.
	 */
	if (uuid_equal(&hdr->uuid, &tb_xdp_uuid)) {
		if (type == TB_CFG_PKG_XDOMAIN_REQ)
			return tb_xdp_schedule_request(tb, hdr, size);
		return false;
	}

	route = ((u64)hdr->xd_hdr.route_hi << 32 | hdr->xd_hdr.route_lo) &
		~BIT_ULL(63);
	source_xd = tb_xdomain_find_by_route_locked(tb, route);
	if (source_xd)
		tb_xdomain_control_event(source_xd,
					 TB_XDOMAIN_CONTROL_INBOUND_PROGRESS);

	/*
	 * The dispatch lock is held across the whole walk including the
	 * callbacks, so tb_unregister_protocol_handler() (which takes it)
	 * cannot delist or free a handler this walk is about to call, and
	 * dropping xdomain_lock around the callback can no longer leave the
	 * cached next pointer dangling. Only registration (xdomain_lock
	 * alone) may run during a callback, and list_add_tail keeps the walk
	 * cursor valid.
	 */
	mutex_lock(&xdomain_dispatch_lock);
	mutex_lock(&xdomain_lock);
	list_for_each_entry(handler, &protocol_handlers, list) {
		if (!uuid_equal(&hdr->uuid, handler->uuid))
			continue;

		mutex_unlock(&xdomain_lock);
		/*
		 * ->callback first: a registrant that set it may be built
		 * against the stock <linux/thunderbolt.h>, whose struct ends
		 * at ->list -- ->callback_xd may only be read once ->callback
		 * is known to be NULL (a source-aware registrant, which
		 * always provides the extended struct). Reading ->callback_xd
		 * unconditionally is how a stock-built tbframe.ko sent this
		 * core through its ->data pointer (appmana-025 NX panic,
		 * kdump 202608031305).
		 */
		if (handler->callback)
			ret = handler->callback(buf, size, handler->data);
		else if (handler->callback_xd)
			ret = handler->callback_xd(source_xd, buf, size,
						   handler->data);
		mutex_lock(&xdomain_lock);

		if (ret)
			break;
	}
	mutex_unlock(&xdomain_lock);
	mutex_unlock(&xdomain_dispatch_lock);
	tb_xdomain_put(source_xd);

	return ret > 0;
}

static int update_xdomain(struct device *dev, void *data)
{
	struct tb_xdomain *xd;

	xd = tb_to_xdomain(dev);
	if (xd) {
		mutex_lock(&xd->lock);
		if (!xd->removing)
			queue_delayed_work(xd->tb->wq,
					   &xd->properties_changed_work,
					   msecs_to_jiffies(50));
		mutex_unlock(&xd->lock);
	}

	return 0;
}

static void update_all_xdomains(void)
{
	bus_for_each_dev(&tb_bus_type, NULL, NULL, update_xdomain);
}

/**
 * tb_reannounce_property_dirs() - Reannounce current host properties
 *
 * Advances the property-block generation and notifies every connected
 * XDomain without changing the advertised service set. Recovery code must
 * use this instead of unregistering and re-registering live directories: a
 * peer can observe the empty intermediate generation and tear down healthy
 * services, then miss the best-effort notification that adds them back.
 */
void tb_reannounce_property_dirs(void)
{
	mutex_lock(&xdomain_lock);
	xdomain_property_block_gen++;
	mutex_unlock(&xdomain_lock);

	update_all_xdomains();
}
EXPORT_SYMBOL_GPL(tb_reannounce_property_dirs);

static bool remove_directory(const char *key, const struct tb_property_dir *dir)
{
	struct tb_property *p;

	p = tb_property_find(xdomain_property_dir, key,
			     TB_PROPERTY_TYPE_DIRECTORY);
	if (p && p->value.dir == dir) {
		tb_property_remove(p);
		return true;
	}
	return false;
}

/**
 * tb_register_property_dir() - Register property directory to the host
 * @key: Key (name) of the directory to add
 * @dir: Directory to add
 *
 * Service drivers can use this function to add new property directory
 * to the host available properties. The other connected hosts are
 * notified so they can re-read properties of this host if they are
 * interested.
 *
 * Return: %0 on success and negative errno on failure
 */
int tb_register_property_dir(const char *key, struct tb_property_dir *dir)
{
	int ret;

	if (WARN_ON(!xdomain_property_dir))
		return -EAGAIN;

	if (!key || strlen(key) > 8)
		return -EINVAL;

	mutex_lock(&xdomain_lock);
	if (tb_property_find(xdomain_property_dir, key,
			     TB_PROPERTY_TYPE_DIRECTORY)) {
		ret = -EEXIST;
		goto err_unlock;
	}

	ret = tb_property_add_dir(xdomain_property_dir, key, dir);
	if (ret)
		goto err_unlock;

	xdomain_property_block_gen++;

	mutex_unlock(&xdomain_lock);
	update_all_xdomains();
	return 0;

err_unlock:
	mutex_unlock(&xdomain_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(tb_register_property_dir);

/**
 * tb_unregister_property_dir() - Removes property directory from host
 * @key: Key (name) of the directory
 * @dir: Directory to remove
 *
 * This will remove the existing directory from this host and notify the
 * connected hosts about the change.
 */
void tb_unregister_property_dir(const char *key, struct tb_property_dir *dir)
{
	int ret = 0;

	mutex_lock(&xdomain_lock);
	if (remove_directory(key, dir))
		xdomain_property_block_gen++;
	mutex_unlock(&xdomain_lock);

	if (!ret)
		update_all_xdomains();
}
EXPORT_SYMBOL_GPL(tb_unregister_property_dir);

int tb_xdomain_init(void)
{
	xdomain_property_dir = tb_property_create_dir(NULL);
	if (!xdomain_property_dir)
		return -ENOMEM;

	/*
	 * Initialize standard set of properties without any service
	 * directories. Those will be added by service drivers
	 * themselves when they are loaded.
	 *
	 * Rest of the properties are filled dynamically based on these
	 * when the P2P connection is made.
	 */
	tb_property_add_immediate(xdomain_property_dir, "vendorid",
				  PCI_VENDOR_ID_INTEL);
	tb_property_add_text(xdomain_property_dir, "vendorid", "Intel Corp.");
	tb_property_add_immediate(xdomain_property_dir, "deviceid", 0x1);
	tb_property_add_immediate(xdomain_property_dir, "devicerv", 0x80000100);

	xdomain_property_block_gen = get_random_u32();
	return 0;
}

void tb_xdomain_exit(void)
{
	tb_property_free_dir(xdomain_property_dir);
}
