/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/*
 * tbrxe_frame.h - the tbframe transport boundary of the tbrxe driver.
 *
 * This header replaces rxe_net.h. The rxe engine's four transport downcalls
 * (rxe_init_packet / rxe_prepare / rxe_xmit_packet / rxe_parent_name, declared
 * in rxe_loc.h) are implemented in tbrxe_frame.c on top of the tbframe
 * contract (../tbframe/tbframe.h, the code form of
 * docs/tbframe-tbrxe-wire-spec.md).
 *
 * All tbframe_* symbols are reached through struct tbrxe_transport_ops so the
 * KUnit tests can run against a mock/null tbframe. The production binding
 * lives in tbrxe_tbframe_glue.c (compiled out under CONFIG_KUNIT).
 */

#ifndef TBRXE_FRAME_H
#define TBRXE_FRAME_H

#include "../tbframe/tbframe.h"

struct rxe_dev;
union ib_gid;

/*
 * Ops indirection over the tbframe downcall surface. One-to-one with the
 * tbframe_* exports; see tbframe.h for semantics.
 */
struct tbrxe_transport_ops {
	int (*register_client)(const struct tbframe_client_ops *ops, void *ctx);
	void (*unregister_client)(void);
	int (*alloc_frame)(struct tbframe_link *link, u16 len, bool is_ctrl,
			   struct tbframe_frame **frame);
	int (*xmit)(struct tbframe_link *link, struct tbframe_frame *frame);
	void (*frame_free)(struct tbframe_link *link,
			   struct tbframe_frame *frame);
	const char *(*link_name)(const struct tbframe_link *link);
	void (*link_info)(const struct tbframe_link *link,
			  struct tbframe_link_info *info);
};

/*
 * Production binding to the real tbframe module. Weak: absent in KUnit
 * builds (tbrxe_tbframe_glue.c is compiled out), where it resolves to NULL
 * and tbrxe runs loopback-only until a test installs mock ops.
 */
const struct tbrxe_transport_ops *tbrxe_builtin_transport(void);

/* Test hook: swap the transport ops (NULL restores the null transport). */
void tbrxe_set_transport_ops(const struct tbrxe_transport_ops *ops);

/* Test hook: the client ops tbrxe registers with tbframe (KUnit drives the
 * link_up/link_down/rx upcalls directly against them).
 */
const struct tbframe_client_ops *tbrxe_frame_client_ops(void);

/* Client lifecycle, driven from rxe.c module init/exit. The self identity
 * (and with it ib_device publication via tbrxe_publish) is deferred to the
 * first tbframe link_up: the self GID must be the identity tbframe
 * advertised in our own HELLO, so both ends derive the same GID.
 */
void tbrxe_frame_init(struct rxe_dev *rxe);
int tbrxe_frame_register(struct rxe_dev *rxe);
void tbrxe_frame_unregister(void);

/*
 * GID derivation per wire-spec section 8: a per-link ULA built from an
 * EUI-64. Mirrors the fleet convention (fd-prefixed /64 + EUI-64 interface
 * id, see packaging/udev/tbv-rdma-addr-lib.sh and the legacy kernel/ibdev.c
 * RAIL_EUI64 identity) in FORMAT only; no identity machinery is imported.
 */
void tbrxe_gid_from_eui64(u64 eui64, union ib_gid *gid);

/* Local (self) GID table backing rxe_query_gid; index 0 is the self GID. */
int tbrxe_query_gid(struct rxe_dev *rxe, int index, union ib_gid *gid);
bool tbrxe_gid_is_local(struct rxe_dev *rxe, const union ib_gid *gid);

#endif /* TBRXE_FRAME_H */
