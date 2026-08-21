// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * tbrxe userspace verbs provider.
 *
 * thunderbolt_frame_rxe.ko (tbrxe.ko before the module rename) is the
 * rxe-derived Thunderbolt/USB4 soft-RDMA engine. It speaks
 * the stock rxe uverbs ABI but registers its devices under a private RDMA
 * driver id: the kernel's ib_unregister_driver() unregisters by driver id
 * with no owner check, so sharing RDMA_DRIVER_RXE meant `rmmod rdma_rxe`
 * swept live tbrxe devices off the node.
 *
 * This provider is the stock rxe provider compiled under a different
 * identity: providers/rxe/rxe.c is #included wholesale below, with two
 * preprocessor rebindings applied first.
 *
 *  - RDMA_DRIVER_RXE is rebound to tbrxe's private driver id. rxe.c uses
 *    the token in exactly two places: the driver-id entry of its match
 *    table (so this provider claims only tbrxe devices) and the driver id
 *    handed to verbs_init_and_alloc_context() (so every uverbs ioctl
 *    header carries the id the kernel expects; uverbs_ioctl.c rejects a
 *    mismatch with EINVAL).
 *
 *  - VERBS_NAME_MATCH is rebound to a duplicate driver-id entry so the
 *    match table carries no name match at all. The stock provider's
 *    VERBS_NAME_MATCH("rxe", ...) fallback must not reappear here: it
 *    would contend with the distro's librxe for in-tree rxe devices
 *    (rxe_lan). Matching stays driver-id only, in both directions.
 *
 * The enum declaring RDMA_DRIVER_RXE has to be parsed before the token is
 * rebound, so the verbs headers (which pull in the uapi ioctl headers) are
 * included first; their include guards turn rxe.c's own includes of them
 * into no-ops.
 *
 * Compiling the build tree's own providers/rxe sources, rather than
 * carrying a copy, keeps the provider automatically in step with whatever
 * rdma-core version the distro ships (PABI and internal driver API alike).
 */

#include <config.h>

#include <infiniband/driver.h>
#include <infiniband/verbs.h>

/* Must match RDMA_DRIVER_USB4_RDMA in rxe/rxe.h: ASCII "USB4",
 * far outside the upstream enum rdma_driver_id allocation. */
#define TBRXE_DRIVER_ID 0x55534234

#define RDMA_DRIVER_RXE TBRXE_DRIVER_ID

#undef VERBS_NAME_MATCH
#define VERBS_NAME_MATCH(_name_prefix, _data) VERBS_DRIVER_ID(TBRXE_DRIVER_ID)

#include "../rxe/rxe.c"

/*
 * rxe.c's PROVIDER_DRIVER(rxe, rxe_dev_ops) already registers the (now
 * rebranded) ops from a constructor at dlopen time; this alias only gives
 * the library a registration symbol under its own name for packaging
 * checks. Note the registered ops still carry .name = "rxe", which only
 * shows up in libibverbs stderr warnings; matching is by driver id.
 */
extern const struct verbs_device_ops verbs_provider_tbrxe
	__attribute__((alias("rxe_dev_ops")));
