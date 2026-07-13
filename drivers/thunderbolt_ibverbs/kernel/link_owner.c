// SPDX-License-Identifier: GPL-2.0
/*
 * Runtime link data-path owner handoff (usb4_rdma <-> thunderbolt_net).
 *
 * See link_owner.h for the arbitration model and the post-mortem it encodes.
 * This file is the bounded workqueue apply behind the writable link_owner
 * module parameter: it toggles ONLY tbv's own DMA tunnels (the same
 * tbv_path_disable_tunnel()/native-negotiation machinery every peer re-HELLO
 * already exercises) and publishes/unpublishes the per-rail ib_devices.
 * thunderbolt_net is never touched -- its session-verify notices the freed
 * link and (re)runs its spec LOGIN on its own.
 *
 * No RTNL, no netdev notifiers (the 0.2.32 RTNL-self-deadlock class is
 * structurally excluded), no module reload, no ICM interaction.
 */

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/thunderbolt.h>
#include <linux/workqueue.h>

#include "tbv.h"

/* Rails per apply pass; 2 peers x TBV_NATIVE_MAX_LANES is the fleet maximum. */
#define TBV_LINK_OWNER_MAX_RAILS 32

const char *tbv_link_owner_name(enum tbv_link_owner owner)
{
	return owner == TBV_LINK_OWNER_TBNET ? "tbnet" : "rdma";
}

static void tbv_link_owner_release_rail(struct tbv_state *state,
					struct tbv_rail *rail)
{
	struct tbv_peer *peer = rail->peer;
	int ret;

	/* Unpublish the usb4_rdma device first so consumers fail fast. */
	tbv_ibdev_rail_event(state, rail, false);

	mutex_lock(&peer->control_lock);
	ret = tbv_path_disable_tunnel(&rail->path, peer->xd);
	mutex_unlock(&peer->control_lock);
	if (ret)
		pr_warn("link_owner=tbnet: disable tunnel route=0x%llx rail=0x%x failed: %d\n",
			rail->key.route, rail->rail_id, ret);
	else
		pr_info("link_owner=tbnet: released tunnel route=0x%llx rail=0x%x\n",
			rail->key.route, rail->rail_id);
}

/*
 * One apply pass: snapshot the rails whose tunnel state disagrees with
 * @desired (planner != NONE), then act outside state->lock (tunnel and
 * ib-device operations sleep). Bounded and idempotent: releasing moves a rail
 * to RING_STARTED, claiming queues its native negotiation work exactly once;
 * re-running the pass with matching state plans NONE for every rail.
 */
static void tbv_link_owner_work_fn(struct work_struct *work)
{
	struct tbv_state *state = container_of(work, struct tbv_state,
					       link_owner_work);
	struct tbv_rail *rails[TBV_LINK_OWNER_MAX_RAILS];
	enum tbv_link_owner desired;
	struct tbv_peer *peer;
	unsigned int count = 0;
	unsigned int i;
	bool overflow = false;

	desired = READ_ONCE(state->link_owner_desired);
	WRITE_ONCE(state->link_owner, desired);

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;

		list_for_each_entry(rail, &peer->rails, node) {
			enum tbv_link_owner_action action;

			if (rail->removing)
				continue;
			action = tbv_link_owner_plan(desired,
						     rail->path.state ==
							     TBV_PATH_TUNNEL_ENABLED);
			if (action == TBV_LINK_ACTION_NONE)
				continue;
			/*
			 * CLAIM only makes sense for a rail whose rings are
			 * started; anything earlier re-converges through the
			 * normal probe/negotiation flow with the gate open.
			 */
			if (action == TBV_LINK_ACTION_CLAIM_TBV &&
			    rail->path.state != TBV_PATH_RING_STARTED)
				continue;
			if (count >= TBV_LINK_OWNER_MAX_RAILS) {
				overflow = true;
				break;
			}
			refcount_inc(&rail->refcnt);
			rails[count++] = rail;
		}
		if (overflow)
			break;
	}
	mutex_unlock(&state->lock);

	for (i = 0; i < count; i++) {
		struct tbv_rail *rail = rails[i];

		if (desired == TBV_LINK_OWNER_TBNET) {
			tbv_link_owner_release_rail(state, rail);
		} else {
			/*
			 * Reclaim through the existing native negotiation
			 * work: full re-HELLO, tunnel enable (the gate is
			 * open again) and ib_device publish.
			 */
			pr_info("link_owner=rdma: reclaiming tunnel route=0x%llx rail=0x%x\n",
				rail->key.route, rail->rail_id);
			tbv_native_control_queue_rail(state, rail);
		}
		tbv_rail_put(rail);
	}

	if (overflow) {
		pr_warn("link_owner: more than %u actionable rails; re-queueing apply\n",
			TBV_LINK_OWNER_MAX_RAILS);
		queue_work(state->workqueue, &state->link_owner_work);
	}

	pr_info("link_owner=%s applied (%u rail%s toggled)\n",
		tbv_link_owner_name(desired), count, count == 1 ? "" : "s");
}

void tbv_link_owner_init(struct tbv_state *state, enum tbv_link_owner owner)
{
	INIT_WORK(&state->link_owner_work, tbv_link_owner_work_fn);
	WRITE_ONCE(state->link_owner_desired, owner);
	WRITE_ONCE(state->link_owner, owner);
}

/*
 * Request an owner change at runtime (module param store). Queues the bounded
 * apply on the driver workqueue; safe to call repeatedly (idempotent planner).
 */
int tbv_link_owner_request(struct tbv_state *state, enum tbv_link_owner desired)
{
	if (!state->workqueue)
		return -ENODEV;

	WRITE_ONCE(state->link_owner_desired, desired);
	queue_work(state->workqueue, &state->link_owner_work);
	return 0;
}
