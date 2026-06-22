// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for thunderbolt_ibverbs per-rail ib_device naming.
 *
 * Drives tbv_ibdev_rail_name_index() -- the CALLER that reads fields off a
 * struct tbv_rail -- because that is where the defect lived: which rail-key
 * field the name is keyed on. Two mock rails model appmana-022's neighbours
 * (021 and 023): identical tb domain, native_lane and local_adapter, differing
 * ONLY in key.route (downstream port 0x3 vs 0x1). The contract is "two
 * neighbours -> two names"; the test is red while the caller keys on
 * local_adapter (both 0 -> usb4_rdma0 -> the 2nd ib_register_device() is
 * -ENFILE/-23) and green once it keys on the route. The test body does not
 * change between the two -- only the code under test does.
 *
 * Built into the module on a CONFIG_KUNIT kernel; run via
 * drivers/thunderbolt_ibverbs/tools/run-kunit.sh (kunit.py on an overlaid tree).
 */
#include <kunit/test.h>
#include <linux/thunderbolt.h>
#include "../tbv.h"

#define L TBV_NATIVE_MAX_LANES

/* Build a native rail to a peer reached over @route on tb domain 0. */
static void mock_rail(struct tbv_rail *rail, struct tbv_peer *peer,
		      struct tb_xdomain *xd, struct tb *dom, u64 route)
{
	dom->index = 0;
	xd->tb = dom;
	peer->backend = TBV_BACKEND_NATIVE;
	peer->xd = xd;
	rail->peer = peer;
	rail->key.route = route;
	rail->key.local_adapter = 0;	/* 0 for every native rail -- the trap */
	rail->native_lane = 0;
}

static void tbv_naming_two_neighbours_must_differ(struct kunit *test)
{
	struct tb dom_a = {0}, dom_b = {0};
	struct tb_xdomain xd_a = {0}, xd_b = {0};
	struct tbv_peer p021 = {0}, p023 = {0};
	struct tbv_rail to_021 = {0}, to_023 = {0};
	int a, b;

	mock_rail(&to_021, &p021, &xd_a, &dom_a, 0x3);	/* neighbour 021 */
	mock_rail(&to_023, &p023, &xd_b, &dom_b, 0x1);	/* neighbour 023 */

	a = tbv_ibdev_rail_name_index(&to_021);
	b = tbv_ibdev_rail_name_index(&to_023);

	KUNIT_EXPECT_GE(test, a, 0);
	KUNIT_EXPECT_GE(test, b, 0);
	/* MUST differ or the 2nd ib_register_device() returns -ENFILE */
	KUNIT_EXPECT_NE(test, a, b);
}

/* a bonded link's two lanes to ONE peer (same route) stay distinct by lane */
static void tbv_naming_lanes_disambiguate(struct kunit *test)
{
	struct tb dom = {0};
	struct tb_xdomain xd = {0};
	struct tbv_peer peer = {0};
	struct tbv_rail lane0 = {0}, lane1 = {0};

	mock_rail(&lane0, &peer, &xd, &dom, 0x3);
	mock_rail(&lane1, &peer, &xd, &dom, 0x3);
	lane1.native_lane = 1;

	KUNIT_EXPECT_NE(test, tbv_ibdev_rail_name_index(&lane0),
			tbv_ibdev_rail_name_index(&lane1));
}

/* pure arithmetic guard: out-of-range lane is rejected */
static void tbv_naming_errors(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tbv_ibdev_name_index(-1, 0, 0, 0, L), -ENODEV);
	KUNIT_EXPECT_EQ(test, tbv_ibdev_name_index(0, 0, L, 0, L), -ERANGE);
}

/*
 * On a netdev rename we must KEEP the ib_device bound to its own per-rail
 * GID-only netdev (kernel name u4r<N> -> udev renames it to tbr-<peer> per link;
 * that rename is BY DESIGN). Detaching there both drops the rail's GID and, since
 * NETDEV_CHANGENAME runs under rtnl, calls unregister_netdev() -> rtnl_lock()
 * recursively -> deadlock (wedged appmana-018 on cable plug-in, 2026-06-20).
 * Only an externally pinned roce_netdev (expected_name != NULL) renamed AWAY from
 * its pinned name should detach.
 */
static void tbv_rename_keep_own_gid_netdev(struct kunit *test)
{
	/* our per-rail netdev: no pinned name -> keep across any rename */
	KUNIT_EXPECT_TRUE(test, tbv_netdev_rename_keep(NULL, "tbr-appmana027"));
	KUNIT_EXPECT_TRUE(test, tbv_netdev_rename_keep(NULL, "u4r0"));
}

static void tbv_rename_keep_pinned_match(struct kunit *test)
{
	/* pinned roce_netdev still at its name -> keep */
	KUNIT_EXPECT_TRUE(test, tbv_netdev_rename_keep("br0.lan", "br0.lan"));
}

static void tbv_rename_detach_pinned_diverged(struct kunit *test)
{
	/* pinned roce_netdev renamed away -> detach (do not keep) */
	KUNIT_EXPECT_FALSE(test, tbv_netdev_rename_keep("br0.lan", "eth9"));
}

/*
 * The per-rail GID netdev MUST NOT be parented to the XDomain device. xd->dev is
 * owned/removed by the thunderbolt core; parenting the netdev there makes it a
 * *sibling* of the tb_services in xd->dev's child list, so unregister_netdev()
 * (in our service .remove, run from inside tb_xdomain_remove's
 * device_for_each_child_reverse) mutates that child list mid-iteration ->
 * kernfs_find_and_get_ns NULL deref -> chain-wide crash cascade on any neighbour
 * disconnect (kdump appmana-002 2026-06-22). It must instead use the stable
 * driver-owned NHI/ring device that the ib_device already parents to
 * (dev->base.dev.parent), so its teardown is ordered and self-contained.
 */
static void tbv_netdev_parent_is_stable_not_xdomain(struct kunit *test)
{
	struct tb_xdomain xd = {0};
	struct device nhi_dev = {0};	/* tb_ring_dma_device: stable, driver-owned */

	/* red while it returns &xd.dev; green once it returns the stable parent */
	KUNIT_EXPECT_PTR_NE(test,
			    tbv_ibdev_netdev_parent(&nhi_dev, &xd), &xd.dev);
	KUNIT_EXPECT_PTR_EQ(test,
			    tbv_ibdev_netdev_parent(&nhi_dev, &xd), &nhi_dev);
}

static struct kunit_case tbv_ibdev_naming_test_cases[] = {
	KUNIT_CASE(tbv_naming_two_neighbours_must_differ),
	KUNIT_CASE(tbv_naming_lanes_disambiguate),
	KUNIT_CASE(tbv_naming_errors),
	KUNIT_CASE(tbv_rename_keep_own_gid_netdev),
	KUNIT_CASE(tbv_rename_keep_pinned_match),
	KUNIT_CASE(tbv_rename_detach_pinned_diverged),
	KUNIT_CASE(tbv_netdev_parent_is_stable_not_xdomain),
	{}
};

static struct kunit_suite tbv_ibdev_naming_test_suite = {
	.name = "thunderbolt_ibverbs_ibdev_naming",
	.test_cases = tbv_ibdev_naming_test_cases,
};
kunit_test_suite(tbv_ibdev_naming_test_suite);
