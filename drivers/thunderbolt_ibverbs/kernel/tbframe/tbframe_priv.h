/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tbframe internals shared between core, the hardware backend and KUnit.
 *
 * Every hardware touch goes through struct tbframe_hw_ops so the session,
 * admission and teardown machinery is testable against a mock ring layer;
 * hw.c provides the only production implementation (tb_ring/tb_xdomain).
 */
#ifndef TBFRAME_PRIV_H
#define TBFRAME_PRIV_H

#include <linux/completion.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/thunderbolt.h>
#include <linux/workqueue.h>

#include "../../proto/thunderbolt_negotiation.h"
#include "tbframe.h"
#include "tbframe_wire.h"

/* RX PDF masks (spec §2): frames open on the SOF marker and close on a
 * frame-type nibble. The masks MUST be disjoint (see tbframe.h).
 */
#define TBFRAME_SOF_MASK	BIT(TBFRAME_PDF_SOF)
#define TBFRAME_EOF_MASK	(BIT(TBFRAME_PDF_DATA) | BIT(TBFRAME_PDF_KEEPALIVE))

#define TBFRAME_HELLO_TIMEOUT_MS	1000
#define TBFRAME_RETRY_DELAY_MS		200
#define TBFRAME_HELLO_RETRIES		5
#define TBFRAME_READY_RETRIES		10
#define TBFRAME_KEEPALIVE_LEN		8
#define TBFRAME_BYE_TIMEOUT_MS		1000
#define TBFRAME_BYE_RETRIES		4
/* LOGOUT re-handshake settle: outlast the peer's whole BYE budget. */
#define TBFRAME_BYE_SETTLE_MS		5000
/* Min spacing between property re-announces (control-channel thrash guard). */
#define TBFRAME_REANNOUNCE_MIN_MS	10000

enum tbframe_link_state {
	TBFRAME_STATE_INIT,		/* handshake not complete */
	TBFRAME_STATE_UP,		/* established, data admitted */
	TBFRAME_STATE_DEAD,		/* DEAD_HW poisoned, terminal */
};

struct tbframe_frame_priv {
	struct tbframe_frame	frame;	/* public part, must stay first */
	struct tbframe_link	*link;
	struct list_head	node;
	bool			is_rx;
	bool			charged_ctrl;
	bool			charged_data;
	/* counted against tx_ring_budget (posted to the hardware ring) */
	bool			hw_posted;
	refcount_t		rx_refs;
	/* hardware backend fields (unused by the mock) */
	struct ring_frame	rf;
	dma_addr_t		dma;
};

/**
 * struct tbframe_hw_ops - everything tbframe asks of the hardware
 *
 * All calls are made from tbframe's own work items (never from client
 * context except ring_tx/post_rx, which must be non-blocking). The
 * backend reports completions through tbframe_core_tx_complete() /
 * tbframe_core_rx_complete(); stop_rings() must cancel every in-flight
 * frame through those same callbacks (the tb_ring_stop() contract).
 */
struct tbframe_hw_ops {
	int	(*alloc_out_hopid)(void *hw);
	void	(*release_out_hopid)(void *hw, int hopid);
	int	(*alloc_in_hopid)(void *hw, int hopid);
	void	(*release_in_hopid)(void *hw, int hopid);
	int	(*alloc_rings)(void *hw, u16 tx_entries, u16 rx_entries,
			       bool e2e);
	void	(*start_rings)(void *hw);
	/*
	 * Bounded wait for the TX ring to drain into the (still fully
	 * programmed) fabric. Runs before the BYE quiesce so the wire is
	 * empty of our frames before the peer is allowed to tear its
	 * ingress down; never fails, a wedged fabric just burns the bound.
	 */
	void	(*quiesce_tx)(void *hw);
	void	(*stop_rings)(void *hw);
	void	(*free_rings)(void *hw);
	int	(*map_frame)(void *hw, struct tbframe_frame_priv *f, bool tx);
	void	(*unmap_frame)(void *hw, struct tbframe_frame_priv *f, bool tx);
	int	(*post_rx)(void *hw, struct tbframe_frame_priv *f);
	int	(*ring_tx)(void *hw, struct tbframe_frame_priv *f);
	int	(*enable_paths)(void *hw, int local_hopid, int remote_hopid);
	int	(*disable_paths)(void *hw, int local_hopid, int remote_hopid);
	/* level-triggered: 1 programmed, 0 gone, <0 unknown (not dead) */
	int	(*paths_active)(void *hw, int local_hopid, int remote_hopid);
	int	(*control_request)(void *hw, const void *req, size_t req_len,
				   void *resp, size_t resp_len,
				   unsigned int timeout_ms);
	int	(*control_response)(void *hw, const void *resp, size_t len);
	bool	(*reannounce)(void *hw);
	void	(*link_attrs)(void *hw, u8 *width, u8 *speed);
	/* optional: stable peer hardware identity for provisioning/naming */
	void	(*peer_identity)(void *hw, u8 uuid[16], char *name,
				 size_t name_len);
	bool	(*match)(void *hw, const void *token);
};

struct tbframe {
	struct mutex		lock;		/* links list, client slot */
	struct list_head	links;
	struct workqueue_struct	*wq;
	struct rw_semaphore	client_rwsem;
	const struct tbframe_client_ops *client_ops;
	void			*client_ctx;
	/* config (module params in production, shrunk by tests) */
	u16			ring_entries;
	bool			e2e;
	bool			keepalive;
	unsigned int		verify_ms;
	unsigned int		xmit_drain_ms;
	unsigned int		teardown_warn_ms;
	unsigned int		teardown_force_ms;
	/*
	 * Max frames resident in the hardware TX ring per link; the excess
	 * waits in the link's software queues where ctrl frames (rxe ACKs,
	 * keepalives) overtake bulk data. Bounds ACK queueing delay to
	 * budget * frame-time instead of a full ring (~9ms at 2048 x 4KiB).
	 * 0 = unbounded (post straight to the ring, pre-budget behavior).
	 */
	u16			tx_ring_budget;
};

struct tbframe_link {
	struct list_head	node;
	struct tbframe		*tf;
	const struct tbframe_hw_ops *ops;
	void			*hw;
	char			name[32];
	u64			route;

	/*
	 * session_lock serializes the session state machine (session work,
	 * down-session, destroy). link->lock (spinlock) covers state,
	 * admission counters, frame lists and the work re-queue gates.
	 */
	struct mutex		session_lock;
	spinlock_t		lock;

	enum tbframe_link_state	state;
	bool			removing;
	/*
	 * Parked by tbframe_unregister_client(): the session is closed and
	 * every work re-queue gate refuses to re-arm it, so no link_up can
	 * be delivered after the client's last link_down. Cleared (and the
	 * session kicked) by the next tbframe_register_client().
	 */
	bool			parked;
	bool			hello_done;
	bool			rings_up;
	bool			paths_enabled;
	bool			in_hopid_held;
	/*
	 * True once this side's TX ring has been drained/cancelled for the
	 * current teardown (down_session past its quiesce point), false
	 * while a session's rings are live. Gates the BYE_ACK: the ack
	 * certifies "no more frames from this side", so it must never be
	 * sent while our own TX can still emit (the cycle-3 wedge: receiver
	 * acked and killed its ingress while the sender's ring still held
	 * the storm backlog).
	 */
	bool			tx_quiesced;
	/*
	 * Set by a LOGOUT (peer BYE) hold: the session is down for the
	 * client and admission is closed, but rings/paths/HopIDs are
	 * deliberately KEPT -- the RX keeps absorbing the peer's teardown
	 * residue, and tearing hardware down here would double the number
	 * of path disable/enable cycles per peer reload (each cycle is a
	 * shot at the router-level born-dead pairing). The deferred
	 * teardown runs at the head of the next session step -- normally
	 * kicked by the returning peer's HELLO -- immediately before the
	 * aligned rebuild.
	 */
	bool			hw_stale;
	/*
	 * The remote HopID the CURRENT session actually allocated its in-HopID
	 * with and enabled its paths on. An inbound HELLO rewrites
	 * ->remote_hopid from the dispatch context at any time, so teardown
	 * must undo what it did, not what the peer most recently said.
	 */
	u16			active_remote_hopid;
	struct tb_xdomain_handshake hs;	/* READY handshake + zombie predicate */
	unsigned int		hello_attempts;
	unsigned long		last_reannounce;

	bool			needs_down;
	enum tbframe_down_reason down_reason;
	bool			announce_pending; /* replay link_up to a late client */
	/*
	 * The client holds an active record for this link: set when
	 * link_up() is delivered, cleared only when a TERMINAL link_down
	 * (CLOSED/UNPLUG/DEAD_HW) is delivered. Non-terminal session
	 * bounces (SUPERSEDE/VERIFY/LOGOUT) do not clear it -- the client's
	 * device outlives them. A terminal teardown delivers link_down
	 * whenever this is set, even if the session was already down: a
	 * destroy right after a peer BYE used to skip the upcall and leak
	 * a stale usb4_rdmaN shadowing the live device on every peer
	 * reboot.
	 */
	bool			up_delivered;

	int			local_hopid;
	u64			local_cookie;
	u64			local_gid_eui64;

	u16			remote_hopid;
	u16			remote_rx_entries;
	u32			remote_caps;
	u64			remote_cookie;
	u64			remote_gid_eui64;

	/* Mode A admission (spec §6) */
	u16			data_window;
	u16			data_inflight;
	u16			ctrl_inflight;
	bool			tx_blocked;
	bool			e2e_active;
	/* publishers between the state check and ring_tx()/post_rx() */
	atomic_t		hw_active;

	/* frame pool: allocated once per link, reused across sessions */
	struct tbframe_frame_priv *tx_frames;
	struct tbframe_frame_priv *rx_frames;
	u16			tx_frame_count;
	u16			rx_frame_count;
	struct list_head	tx_free;
	struct list_head	rx_free;

	/* budgeted TX: software backlog (ctrl overtakes data) + residency */
	struct list_head	txq_ctrl;
	struct list_head	txq_data;
	u16			ring_posted;

	/* one ref per frame outside the free lists, plus the base ref */
	refcount_t		refcnt;
	struct completion	refs_zero;

	struct delayed_work	session_work;
	struct work_struct	tx_released_work;
	struct work_struct	verify_work;
	struct timer_list	verify_timer;
};

/* core.c */
void tbframe_state_init(struct tbframe *tf);
struct tbframe_link *tbframe_link_create(struct tbframe *tf,
					 const struct tbframe_hw_ops *ops,
					 void *hw, u64 route, u64 gid_eui64,
					 bool autostart);
int tbframe_link_destroy(struct tbframe_link *link,
			 enum tbframe_down_reason reason);
void tbframe_link_session_step(struct tbframe_link *link);
void tbframe_link_verify_step(struct tbframe_link *link);
int tbframe_link_handle_packet(struct tbframe_link *link, const void *buf,
			       size_t size);
int tbframe_handle_packet(struct tbframe *tf, const void *token,
			  const void *buf, size_t size);
/* Instance-explicit forms of the public client API (KUnit uses these). */
int tbframe_register_client_tf(struct tbframe *tf,
			       const struct tbframe_client_ops *ops, void *ctx);
void tbframe_unregister_client_tf(struct tbframe *tf);
void tbframe_core_tx_complete(struct tbframe_frame_priv *f, bool canceled);
void tbframe_core_rx_complete(struct tbframe_frame_priv *f, bool canceled,
			      u16 len, u8 pdf, bool bad);
struct tbframe *tbframe_instance(void);

/* service.c / hw.c (production glue) */
int tbframe_service_start(struct tbframe *tf);
void tbframe_service_stop(struct tbframe *tf);
struct tbframe_hw;
struct tbframe_hw *tbframe_hw_create(struct tbframe *tf, struct tb_xdomain *xd);
void tbframe_hw_destroy(struct tbframe_hw *hw);
extern const struct tbframe_hw_ops tbframe_hw_real_ops;

#endif /* TBFRAME_PRIV_H */
