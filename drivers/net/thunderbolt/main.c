// SPDX-License-Identifier: GPL-2.0
/*
 * Networking over Thunderbolt/USB4 cables using USB4NET protocol
 * (formerly Apple ThunderboltIP).
 *
 * Copyright (C) 2017, Intel Corporation
 * Authors: Amir Levy <amir.jer.levy@intel.com>
 *          Michael Jamet <michael.jamet@intel.com>
 *          Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#include <linux/atomic.h>
#include <linux/ethtool.h>
#include <linux/highmem.h>
#include <linux/if_vlan.h>
#include <linux/jhash.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/sizes.h>
#include <linux/thunderbolt.h>

#include "../../thunderbolt/thunderbolt_negotiation.h"
#include <linux/uuid.h>
#include <linux/workqueue.h>

#include <net/ip6_checksum.h>

#include "trace.h"

/* Protocol timeouts in ms */
#define TBNET_LOGIN_DELAY	4500
#define TBNET_LOGIN_TIMEOUT	500
#define TBNET_LOGOUT_TIMEOUT	1000

#define TBNET_MIN_RING_SIZE	256
#define TBNET_DEFAULT_RING_SIZE	2048
#define TBNET_MAX_RING_SIZE	4096
#define TBNET_DEFAULT_INT_COALESCE_USECS 128
#define TBNET_LOGIN_RETRIES	60
#define TBNET_LOGOUT_RETRIES	10
#define TBNET_E2E		BIT(0)
#define TBNET_MATCH_FRAGS_ID	BIT(1)
#define TBNET_64K_FRAMES	BIT(2)
#define TBNET_MAX_MTU		SZ_64K
#define TBNET_FRAME_SIZE	SZ_4K
#define TBNET_MAX_PAYLOAD_SIZE	\
	(TBNET_FRAME_SIZE - sizeof(struct thunderbolt_ip_frame_header))
/* Rx packets need to hold space for skb_shared_info */
#define TBNET_RX_MAX_SIZE	\
	(TBNET_FRAME_SIZE + SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))
#define TBNET_RX_PAGE_ORDER	get_order(TBNET_RX_MAX_SIZE)
#define TBNET_RX_PAGE_SIZE	(PAGE_SIZE << TBNET_RX_PAGE_ORDER)

#define TBNET_L0_PORT_NUM(route) ((route) & GENMASK(5, 0))

int tb_ring_throttling(struct tb_ring *ring, unsigned int interval_nsec);

/**
 * struct thunderbolt_ip_frame_header - Header for each Thunderbolt frame
 * @frame_size: size of the data with the frame
 * @frame_index: running index on the frames
 * @frame_id: ID of the frame to match frames to specific packet
 * @frame_count: how many frames assembles a full packet
 *
 * Each data frame passed to the high-speed DMA ring has this header. If
 * the XDomain network directory announces that %TBNET_MATCH_FRAGS_ID is
 * supported then @frame_id is filled, otherwise it stays %0.
 */
struct thunderbolt_ip_frame_header {
	__le32 frame_size;
	__le16 frame_index;
	__le16 frame_id;
	__le32 frame_count;
};

enum thunderbolt_ip_frame_pdf {
	TBIP_PDF_FRAME_START = 1,
	TBIP_PDF_FRAME_END,
};

enum thunderbolt_ip_type {
	TBIP_LOGIN,
	TBIP_LOGIN_RESPONSE,
	TBIP_LOGOUT,
	TBIP_STATUS,
};

struct thunderbolt_ip_header {
	u32 route_hi;
	u32 route_lo;
	u32 length_sn;
	uuid_t uuid;
	uuid_t initiator_uuid;
	uuid_t target_uuid;
	u32 type;
	u32 command_id;
};

#define TBIP_HDR_LENGTH_MASK		GENMASK(5, 0)
#define TBIP_HDR_SN_MASK		GENMASK(28, 27)
#define TBIP_HDR_SN_SHIFT		27

struct thunderbolt_ip_login {
	struct thunderbolt_ip_header hdr;
	u32 proto_version;
	u32 transmit_path;
	u32 reserved[4];
};

#define TBIP_LOGIN_PROTO_VERSION	1

struct thunderbolt_ip_login_response {
	struct thunderbolt_ip_header hdr;
	u32 status;
	u32 receiver_mac[2];
	u32 receiver_mac_len;
	u32 reserved[4];
};

struct thunderbolt_ip_logout {
	struct thunderbolt_ip_header hdr;
};

struct thunderbolt_ip_status {
	struct thunderbolt_ip_header hdr;
	u32 status;
};

struct tbnet_stats {
	u64 tx_packets;
	u64 rx_packets;
	u64 tx_bytes;
	u64 rx_bytes;
	u64 rx_errors;
	u64 tx_errors;
	u64 rx_length_errors;
	u64 rx_over_errors;
	u64 rx_crc_errors;
	u64 rx_missed_errors;
};

struct tbnet_frame {
	struct net_device *dev;
	struct page *page;
	struct ring_frame frame;
};

struct tbnet_ring {
	struct tbnet_frame *frames;
	unsigned int size;
	unsigned int cons;
	unsigned int prod;
	struct tb_ring *ring;
};

/**
 * struct tbnet - ThunderboltIP network driver private data
 * @svc: XDomain service the driver is bound to
 * @xd: XDomain the service belongs to
 * @handler: ThunderboltIP configuration protocol handler
 * @dev: Networking device
 * @napi: NAPI structure for Rx polling
 * @stats: Network statistics
 * @skb: Network packet that is currently processed on Rx path
 * @command_id: ID used for next configuration protocol packet
 * @login_hs: ThunderboltIP login handshake state (shared tb_xdomain_handshake:
 *	      request_sent == our login sent, peer_seen == remote login received,
 *	      complete == both, so a soft reconnect can re-arm/supersede it the
 *	      same way the core and the RDMA stack do)
 * @local_transmit_path: HopID we are using to send out packets
 * @remote_transmit_path: HopID the other end is using to send packets to us
 * @session_active: DMA paths were enabled for the committed session
 * @resources_owned: input HopID and started rings require local teardown
 * @active_remote_transmit_path: input HopID owned by the active session
 * @connection_lock: Lock serializing access to @login_hs and @transmit_path.
 * @login_retries: Number of login retries currently done
 * @login_work: Worker to send ThunderboltIP login packets
 * @connected_work: Worker that finalizes the ThunderboltIP connection
 *		    setup and enables DMA paths for high speed data
 *		    transfers
 * @verify_work: Worker that periodically re-checks (while carrier is on)
 *		 that the DMA tunnel behind the session is still programmed
 *		 in the routers, and re-runs the login when it is not
 * @disconnect_work: Worker that handles tearing down the ThunderboltIP
 *		     connection
 * @rx_hdr: Copy of the currently processed Rx frame. Used when a
 *	    network packet consists of multiple Thunderbolt frames.
 *	    In host byte order.
 * @rx_ring: Software ring holding Rx frames
 * @rx_ring_size: Configured Rx ring size
 * @int_coalesce_usecs: Ring interrupt coalescing interval in usec
 * @frame_id: Frame ID use for next Tx packet
 *            (if %TBNET_MATCH_FRAGS_ID is supported in both ends)
 * @tx_ring: Software ring holding Tx frames
 * @tx_ring_size: Configured Tx ring size
 */
struct tbnet {
	const struct tb_service *svc;
	struct tb_xdomain *xd;
	struct tb_protocol_handler handler;
	/*
	 * Both the shutdown and the unbind path fence inbound dispatch before
	 * touching the data path, and shutdown is followed by unbind on a
	 * driver unload, so the unregister must be idempotent and observable.
	 */
	bool handler_registered;
	struct net_device *dev;
	struct napi_struct napi;
	struct tbnet_stats stats;
	struct sk_buff *skb;
	atomic_t command_id;
	struct tb_xdomain_handshake login_hs;
	int local_transmit_path;
	int remote_transmit_path;
	bool session_active;
	bool resources_owned;
	int active_remote_transmit_path;
	struct mutex connection_lock;
	int login_retries;
	struct delayed_work login_work;
	struct work_struct connected_work;
	struct delayed_work verify_work;
	struct work_struct disconnect_work;
	struct thunderbolt_ip_frame_header rx_hdr;
	struct tbnet_ring rx_ring;
	unsigned int rx_ring_size;
	unsigned int int_coalesce_usecs;
	atomic_t frame_id;
	struct tbnet_ring tx_ring;
	unsigned int tx_ring_size;
};

/* Network property directory UUID: c66189ca-1cce-4195-bdb8-49592e5f5a4f */
static const uuid_t tbnet_dir_uuid =
	UUID_INIT(0xc66189ca, 0x1cce, 0x4195,
		  0xbd, 0xb8, 0x49, 0x59, 0x2e, 0x5f, 0x5a, 0x4f);

/* ThunderboltIP protocol UUID: 798f589e-3616-8a47-97c6-5664a920c8dd */
static const uuid_t tbnet_svc_uuid =
	UUID_INIT(0x798f589e, 0x3616, 0x8a47,
		  0x97, 0xc6, 0x56, 0x64, 0xa9, 0x20, 0xc8, 0xdd);

static void tbnet_fence_handler(struct tbnet *net);
static void tbnet_cancel_all_work(struct tbnet *net);

static struct tb_property_dir *tbnet_dir;

static bool tbnet_e2e = true;
module_param_named(e2e, tbnet_e2e, bool, 0444);
MODULE_PARM_DESC(e2e, "USB4NET full end-to-end flow control (default: true)");

/*
 * Level-triggered session revalidation cadence. The ThunderboltIP session is
 * one-shot: once carrier is on, login_work and connected_work both
 * early-return, so a DMA tunnel that dies underneath the session (peer
 * reboot or re-negotiation without a processed hotplug edge -- the routers'
 * hop entries read back enable=0) leaves a permanent carrier-up/zero-packet
 * zombie on BOTH ends: each side latches "login complete" and neither ever
 * sends a LOGIN again. While carrier is on, re-check the tunnel against the
 * routers' path config space every this many ms and, when it is gone, tear
 * the session down and re-run the normal LOGIN negotiation. 0 disables.
 */
static uint tbnet_session_verify_ms = 5000;
module_param_named(session_verify_ms, tbnet_session_verify_ms, uint, 0644);
MODULE_PARM_DESC(session_verify_ms,
		 "Re-check the DMA tunnel behind an established ThunderboltIP session every N ms; re-login when it died (0 disables)");

/*
 * Hard deadline for the orderly LOGOUT handshake in tbnet_tear_down(). The
 * retry count alone (TBNET_LOGOUT_RETRIES * TBNET_LOGOUT_TIMEOUT = 10 s) is a
 * bound in name only for the paths that matter: ndo_stop holds RTNL across it,
 * and device_shutdown() runs it once per link while the machine is trying to
 * reboot. On expiry the teardown proceeds unchanged; only the courtesy notice
 * to the peer is skipped.
 */
static uint tbnet_logout_budget_ms = 3000;
module_param_named(logout_budget_ms, tbnet_logout_budget_ms, uint, 0644);
MODULE_PARM_DESC(logout_budget_ms,
		 "Hard deadline (ms) for the LOGOUT handshake during teardown; on expiry the teardown proceeds without it");

static void tbnet_fill_header(struct thunderbolt_ip_header *hdr, u64 route,
	u8 sequence, const uuid_t *initiator_uuid, const uuid_t *target_uuid,
	enum thunderbolt_ip_type type, size_t size, u32 command_id)
{
	u32 length_sn;

	/* Length does not include route_hi/lo and length_sn fields */
	length_sn = (size - 3 * 4) / 4;
	length_sn |= (sequence << TBIP_HDR_SN_SHIFT) & TBIP_HDR_SN_MASK;

	hdr->route_hi = upper_32_bits(route);
	hdr->route_lo = lower_32_bits(route);
	hdr->length_sn = length_sn;
	uuid_copy(&hdr->uuid, &tbnet_svc_uuid);
	uuid_copy(&hdr->initiator_uuid, initiator_uuid);
	uuid_copy(&hdr->target_uuid, target_uuid);
	hdr->type = type;
	hdr->command_id = command_id;
}

static int tbnet_login_response(struct tbnet *net, u64 route, u8 sequence,
				u32 command_id)
{
	struct thunderbolt_ip_login_response reply;
	struct tb_xdomain *xd = net->xd;

	memset(&reply, 0, sizeof(reply));
	tbnet_fill_header(&reply.hdr, route, sequence, xd->local_uuid,
			  xd->remote_uuid, TBIP_LOGIN_RESPONSE, sizeof(reply),
			  command_id);
	memcpy(reply.receiver_mac, net->dev->dev_addr, ETH_ALEN);
	reply.receiver_mac_len = ETH_ALEN;

	return tb_xdomain_response(xd, &reply, sizeof(reply),
				   TB_CFG_PKG_XDOMAIN_RESP);
}

static int tbnet_login_request(struct tbnet *net, u8 sequence)
{
	struct thunderbolt_ip_login_response reply;
	struct thunderbolt_ip_login request;
	struct tb_xdomain *xd = net->xd;

	memset(&request, 0, sizeof(request));
	tbnet_fill_header(&request.hdr, xd->route, sequence, xd->local_uuid,
			  xd->remote_uuid, TBIP_LOGIN, sizeof(request),
			  atomic_inc_return(&net->command_id));

	request.proto_version = TBIP_LOGIN_PROTO_VERSION;
	request.transmit_path = net->local_transmit_path;

	return tb_xdomain_request(xd, &request, sizeof(request),
				  TB_CFG_PKG_XDOMAIN_RESP, &reply,
				  sizeof(reply), TB_CFG_PKG_XDOMAIN_RESP,
				  TBNET_LOGIN_TIMEOUT);
}

static int tbnet_logout_response(struct tbnet *net, u64 route, u8 sequence,
				 u32 command_id)
{
	struct thunderbolt_ip_status reply;
	struct tb_xdomain *xd = net->xd;

	memset(&reply, 0, sizeof(reply));
	tbnet_fill_header(&reply.hdr, route, sequence, xd->local_uuid,
			  xd->remote_uuid, TBIP_STATUS, sizeof(reply),
			  atomic_inc_return(&net->command_id));
	return tb_xdomain_response(xd, &reply, sizeof(reply),
				   TB_CFG_PKG_XDOMAIN_RESP);
}

static int tbnet_logout_request(struct tbnet *net)
{
	struct thunderbolt_ip_logout request;
	struct thunderbolt_ip_status reply;
	struct tb_xdomain *xd = net->xd;

	memset(&request, 0, sizeof(request));
	tbnet_fill_header(&request.hdr, xd->route, 0, xd->local_uuid,
			  xd->remote_uuid, TBIP_LOGOUT, sizeof(request),
			  atomic_inc_return(&net->command_id));

	return tb_xdomain_request(xd, &request, sizeof(request),
				  TB_CFG_PKG_XDOMAIN_RESP, &reply,
				  sizeof(reply), TB_CFG_PKG_XDOMAIN_RESP,
				  TBNET_LOGOUT_TIMEOUT);
}

static void start_login(struct tbnet *net)
{
	netdev_dbg(net->dev, "login started\n");

	mutex_lock(&net->connection_lock);
	tb_xdomain_handshake_reset(&net->login_hs);
	mutex_unlock(&net->connection_lock);

	queue_delayed_work(system_long_wq, &net->login_work,
			   msecs_to_jiffies(1000));
}

static void stop_login(struct tbnet *net)
{
	cancel_delayed_work_sync(&net->login_work);
	cancel_work_sync(&net->connected_work);

	netdev_dbg(net->dev, "login stopped\n");
}

static inline unsigned int tbnet_frame_size(const struct tbnet_frame *tf)
{
	return tf->frame.size ? : TBNET_FRAME_SIZE;
}

static void tbnet_free_buffers(struct tbnet_ring *ring)
{
	unsigned int i;

	for (i = 0; i < ring->size; i++) {
		struct device *dma_dev = tb_ring_dma_device(ring->ring);
		struct tbnet_frame *tf = &ring->frames[i];
		enum dma_data_direction dir;
		unsigned int order;
		size_t size;

		if (!tf->page)
			continue;

		if (ring->ring->is_tx) {
			dir = DMA_TO_DEVICE;
			order = 0;
			size = TBNET_FRAME_SIZE;
		} else {
			dir = DMA_FROM_DEVICE;
			order = TBNET_RX_PAGE_ORDER;
			size = TBNET_RX_PAGE_SIZE;
		}

		trace_tbnet_free_frame(i, tf->page, tf->frame.buffer_phy, dir);

		if (tf->frame.buffer_phy)
			dma_unmap_page(dma_dev, tf->frame.buffer_phy, size,
				       dir);

		__free_pages(tf->page, order);
		tf->page = NULL;
	}

	ring->cons = 0;
	ring->prod = 0;
}

static bool tbnet_session_needs_teardown(bool handshake_complete,
					 bool session_active,
					 bool resources_owned)
{
	(void)handshake_complete;
	return session_active || resources_owned;
}

#if IS_ENABLED(CONFIG_USB4_NET_KUNIT_TEST)
bool tbnet_test_session_needs_teardown(bool handshake_complete,
				       bool session_active,
				       bool resources_owned);

bool tbnet_test_session_needs_teardown(bool handshake_complete,
				       bool session_active,
				       bool resources_owned)
{
	return tbnet_session_needs_teardown(handshake_complete, session_active,
					  resources_owned);
}
#endif

static void tbnet_tear_down(struct tbnet *net, bool send_logout)
{
	netif_carrier_off(net->dev);
	netif_stop_queue(net->dev);

	stop_login(net);
	/*
	 * Non-sync on purpose: tbnet_verify_work() itself calls
	 * tbnet_tear_down() when it finds the tunnel dead. Full-stop points
	 * (ndo_stop, suspend) cancel it synchronously before the rings go
	 * away.
	 */
	cancel_delayed_work(&net->verify_work);

	mutex_lock(&net->connection_lock);

	if (tbnet_session_needs_teardown(
			tb_xdomain_handshake_complete(&net->login_hs),
			net->session_active, net->resources_owned)) {
		int ret, retries = TBNET_LOGOUT_RETRIES;
		unsigned long deadline;
		int remote_transmit_path = net->active_remote_transmit_path;

		/*
		 * Hard deadline on the peer wait, on top of the retry count.
		 * TBNET_LOGOUT_RETRIES * TBNET_LOGOUT_TIMEOUT is 10 s against a
		 * peer that never answers, and this runs from ndo_stop (under
		 * RTNL, so every rtnl_lock() user on the box stalls with it),
		 * from device_shutdown() during a reboot, and once per link on
		 * a chain node. A peer that is powered off or wedged must not
		 * be able to buy that much of a shutdown. Defined failure
		 * action on expiry: log once and proceed with the local
		 * teardown exactly as if the logout had been refused -- the
		 * peer's own session verify collects the stale session.
		 */
		deadline = jiffies +
			   msecs_to_jiffies(READ_ONCE(tbnet_logout_budget_ms));

		while (send_logout && net->session_active && retries-- > 0) {
			netdev_dbg(net->dev, "sending logout request %u\n",
				   retries);
			ret = tbnet_logout_request(net);
			if (ret != -ETIMEDOUT)
				break;
			if (time_after_eq(jiffies, deadline)) {
				netdev_warn(net->dev,
					    "logout unacknowledged within %u ms; proceeding with teardown\n",
					    READ_ONCE(tbnet_logout_budget_ms));
				break;
			}
		}

		if (net->resources_owned) {
			tb_ring_stop(net->rx_ring.ring);
			tb_ring_stop(net->tx_ring.ring);
			tbnet_free_buffers(&net->rx_ring);
			tbnet_free_buffers(&net->tx_ring);
		}

		if (net->session_active) {
			ret = tb_xdomain_disable_paths(net->xd,
						       net->local_transmit_path,
						       net->tx_ring.ring->hop,
						       remote_transmit_path,
						       net->rx_ring.ring->hop);
			if (ret)
				netdev_warn(net->dev,
					    "failed to disable DMA paths\n");
		}

		if (net->resources_owned)
			tb_xdomain_release_in_hopid(net->xd,
						    remote_transmit_path);
		net->session_active = false;
		net->resources_owned = false;
		net->active_remote_transmit_path = 0;
		net->remote_transmit_path = 0;
	}

	net->login_retries = 0;
	tb_xdomain_handshake_reset(&net->login_hs);

	netdev_dbg(net->dev, "network traffic stopped\n");

	mutex_unlock(&net->connection_lock);
}

static void tbnet_queue_verify(struct tbnet *net)
{
	unsigned int ms = READ_ONCE(tbnet_session_verify_ms);

	if (!IS_ENABLED(TB_XDOMAIN_HAS_PATHS_ACTIVE) || !ms)
		return;

	queue_delayed_work(system_long_wq, &net->verify_work,
			   msecs_to_jiffies(ms));
}

static void tbnet_verify_work(struct work_struct *work)
{
#ifdef TB_XDOMAIN_HAS_PATHS_ACTIVE
	struct tbnet *net = container_of(work, typeof(*net), verify_work.work);
	int transmit_path = -1, transmit_ring = -1;
	int receive_path = -1, receive_ring = -1;
	bool established;
	int ret;

	if (!netif_carrier_ok(net->dev))
		return;

	mutex_lock(&net->connection_lock);
	established = net->session_active && net->tx_ring.ring &&
		      net->rx_ring.ring;
	if (established) {
		transmit_path = net->local_transmit_path;
		transmit_ring = net->tx_ring.ring->hop;
		receive_path = net->active_remote_transmit_path;
		receive_ring = net->rx_ring.ring->hop;
	}
	mutex_unlock(&net->connection_lock);

	if (!established) {
		tbnet_queue_verify(net);
		return;
	}

	ret = tb_xdomain_paths_active(net->xd, transmit_path, transmit_ring,
				      receive_path, receive_ring);
	if (ret != 0) {
		/* Still programmed (>0), or state unknown (<0): keep watching. */
		tbnet_queue_verify(net);
		return;
	}

	/*
	 * The routers no longer carry our DMA tunnel (peer reboot or
	 * re-negotiation without a processed hotplug edge). The session is a
	 * zombie: carrier on, hop entries enable=0, every frame silently
	 * dropped at the lane adapter, and both ends latched "login complete"
	 * so neither would ever re-login. Tear down and re-run the normal
	 * spec negotiation.
	 */
	netdev_warn(net->dev,
		    "ThunderboltIP session lost its DMA tunnel; tearing down and re-logging in\n");
	tbnet_tear_down(net, false);
	start_login(net);
#endif /* TB_XDOMAIN_HAS_PATHS_ACTIVE */
}

static int tbnet_handle_packet(const void *buf, size_t size, void *data)
{
	const struct thunderbolt_ip_login *pkg = buf;
	struct tbnet *net = data;
	u32 command_id;
	int ret = 0;
	u32 sequence;
	u64 route;

	/* Make sure the packet is for us */
	if (size < sizeof(struct thunderbolt_ip_header))
		return 0;
	if (!uuid_equal(&pkg->hdr.initiator_uuid, net->xd->remote_uuid))
		return 0;
	if (!uuid_equal(&pkg->hdr.target_uuid, net->xd->local_uuid))
		return 0;

	route = ((u64)pkg->hdr.route_hi << 32) | pkg->hdr.route_lo;
	route &= ~BIT_ULL(63);
	if (route != net->xd->route)
		return 0;

	sequence = pkg->hdr.length_sn & TBIP_HDR_SN_MASK;
	sequence >>= TBIP_HDR_SN_SHIFT;
	command_id = pkg->hdr.command_id;

	switch (pkg->hdr.type) {
	case TBIP_LOGIN:
		netdev_dbg(net->dev, "remote login request received\n");
		if (!netif_running(net->dev))
			break;

		ret = tbnet_login_response(net, route, sequence,
					   pkg->hdr.command_id);
		if (!ret) {
			netdev_dbg(net->dev, "remote login response sent\n");

			mutex_lock(&net->connection_lock);
			/* A fresh LOGIN arriving while our handshake is already
			 * complete means the peer restarted its side WITHOUT a
			 * physical link edge, so our session points at its freed
			 * rings. Supersede the stale handshake and tear the
			 * tunnel down; the peer's ongoing login retries then
			 * re-establish a fresh session. (Shared contract with the
			 * core and the RDMA stack; see thunderbolt_negotiation.h.)
			 */
			if (tb_xdomain_handshake_supersede(&net->login_hs)) {
				mutex_unlock(&net->connection_lock);
				netdev_dbg(net->dev,
					   "peer re-login: tearing down stale session\n");
				queue_work(system_long_wq, &net->disconnect_work);
				break;
			}
			net->login_hs.peer_seen = true;
			net->remote_transmit_path = pkg->transmit_path;

			/* If we reached the number of max retries or
			 * previous logout, schedule another round of
			 * login retries
			 */
			if (net->login_retries >= TBNET_LOGIN_RETRIES ||
			    !net->login_hs.request_sent) {
				net->login_retries = 0;
				queue_delayed_work(system_long_wq,
						   &net->login_work, 0);
			}
			mutex_unlock(&net->connection_lock);

			queue_work(system_long_wq, &net->connected_work);
		}
		break;

	case TBIP_LOGOUT:
		netdev_dbg(net->dev, "remote logout request received\n");
		ret = tbnet_logout_response(net, route, sequence, command_id);
		if (!ret) {
			netdev_dbg(net->dev, "remote logout response sent\n");
			queue_work(system_long_wq, &net->disconnect_work);
		}
		break;

	default:
		return 0;
	}

	if (ret)
		netdev_warn(net->dev, "failed to send ThunderboltIP response\n");

	return 1;
}

static unsigned int tbnet_available_buffers(const struct tbnet_ring *ring)
{
	return ring->prod - ring->cons;
}

static int tbnet_alloc_rx_buffers(struct tbnet *net, unsigned int nbuffers)
{
	struct tbnet_ring *ring = &net->rx_ring;
	int ret;

	while (nbuffers--) {
		struct device *dma_dev = tb_ring_dma_device(ring->ring);
		unsigned int index = ring->prod & (ring->size - 1);
		struct tbnet_frame *tf = &ring->frames[index];
		dma_addr_t dma_addr;

		if (tf->page)
			break;

		/* Allocate page (order > 0) so that it can hold maximum
		 * ThunderboltIP frame (4kB) and the additional room for
		 * SKB shared info required by build_skb().
		 */
		tf->page = dev_alloc_pages(TBNET_RX_PAGE_ORDER);
		if (!tf->page) {
			ret = -ENOMEM;
			goto err_free;
		}

		dma_addr = dma_map_page(dma_dev, tf->page, 0,
					TBNET_RX_PAGE_SIZE, DMA_FROM_DEVICE);
		if (dma_mapping_error(dma_dev, dma_addr)) {
			ret = -ENOMEM;
			goto err_free;
		}

		tf->frame.buffer_phy = dma_addr;
		tf->dev = net->dev;

		trace_tbnet_alloc_rx_frame(index, tf->page, dma_addr,
					   DMA_FROM_DEVICE);

		tb_ring_rx(ring->ring, &tf->frame);

		ring->prod++;
	}

	return 0;

err_free:
	tbnet_free_buffers(ring);
	return ret;
}

static struct tbnet_frame *tbnet_get_tx_buffer(struct tbnet *net)
{
	struct tbnet_ring *ring = &net->tx_ring;
	struct device *dma_dev = tb_ring_dma_device(ring->ring);
	struct tbnet_frame *tf;
	unsigned int index;

	if (!tbnet_available_buffers(ring))
		return NULL;

	index = ring->cons++ & (ring->size - 1);

	tf = &ring->frames[index];
	tf->frame.size = 0;

	dma_sync_single_for_cpu(dma_dev, tf->frame.buffer_phy,
				tbnet_frame_size(tf), DMA_TO_DEVICE);

	return tf;
}

static void tbnet_tx_callback(struct tb_ring *ring, struct ring_frame *frame,
			      bool canceled)
{
	struct tbnet_frame *tf = container_of(frame, typeof(*tf), frame);
	struct tbnet *net = netdev_priv(tf->dev);

	/* Return buffer to the ring */
	net->tx_ring.prod++;

	if (tbnet_available_buffers(&net->tx_ring) >= net->tx_ring.size / 2)
		netif_wake_queue(net->dev);
}

static int tbnet_alloc_tx_buffers(struct tbnet *net)
{
	struct tbnet_ring *ring = &net->tx_ring;
	struct device *dma_dev = tb_ring_dma_device(ring->ring);
	unsigned int i;

	for (i = 0; i < ring->size; i++) {
		struct tbnet_frame *tf = &ring->frames[i];
		dma_addr_t dma_addr;

		tf->page = alloc_page(GFP_KERNEL);
		if (!tf->page) {
			tbnet_free_buffers(ring);
			return -ENOMEM;
		}

		dma_addr = dma_map_page(dma_dev, tf->page, 0, TBNET_FRAME_SIZE,
					DMA_TO_DEVICE);
		if (dma_mapping_error(dma_dev, dma_addr)) {
			__free_page(tf->page);
			tf->page = NULL;
			tbnet_free_buffers(ring);
			return -ENOMEM;
		}

		tf->dev = net->dev;
		tf->frame.buffer_phy = dma_addr;
		tf->frame.callback = tbnet_tx_callback;
		tf->frame.sof = TBIP_PDF_FRAME_START;
		tf->frame.eof = TBIP_PDF_FRAME_END;

		trace_tbnet_alloc_tx_frame(i, tf->page, dma_addr, DMA_TO_DEVICE);
	}

	ring->cons = 0;
	ring->prod = ring->size - 1;

	return 0;
}

static void tbnet_connected_work(struct work_struct *work)
{
	struct tbnet *net = container_of(work, typeof(*net), connected_work);
	bool connected;
	int remote_transmit_path;
	int ret;

	if (netif_carrier_ok(net->dev))
		return;

	mutex_lock(&net->connection_lock);
	connected = tb_xdomain_handshake_complete(&net->login_hs);
	remote_transmit_path = net->remote_transmit_path;

	if (!connected) {
		mutex_unlock(&net->connection_lock);
		return;
	}

	netdev_dbg(net->dev, "login successful, enabling paths\n");

	ret = tb_xdomain_alloc_in_hopid(net->xd, remote_transmit_path);
	if (ret != remote_transmit_path) {
		netdev_err(net->dev, "failed to allocate Rx HopID\n");
		mutex_unlock(&net->connection_lock);
		return;
	}
	net->active_remote_transmit_path = remote_transmit_path;

	/* Both logins successful so enable the rings, high-speed DMA
	 * paths and start the network device queue.
	 *
	 * Note we enable the DMA paths last to make sure we have primed
	 * the Rx ring before any incoming packets are allowed to
	 * arrive.
	 */
	tb_ring_start(net->tx_ring.ring);
	tb_ring_start(net->rx_ring.ring);
	net->resources_owned = true;

	ret = tbnet_alloc_rx_buffers(net, net->rx_ring.size);
	if (ret)
		goto err_stop_rings;

	ret = tbnet_alloc_tx_buffers(net);
	if (ret)
		goto err_free_rx_buffers;

	ret = tb_xdomain_enable_paths(net->xd, net->local_transmit_path,
				      net->tx_ring.ring->hop,
				      remote_transmit_path,
				      net->rx_ring.ring->hop);
	if (ret) {
		netdev_err(net->dev, "failed to enable DMA paths\n");
		goto err_free_tx_buffers;
	}

	net->session_active = true;

	netif_carrier_on(net->dev);
	netif_start_queue(net->dev);
	mutex_unlock(&net->connection_lock);

	/* Level-triggered zombie detection while the session is established. */
	tbnet_queue_verify(net);

	netdev_dbg(net->dev, "network traffic started\n");
	return;

err_free_tx_buffers:
	tbnet_free_buffers(&net->tx_ring);
err_free_rx_buffers:
	tbnet_free_buffers(&net->rx_ring);
err_stop_rings:
	tb_ring_stop(net->rx_ring.ring);
	tb_ring_stop(net->tx_ring.ring);
	tb_xdomain_release_in_hopid(net->xd, remote_transmit_path);
	net->resources_owned = false;
	net->active_remote_transmit_path = 0;
	mutex_unlock(&net->connection_lock);
}

static void tbnet_login_work(struct work_struct *work)
{
	struct tbnet *net = container_of(work, typeof(*net), login_work.work);
	unsigned long delay = msecs_to_jiffies(TBNET_LOGIN_DELAY);
	int ret;

	if (netif_carrier_ok(net->dev))
		return;

	netdev_dbg(net->dev, "sending login request, retries=%u\n",
		   net->login_retries);

	ret = tbnet_login_request(net, net->login_retries % 4);
	if (ret) {
		netdev_dbg(net->dev, "sending login request failed, ret=%d\n",
			   ret);
		if (net->login_retries++ < TBNET_LOGIN_RETRIES) {
			queue_delayed_work(system_long_wq, &net->login_work,
					   delay);
		} else {
			netdev_info(net->dev, "ThunderboltIP login timed out\n");
		}
	} else {
		netdev_dbg(net->dev, "received login reply\n");

		net->login_retries = 0;

		mutex_lock(&net->connection_lock);
		net->login_hs.request_sent = true;
		mutex_unlock(&net->connection_lock);

		queue_work(system_long_wq, &net->connected_work);
	}
}

static void tbnet_disconnect_work(struct work_struct *work)
{
	struct tbnet *net = container_of(work, typeof(*net), disconnect_work);

	tbnet_tear_down(net, false);
}

static bool tbnet_check_frame(struct tbnet *net, const struct tbnet_frame *tf,
			      const struct thunderbolt_ip_frame_header *hdr)
{
	u32 frame_id, frame_count, frame_size, frame_index;
	unsigned int size;

	if (tf->frame.flags & RING_DESC_CRC_ERROR) {
		net->stats.rx_crc_errors++;
		return false;
	} else if (tf->frame.flags & RING_DESC_BUFFER_OVERRUN) {
		net->stats.rx_over_errors++;
		return false;
	}

	/* Should be greater than just header i.e. contains data */
	size = tbnet_frame_size(tf);
	if (size <= sizeof(*hdr)) {
		net->stats.rx_length_errors++;
		return false;
	}

	frame_count = le32_to_cpu(hdr->frame_count);
	frame_size = le32_to_cpu(hdr->frame_size);
	frame_index = le16_to_cpu(hdr->frame_index);
	frame_id = le16_to_cpu(hdr->frame_id);

	if ((frame_size > size - sizeof(*hdr)) || !frame_size) {
		net->stats.rx_length_errors++;
		return false;
	}

	/* In case we're in the middle of packet, validate the frame
	 * header based on first fragment of the packet.
	 */
	if (net->skb && net->rx_hdr.frame_count) {
		/* Check the frame count fits the count field */
		if (frame_count != le32_to_cpu(net->rx_hdr.frame_count)) {
			net->stats.rx_length_errors++;
			return false;
		}

		/* Check the frame identifiers are incremented correctly,
		 * and id is matching.
		 */
		if (frame_index != le16_to_cpu(net->rx_hdr.frame_index) + 1 ||
		    frame_id != le16_to_cpu(net->rx_hdr.frame_id)) {
			net->stats.rx_missed_errors++;
			return false;
		}

		if (net->skb->len + frame_size > TBNET_MAX_MTU) {
			net->stats.rx_length_errors++;
			return false;
		}

		return true;
	}

	/* Start of packet, validate the frame header. tbnet_poll() puts the
	 * first frame in the skb linear area and every further frame in a page
	 * fragment, so a packet may not span more than MAX_SKB_FRAGS + 1 frames
	 * without overflowing skb_shinfo()->frags[].
	 */
	if (frame_count == 0 || frame_count > MAX_SKB_FRAGS + 1) {
		net->stats.rx_length_errors++;
		return false;
	}
	if (frame_index != 0) {
		net->stats.rx_missed_errors++;
		return false;
	}

	return true;
}

static int tbnet_poll(struct napi_struct *napi, int budget)
{
	struct tbnet *net = container_of(napi, struct tbnet, napi);
	unsigned int cleaned_count = tbnet_available_buffers(&net->rx_ring);
	struct device *dma_dev = tb_ring_dma_device(net->rx_ring.ring);
	unsigned int rx_packets = 0;

	while (rx_packets < budget) {
		const struct thunderbolt_ip_frame_header *hdr;
		unsigned int hdr_size = sizeof(*hdr);
		struct sk_buff *skb = NULL;
		struct ring_frame *frame;
		struct tbnet_frame *tf;
		struct page *page;
		bool last = true;
		u32 frame_size;

		/* Return some buffers to hardware, one at a time is too
		 * slow so allocate MAX_SKB_FRAGS buffers at the same
		 * time.
		 */
		if (cleaned_count >= MAX_SKB_FRAGS) {
			tbnet_alloc_rx_buffers(net, cleaned_count);
			cleaned_count = 0;
		}

		frame = tb_ring_poll(net->rx_ring.ring);
		if (!frame)
			break;

		dma_unmap_page(dma_dev, frame->buffer_phy,
			       TBNET_RX_PAGE_SIZE, DMA_FROM_DEVICE);

		tf = container_of(frame, typeof(*tf), frame);

		page = tf->page;
		tf->page = NULL;
		net->rx_ring.cons++;
		cleaned_count++;

		hdr = page_address(page);
		if (!tbnet_check_frame(net, tf, hdr)) {
			trace_tbnet_invalid_rx_ip_frame(hdr->frame_size,
				hdr->frame_id, hdr->frame_index, hdr->frame_count);
			__free_pages(page, TBNET_RX_PAGE_ORDER);
			dev_kfree_skb_any(net->skb);
			net->skb = NULL;
			continue;
		}

		trace_tbnet_rx_ip_frame(hdr->frame_size, hdr->frame_id,
					hdr->frame_index, hdr->frame_count);
		frame_size = le32_to_cpu(hdr->frame_size);

		skb = net->skb;
		if (!skb) {
			skb = build_skb(page_address(page),
					TBNET_RX_PAGE_SIZE);
			if (!skb) {
				__free_pages(page, TBNET_RX_PAGE_ORDER);
				net->stats.rx_errors++;
				break;
			}

			skb_reserve(skb, hdr_size);
			skb_put(skb, frame_size);

			net->skb = skb;
		} else {
			skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
					page, hdr_size, frame_size,
					TBNET_RX_PAGE_SIZE - hdr_size);
		}

		net->rx_hdr.frame_size = hdr->frame_size;
		net->rx_hdr.frame_count = hdr->frame_count;
		net->rx_hdr.frame_index = hdr->frame_index;
		net->rx_hdr.frame_id = hdr->frame_id;
		last = le16_to_cpu(net->rx_hdr.frame_index) ==
		       le32_to_cpu(net->rx_hdr.frame_count) - 1;

		rx_packets++;
		net->stats.rx_bytes += frame_size;

		if (last) {
			skb->protocol = eth_type_trans(skb, net->dev);
			trace_tbnet_rx_skb(skb);
			napi_gro_receive(&net->napi, skb);
			net->skb = NULL;
		}
	}

	net->stats.rx_packets += rx_packets;

	if (cleaned_count)
		tbnet_alloc_rx_buffers(net, cleaned_count);

	if (rx_packets >= budget)
		return budget;

	napi_complete_done(napi, rx_packets);
	/* Re-enable the ring interrupt */
	tb_ring_poll_complete(net->rx_ring.ring);

	return rx_packets;
}

static void tbnet_start_poll(void *data)
{
	struct tbnet *net = data;

	napi_schedule(&net->napi);
}

static int tbnet_open(struct net_device *dev)
{
	struct tbnet *net = netdev_priv(dev);
	struct tb_xdomain *xd = net->xd;
	u16 sof_mask, eof_mask;
	struct tb_ring *ring;
	unsigned int flags;
	int hopid;
	int ret;

	netif_carrier_off(dev);
	net->tx_ring.size = net->tx_ring_size;
	net->rx_ring.size = net->rx_ring_size;
	net->tx_ring.frames = kcalloc(net->tx_ring.size,
				      sizeof(*net->tx_ring.frames), GFP_KERNEL);
	if (!net->tx_ring.frames)
		return -ENOMEM;

	net->rx_ring.frames = kcalloc(net->rx_ring.size,
				      sizeof(*net->rx_ring.frames), GFP_KERNEL);
	if (!net->rx_ring.frames) {
		ret = -ENOMEM;
		goto err_free_tx_frames;
	}

	flags = RING_FLAG_FRAME;
	/* Only enable full E2E if the other end supports it too */
	if (tbnet_e2e && net->svc->prtcstns & TBNET_E2E)
		flags |= RING_FLAG_E2E;

	ring = tb_ring_alloc_tx(xd->tb->nhi, -1, net->tx_ring.size, flags);
	if (!ring) {
		netdev_err(dev, "failed to allocate Tx ring\n");
		ret = -ENOMEM;
		goto err_free_rx_frames;
	}
	net->tx_ring.ring = ring;

	hopid = tb_xdomain_alloc_out_hopid(xd, -1);
	if (hopid < 0) {
		netdev_err(dev, "failed to allocate Tx HopID\n");
		tb_ring_free(net->tx_ring.ring);
		net->tx_ring.ring = NULL;
		ret = hopid;
		goto err_free_rx_frames;
	}
	net->local_transmit_path = hopid;

	sof_mask = BIT(TBIP_PDF_FRAME_START);
	eof_mask = BIT(TBIP_PDF_FRAME_END);

	ring = tb_ring_alloc_rx(xd->tb->nhi, -1, net->rx_ring.size, flags,
				net->tx_ring.ring->hop, sof_mask,
				eof_mask, tbnet_start_poll, net);
	if (!ring) {
		netdev_err(dev, "failed to allocate Rx ring\n");
		tb_xdomain_release_out_hopid(xd, hopid);
		tb_ring_free(net->tx_ring.ring);
		net->tx_ring.ring = NULL;
		ret = -ENOMEM;
		goto err_free_rx_frames;
	}
	net->rx_ring.ring = ring;

	ret = tb_ring_throttling(net->tx_ring.ring,
				 net->int_coalesce_usecs * NSEC_PER_USEC);
	if (ret)
		goto err_free_rx_ring;

	ret = tb_ring_throttling(net->rx_ring.ring,
				 net->int_coalesce_usecs * NSEC_PER_USEC);
	if (ret)
		goto err_free_rx_ring;

	napi_enable(&net->napi);
	start_login(net);

	return 0;

err_free_rx_ring:
	tb_ring_free(net->rx_ring.ring);
	net->rx_ring.ring = NULL;
	tb_xdomain_release_out_hopid(xd, hopid);
	tb_ring_free(net->tx_ring.ring);
	net->tx_ring.ring = NULL;
err_free_rx_frames:
	kfree(net->rx_ring.frames);
	net->rx_ring.frames = NULL;
	net->rx_ring.size = 0;
err_free_tx_frames:
	kfree(net->tx_ring.frames);
	net->tx_ring.frames = NULL;
	net->tx_ring.size = 0;
	return ret;
}

static int tbnet_stop(struct net_device *dev)
{
	struct tbnet *net = netdev_priv(dev);

	napi_disable(&net->napi);

	cancel_delayed_work_sync(&net->verify_work);
	cancel_work_sync(&net->disconnect_work);
	tbnet_tear_down(net, true);

	tb_ring_free(net->rx_ring.ring);
	net->rx_ring.ring = NULL;
	kfree(net->rx_ring.frames);
	net->rx_ring.frames = NULL;
	net->rx_ring.size = 0;

	tb_xdomain_release_out_hopid(net->xd, net->local_transmit_path);
	tb_ring_free(net->tx_ring.ring);
	net->tx_ring.ring = NULL;
	kfree(net->tx_ring.frames);
	net->tx_ring.frames = NULL;
	net->tx_ring.size = 0;

	return 0;
}

static bool tbnet_xmit_csum_and_map(struct tbnet *net, struct sk_buff *skb,
	struct tbnet_frame **frames, u32 frame_count)
{
	struct thunderbolt_ip_frame_header *hdr = page_address(frames[0]->page);
	struct device *dma_dev = tb_ring_dma_device(net->tx_ring.ring);
	unsigned int i, len, offset = skb_transport_offset(skb);
	/* Remove payload length from checksum */
	u32 paylen = skb->len - skb_transport_offset(skb);
	__wsum wsum = (__force __wsum)htonl(paylen);
	__be16 protocol = skb->protocol;
	void *data = skb->data;
	void *dest = hdr + 1;
	__sum16 *tucso;

	if (skb->ip_summed != CHECKSUM_PARTIAL) {
		/* No need to calculate checksum so we just update the
		 * total frame count and sync the frames for DMA.
		 */
		for (i = 0; i < frame_count; i++) {
			hdr = page_address(frames[i]->page);
			hdr->frame_count = cpu_to_le32(frame_count);
			trace_tbnet_tx_ip_frame(hdr->frame_size, hdr->frame_id,
						hdr->frame_index, hdr->frame_count);
			dma_sync_single_for_device(dma_dev,
				frames[i]->frame.buffer_phy,
				tbnet_frame_size(frames[i]), DMA_TO_DEVICE);
		}

		return true;
	}

	if (protocol == htons(ETH_P_8021Q)) {
		struct vlan_hdr *vhdr, vh;

		vhdr = skb_header_pointer(skb, ETH_HLEN, sizeof(vh), &vh);
		if (!vhdr)
			return false;

		protocol = vhdr->h_vlan_encapsulated_proto;
	}

	/* Data points on the beginning of packet.
	 * Check is the checksum absolute place in the packet.
	 * ipcso will update IP checksum.
	 * tucso will update TCP/UDP checksum.
	 */
	if (protocol == htons(ETH_P_IP)) {
		__sum16 *ipcso = dest + ((void *)&(ip_hdr(skb)->check) - data);

		*ipcso = 0;
		*ipcso = ip_fast_csum(dest + skb_network_offset(skb),
				      ip_hdr(skb)->ihl);

		if (ip_hdr(skb)->protocol == IPPROTO_TCP)
			tucso = dest + ((void *)&(tcp_hdr(skb)->check) - data);
		else if (ip_hdr(skb)->protocol == IPPROTO_UDP)
			tucso = dest + ((void *)&(udp_hdr(skb)->check) - data);
		else
			return false;

		*tucso = ~csum_tcpudp_magic(ip_hdr(skb)->saddr,
					    ip_hdr(skb)->daddr, 0,
					    ip_hdr(skb)->protocol, 0);
	} else if (skb_is_gso(skb) && skb_is_gso_v6(skb)) {
		tucso = dest + ((void *)&(tcp_hdr(skb)->check) - data);
		*tucso = ~csum_ipv6_magic(&ipv6_hdr(skb)->saddr,
					  &ipv6_hdr(skb)->daddr, 0,
					  IPPROTO_TCP, 0);
	} else if (protocol == htons(ETH_P_IPV6)) {
		tucso = dest + skb_checksum_start_offset(skb) + skb->csum_offset;
		*tucso = ~csum_ipv6_magic(&ipv6_hdr(skb)->saddr,
					  &ipv6_hdr(skb)->daddr, 0,
					  ipv6_hdr(skb)->nexthdr, 0);
	} else {
		return false;
	}

	/* First frame was headers, rest of the frames contain data.
	 * Calculate checksum over each frame.
	 */
	for (i = 0; i < frame_count; i++) {
		hdr = page_address(frames[i]->page);
		dest = (void *)(hdr + 1) + offset;
		len = le32_to_cpu(hdr->frame_size) - offset;
		wsum = csum_partial(dest, len, wsum);
		hdr->frame_count = cpu_to_le32(frame_count);
		trace_tbnet_tx_ip_frame(hdr->frame_size, hdr->frame_id,
					hdr->frame_index, hdr->frame_count);

		offset = 0;
	}

	*tucso = csum_fold(wsum);

	/* Checksum is finally calculated and we don't touch the memory
	 * anymore, so DMA sync the frames now.
	 */
	for (i = 0; i < frame_count; i++) {
		dma_sync_single_for_device(dma_dev, frames[i]->frame.buffer_phy,
			tbnet_frame_size(frames[i]), DMA_TO_DEVICE);
	}

	return true;
}

static void *tbnet_kmap_frag(struct sk_buff *skb, unsigned int frag_num,
			     unsigned int *len)
{
	const skb_frag_t *frag = &skb_shinfo(skb)->frags[frag_num];

	*len = skb_frag_size(frag);
	return kmap_local_page(skb_frag_page(frag)) + skb_frag_off(frag);
}

static netdev_tx_t tbnet_start_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	struct tbnet *net = netdev_priv(dev);
	struct tbnet_frame *frames[MAX_SKB_FRAGS];
	u16 frame_id = atomic_read(&net->frame_id);
	struct thunderbolt_ip_frame_header *hdr;
	unsigned int len = skb_headlen(skb);
	unsigned int data_len = skb->len;
	unsigned int nframes, i;
	unsigned int frag = 0;
	void *src = skb->data;
	u32 frame_index = 0;
	bool unmap = false;
	void *dest;

	trace_tbnet_tx_skb(skb);

	nframes = DIV_ROUND_UP(data_len, TBNET_MAX_PAYLOAD_SIZE);
	if (tbnet_available_buffers(&net->tx_ring) < nframes) {
		netif_stop_queue(net->dev);
		return NETDEV_TX_BUSY;
	}

	frames[frame_index] = tbnet_get_tx_buffer(net);
	if (!frames[frame_index])
		goto err_drop;

	hdr = page_address(frames[frame_index]->page);
	dest = hdr + 1;

	/* If overall packet is bigger than the frame data size */
	while (data_len > TBNET_MAX_PAYLOAD_SIZE) {
		unsigned int size_left = TBNET_MAX_PAYLOAD_SIZE;

		hdr->frame_size = cpu_to_le32(TBNET_MAX_PAYLOAD_SIZE);
		hdr->frame_index = cpu_to_le16(frame_index);
		hdr->frame_id = cpu_to_le16(frame_id);

		do {
			if (len > size_left) {
				/* Copy data onto Tx buffer data with
				 * full frame size then break and go to
				 * next frame
				 */
				memcpy(dest, src, size_left);
				len -= size_left;
				dest += size_left;
				src += size_left;
				break;
			}

			memcpy(dest, src, len);
			size_left -= len;
			dest += len;

			if (unmap) {
				kunmap_local(src);
				unmap = false;
			}

			/* Ensure all fragments have been processed */
			if (frag < skb_shinfo(skb)->nr_frags) {
				/* Map and then unmap quickly */
				src = tbnet_kmap_frag(skb, frag++, &len);
				unmap = true;
			} else if (unlikely(size_left > 0)) {
				goto err_drop;
			}
		} while (size_left > 0);

		data_len -= TBNET_MAX_PAYLOAD_SIZE;
		frame_index++;

		frames[frame_index] = tbnet_get_tx_buffer(net);
		if (!frames[frame_index])
			goto err_drop;

		hdr = page_address(frames[frame_index]->page);
		dest = hdr + 1;
	}

	hdr->frame_size = cpu_to_le32(data_len);
	hdr->frame_index = cpu_to_le16(frame_index);
	hdr->frame_id = cpu_to_le16(frame_id);

	frames[frame_index]->frame.size = data_len + sizeof(*hdr);

	/* In case the remaining data_len is smaller than a frame */
	while (len < data_len) {
		memcpy(dest, src, len);
		data_len -= len;
		dest += len;

		if (unmap) {
			kunmap_local(src);
			unmap = false;
		}

		if (frag < skb_shinfo(skb)->nr_frags) {
			src = tbnet_kmap_frag(skb, frag++, &len);
			unmap = true;
		} else if (unlikely(data_len > 0)) {
			goto err_drop;
		}
	}

	memcpy(dest, src, data_len);

	if (unmap)
		kunmap_local(src);

	if (!tbnet_xmit_csum_and_map(net, skb, frames, frame_index + 1))
		goto err_drop;

	for (i = 0; i < frame_index + 1; i++)
		tb_ring_tx(net->tx_ring.ring, &frames[i]->frame);

	if (net->svc->prtcstns & TBNET_MATCH_FRAGS_ID)
		atomic_inc(&net->frame_id);

	net->stats.tx_packets++;
	net->stats.tx_bytes += skb->len;

	trace_tbnet_consume_skb(skb);
	dev_consume_skb_any(skb);

	return NETDEV_TX_OK;

err_drop:
	/* We can re-use the buffers */
	net->tx_ring.cons -= frame_index;

	dev_kfree_skb_any(skb);
	net->stats.tx_errors++;

	return NETDEV_TX_OK;
}

static void tbnet_get_stats64(struct net_device *dev,
			      struct rtnl_link_stats64 *stats)
{
	struct tbnet *net = netdev_priv(dev);

	stats->tx_packets = net->stats.tx_packets;
	stats->rx_packets = net->stats.rx_packets;
	stats->tx_bytes = net->stats.tx_bytes;
	stats->rx_bytes = net->stats.rx_bytes;
	stats->rx_errors = net->stats.rx_errors + net->stats.rx_length_errors +
		net->stats.rx_over_errors + net->stats.rx_crc_errors +
		net->stats.rx_missed_errors;
	stats->tx_errors = net->stats.tx_errors;
	stats->rx_length_errors = net->stats.rx_length_errors;
	stats->rx_over_errors = net->stats.rx_over_errors;
	stats->rx_crc_errors = net->stats.rx_crc_errors;
	stats->rx_missed_errors = net->stats.rx_missed_errors;
}

static const struct net_device_ops tbnet_netdev_ops = {
	.ndo_open = tbnet_open,
	.ndo_stop = tbnet_stop,
	.ndo_start_xmit = tbnet_start_xmit,
	.ndo_get_stats64 = tbnet_get_stats64,
};

static int tbnet_get_coalesce(struct net_device *dev,
			      struct ethtool_coalesce *ec,
			      struct kernel_ethtool_coalesce *kernel_ec,
			      struct netlink_ext_ack *extack)
{
	const struct tbnet *net = netdev_priv(dev);

	ec->rx_coalesce_usecs = net->int_coalesce_usecs;
	ec->tx_coalesce_usecs = net->int_coalesce_usecs;

	return 0;
}

static int tbnet_set_coalesce(struct net_device *dev,
			      struct ethtool_coalesce *ec,
			      struct kernel_ethtool_coalesce *kernel_ec,
			      struct netlink_ext_ack *extack)
{
	struct tbnet *net = netdev_priv(dev);

	if (ec->rx_coalesce_usecs != ec->tx_coalesce_usecs)
		return -EINVAL;

	if (netif_running(dev)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "interrupt coalescing can be changed only while the interface is down");
		return -EBUSY;
	}

	net->int_coalesce_usecs = ec->rx_coalesce_usecs;
	return 0;
}

static void tbnet_get_ringparam(struct net_device *dev,
				struct ethtool_ringparam *ering,
				struct kernel_ethtool_ringparam *kernel_ering,
				struct netlink_ext_ack *extack)
{
	struct tbnet *net = netdev_priv(dev);

	ering->rx_max_pending = TBNET_MAX_RING_SIZE;
	ering->tx_max_pending = TBNET_MAX_RING_SIZE;
	ering->rx_pending = net->rx_ring_size;
	ering->tx_pending = net->tx_ring_size;
}

static int tbnet_validate_ring_size(u32 size, struct netlink_ext_ack *extack)
{
	if (size < TBNET_MIN_RING_SIZE || size > TBNET_MAX_RING_SIZE) {
		NL_SET_ERR_MSG_MOD(extack, "ring size is outside supported range");
		return -EINVAL;
	}

	if (!is_power_of_2(size)) {
		NL_SET_ERR_MSG_MOD(extack, "ring size must be a power of two");
		return -EINVAL;
	}

	return 0;
}

static int tbnet_set_ringparam(struct net_device *dev,
			       struct ethtool_ringparam *ering,
			       struct kernel_ethtool_ringparam *kernel_ering,
			       struct netlink_ext_ack *extack)
{
	struct tbnet *net = netdev_priv(dev);
	int ret;

	if (ering->rx_mini_pending || ering->rx_jumbo_pending) {
		NL_SET_ERR_MSG_MOD(extack, "mini and jumbo rings are not supported");
		return -EINVAL;
	}

	if (netif_running(dev)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "ring size can be changed only while the interface is down");
		return -EBUSY;
	}

	ret = tbnet_validate_ring_size(ering->rx_pending, extack);
	if (ret)
		return ret;

	ret = tbnet_validate_ring_size(ering->tx_pending, extack);
	if (ret)
		return ret;

	net->rx_ring_size = ering->rx_pending;
	net->tx_ring_size = ering->tx_pending;
	return 0;
}

static const struct ethtool_ops tbnet_ethtool_ops = {
	.supported_coalesce_params = ETHTOOL_COALESCE_RX_USECS |
				     ETHTOOL_COALESCE_TX_USECS,
	.get_coalesce = tbnet_get_coalesce,
	.set_coalesce = tbnet_set_coalesce,
	.get_ringparam = tbnet_get_ringparam,
	.set_ringparam = tbnet_set_ringparam,
};

static void tbnet_generate_mac(struct net_device *dev)
{
	const struct tbnet *net = netdev_priv(dev);
	const struct tb_xdomain *xd = net->xd;
	u8 addr[ETH_ALEN];
	u8 phy_port;
	u32 hash;

	phy_port = tb_phy_port_from_link(TBNET_L0_PORT_NUM(xd->route));

	/* Unicast and locally administered MAC */
	addr[0] = phy_port << 4 | 0x02;
	hash = jhash2((u32 *)xd->local_uuid, 4, 0);
	memcpy(addr + 1, &hash, sizeof(hash));
	hash = jhash2((u32 *)xd->local_uuid, 4, hash);
	addr[5] = hash & 0xff;
	eth_hw_addr_set(dev, addr);
}

static int tbnet_probe(struct tb_service *svc, const struct tb_service_id *id)
{
	struct tb_xdomain *xd = tb_service_parent(svc);
	struct net_device *dev;
	struct tbnet *net;
	int ret;

	dev = alloc_etherdev(sizeof(*net));
	if (!dev)
		return -ENOMEM;

	SET_NETDEV_DEV(dev, &svc->dev);

	net = netdev_priv(dev);
	INIT_DELAYED_WORK(&net->login_work, tbnet_login_work);
	INIT_WORK(&net->connected_work, tbnet_connected_work);
	INIT_DELAYED_WORK(&net->verify_work, tbnet_verify_work);
	INIT_WORK(&net->disconnect_work, tbnet_disconnect_work);
	mutex_init(&net->connection_lock);
	atomic_set(&net->command_id, 0);
	atomic_set(&net->frame_id, 0);
	net->svc = svc;
	net->dev = dev;
	net->xd = xd;

	tbnet_generate_mac(dev);

	strcpy(dev->name, "thunderbolt%d");
	dev->netdev_ops = &tbnet_netdev_ops;
	dev->ethtool_ops = &tbnet_ethtool_ops;

	/* ThunderboltIP takes advantage of TSO packets but instead of
	 * segmenting them we just split the packet into Thunderbolt
	 * frames (maximum payload size of each frame is 4084 bytes) and
	 * calculate checksum over the whole packet here.
	 *
	 * The receiving side does the opposite if the host OS supports
	 * LRO, otherwise it needs to split the large packet into MTU
	 * sized smaller packets.
	 *
	 * In order to receive large packets from the networking stack,
	 * we need to announce support for most of the offloading
	 * features here.
	 */
	dev->hw_features = NETIF_F_SG | NETIF_F_ALL_TSO | NETIF_F_GRO |
			   NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
	dev->features = dev->hw_features | NETIF_F_HIGHDMA;
	dev->hard_header_len += sizeof(struct thunderbolt_ip_frame_header);

	netif_napi_add_weight(dev, &net->napi, tbnet_poll, 256);

	/* MTU range: 68 - 65522 */
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = TBNET_MAX_MTU - ETH_HLEN;

	net->handler.uuid = &tbnet_svc_uuid;
	net->handler.callback = tbnet_handle_packet;
	net->handler.data = net;
	net->rx_ring_size = TBNET_DEFAULT_RING_SIZE;
	net->tx_ring_size = TBNET_DEFAULT_RING_SIZE;
	net->int_coalesce_usecs = TBNET_DEFAULT_INT_COALESCE_USECS;
	if (!tb_register_protocol_handler(&net->handler))
		net->handler_registered = true;

	tb_service_set_drvdata(svc, net);

	ret = register_netdev(dev);
	if (ret) {
		tbnet_fence_handler(net);
		free_netdev(dev);
		return ret;
	}

	return 0;
}

/*
 * Cancel every work item this instance can own. Only meaningful once the
 * protocol handler is gone: tbnet_handle_packet() queues login_work,
 * connected_work and disconnect_work from the XDomain dispatch context, so a
 * cancel that races a still-registered handler proves nothing.
 */
/*
 * Fence inbound XDomain dispatch for this instance. Idempotent: shutdown runs
 * before unbind on a driver unload and both need the fence.
 *
 * tb_unregister_protocol_handler() takes the core's xdomain_dispatch_lock,
 * which tb_xdomain_handle_request() holds across the handler callbacks, so on
 * return tbnet_handle_packet() is neither running nor startable. That is the
 * only thing that makes a subsequent work cancel meaningful.
 */
static void tbnet_fence_handler(struct tbnet *net)
{
	if (!net->handler_registered)
		return;
	net->handler_registered = false;
	tb_unregister_protocol_handler(&net->handler);
}

static void tbnet_cancel_all_work(struct tbnet *net)
{
	cancel_delayed_work_sync(&net->login_work);
	cancel_work_sync(&net->connected_work);
	cancel_delayed_work_sync(&net->verify_work);
	cancel_work_sync(&net->disconnect_work);
}

/*
 * Unbind order is NOT the mirror of probe, deliberately -- the same rule
 * thunderbolt_frame's service.c documents and for the same reason.
 *
 * The protocol handler goes FIRST. tb_unregister_protocol_handler() takes the
 * core's xdomain_dispatch_lock, which tb_xdomain_handle_request() holds across
 * the handler callbacks (drivers/thunderbolt/xdomain.c), so its return is a
 * hard guarantee that tbnet_handle_packet() is not running and can never run
 * again. Everything after it therefore runs with inbound XDomain requests
 * structurally excluded.
 *
 * The old order (unregister_netdev() first) left the handler live across the
 * whole of ndo_stop. That path is long: tbnet_stop() -> tbnet_tear_down(true)
 * sends up to TBNET_LOGOUT_RETRIES (10) logout requests at TBNET_LOGOUT_TIMEOUT
 * (1000 ms) each, all under RTNL. tbnet_stop() cancels verify_work and
 * disconnect_work at the START of that window; an inbound LOGIN or LOGOUT
 * arriving any time afterwards re-queued login_work / connected_work /
 * disconnect_work on system_long_wq, and nothing cancelled them again before
 * free_netdev() freed the netdev that netdev_priv() hands those handlers.
 * tbnet_connected_work() then calls tb_ring_start(net->tx_ring.ring) and
 * tbnet_disconnect_work() -> tbnet_tear_down() calls tb_ring_stop() on freed
 * struct tbnet state, i.e. NHI MMIO writes derived from freed ring pointers --
 * a use-after-free that presents as a hard, unrecoverable machine hang rather
 * than a clean oops, and only when a peer happens to be mid-handshake during
 * the unload. That is the appmana-019 2026-08-24 signature (hang on
 * `modprobe -r thunderbolt_net` with both chain links live, while the same
 * sequence on appmana-021 completed cleanly).
 *
 * Unregistering the handler first is safe for our own in-flight requests: an
 * XDomain RESPONSE is matched by tb_cfg_request_find() in ctl.c, which runs
 * only when no protocol handler CONSUMED the packet (tb_ctl_rx_callback() ->
 * tb_ctl_handle_event() -> tb_domain_event_cb() -> tb_xdomain_handle_request()).
 * With no handler registered the walk consumes nothing, so the logout response
 * still completes tbnet_logout_request(). The only cost is that a peer
 * mid-handshake sees one retry interval of unanswered requests, which its own
 * retry budget already absorbs.
 */
static void tbnet_remove(struct tb_service *svc)
{
	struct tbnet *net = tb_service_get_drvdata(svc);

	tbnet_fence_handler(net);

	unregister_netdev(net->dev);

	/*
	 * Hard fence before the netdev (and with it struct tbnet) is freed.
	 * unregister_netdev() only runs ndo_stop for an interface that was UP,
	 * and even then tbnet_stop() cancels just two of the four work items;
	 * with the handler gone nothing can re-queue, so this sweep is final.
	 */
	tbnet_cancel_all_work(net);

	free_netdev(net->dev);
}

/*
 * System shutdown / reboot (device_shutdown()), NOT a driver unbind.
 *
 * Refusing is not an option here -- the machine is going down either way -- so
 * the rule is bounded-and-proceed: every peer wait must have a deadline and a
 * defined failure action, and nothing may be left able to re-arm the data path
 * behind us.
 *
 * The fence is the whole point. The old shutdown left the protocol handler
 * registered across tbnet_tear_down(), and a peer that is HELLO-ing/LOGIN-ing
 * at us (the appmana-008 2026-08-24 configuration: peer had been powered off
 * at boot, came back, and was retrying the login endlessly) re-queues
 * connected_work on system_long_wq as fast as tear_down's stop_login() cancels
 * it. tbnet_connected_work() then re-runs tb_ring_start() and
 * tb_xdomain_enable_paths(), re-arming DMA into rings that the shutdown
 * sequence is about to take away underneath nhi_shutdown() -- which then walks
 * every hop and dev_WARNs "ring is still active" before disabling interrupts.
 * Fencing first makes the tear-down monotonic: nothing can put the data path
 * back after it has been taken down.
 */
static void tbnet_shutdown(struct tb_service *svc)
{
	struct tbnet *net = tb_service_get_drvdata(svc);

	if (!net)
		return;

	tbnet_fence_handler(net);
	tbnet_tear_down(net, true);
	tbnet_cancel_all_work(net);
}

static int tbnet_suspend(struct device *dev)
{
	struct tb_service *svc = tb_to_service(dev);
	struct tbnet *net = tb_service_get_drvdata(svc);

	/*
	 * Fence first, same rule as shutdown/unbind: an inbound LOGIN during
	 * the tear-down re-queues connected_work, which would re-enable the
	 * paths we just disabled and leave a live DMA tunnel across a suspend.
	 */
	tbnet_fence_handler(net);

	stop_login(net);
	cancel_delayed_work_sync(&net->verify_work);
	if (netif_running(net->dev)) {
		netif_device_detach(net->dev);
		tbnet_tear_down(net, true);
	}
	tbnet_cancel_all_work(net);

	return 0;
}

static int tbnet_resume(struct device *dev)
{
	struct tb_service *svc = tb_to_service(dev);
	struct tbnet *net = tb_service_get_drvdata(svc);

	if (!tb_register_protocol_handler(&net->handler))
		net->handler_registered = true;

	netif_carrier_off(net->dev);
	if (netif_running(net->dev)) {
		netif_device_attach(net->dev);
		start_login(net);
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(tbnet_pm_ops, tbnet_suspend, tbnet_resume);

static const struct tb_service_id tbnet_ids[] = {
	{ TB_SERVICE("network", 1) },
	{ },
};
MODULE_DEVICE_TABLE(tbsvc, tbnet_ids);

static struct tb_service_driver tbnet_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "thunderbolt-net",
		.pm = pm_sleep_ptr(&tbnet_pm_ops),
	},
	.probe = tbnet_probe,
	.remove = tbnet_remove,
	.shutdown = tbnet_shutdown,
	.id_table = tbnet_ids,
};

static int __init tbnet_init(void)
{
	unsigned int flags;
	int ret;

	tbnet_dir = tb_property_create_dir(&tbnet_dir_uuid);
	if (!tbnet_dir)
		return -ENOMEM;

	tb_property_add_immediate(tbnet_dir, "prtcid", 1);
	tb_property_add_immediate(tbnet_dir, "prtcvers", 1);
	tb_property_add_immediate(tbnet_dir, "prtcrevs", 1);

	flags = TBNET_MATCH_FRAGS_ID | TBNET_64K_FRAMES;
	if (tbnet_e2e)
		flags |= TBNET_E2E;
	tb_property_add_immediate(tbnet_dir, "prtcstns", flags);

	ret = tb_register_property_dir("network", tbnet_dir);
	if (ret)
		goto err_free_dir;

	ret = tb_register_service_driver(&tbnet_driver);
	if (ret)
		goto err_unregister;

	return 0;

err_unregister:
	tb_unregister_property_dir("network", tbnet_dir);
err_free_dir:
	tb_property_free_dir(tbnet_dir);

	return ret;
}
module_init(tbnet_init);

static void __exit tbnet_exit(void)
{
	tb_unregister_service_driver(&tbnet_driver);
	tb_unregister_property_dir("network", tbnet_dir);
	tb_property_free_dir(tbnet_dir);
}
module_exit(tbnet_exit);

MODULE_AUTHOR("Amir Levy <amir.jer.levy@intel.com>");
MODULE_AUTHOR("Michael Jamet <michael.jamet@intel.com>");
MODULE_AUTHOR("Mika Westerberg <mika.westerberg@linux.intel.com>");
MODULE_DESCRIPTION("Thunderbolt/USB4 network driver");
MODULE_LICENSE("GPL v2");
#ifdef TBV_PKG_VERSION
MODULE_VERSION(TBV_PKG_VERSION);
#endif
