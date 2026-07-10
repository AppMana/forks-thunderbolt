/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_H
#define TBV_H

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/if.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/refcount.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/thunderbolt.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/workqueue.h>

/*
 * The shared XDomain negotiation header. thunderbolt_ibverbs keeps a
 * byte-identical copy under proto/ (kept in sync with the canonical
 * drivers/thunderbolt/thunderbolt_negotiation.h) so this include resolves the
 * same way in-tree, in the standalone DKMS package and in the split package --
 * proto/ is always kernel/'s sibling, whereas the canonical drivers/thunderbolt
 * is not present in the standalone package.
 */
#include "../proto/thunderbolt_negotiation.h"
#include <linux/xarray.h>

#include "proto/config.h"

#define TBV_DRV_NAME "thunderbolt_ibverbs"
#define TBV_ETH_ALEN 6
#define TBV_NATIVE_PROTOCOL_KEY "tbverbs"
#define TBV_NATIVE_MAX_LANES 4
/*
 * Upper bound on a TB downstream-port number (the low byte of an XDomain
 * route), used to key the deterministic ib_device name index on the peer's
 * port so two neighbours on one domain do not collide (see
 * tbv_ibdev_name_index()). TB port numbers fit comfortably below this.
 */
#define TBV_NAME_MAX_PORTS 64
#define TBV_DATA_PDF_FRAME_START 1
#define TBV_DATA_PDF_FRAME_END 3
#define TBV_NATIVE_PRTCID 1
#define TBV_NATIVE_PRTCVERS 1
#define TBV_NATIVE_PRTCREVS 0
#define TBV_APPLE_PRTCID 0xfa57
#define TBV_APPLE_PRTCVERS 1
#define TBV_APPLE_PRTCREVS 0
#define TBV_APPLE_QPN_SHIFT 8
#define TBV_APPLE_FRAME_SIZE SZ_4K
#define TBV_APPLE_MAX_MSG_SIZE SZ_16M

/*
 * True when the NHI flagged the received frame as corrupt. Frame-mode RX sets
 * RING_DESC_CRC_ERROR on a failed CRC and RING_DESC_BUFFER_OVERRUN when the
 * frame did not fit; the payload is garbage in both cases and must not be
 * parsed as a header or scattered into a user MR.
 *
 * enum ring_desc_flags aliases TX and RX meanings on the same bits
 * (0x1 ISOCH/CRC_ERROR, 0x4 POSTED/BUFFER_OVERRUN), so this is only meaningful
 * on an RX completion, where the NHI has written descriptor status. Pure so
 * the KUnit can pin it.
 */
static inline bool tbv_frame_hw_error(u32 flags)
{
	return flags & (RING_DESC_CRC_ERROR | RING_DESC_BUFFER_OVERRUN);
}

static inline bool tbv_dma_device_ready(const struct device *dev)
{
	if (!dev)
		return false;

	/*
	 * A Thunderbolt core reprobe can briefly expose ring DMA devices whose
	 * IOMMU group is attached before dev->iommu is populated. Calling
	 * dma_map_* in that window reaches iommu_get_dma_domain() and oopses.
	 * Treat it as probe deferral; direct-DMA systems have no iommu_group and
	 * still pass this check.
	 */
	return !dev->iommu_group || dev->iommu;
}

#define TBV_TBNET_ID_STATE_CARRIER		BIT(0)
#define TBV_TBNET_ID_STATE_NEIGHBOR_READY	BIT(1)
#define TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE	BIT(2)
#define TBV_TBNET_ID_STATE_FULL_IP_ACTIVE	BIT(3)

struct tb_property_dir;
struct tbv_tbnet_minimal_session;
struct seq_file;

enum tbv_compat_mode {
	TBV_COMPAT_AUTO,
	TBV_COMPAT_FORCE,
	TBV_COMPAT_OFF,
};

enum tbv_profile {
	TBV_PROFILE_AUTO,
	TBV_PROFILE_MAC_COMPAT,
	TBV_PROFILE_LINUX_PERF,
	TBV_PROFILE_MIXED,
};

enum tbv_tbnet_policy {
	TBV_TBNET_AUTO,
	TBV_TBNET_ALLOW,
	TBV_TBNET_PREFER_RDMA,
	TBV_TBNET_BLOCK,
};

enum tbv_tbnet_identity_mode {
	TBV_TBNET_ID_AUTO,
	TBV_TBNET_ID_STOCK,
	TBV_TBNET_ID_STOCK_PROXY,
	TBV_TBNET_ID_MINIMAL_PACKET,
	TBV_TBNET_ID_OFF,
};

enum tbv_backend_type {
	TBV_BACKEND_NATIVE,
	TBV_BACKEND_APPLE,
};

enum tbv_path_state {
	TBV_PATH_NEW,
	TBV_PATH_RING_ALLOCATED,
	TBV_PATH_RING_STARTED,
	TBV_PATH_TUNNEL_ENABLED,
	TBV_PATH_STOPPED,
};

struct tbv_config {
	enum tbv_compat_mode compat;
	enum tbv_profile profile;
	enum tbv_tbnet_policy tbnet;
	enum tbv_tbnet_identity_mode tbnet_identity;
	u32 lanes_min;
	u32 lanes_max;
	bool lanes_auto;
};

struct tbv_resolved_config {
	struct tbv_config requested;
	enum tbv_profile profile;
	enum tbv_tbnet_identity_mode tbnet_identity;
	bool native_enabled;
	bool apple_enabled;
	bool rc_supported;
	bool uc_supported;
};

struct tbv_backend_ops {
	enum tbv_backend_type type;
	const char *name;
	bool supports_rc;
	bool supports_uc;
	bool needs_tbnet_identity;
};

struct tbv_rail_key {
	u64 route;
	u32 local_adapter;
	u32 remote_adapter;
	u32 path_id;
};

struct tbv_path_config {
	u32 tx_ring_size;
	u32 rx_ring_size;
	u32 tx_flags;
	u32 rx_flags;
	int tx_hop;
	int rx_hop;
	int transmit_path;
	int receive_path;
	u16 sof_mask;
	u16 eof_mask;
	bool e2e;
};

struct tbv_path_owned_frame {
	struct list_head node;
	void *data;
	u32 len;
	u8 sof;
	u8 eof;
};

struct tbv_path {
	enum tbv_path_state state;
	struct tbv_path_config cfg;
	struct tbv_rail *rail;
	struct tb_ring *tx_ring;
	struct tb_ring *rx_ring;
	struct tbv_data_frame *tx_frames;
	struct tbv_data_frame *rx_frames;
	struct tbv_tx_packet *tx_control_packets;
	struct tbv_tx_packet *tx_data_packets;
	u32 tx_frame_count;
	u32 rx_frame_count;
	u32 tx_control_packet_count;
	u32 tx_data_packet_count;
	u32 tx_control_queued;
	u32 tx_data_queued;
	u32 tx_data_reserved;
	u32 tx_data_queue_limit;
	u32 tx_remote_data_credits;
	u32 tx_remote_data_credit_max;
	u32 rx_data_credit_pending;
	spinlock_t tx_lock;
	struct list_head tx_free;
	struct list_head tx_control_free;
	struct list_head tx_data_free;
	struct list_head tx_control_queue;
	struct list_head tx_data_queue;
	struct list_head tx_zcopy_inflight;
	struct delayed_work tx_poll_work;
	struct delayed_work rx_supp_poll_work;
	unsigned long tx_last_progress_jiffies;
	atomic_t tx_inflight;
	atomic64_t data_tx_enqueued;
	atomic64_t data_tx_posted;
	atomic64_t data_tx_completed;
	atomic64_t control_tx_enqueued;
	atomic64_t control_tx_posted;
	atomic64_t control_tx_completed;
	atomic64_t control_tx_queue_max_ms;
	atomic64_t data_tx_credit_stalls;
	atomic64_t data_tx_credit_received;
	/*
	 * Leak reconciliation for the software data-credit window. Across a link
	 * the sender's consumed (every charged data frame) must equal the
	 * receiver's eligible (every received credit-consuming frame); locally
	 * the receiver's eligible must equal credit_sent + rx_data_credit_pending.
	 * A persistent divergence localizes a credit leak (frames lost in transit
	 * -> consumed > peer eligible; return-side drop -> eligible > sent+pending).
	 */
	atomic64_t data_tx_credit_consumed;
	atomic64_t data_rx_credit_eligible;
	atomic64_t data_rx_completed;
	atomic64_t data_rx_credit_sent;
	atomic64_t data_rx_credit_send_error;
	atomic64_t data_rx_repost_failed;
	atomic64_t tx_poll_calls;
	atomic64_t tx_poll_completed;
	atomic64_t rx_supp_poll_calls;
	atomic64_t rx_supp_poll_completed;
	unsigned long rx_supp_poll_until;
	u8 rx_raw_opcode;
	u8 rx_raw_flags;
	u32 rx_raw_dest_qp;
	u32 rx_raw_src_qp;
	u32 rx_raw_psn;
	u32 rx_raw_imm_data;
	u32 rx_raw_rkey;
	u32 rx_raw_done;
	u32 rx_raw_remaining;
	u64 rx_raw_base;
	/*
	 * Stream base byte offset within the operation (header frag_offset).
	 * Zero for legacy full-message streams; per-fragment split streams
	 * (TBV_NATIVE_WIRE_CAP_SPLIT_DATA) carry the fragment's offset.
	 */
	u32 rx_raw_frag_base;
	bool rx_raw_pending;
	bool tx_poll_enabled;
	bool rx_supp_poll_enabled;
	bool tx_scheduling;
	bool tx_raw_stream_active;
	bool tx_raw_stream_end_seen;
	/*
	 * True from a stream header's DEQUEUE until its end packet's DEQUEUE:
	 * the window where nothing else may be posted to the ring or the
	 * receiver would consume it as raw payload. Between windows (chained
	 * per-fragment split streams) scheduling is normal, so ACKs no longer
	 * wait for a whole message.
	 */
	bool tx_raw_stream_window_open;
	u32 tx_raw_stream_inflight;
	void *tx_raw_stream_owner;
	int local_transmit_path;
	int local_tx_hop;
	int local_rx_hop;
	int remote_transmit_path;
	/*
	 * Set by tbv_path_fence() once the NHI rings are stopped (in-flight
	 * frames canceled) ahead of the refs_zero wait in tbv_peer_remove_rail.
	 * tbv_path_destroy() consults it so it does not tb_ring_stop() a ring
	 * that is already stopped (dev_WARN "already stopped"), while still
	 * running the tunnel/hopid teardown its state gates.
	 */
	bool rings_fenced;
};

struct tbv_rail {
	struct list_head node;
	struct tbv_peer *peer;
	struct tbv_state *native_work_state;
	struct tbv_rail_key key;
	struct tbv_path path;
	struct delayed_work native_work;
	refcount_t refcnt;
	struct completion refs_zero;
	/*
	 * Per-rail IB device. Lifecycle managed by tbv_ibdev_rail_event()
	 * (see ibdev.c) under state->rail_register_lock. NULL means this rail
	 * has not yet reached the data-ready edge (or has been torn down).
	 */
	struct tbv_ibdev *ibdev;
	atomic_t native_qp_bind_count;
	u32 rail_id;
	u32 link_speed;
	u32 link_width;
	u32 remote_rail_id;
	int remote_transmit_path;
	int remote_tx_hop;
	int remote_rx_hop;
	u32 native_attempts;
	u32 native_tunnel_attempts;
	int native_last_error;
	/*
	 * Physical lane index for native rails (0..TBV_NATIVE_MAX_LANES-1).
	 * Set in tbv_peer_add_rail() from the matched service id and consumed
	 * by tbv_ibdev_rail_name_index(). Don't derive this from rail->key.path_id;
	 * the encoded path_id uses TBV_NATIVE_PRTCID as its low byte for lane 0,
	 * which collides with the (prtcid << 8) | lane scheme used for the
	 * other lanes (both lane 0 and lane 1 would round-trip to "lane 1").
	 * Undefined for non-native backends.
	 */
	u32 native_lane;
	bool active;
	bool removing;
	/*
	 * Registration unwinding marker. Set under state->rail_register_lock
	 * when ib_register_device() returns nonzero for this rail. While true
	 * tbv_ibdev_start()'s catchup loop and tbv_ibdev_rail_event() will
	 * skip the rail, breaking the spin loop the failed lane would
	 * otherwise cause.
	 */
	bool ibdev_register_failed;
	/*
	 * Retryable registration blocker. A ready rail can reach verbs
	 * registration before the configured RoCE netdev exists; keep it out of
	 * the catchup loop until a matching netdev notifier event retries it.
	 */
	bool ibdev_register_deferred;
	bool native_negotiated;
	/*
	 * READY/confirmation handshake, using the shared cross-driver contract
	 * (thunderbolt_negotiation.h): request_sent == our READY sent,
	 * peer_seen == the peer's READY received, established == both. Re-armed
	 * with tb_xdomain_handshake_reset() on every reconnect so a coordinated
	 * reload cannot strand the rail (see tb_test_xdomain_negotiation_hang).
	 */
	struct tb_xdomain_handshake native_hs;
	/*
	 * Set when an inbound re-HELLO carried a transmit_path different from the
	 * one the live tunnel was enabled with (the peer reloaded with new rings).
	 * tbv_native_control_work() then disables the stale tunnel back to
	 * RING_STARTED so the tunnel phase re-enables it with the new hop, instead
	 * of leaving a data-ready rail pointed at a dead hop. Reproduced by
	 * reconnect_userspace.c scenario 2 / tbv_test_native_rehello_changed_hops.
	 */
	bool native_tunnel_rehop;
	bool native_work_stop;
};

struct tbv_peer {
	struct list_head node;
	refcount_t refcnt;
	struct tbv_state *state;
	u32 peer_id;
	enum tbv_backend_type backend;
	struct tb_xdomain *xd;
	struct list_head rails;
	struct ida rail_ids;
	/* Serializes XDomain control and tunnel setup transactions per link. */
	struct mutex control_lock;
	u32 native_qp_rr_rail_id;
	u32 nr_rails;
	bool lane_bonded;
	/*
	 * The peer's RoCE GID identity, learned from its HELLO (wire v2):
	 * remote_roce_eui64 is the modified-EUI-64 the peer's kernel derives
	 * from its roce_netdev MAC (== bytes 8..15 of its link-local and SLAAC
	 * GIDs, big-endian), remote_roce_ipv4 matches its IPv4-mapped GID
	 * (network byte order). Used at modify_qp(RTR) to map a destination GID
	 * to the Thunderbolt peer that owns it, so QPs on a multi-peer node
	 * (mid-chain host cabled to two neighbours) bind to the rail that
	 * actually reaches the destination instead of the create-time
	 * round-robin guess. Written under state->lock in
	 * tbv_native_control_apply_remote(); zero until the HELLO lands.
	 */
	u64 remote_roce_eui64;
	u32 remote_roce_ipv4;
	bool remote_identity_valid;
	/*
	 * The peer's HELLO capability bits (TBV_NATIVE_WIRE_CAP_*), written
	 * under state->lock in tbv_native_control_apply_remote() and read
	 * with READ_ONCE on the data path. Zero until the HELLO lands, so
	 * every capability-gated behavior defaults off.
	 */
	u32 remote_caps;
};

bool tbv_path_tx_stalled(const struct tbv_path *path);

static inline bool tbv_rail_data_ready(const struct tbv_rail *rail)
{
	if (!rail || rail->path.state != TBV_PATH_TUNNEL_ENABLED)
		return false;
	if (!rail->peer || rail->peer->backend != TBV_BACKEND_NATIVE)
		return true;
	return tb_xdomain_handshake_complete(&rail->native_hs);
}

static inline bool tbv_rail_apple_data_ready(const struct tbv_rail *rail)
{
	return rail && rail->path.state == TBV_PATH_TUNNEL_ENABLED;
}

struct tbv_tbnet_identity {
	enum tbv_tbnet_identity_mode mode;
	unsigned long state;
	struct mutex lock;
	struct list_head minimal_sessions;
	struct tb_property_dir *minimal_dir;
	char tbnet_netdev_name[IFNAMSIZ];
	char gid_netdev_name[IFNAMSIZ];
	struct net_device *tbnet_dev;
	struct net_device *gid_dev;
	struct notifier_block netdev_nb;
	struct notifier_block inetaddr_nb;
	__be32 proxy_ipv4;
	bool minimal_e2e;
	bool minimal_apple_only;
	bool minimal_neighbor_seen;
	bool minimal_dir_registered;
	bool minimal_driver_registered;
	bool minimal_started;
	bool netdev_nb_registered;
	bool inetaddr_nb_registered;
	bool rx_handler_registered;
	atomic64_t minimal_login_rx;
	atomic64_t minimal_login_tx;
	atomic64_t minimal_logout_rx;
	atomic64_t minimal_logout_tx;
	atomic64_t minimal_status_rx;
	atomic64_t minimal_status_tx;
	atomic64_t minimal_packet_rx;
	atomic64_t minimal_packet_tx_posted;
	atomic64_t minimal_packet_tx;
	atomic64_t minimal_packet_tx_errors;
	atomic64_t minimal_path_errors;
	atomic64_t arp_requests;
	atomic64_t arp_replies;
	atomic64_t arp_ignored;
	atomic64_t arp_errors;
};

enum tbv_tbip_type {
	TBV_TBIP_LOGIN,
	TBV_TBIP_LOGIN_RESPONSE,
	TBV_TBIP_LOGOUT,
	TBV_TBIP_STATUS,
};

struct tbv_tbip_control {
	u64 route;
	u8 sequence;
	uuid_t initiator_uuid;
	uuid_t target_uuid;
	u32 command_id;
};

struct tbv_tbip_login_params {
	struct tbv_tbip_control ctrl;
	u32 transmit_path;
};

struct tbv_tbip_login_response_params {
	struct tbv_tbip_control ctrl;
	u32 status;
	u8 receiver_mac[TBV_ETH_ALEN];
};

struct tbv_tbip_status_params {
	struct tbv_tbip_control ctrl;
	u32 status;
};

struct tbv_tbip_status_result {
	struct tbv_tbip_control ctrl;
	u32 status;
};

struct tbv_tbip_login_response_result {
	struct tbv_tbip_control ctrl;
	u32 status;
	u8 receiver_mac[TBV_ETH_ALEN];
	u32 receiver_mac_len;
};

struct tbv_tbnet_arp_proxy {
	__be32 ipv4;
	u8 mac[TBV_ETH_ALEN];
};

struct tbv_tbnet_identity_config {
	const char *tbnet_netdev;
	const char *gid_netdev;
	bool minimal_e2e;
	bool minimal_apple_only;
};

#define TBV_CONFIGURED_LINK_NAME_LEN (TBV_CFG_LINK_NAME_MAX + 1u)

struct tbv_configured_link {
	struct list_head node;
	u32 link_id;
	enum tbv_backend_type backend;
	struct tbv_id_selection app_selection;
	char name[TBV_CONFIGURED_LINK_NAME_LEN];
};

struct tbv_state {
	struct tbv_resolved_config cfg;
	struct mutex lock;
	struct list_head peers;
	struct list_head configured_links;
	u32 next_peer_id;
	u32 configured_link_count;
	struct tbv_tbnet_identity tbnet_identity;
	struct tb_property_dir *native_dirs[TBV_NATIVE_MAX_LANES];
	u32 native_dir_count;
	/*
	 * Saved prtcstns + rate limit for tbv_services_reannounce_native(): when
	 * a rail exhausts its HELLO retries (the peer has not recreated our
	 * service after a module reload -- its one-shot XDomain property read
	 * raced our initialization and gave up), re-registering the property
	 * dirs sends a fresh properties-changed notification that restarts the
	 * peer's read cycle. Without this, module reloads converge ~1 in 10
	 * (2026-06-12 fleet roll) and the only reliable recovery is a reboot.
	 */
	u32 native_prtcstns;
	unsigned long native_reannounce_jiffies;
	/*
	 * Boot-time identity race (2026-06-13 DSV4 outage): the module loads
	 * and HELLOs within seconds of POST, before DHCP has assigned the
	 * roce_netdev's IPv4. The HELLO then advertises eui64 with ipv4=0,
	 * peers store it as a valid identity, and every later v4-mapped dgid
	 * hard-fails modify_qp(RTR) with -ENETUNREACH. Three defenses:
	 *  1. identity_grace_until: defer the first HELLO while the local
	 *     identity is incomplete, up to this deadline.
	 *  2. hello_sent_incomplete: set when a HELLO went out with ipv4=0
	 *     after the grace expired.
	 *  3. identity_refresh_work: scheduled by the inetaddr notifier when
	 *     the roce_netdev gains an address; re-HELLOs negotiated rails so
	 *     peers replace the incomplete identity.
	 * The receive-side defense is tbv_gid_identity_verdict(): an identity
	 * that cannot adjudicate a dgid family is INCONCLUSIVE and never
	 * contributes to a hard -ENETUNREACH reject.
	 */
	unsigned long identity_grace_until;
	bool hello_sent_incomplete;
	struct work_struct identity_refresh_work;
	struct tb_property_dir *apple_dir;
	struct dentry *debugfs_dir;
	bool allocate_rings;
	bool start_rings;
	bool negotiate_native;
	bool enable_tunnels;
	bool native_data;
	bool apple_data;
	bool native_fragment_striping;
	bool native_home_rail_qp;
	int native_data_e2e; /* -1 auto, 0 off, 1 on */
	bool register_verbs;
	bool services_registered;
	bool verbs_registered;
	bool native_control_registered;
	bool native_control_source_aware;
	bool native_legacy_multicable_warned;
	bool apple_tunnels_wait_tbnet;
	bool apple_tunnels_pending;
	struct work_struct apple_tunnel_work;
	struct workqueue_struct *workqueue;
	struct notifier_block ibdev_netdev_nb;
	atomic_t verbs_ucontexts;
	atomic_t verbs_pds;
	atomic_t verbs_cqs;
	atomic_t verbs_qps;
	atomic_t verbs_mrs;
	atomic_t verbs_recv_wqes;
	atomic64_t data_wr_send;
	atomic64_t data_wr_op_send;
	atomic64_t data_wr_op_send_imm;
	atomic64_t data_wr_op_write;
	atomic64_t data_wr_op_write_imm;
	atomic64_t data_wr_op_unsupported;
	atomic64_t data_wr_live;
	atomic64_t data_wr_no_path;
	atomic64_t data_wr_no_recv_credit;
	atomic64_t data_wr_copied;
	atomic64_t data_wr_zcopy;
	atomic64_t data_wr_zcopy_fallback;
	atomic64_t data_wr_zcopy_fallback_striping;
	atomic64_t data_wr_zcopy_fallback_unsafe_sge;
	atomic64_t data_wr_zcopy_fallback_peer;
	atomic64_t data_wr_zcopy_fallback_unaligned;
	/*
	 * Hypothesis-discriminating emission counters for the zcopy CRC
	 * investigation: which window class the sender emitted. Correlated
	 * with the peer's data_rx_crc_error across A/B runs they localize the
	 * corrupting class (the receiver cannot attribute a CRC-dropped frame
	 * to a window, so the discrimination is sender-side + module params).
	 */
	atomic64_t data_wr_zcopy_window_first;
	atomic64_t data_wr_zcopy_window_rest;
	atomic64_t data_wr_zcopy_full;
	atomic64_t data_wr_zcopy_partial;
	atomic64_t data_wr_zcopy_retransmit;
	atomic64_t data_wr_zcopy_frames;
	atomic64_t data_wr_zcopy_page_suspect;
	/*
	 * Sender frame-class counts, paired with the receiver's crc_error
	 * split so one run says which class corrupts: _hdr = the 48-byte
	 * staged header frame per window, _payload_full = a full 4096 zcopy
	 * payload frame, _payload_tail = a sub-4096 last-window payload frame.
	 */
	atomic64_t data_wr_zcopy_hdr;
	atomic64_t data_wr_zcopy_payload_full;
	atomic64_t data_wr_zcopy_payload_tail;
	/* payload frames served from a stable kernel copy (zcopy_stage_payload) */
	atomic64_t data_wr_zcopy_staged;
	/*
	 * Persistent-mapping confirmation: _mr_mapped counts frames served
	 * from the MR's existing umem DMA mapping (no per-frame map/unmap),
	 * _remapped counts frames that fell back to a per-frame dma_map_page
	 * because the send's rail used a different DMA device. Expect
	 * _mr_mapped ~= _frames and _remapped ~= 0 once the churn is gone.
	 */
	atomic64_t data_wr_zcopy_mr_mapped;
	atomic64_t data_wr_zcopy_remapped;
	/*
	 * Self-explaining fallback reasons (why a frame did NOT use the
	 * persistent MR mapping), so a nonzero _remapped on hardware names the
	 * cause instead of costing another cold boot. no_mr_mapping is the
	 * predicate that was silently false in 8d31089 (dma_dev NULL under
	 * virt DMA); once fixed, all of these stay 0.
	 */
	atomic64_t data_wr_zcopy_fb_no_mr_mapping;
	atomic64_t data_wr_zcopy_fb_device_mismatch;
	atomic64_t data_wr_zcopy_fb_dmabuf_or_odp;
	atomic64_t data_wr_zcopy_fb_offset_not_found;
	atomic64_t data_wr_zcopy_fb_other;
	atomic_t zcopy_fallback_reported;
	atomic64_t data_wr_copy_error;
	atomic64_t data_wr_path_send;
	atomic64_t data_wr_path_send_error;
	atomic64_t data_wr_retransmit;
	atomic64_t data_wr_rnr_retransmit;
	atomic64_t data_wr_nak_retransmit;
	atomic64_t data_wr_retry_enqueue_error;
	atomic64_t data_wr_retry_exhausted;
	atomic64_t data_wr_rnr_retry_exhausted;
	atomic64_t data_wr_timeout;
	atomic64_t apple_sq_queued;
	atomic64_t apple_sq_dequeued;
	atomic64_t apple_sq_full;
	atomic64_t apple_sq_flushed;
	atomic64_t data_tx_accepted;
	atomic64_t data_tx_posted;
	atomic64_t data_tx_completed;
	atomic64_t data_tx_canceled;
	atomic64_t data_tx_errors;
	atomic64_t data_tx_credit_stalls;
	atomic64_t data_tx_credit_received;
	/*
	 * Leak reconciliation for the software data-credit window. Across a link
	 * the sender's consumed (every charged data frame) must equal the
	 * receiver's eligible (every received credit-consuming frame); locally
	 * the receiver's eligible must equal credit_sent + rx_data_credit_pending.
	 * A persistent divergence localizes a credit leak (frames lost in transit
	 * -> consumed > peer eligible; return-side drop -> eligible > sent+pending).
	 */
	atomic64_t data_tx_credit_consumed;
	atomic64_t data_rx_credit_eligible;
	atomic64_t data_rx_completed;
	atomic64_t data_rx_credit_sent;
	atomic64_t data_rx_credit_send_error;
	atomic64_t data_rx_repost_failed;
	atomic64_t data_rx_bad_frame;
	atomic64_t data_rx_bad_header;
	/* NHI-reported per-frame errors; see tbv_frame_hw_error(). */
	atomic64_t data_rx_crc_error;
	atomic64_t data_rx_overrun;
	/*
	 * Frame-class localization of the residual zcopy CRC corruption:
	 * _in_stream = corrupt frame arrived mid-raw-stream (a zcopy PAYLOAD
	 * frame), _standalone = a raw-stream header / copied / control frame,
	 * _maxsize = the corrupt frame was exactly one full frame (4096).
	 */
	atomic64_t data_rx_crc_error_in_stream;
	atomic64_t data_rx_crc_error_standalone;
	atomic64_t data_rx_crc_error_maxsize;
	atomic_t crc_error_reported;
	atomic64_t data_rx_send;
	atomic64_t data_rx_op_send;
	atomic64_t data_rx_op_send_imm;
	atomic64_t data_rx_op_write;
	atomic64_t data_rx_op_write_imm;
	atomic64_t data_rx_ack;
	atomic64_t data_rx_ack_matched;
	atomic64_t data_rx_ack_match_retried;
	atomic64_t data_rx_ack_match_max_ms;
	atomic64_t data_rx_ack_match_current_max_ms;
	atomic64_t data_rx_ack_match_over_10ms;
	atomic64_t data_rx_ack_match_over_64ms;
	atomic64_t data_rx_ack_miss;
	atomic64_t data_rx_late_ack;
	atomic64_t data_rx_ack_cumulative;
	atomic64_t data_tx_ack_ok;
	atomic64_t data_tx_ack_rnr;
	atomic64_t data_tx_ack_error;
	atomic64_t data_tx_ack_send_error;
	atomic64_t data_rx_ack_rnr;
	atomic64_t data_rx_duplicate_ack;
	atomic64_t data_rx_ack_history_miss;
	atomic64_t data_tx_nak;
	atomic64_t data_tx_nak_send_error;
	atomic64_t data_rx_nak;
	atomic64_t data_rx_nak_matched;
	atomic64_t data_rx_nak_miss;
	atomic64_t data_tx_read_ack_ok;
	atomic64_t data_tx_read_ack_retry;
	atomic64_t data_tx_read_ack_error;
	atomic64_t data_rx_read_ack_ok;
	atomic64_t data_rx_read_ack_retry;
	atomic64_t data_rx_read_ack_error;
	atomic64_t data_read_resp_retransmit;
	atomic64_t data_read_resp_drop;
	atomic64_t data_rx_read_resp_duplicate;
	atomic64_t data_rx_read_resp_gap;
	atomic64_t data_rx_read_resp_remote_error;
	atomic64_t data_rx_read_resp_bad_header;
	atomic64_t data_rx_read_resp_copy_error;
	atomic64_t data_rx_read_resp_short;
	atomic64_t data_rx_read_req_no_access;
	atomic64_t data_rx_read_req_no_mr;
	atomic64_t data_rx_read_req_mr_access;
	atomic64_t data_rx_read_req_too_large;
	atomic64_t data_rx_read_req_bad_iova;
	atomic64_t data_rx_read_req_alloc_error;
	atomic64_t data_rx_read_req_resp_busy;
	atomic64_t data_rx_read_req_resp_error;
	atomic64_t data_rx_no_qp;
	atomic64_t data_rx_bad_peer;
	atomic64_t data_rx_unconnected_qp;
	atomic64_t data_rx_qp_error;
	atomic64_t data_rx_no_recv;
	atomic64_t data_rx_rnr;
	atomic64_t data_rx_rnr_suppressed;
	atomic64_t data_rx_copy_error;
	atomic64_t data_rx_send_len_error;
	atomic64_t data_rx_send_prot_error;
	atomic64_t data_rx_send_cq_error;
	atomic64_t data_rx_send_bad_fragment;
	atomic64_t data_rx_send_sequence_error;
	atomic64_t data_rx_active_timeout;
	atomic64_t data_rx_reorder_buffered;
	atomic64_t data_rx_reorder_delivered;
	atomic64_t data_rx_reorder_dropped;
	atomic64_t data_rx_reorder_timeout;
	atomic64_t data_rx_reorder_window;
	atomic64_t data_rx_pending_discarded;
	atomic64_t apple_rx_sof;
	atomic64_t apple_rx_eof3;
	atomic64_t apple_rx_eof_other;
	atomic64_t apple_rx_sof_while_active;
	atomic64_t apple_rx_no_sof_when_idle;
	atomic64_t apple_rx_eof_without_active;
	atomic64_t apple_rx_len_overrun;
	atomic64_t data_cq_overflow;
	atomic64_t native_legacy_ambiguous_limited;
	struct xarray verbs_mrs_xa;
	struct xarray verbs_qps_xa;
	/*
	 * Serializes per-rail ib_device registration against teardown.
	 * tbv_ibdev_rail_event() publishes one ib_device per active rail as
	 * its data path comes up; tbv_peer_remove_rail() and module-exit
	 * tear them down. Kept separate from state->lock so the sleeping
	 * ib_(un)register_device path doesn't invert against verbs ops that
	 * take state->lock.
	 */
	struct mutex rail_register_lock;
	struct work_struct ibdev_netdev_retry_work;
	/* Reaps netdevs unbound from notifier context (RTNL held there). */
	struct work_struct ibdev_netdev_reap_work;
	/*
	 * Up-event gate, owned by rail_register_lock. Set to true by
	 * tbv_ibdev_start() before any rising-edge events may publish; cleared
	 * by tbv_ibdev_stop() before draining so no late ready-edge event
	 * sneaks a fresh ib_device past module exit. Down events ignore this
	 * flag so existing devices can always be torn down.
	 */
	bool register_enabled;
	bool ibdev_netdev_nb_registered;
};

struct dentry;
struct tbv_service_config {
	u32 native_prtcstns;
	u32 apple_prtcstns;
	bool allocate_rings;
	bool start_rings;
	bool negotiate_native;
	bool enable_tunnels;
};

struct tb_property_dir;
struct tbv_data_frame;
struct tbv_native_data_header;
struct tbv_tx_packet;
struct device;
struct page;
struct tb_ring;
struct tb_xdomain;
typedef void (*tbv_path_tx_done_fn)(void *ctx, int status);
typedef int (*tbv_path_tx_fill_fn)(void *ctx, void *dst, u32 len);
/*
 * Yield the next zero-copy TX fragment, one of three ways:
 *   - *owned set: a kmalloc'd kernel buffer holding a COPY of *length payload
 *     bytes (the zcopy_stage_payload diagnostic). The path frames it as a
 *     normal owned data packet (staged into the ring's kernel TX frame), so
 *     the NHI DMA-reads stable kernel memory, not the live MR. The path frees
 *     the buffer on completion.
 *   - *premapped set: *dma is a DMA address from the MR's PERSISTENT mapping;
 *     the path must NOT dma_map/unmap it (page/page_off unused).
 *   - otherwise: the path dma_map_page(*page, *page_off, ...) itself and unmaps
 *     on completion (the churning fallback for a rail on a different device).
 */
typedef int (*tbv_path_next_page_fn)(void *ctx, struct page **page,
				     u32 *page_off, u32 *length,
				     dma_addr_t *dma, bool *premapped,
				     void **owned,
				     tbv_path_tx_done_fn *done,
				     void **done_ctx);
#define TBV_PATH_SEND_CONTROL	BIT(0)
#define TBV_PATH_SEND_DEFER	BIT(1)
/*
 * Retransmit marker for tbv_path_send_page_stream(): refund one remote data
 * credit per posted frame before they re-charge, reclaiming the credits the
 * lost attempt leaked (same contract as tbv_path_refund_remote_data_credits
 * in the framed retransmit path; see the leak note there).
 */
#define TBV_PATH_SEND_REFUND	BIT(2)
extern const uuid_t tbv_native_service_uuid;

int tbv_config_parse(struct tbv_config *cfg, const char *compat,
		     const char *profile, const char *tbnet,
		     const char *tbnet_identity, const char *lanes);
int tbv_config_resolve(struct tbv_resolved_config *resolved,
		       const struct tbv_config *cfg);

const char *tbv_compat_name(enum tbv_compat_mode mode);
const char *tbv_profile_name(enum tbv_profile profile);
const char *tbv_tbnet_policy_name(enum tbv_tbnet_policy policy);
const char *tbv_tbnet_identity_name(enum tbv_tbnet_identity_mode mode);
const char *tbv_backend_name(enum tbv_backend_type type);

int tbv_ibdev_start(struct tbv_state *state, bool register_verbs);
void tbv_ibdev_stop(struct tbv_state *state);
const char *tbv_ibdev_roce_netdev_name(void);

/* write()-ABI data-path command mask + ops accessor. Both halves are required
 * for the provider's ibv_cmd_post_recv (write path) to reach the driver instead
 * of ENOSYS; KUnit-pinned in tests/post_recv_dispatch_test.c. */
u64 tbv_ibdev_uverbs_cmd_mask(void);
const struct ib_device_ops *tbv_ibdev_ops_ref(void);
/*
 * Notify the verbs layer that rail's data path has come up (joined=true) or
 * is about to be torn down (joined=false). Safe to call repeatedly; only the
 * rising/falling edge of "ibdev published" causes registration changes.
 *
 * Up events are gated on state->register_enabled (flipped off by
 * tbv_ibdev_stop()). Down events are unconditional so module-exit and rail
 * remove can always undo a published device. Returns 0 on success/no-op, or a
 * negative errno if an up event failed to publish (the rail is then
 * permanently marked failed and will be skipped on retry).
 */
int tbv_ibdev_rail_event(struct tbv_state *state, struct tbv_rail *rail,
			 bool joined);
void tbv_ibdev_rx_frame(struct tbv_state *state, struct tbv_path *rx_path,
			const void *data, u32 len);
void tbv_ibdev_rx_native_frame(struct tbv_state *state,
			       struct tbv_path *rx_path,
			       const struct tbv_native_data_header *hdr,
			       const void *payload);
void tbv_ibdev_rx_apple_frame(struct tbv_state *state,
			      const struct tbv_path *path,
			      const void *payload, u32 len, u8 sof, u8 eof);

int tbv_tbnet_identity_check_config(const struct tbv_resolved_config *cfg);
int tbv_tbnet_identity_prepare(struct tbv_tbnet_identity *identity,
			       const struct tbv_resolved_config *cfg,
			       const struct tbv_tbnet_identity_config *identity_cfg);
void tbv_tbnet_identity_stop(struct tbv_tbnet_identity *identity);
int tbv_tbip_build_login(void *buf, size_t size,
			 const struct tbv_tbip_login_params *params);
int tbv_tbip_build_login_response(void *buf, size_t size,
				  const struct tbv_tbip_login_response_params *params);
int tbv_tbip_build_logout(void *buf, size_t size,
			  const struct tbv_tbip_control *ctrl);
int tbv_tbip_build_status(void *buf, size_t size,
			  const struct tbv_tbip_status_params *params);
int tbv_tbip_parse_type(const void *buf, size_t size,
			enum tbv_tbip_type *type,
			struct tbv_tbip_control *ctrl);
int tbv_tbip_parse_login(const void *buf, size_t size,
			 struct tbv_tbip_login_params *params);
int tbv_tbip_parse_login_response(const void *buf, size_t size,
				  struct tbv_tbip_login_response_result *result);
int tbv_tbip_parse_status(const void *buf, size_t size,
			  struct tbv_tbip_status_result *result);
int tbv_tbnet_arp_reply_for_request(void *reply, size_t reply_size,
				    const void *request, size_t request_size,
				    const struct tbv_tbnet_arp_proxy *proxy);
int tbv_tbnet_minimal_start(struct tbv_tbnet_identity *identity);
void tbv_tbnet_minimal_stop(struct tbv_tbnet_identity *identity);
void tbv_tbnet_minimal_recompute_state_locked(struct tbv_tbnet_identity *identity);
bool tbv_tbnet_minimal_neighbor_ready(struct tbv_tbnet_identity *identity,
				      const uuid_t *remote_uuid);
void tbv_tbnet_minimal_clear_neighbors_locked(struct tbv_tbnet_identity *identity);
void tbv_tbnet_minimal_debugfs_show(struct seq_file *s,
				    struct tbv_tbnet_identity *identity);
void tbv_services_tbnet_identity_ready(struct tbv_tbnet_identity *identity);
struct tb_property_dir *tbv_service_create_native_dir(void);
struct tb_property_dir *tbv_service_create_apple_dir(u32 prtcstns);
int tbv_services_start(struct tbv_state *state, bool bind_services,
		       const struct tbv_service_config *service_cfg);
void tbv_services_stop(struct tbv_state *state);
/*
 * Re-register the native property dirs to push a fresh XDomain
 * properties-changed notification to every neighbour (rate-limited; returns
 * false when skipped). Used by native control when HELLO retries exhaust.
 */
bool tbv_services_reannounce_native(struct tbv_state *state);
int tbv_native_control_start(struct tbv_state *state);
void tbv_native_control_stop(struct tbv_state *state);
const char *tbv_native_control_mode_name(const struct tbv_state *state);
int tbv_native_control_xdomain_start(struct tbv_state *state);
void tbv_native_control_xdomain_stop(void);
int tbv_native_control_legacy_start(struct tbv_state *state);
void tbv_native_control_legacy_stop(void);
int tbv_native_control_handle_packet(struct tbv_state *state,
				     struct tb_xdomain *source_xd,
				     const void *buf, size_t size);
void tbv_native_control_init_rail(struct tbv_rail *rail,
				  struct tbv_peer *peer);
void tbv_native_control_queue_rail(struct tbv_state *state,
				   struct tbv_rail *rail);
void tbv_native_control_cancel_rail(struct tbv_rail *rail);
int tbv_native_control_exchange(struct tbv_state *state, struct tbv_peer *peer,
				struct tbv_rail *rail);
void tbv_rail_key_init(struct tbv_rail_key *key, u64 route,
		       u32 local_adapter, u32 remote_adapter, u32 path_id);
int tbv_rail_key_cmp(const struct tbv_rail_key *a,
		     const struct tbv_rail_key *b);
u32 tbv_rail_key_hash(const struct tbv_rail_key *key);
struct tbv_peer *tbv_peer_get_or_create(struct tbv_state *state,
					enum tbv_backend_type backend,
					struct tb_xdomain *xd);
void tbv_peer_put(struct tbv_state *state, struct tbv_peer *peer);
/*
 * Synchronously bring an XDomain link up at dual lane (40 Gb/s) before rails
 * are built, so tbv_service_probe's lane-count gate exposes the second rail.
 * No-op when native_lane_bonding is off or the link is already DUAL. Returns
 * the resulting xd->link_width is DUAL (true) or stayed SINGLE (false).
 */
bool tbv_xdomain_bond_sync(struct tb_xdomain *xd);
struct tbv_rail *tbv_peer_add_rail(struct tbv_peer *peer,
				   const struct tbv_rail_key *key,
				   u32 native_lane);
void tbv_peer_remove_rail(struct tbv_rail *rail);
void tbv_rail_put(struct tbv_rail *rail);

/*
 * Pure helpers exposed for kunit (kernel/tests/). tbv_ack_route_peer decides
 * which peer an ACK/control frame routes to (the requester via rx_path, not the
 * QP's bound peer); tbv_psn_delta is signed 24-bit PSN distance with wraparound;
 * tbv_gid_matches_identity decides whether a 16-byte destination GID belongs to
 * a peer identified by (eui64, ipv4) from its wire-v2 HELLO.
 */
struct tbv_peer *tbv_ack_route_peer(struct tbv_rail *qp_rail,
				    struct tbv_path *rx_path);
/*
 * Task 2 (local-completion WC) contract, design-only -- pins the floor the
 * implementation must not drop below, unwired from the WC path for now. The
 * earliest point at which a signaled send WC may fire depends on where the
 * payload lives during TX: a copied send stages the payload into a kernel ring
 * frame at post, so the user buffer is free immediately; a zero-copy send has
 * the NHI DMA-read the live user MR pages, so the buffer is not reusable until
 * local TX-ring completion (firing earlier reintroduces the reuse race the
 * current ACK-gating prevents by construction). See docs/dsv4_2026_07_09_
 * changes.md "Task 2 design" and kernel/tests/send_wc_completion_test.c.
 */
enum tbv_wc_completion_point {
	TBV_WC_AT_POST = 0,	/* buffer free right after post (copied: staged) */
	TBV_WC_AT_LOCAL_TX = 1,	/* buffer free after the NHI finished the read (zcopy) */
	TBV_WC_AT_REMOTE_ACK = 2, /* current conservative baseline: after peer ACK */
};
enum tbv_wc_completion_point tbv_send_wc_earliest_point(bool zcopy);
bool tbv_send_wc_may_fire(enum tbv_wc_completion_point earliest,
			  bool posted, bool local_tx_complete,
			  bool remote_acked);
/*
 * Zero-copy TX mode selection for one native send ctx, decided ONCE at the
 * initial post and pinned for every retransmit (framing must not change
 * between attempts or the receiver's fragment bitmaps mismatch). SPLIT is the
 * retransmit-safe per-fragment raw stream and requires the peer to have
 * advertised TBV_NATIVE_WIRE_CAP_SPLIT_DATA; RAW_STREAM is the legacy
 * full-message stream, only safe when the ctx can never retransmit. Exposed
 * for kunit (tests/zcopy_split_test.c).
 */
enum tbv_zcopy_tx_mode {
	TBV_ZCOPY_TX_NONE = 0,
	TBV_ZCOPY_TX_RAW_STREAM,
	TBV_ZCOPY_TX_SPLIT,
};
enum tbv_zcopy_tx_mode tbv_zcopy_select_mode(bool is_write, bool striping,
					     bool retryable, u32 total_len,
					     u32 min_bytes, u32 peer_caps);
/*
 * Zero-copy TX frames the NHI can transmit without corrupting the on-wire CRC
 * must start at page offset 0. tbnet, the reference consumer of these rings,
 * always DMAs driver-owned, offset-0 pages (drivers/net/thunderbolt/main.c);
 * an ib_umem window whose first mapped byte sits mid-page yields a mid-page
 * (page_off != 0) buffer_phy, and hardware validation (0.2.26 on 027<->019)
 * showed exactly those frames fail the receiver CRC. A source is clean for
 * split zero-copy iff its first mapped byte is page-aligned AND the 4096-byte
 * split unit divides the page size, so every window maps to one offset-0 page
 * (full) or one offset-0 trailing partial page. Unclean sources fall back to
 * the framed copy path. Exposed for kunit (tests/zcopy_split_test.c).
 */
bool tbv_zcopy_split_page_aligned(u32 first_page_off, u32 split_unit,
				  u32 page_size);
/*
 * DMA map length for a zero-copy TX frame. The copied path always presents a
 * full 4096-byte mapping (dma_map_single of a kmalloc'd frame) and transmits
 * frame->size <= 4096, so an NHI burst that rounds the read up past the frame
 * size stays inside the mapping. A zcopy frame that maps only payload_len
 * leaves the tail of the page unmapped; a rounded-up read there faults the
 * IOMMU (iommu=pt makes external TB devices translated, not identity) and the
 * NHI emits a corrupt frame the receiver drops as a CRC error. Mapping to the
 * page boundary (page-aligned frames only, so page_off==0 => a full page)
 * closes that gap while frame->size still transmits exactly payload_len.
 * Exposed for kunit.
 */
u32 tbv_zcopy_frame_map_len(u32 page_off, u32 payload_len, u32 page_size,
			    u32 frame_size, bool full_page);
/*
 * A zero-copy send derives its frame DMA addresses from the MR's PERSISTENT
 * umem mapping (ib_umem_get already dma_map'd the sgt to the ib_device's DMA
 * device) only when the send's rail uses that same DMA device. Per-frame
 * dma_map_page/dma_unmap_page (191k/run) under lazy AMD-Vi IOTLB invalidation
 * recycles IOVAs before the flush, and a transfer that catches a torn-down
 * translation aborts mid-read -> a wire frame that fails its CRC (0.2.26
 * hardware finding, case 2). Reusing the persistent mapping removes the churn
 * entirely; a rail on a different NHI (different DMA device) falls back to the
 * framed copy. Exposed for kunit.
 */
bool tbv_zcopy_use_persistent(const void *mr_dma_dev, const void *path_dma_dev);
/*
 * Locate a byte offset within a DMA-mapped scatter/gather table (coalesced
 * segments, so fewer/larger than the CPU page list). Returns the segment
 * index, the intra-segment byte offset, and the contiguous chunk available
 * from there (capped at max_len), or -EFAULT if offset is past the mapped
 * bytes. The real walk (tbv_umem_dma_from_addr) reads sg_dma_address/
 * sg_dma_len from those segments; this pins the arithmetic. Exposed for kunit.
 */
int tbv_dma_sgt_locate(const u32 *seg_dma_lens, u32 nsegs, u32 offset,
		       u32 max_len, u32 *seg_idx, u32 *seg_off, u32 *chunk);
/*
 * Byte range [start, end) a retransmit must cover. A NAK carries the missing
 * range; opcodes whose receive path cannot buffer past a hole (SEND streaming,
 * RDMA_WRITE_IMM) must go-back-N to the end. acked_prefix is the cumulative
 * SACK floor (bytes below it are known-delivered). Exposed for kunit.
 */
void tbv_send_retry_range(u8 opcode, u32 total_len, u32 acked_prefix,
			  bool nak_retry, u32 nak_start, u32 nak_len,
			  u32 *start, u32 *end);
/* Whether fragment [frag_off, frag_off+frag_len) intersects [start, end). */
bool tbv_send_frag_needed(u32 frag_off, u32 frag_len, u32 start, u32 end);
/* Monotone cumulative-SACK update, clamped to the message length. */
u32 tbv_send_acked_prefix_update(u32 cur, u32 missing_start, u32 total_len);
/*
 * Receiver-side NAK emission gate: capability + per-QP duplicate suppression
 * (one NAK per distinct (psn, hole start); re-arm after min_interval so a
 * lost NAK is eventually re-sent). Exposed for kunit.
 */
bool tbv_rx_nak_should_send(u32 peer_caps, bool last_valid, u32 last_psn,
			    u32 last_start, unsigned long last_jiffies,
			    u32 psn, u32 start, unsigned long now,
			    unsigned long min_interval);
bool tbv_native_control_path_should_replace(u32 data_score, u32 control_score,
					    bool is_rx_path,
					    u32 best_data_score,
					    u32 best_control_score,
					    bool best_is_rx_path);
s32 tbv_psn_delta(u32 a, u32 b);
/*
 * tbv_ibdev_name_index maps (tb domain, route downstream-port, native lane) to
 * a deterministic, locally-unique "usb4_rdma%d" suffix. Keying on the route's
 * downstream port is what keeps a mid-chain host's two neighbour rails (which
 * share domain, lane 0 and local_adapter 0, differing only in route) from
 * colliding on one name and failing ib_register_device with -ENFILE. @apple is
 * non-zero for an Apple-backend rail (no lane subdivision).
 */
int tbv_ibdev_name_index(int domain_idx, u32 route_port, u32 native_lane,
			 int apple, unsigned int max_lanes);
/*
 * tbv_ibdev_rail_name_index reads the naming inputs off a struct tbv_rail and
 * calls tbv_ibdev_name_index. Exposed for kunit: this is where the field choice
 * lives (it must key on the rail key's route, NOT local_adapter, which is 0 for
 * every native rail on a node).
 */
int tbv_ibdev_rail_name_index(const struct tbv_rail *rail);
/*
 * tbv_netdev_rename_keep decides, on a NETDEV_CHANGENAME, whether to KEEP the
 * ib_device bound to its netdev. Our per-rail GID-only netdev has no externally
 * pinned name (expected_name == NULL) and is renamed by udev per link by design,
 * so it must be kept. Only a pinned external roce_netdev renamed AWAY from its
 * name should detach. Detaching our own netdev here drops the rail GID and
 * deadlocks (unregister_netdev under the rename's rtnl). Exposed for kunit.
 */
bool tbv_netdev_rename_keep(const char *expected_name, const char *new_name);
/*
 * tbv_ibdev_netdev_name_for returns the EXTERNALLY-pinned netdev name the
 * detach-on-rename guard compares against, or NULL when the ib_device owns a
 * self-created per-rail netdev (the native backend's u4rN) that udev renames
 * by design. It MUST return NULL for the native backend: returning roce_netdev
 * there made the guard detach our own renamed netdev and hard-lock the node
 * with native_data_e2e=1 (2026-06-26). Exposed for kunit.
 */
const char *tbv_ibdev_netdev_name_for(struct tbv_state *state,
				      enum tbv_backend_type backend);
/*
 * tbv_ibdev_netdev_parent returns the device the per-rail GID netdev must be
 * parented to. It MUST be a stable, driver-owned device (the NHI/ring device the
 * ib_device already uses, dev->base.dev.parent), NOT the XDomain device: the
 * netdev must not be a direct child of xd->dev, or unregister_netdev() races
 * tb_xdomain_remove()'s child-list iteration and NULL-derefs kernfs. Exposed for
 * kunit.
 */
struct device *tbv_ibdev_netdev_parent(struct device *ib_parent,
				       struct tb_xdomain *xd);
bool tbv_gid_matches_identity(const u8 gid[16], u64 eui64, u32 ipv4_be);
/*
 * tbv_gid_identity_verdict classifies a dgid against a stored peer identity.
 * INCONCLUSIVE means the identity cannot adjudicate this dgid's address
 * family (e.g. a v4-mapped dgid against an identity whose ipv4 was 0 because
 * the peer HELLOed before DHCP) and MUST NOT count toward the all-peers-
 * valid -ENETUNREACH rejection in tbv_qp_rebind_rail_for_dgid().
 */
/*
 * How long the first HELLO may wait for the roce_netdev to get an IPv4
 * address (DHCP at boot). A HELLO sent earlier advertises identity ipv4=0,
 * which peers can never resolve a v4-mapped dgid against. Past the grace we
 * send anyway (a v6-only or address-less deployment must still negotiate)
 * and rely on the inetaddr notifier to re-HELLO when an address appears.
 */
#define TBV_IDENTITY_GRACE_MS 30000

enum tbv_identity_verdict {
	TBV_IDENTITY_NO_MATCH = 0,
	TBV_IDENTITY_MATCH,
	TBV_IDENTITY_INCONCLUSIVE,
};
enum tbv_identity_verdict tbv_gid_identity_verdict(const u8 gid[16],
						   bool identity_valid,
						   u64 eui64, u32 ipv4_be);
bool tbv_native_control_local_identity_incomplete(void);
/*
 * tbv_rail_netdev_mac derives a rail's private netdev MAC from its node_guid so
 * each rail gets a distinct RoCE GID. See kernel/native_control.c +
 * tests/rail_mac_test.c.
 */
void tbv_rail_netdev_mac(u64 node_guid, u8 mac[6]);
void tbv_native_control_identity_refresh_workfn(struct work_struct *work);
int tbv_native_control_identity_notifier_register(struct tbv_state *state);
void tbv_native_control_identity_notifier_unregister(void);
bool tbv_path_apple_tx_raw_mode(void);
bool tbv_path_apple_rx_raw_mode(void);
void tbv_path_default_config(enum tbv_backend_type backend,
			     struct tbv_path_config *cfg);
void tbv_path_init_optional_symbols(void);
void tbv_path_exit_optional_symbols(void);
/*
 * Exponential-backoff RC retransmit interval for retry @retries: base<<retries
 * capped at @qp_timeout. Exposed for the KUnit that simulates loss recovery and
 * pins the cumulative-budget invariant the flat clamp violated.
 */
unsigned long tbv_send_retry_backoff_jiffies(unsigned long qp_timeout,
					     u8 retries,
					     unsigned long base_jiffies,
					     u8 max_retries);
/*
 * Deadline-aware timeout-work arming. The reap work only CHECKS deadlines when
 * it runs; scheduling it at the flat min3 interval quantized every loss
 * recovery to TBV_READ_RESP_RETRY_MS (~100 ms ACK stalls on hardware). These
 * pure helpers compute the delay to a send's actual retransmit deadline
 * (interval-backstopped) and the reduce-only re-arm decision; exposed for the
 * send_timeout_arming KUnit.
 */
unsigned long tbv_qp_send_timeout_delay(unsigned long qp_timeout,
					unsigned long interval,
					u8 retries,
					unsigned long base_jiffies,
					u8 max_retries,
					unsigned long queued,
					unsigned long now);
bool tbv_qp_timeout_rearm_needed(bool armed, unsigned long armed_expires,
				 unsigned long new_expires, bool replace);
void tbv_path_init(struct tbv_path *path,
		   const struct tbv_path_config *cfg, struct tbv_rail *rail);
void tbv_path_reset(struct tbv_path *path);
const char *tbv_path_state_name(enum tbv_path_state state);
int tbv_path_alloc_rings(struct tbv_path *path, struct tb_xdomain *xd,
			 int requested_transmit_path);
int tbv_path_start_rings(struct tbv_path *path);
int tbv_path_enable_tunnel(struct tbv_path *path, struct tb_xdomain *xd,
			   int remote_transmit_path);
int tbv_path_disable_tunnel(struct tbv_path *path, struct tb_xdomain *xd);
void tbv_path_set_remote_rx_capacity(struct tbv_path *path, u32 rx_ring_size);
void tbv_path_add_remote_rx_credits(struct tbv_path *path, u32 credits);
void tbv_path_refund_remote_data_credits(struct tbv_path *path, u32 frames);
int tbv_path_reserve_data(struct tbv_path *path, u32 frames);
void tbv_path_release_data_reservation(struct tbv_path *path, u32 frames);
int tbv_path_send(struct tbv_path *path, const void *data, u32 len,
		  unsigned int flags,
		  tbv_path_tx_done_fn done, void *done_ctx);
int tbv_path_send_owned(struct tbv_path *path, void *data, u32 len,
			unsigned int flags,
			tbv_path_tx_done_fn done, void *done_ctx);
int tbv_path_send_marked_owned(struct tbv_path *path, void *data, u32 len,
			       u8 sof, u8 eof, unsigned int flags,
			       tbv_path_tx_done_fn done, void *done_ctx);
int tbv_path_send_owned_list_reserved(struct tbv_path *path,
				      struct list_head *frames,
				      unsigned int flags,
				      tbv_path_tx_done_fn done,
				      void *done_ctx);
int tbv_path_prepare_owned_list(struct tbv_path *path,
				struct list_head *frames,
				struct list_head *packets,
				u32 *packet_count,
				unsigned int flags,
				tbv_path_tx_done_fn done,
				void *done_ctx);
int tbv_path_enqueue_prepared_reserved(struct tbv_path *path,
				       struct list_head *packets,
				       u32 packet_count,
				       unsigned int flags);
void tbv_path_release_prepared_list_silent(struct list_head *packets,
					   int status);
int tbv_path_send_marked_fill(struct tbv_path *path, u32 len,
			      u8 sof, u8 eof, unsigned int flags,
			      tbv_path_tx_fill_fn fill, void *fill_ctx,
			      tbv_path_tx_done_fn done, void *done_ctx);
int tbv_path_send_page_stream(struct tbv_path *path,
			      const struct tbv_native_data_header *hdr,
			      u32 total_length, unsigned int flags,
			      tbv_path_tx_done_fn meta_done,
			      void *meta_done_ctx,
			      tbv_path_next_page_fn next, void *next_ctx);
void tbv_path_kick_tx(struct tbv_path *path);
void tbv_path_cancel_data_done_ctx(struct tbv_path *path,
				   tbv_path_tx_done_fn done, void *done_ctx);
void tbv_path_cancel_data_owner_ctx(struct tbv_path *path, void *owner_ctx);
void tbv_path_fence(struct tbv_path *path);
void tbv_path_destroy(struct tbv_path *path, struct tb_xdomain *xd);

const struct tbv_backend_ops *tbv_backend_get(enum tbv_backend_type type);
int tbv_link_activate_config(struct tbv_state *state, const char *name,
			     enum tbv_backend_type backend,
			     const struct tbv_cfg_link *cfg);
void tbv_link_deactivate_config(struct tbv_state *state, u32 link_id);
u32 tbv_link_count(struct tbv_state *state);
void tbv_link_debugfs_show(struct seq_file *s, struct tbv_state *state);
int tbv_debugfs_init(struct tbv_state *state);
void tbv_debugfs_exit(struct tbv_state *state);
int tbv_configfs_start(struct tbv_state *state);
void tbv_configfs_stop(struct tbv_state *state);

int tbv_core_init(struct tbv_state *state,
		  const struct tbv_resolved_config *cfg,
		  const struct tbv_tbnet_identity_config *identity_cfg);
void tbv_core_exit(struct tbv_state *state);

#endif
