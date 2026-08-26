/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Thunderbolt driver - control channel and configuration commands
 *
 * Copyright (c) 2014 Andreas Noever <andreas.noever@gmail.com>
 * Copyright (C) 2018, Intel Corporation
 */

#ifndef _TB_CFG
#define _TB_CFG

#include <linux/kref.h>
#include <linux/thunderbolt.h>

#include "nhi.h"
#include "tb_msgs.h"

/* control channel */
struct tb_ctl;

typedef bool (*event_cb)(void *data, enum tb_cfg_pkg_type type,
			 const void *buf, size_t size);

struct tb_ctl *tb_ctl_alloc(struct tb_nhi *nhi, int index, int timeout_msec,
			    event_cb cb, void *cb_data);
void tb_ctl_start(struct tb_ctl *ctl);
void tb_ctl_stop(struct tb_ctl *ctl);
void tb_ctl_free(struct tb_ctl *ctl);

/* configuration commands */

struct tb_cfg_result {
	u64 response_route;
	u32 response_port; /*
			    * If err = 1 then this is the port that send the
			    * error.
			    * If err = 0 and if this was a cfg_read/write then
			    * this is the upstream port of the responding
			    * switch.
			    * Otherwise the field is set to zero.
			    */
	int err; /* negative errors, 0 for success, 1 for tb errors */
	enum tb_cfg_error tb_error; /* valid if err == 1 */
};

struct ctl_pkg {
	struct tb_ctl *ctl;
	void *buffer;
	struct ring_frame frame;
};

enum tb_cfg_local_state {
	TB_CFG_LOCAL_DISABLED,
	TB_CFG_LOCAL_WAITING,
	TB_CFG_LOCAL_ACCEPTED,
	TB_CFG_LOCAL_FAILED,
	TB_CFG_LOCAL_TIMED_OUT,
};

enum tb_cfg_peer_state {
	TB_CFG_PEER_DISABLED,
	TB_CFG_PEER_WAITING,
	TB_CFG_PEER_MATCHED,
	TB_CFG_PEER_CANCELED,
	TB_CFG_PEER_TIMED_OUT,
};

enum tb_cfg_request_event {
	TB_CFG_REQUEST_EVENT_NONE,
	TB_CFG_REQUEST_EVENT_LOCAL_ACCEPTED,
	TB_CFG_REQUEST_EVENT_LOCAL_FAILED,
	TB_CFG_REQUEST_EVENT_LOCAL_TIMED_OUT,
	TB_CFG_REQUEST_EVENT_PEER_MATCHED,
	TB_CFG_REQUEST_EVENT_PEER_TIMED_OUT,
	TB_CFG_REQUEST_EVENT_CANCELED,
};

enum tb_cfg_request_action {
	TB_CFG_REQUEST_ACTION_NONE,
	TB_CFG_REQUEST_ACTION_COMPLETE,
	TB_CFG_REQUEST_ACTION_FAIL,
};

struct tb_cfg_request_state {
	enum tb_cfg_local_state local;
	enum tb_cfg_peer_state peer;
};

static inline enum tb_cfg_request_action
tb_cfg_request_state_step(struct tb_cfg_request_state *state,
			  enum tb_cfg_request_event event)
{
	if (state->local == TB_CFG_LOCAL_DISABLED ||
	    state->peer == TB_CFG_PEER_DISABLED)
		return TB_CFG_REQUEST_ACTION_NONE;

	switch (event) {
	case TB_CFG_REQUEST_EVENT_LOCAL_ACCEPTED:
		if (state->local == TB_CFG_LOCAL_WAITING)
			state->local = TB_CFG_LOCAL_ACCEPTED;
		return TB_CFG_REQUEST_ACTION_NONE;

	case TB_CFG_REQUEST_EVENT_LOCAL_FAILED:
		if (state->local != TB_CFG_LOCAL_WAITING)
			return TB_CFG_REQUEST_ACTION_NONE;
		state->local = TB_CFG_LOCAL_FAILED;
		if (state->peer == TB_CFG_PEER_WAITING) {
			state->peer = TB_CFG_PEER_CANCELED;
			return TB_CFG_REQUEST_ACTION_FAIL;
		}
		return TB_CFG_REQUEST_ACTION_NONE;

	case TB_CFG_REQUEST_EVENT_LOCAL_TIMED_OUT:
		if (state->local == TB_CFG_LOCAL_WAITING)
			state->local = TB_CFG_LOCAL_TIMED_OUT;
		return TB_CFG_REQUEST_ACTION_NONE;

	case TB_CFG_REQUEST_EVENT_PEER_MATCHED:
		if (state->peer != TB_CFG_PEER_WAITING)
			return TB_CFG_REQUEST_ACTION_NONE;
		state->peer = TB_CFG_PEER_MATCHED;
		return TB_CFG_REQUEST_ACTION_COMPLETE;

	case TB_CFG_REQUEST_EVENT_PEER_TIMED_OUT:
		if (state->peer != TB_CFG_PEER_WAITING)
			return TB_CFG_REQUEST_ACTION_NONE;
		state->peer = TB_CFG_PEER_TIMED_OUT;
		return TB_CFG_REQUEST_ACTION_FAIL;

	case TB_CFG_REQUEST_EVENT_CANCELED:
		if (state->peer != TB_CFG_PEER_WAITING)
			return TB_CFG_REQUEST_ACTION_NONE;
		state->peer = TB_CFG_PEER_CANCELED;
		return TB_CFG_REQUEST_ACTION_FAIL;

	case TB_CFG_REQUEST_EVENT_NONE:
	default:
		return TB_CFG_REQUEST_ACTION_NONE;
	}
}

/* An unsequenced local completion channel can have only one owner. */
static inline bool tb_cfg_local_slot_may_claim(bool occupied)
{
	return !occupied;
}

static inline bool tb_cfg_local_slot_is_owned(enum tb_cfg_local_state state)
{
	return state == TB_CFG_LOCAL_WAITING;
}

/**
 * struct tb_cfg_request - Control channel request
 * @kref: Reference count
 * @ctl: Pointer to the control channel structure. Only set when the
 *	 request is queued.
 * @request_size: Size of the request packet (in bytes)
 * @request_type: Type of the request packet
 * @response: Response is stored here
 * @response_size: Maximum size of one response packet
 * @response_type: Expected type of the response packet
 * @npackets: Number of packets expected to be returned with this request
 * @match: Function used to match the incoming packet
 * @intermediate: Function used to classify a non-terminal packet
 * @copy: Function used to copy the incoming packet to @response
 * @callback: Callback called when the request is finished successfully
 * @callback_data: Data to be passed to @callback
 * @flags: Flags for the request
 * @state_lock: Serializes the two request state machines
 * @state: Independent local-submission and peer-protocol states
 * @work: Work item used to complete the request
 * @result: Result after the request has been completed
 * @list: Requests are queued using this field
 *
 * An arbitrary request over Thunderbolt control channel. For standard
 * control channel message, one should use tb_cfg_read/write() and
 * friends if possible.
 */
struct tb_cfg_request {
	struct kref kref;
	struct tb_ctl *ctl;
	const void *request;
	size_t request_size;
	enum tb_cfg_pkg_type request_type;
	void *response;
	size_t response_size;
	enum tb_cfg_pkg_type response_type;
	size_t npackets;
	bool (*match)(const struct tb_cfg_request *req,
		      const struct ctl_pkg *pkg);
	enum tb_cfg_request_event
		(*intermediate)(const struct tb_cfg_request *req,
				const struct ctl_pkg *pkg);
	bool (*copy)(struct tb_cfg_request *req, const struct ctl_pkg *pkg);
	void (*callback)(void *callback_data);
	void *callback_data;
	unsigned long flags;
	/* Protects state transitions. */
	spinlock_t state_lock;
	struct tb_cfg_request_state state;
	struct work_struct work;
	struct tb_cfg_result result;
	struct list_head list;
};

#define TB_CFG_REQUEST_ACTIVE		0
#define TB_CFG_REQUEST_CANCELED		1
#define TB_CFG_REQUEST_HOLD_LOCAL	2
#define TB_CFG_REQUEST_LOCAL_SLOT	3

struct tb_cfg_request *tb_cfg_request_alloc(void);
void tb_cfg_request_get(struct tb_cfg_request *req);
void tb_cfg_request_put(struct tb_cfg_request *req);
int tb_cfg_request(struct tb_ctl *ctl, struct tb_cfg_request *req,
		   void (*callback)(void *), void *callback_data);
void tb_cfg_request_cancel(struct tb_cfg_request *req, int err);
struct tb_cfg_result tb_cfg_request_sync(struct tb_ctl *ctl,
			struct tb_cfg_request *req, int timeout_msec);

static inline u64 tb_cfg_get_route(const struct tb_cfg_header *header)
{
	return (u64) header->route_hi << 32 | header->route_lo;
}

static inline struct tb_cfg_header tb_cfg_make_header(u64 route)
{
	struct tb_cfg_header header = {
		.route_hi = route >> 32,
		.route_lo = route,
	};
	/* check for overflow, route_hi is not 32 bits! */
	WARN_ON(tb_cfg_get_route(&header) != route);
	return header;
}

int tb_cfg_ack_notification(struct tb_ctl *ctl, u64 route,
			    const struct cfg_error_pkg *error);
int tb_cfg_ack_plug(struct tb_ctl *ctl, u64 route, u32 port, bool unplug);
struct tb_cfg_result tb_cfg_reset(struct tb_ctl *ctl, u64 route);
struct tb_cfg_result tb_cfg_read_raw(struct tb_ctl *ctl, void *buffer,
				     u64 route, u32 port,
				     enum tb_cfg_space space, u32 offset,
				     u32 length, int timeout_msec);
struct tb_cfg_result tb_cfg_write_raw(struct tb_ctl *ctl, const void *buffer,
				      u64 route, u32 port,
				      enum tb_cfg_space space, u32 offset,
				      u32 length, int timeout_msec);
int tb_cfg_read(struct tb_ctl *ctl, void *buffer, u64 route, u32 port,
		enum tb_cfg_space space, u32 offset, u32 length);
int tb_cfg_write(struct tb_ctl *ctl, const void *buffer, u64 route, u32 port,
		 enum tb_cfg_space space, u32 offset, u32 length);
int tb_cfg_get_upstream_port(struct tb_ctl *ctl, u64 route);

/*
 * Consecutive unanswered control requests after which the channel is treated
 * as dead by tb_ctl_is_responsive(). Three, because a single timeout is
 * routine under contention and two can happen across a link retrain, but a
 * controller that has ignored three requests in a row with no matched reply
 * in between is not coming back inside a teardown.
 */
#define TB_CTL_DEAD_TIMEOUTS 3

/* Ring 0 events that move the control-channel liveness counter. */
enum tb_ctl_ring_event {
	TB_CTL_EVENT_TIMEOUT,
	TB_CTL_EVENT_REPLY_MATCHED,
	TB_CTL_EVENT_REPLY_UNMATCHED,
};

static inline bool tb_ctl_is_xdomain_tx_status(enum tb_cfg_pkg_type type,
					       const void *buf, size_t size)
{
	const struct icm_pkg_header *hdr = buf;

	if (type != TB_CFG_PKG_ICM_RESP ||
	    size != sizeof(struct icm_tr_pkg_xdomain_packet_response))
		return false;

	return hdr->code == ICM_XDOMAIN_PACKET && hdr->packet_id == 0 &&
	       hdr->total_packets == 1;
}

/**
 * tb_ctl_liveness_next() - Liveness counter transition
 * @consec_timeouts: Current run of unanswered requests
 * @ev: What ring 0 just did
 *
 * The whole liveness policy, kept pure so KUnit drives the same code the
 * driver runs instead of a re-implementation of it.
 *
 * Only a MATCHED reply clears the run. An UNMATCHED reply deliberately does
 * not: replies arriving that pair with nothing means ring 0 moves packets
 * while request/response matching is broken, which is precisely the degraded
 * state that must still gate teardown I/O. XDomain transmit-status packets are
 * classified separately because they acknowledge only local acceptance, not
 * an end-to-end peer response.
 *
 * Return: the new consecutive-timeout count.
 */
static inline int tb_ctl_liveness_next(int consec_timeouts,
				       enum tb_ctl_ring_event ev)
{
	switch (ev) {
	case TB_CTL_EVENT_TIMEOUT:
		return consec_timeouts + 1;
	case TB_CTL_EVENT_REPLY_MATCHED:
		return 0;
	case TB_CTL_EVENT_REPLY_UNMATCHED:
	default:
		return consec_timeouts;
	}
}

/**
 * tb_ctl_timeouts_indicate_dead() - Liveness verdict for a timeout run
 * @consec_timeouts: Run of unanswered requests, from tb_ctl_liveness_next()
 *
 * Return: %true when config space I/O should be skipped.
 */
static inline bool tb_ctl_timeouts_indicate_dead(int consec_timeouts)
{
	return consec_timeouts >= TB_CTL_DEAD_TIMEOUTS;
}

bool tb_ctl_is_responsive(struct tb_ctl *ctl);

#endif
