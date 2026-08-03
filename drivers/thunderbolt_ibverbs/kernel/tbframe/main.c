// SPDX-License-Identifier: GPL-2.0
/*
 * tbframe: lossless frame service over Thunderbolt/USB4 XDomain DMA rings.
 * Module entry points and configuration.
 */

#define pr_fmt(fmt) "tbframe: " fmt

#include <linux/log2.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "tbframe_priv.h"

static unsigned short ring_entries = 2048;
module_param(ring_entries, ushort, 0444);
MODULE_PARM_DESC(ring_entries,
		 "TX/RX ring entries per link (power of two, 256..4096)");

/*
 * Mode B (hardware E2E) stays opt-in: the known Intel behavior of passing
 * a saturated run and then wedging newly created rings on the same
 * controller (spec §6). Effective only when the peer also advertises it.
 */
static bool e2e;
module_param(e2e, bool, 0444);
MODULE_PARM_DESC(e2e, "Advertise and use hardware E2E flow control (default n)");

static bool keepalive = true;
module_param(keepalive, bool, 0444);
MODULE_PARM_DESC(keepalive,
		 "Advertise and send session-cookie keepalive frames (default y)");

static unsigned int verify_ms = 5000;
module_param(verify_ms, uint, 0644);
MODULE_PARM_DESC(verify_ms,
		 "Level-triggered session verify interval in ms (default 5000)");

static unsigned int xmit_drain_ms = 1000;
module_param(xmit_drain_ms, uint, 0644);
MODULE_PARM_DESC(xmit_drain_ms,
		 "Bounded wait for in-flight publishers on session down; expiry poisons the link DEAD_HW");

static unsigned int teardown_warn_ms = 2000;
module_param(teardown_warn_ms, uint, 0644);
MODULE_PARM_DESC(teardown_warn_ms,
		 "Interval (ms) between teardown 'waiting for frame refs' warnings");

static unsigned int teardown_force_ms = 10000;
module_param(teardown_force_ms, uint, 0644);
MODULE_PARM_DESC(teardown_force_ms,
		 "Hard cap (ms) after which link teardown force-proceeds with a deliberate leak; 0 = wait forever");

static struct tbframe tbframe_global;
static bool tbframe_global_ready;

struct tbframe *tbframe_instance(void)
{
	return tbframe_global_ready ? &tbframe_global : NULL;
}

static int __init tbframe_init(void)
{
	struct tbframe *tf = &tbframe_global;
	int ret;

	if (!is_power_of_2(ring_entries) || ring_entries < 256 ||
	    ring_entries > 4096) {
		unsigned short fixed = clamp_t(unsigned short,
					       roundup_pow_of_two(ring_entries),
					       256, 4096);

		pr_warn("ring_entries=%u invalid; using %u\n", ring_entries,
			fixed);
		ring_entries = fixed;
	}

	tbframe_state_init(tf);
	tf->ring_entries = ring_entries;
	tf->e2e = e2e;
	tf->keepalive = keepalive;
	tf->verify_ms = verify_ms;
	tf->xmit_drain_ms = xmit_drain_ms;
	tf->teardown_warn_ms = teardown_warn_ms;
	tf->teardown_force_ms = teardown_force_ms;

	/*
	 * Not WQ_MEM_RECLAIM: session work issues tb_xdomain_request(),
	 * whose tb_cfg_request_sync() flushes the (non-reclaim) thunderbolt
	 * control workqueue -- a reclaim queue may never wait on a
	 * non-reclaim queue (check_flush_dependency). Nothing in the
	 * session path is on a memory-reclaim I/O path.
	 */
	tf->wq = alloc_workqueue("tbframe", WQ_UNBOUND, 0);
	if (!tf->wq)
		return -ENOMEM;

	/*
	 * Publish the instance only once the service is fully advertised: a
	 * client that calls tbframe_register_client() must never attach to a
	 * half-built module. Links created by an early service probe do not
	 * need the instance pointer (they carry their own tf).
	 */
	ret = tbframe_service_start(tf);
	if (ret) {
		destroy_workqueue(tf->wq);
		return ret;
	}
	tbframe_global_ready = true;

	pr_info("initialized ring_entries=%u e2e=%u keepalive=%u\n",
		tf->ring_entries, tf->e2e, tf->keepalive);
	return 0;
}
module_init(tbframe_init);

static void __exit tbframe_exit(void)
{
	struct tbframe *tf = &tbframe_global;

	/*
	 * Unpublish first so no client can attach while we tear down; then
	 * stop the service (protocol handler, service driver and every link),
	 * and only then destroy the workqueue -- by that point every link is
	 * gone and no work item can be re-armed (link_destroy shuts the
	 * verify timer down and syncs all three work items).
	 */
	tbframe_global_ready = false;
	tbframe_service_stop(tf);

	/*
	 * A registered client here would mean a module that uses our exported
	 * symbols outlived us, which the module dependency refcount makes
	 * impossible (rmmod tbframe returns -EWOULDBLOCK while tbrxe is
	 * loaded). Assert it rather than trust it: its ops vector lives in
	 * that module's text, so a stale pointer here is an NX-execute panic
	 * on the next upcall.
	 */
	down_write(&tf->client_rwsem);
	WARN_ON(tf->client_ops);
	tf->client_ops = NULL;
	tf->client_ctx = NULL;
	up_write(&tf->client_rwsem);

	destroy_workqueue(tf->wq);
}
module_exit(tbframe_exit);

MODULE_DESCRIPTION("Lossless frame service over Thunderbolt/USB4 XDomain DMA rings");
MODULE_LICENSE("GPL");
