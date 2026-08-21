// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * tbrxe_tbframe_glue.c - production binding of the tbrxe transport ops to
 * the real tbframe module exports.
 *
 * Compiled out under CONFIG_KUNIT (see Makefile): the KUnit kernel has no
 * tbframe module, and built-in undefined symbols would fail the vmlinux
 * link. Tests install mock ops through tbrxe_set_transport_ops() instead.
 *
 * In the packaged build this file is what makes thunderbolt_frame_rxe.ko
 * depend on thunderbolt_frame.ko at load time; the Makefile passes the frame
 * module's Module.symvers via
 * KBUILD_EXTRA_SYMBOLS when it exists (tbframe builds in parallel, so a
 * missing symvers only downgrades modpost to "undefined!" warnings).
 */

#include "tbrxe_frame.h"

static const struct tbrxe_transport_ops tbrxe_tbframe_ops = {
	.register_client	= tbframe_register_client,
	.unregister_client	= tbframe_unregister_client,
	.alloc_frame		= tbframe_alloc_frame,
	.xmit			= tbframe_xmit,
	.frame_free		= tbframe_frame_free,
	.link_name		= tbframe_link_name,
	.link_info		= tbframe_link_info,
};

const struct tbrxe_transport_ops *tbrxe_builtin_transport(void)
{
	return &tbrxe_tbframe_ops;
}
