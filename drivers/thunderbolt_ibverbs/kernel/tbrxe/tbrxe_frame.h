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
 * Device model (wire-spec section 8, normative): one ib_device per tbframe
 * link, published at that link's first link_up and unpublished on terminal
 * link_down, each bound to a GID-anchor netdev whose IPv6 addresses ib_core
 * turns into the RoCE GID table. The driver carries no GID identity and no
 * peer table: the device IS the link.
 *
 * All tbframe_* symbols are reached through struct tbrxe_transport_ops so the
 * KUnit tests can run against a mock/null tbframe. The production binding
 * lives in tbrxe_tbframe_glue.c (compiled out under CONFIG_KUNIT).
 */

#ifndef TBRXE_FRAME_H
#define TBRXE_FRAME_H

#include "../tbframe/tbframe.h"

struct rxe_dev;

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

/* The ib_device published for one tbframe link (NULL when none). */
struct rxe_dev *tbrxe_link_device(struct tbframe_link *tblink);

#ifdef CONFIG_KUNIT
/* Test observability: the link's live aggregate admission charge. */
u32 tbrxe_link_unacked(struct rxe_dev *rxe);
#endif

/*
 * dealloc_driver tail: release the link record and GID-anchor netdev that
 * belong to @rxe. Called from rxe_dealloc(), the last op ib_core invokes on
 * a device, because unpublishing is asynchronous
 * (ib_unregister_device_queued) and nothing tied to the device's lifetime
 * may be freed before then. No-op for a device that never got a record.
 */
void tbrxe_link_release(struct rxe_dev *rxe);

/*
 * Wait for every queued unpublish to have run its dealloc_driver. Sleeps;
 * callable from module exit and from tests, never from a tbframe upcall.
 */
void tbrxe_frame_drain(void);

/* Client lifecycle, driven from rxe.c module init/exit. Devices are
 * created per link at link_up; the module owns none of its own.
 * tbrxe_frame_unregister() is a full fence: on return no upcall, no queued
 * unregistration and no driver op of this module can still be running.
 */
int tbrxe_frame_register(void);
void tbrxe_frame_unregister(void);

#endif /* TBRXE_FRAME_H */
