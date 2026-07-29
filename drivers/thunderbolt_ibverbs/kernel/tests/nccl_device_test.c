// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: the contract NCCL's net_ib transport requires of the usb4_rdma verbs
 * device. usb4_rdma is thunderbolt_net with ibverbs instead of TCP: one RoCE
 * device per Thunderbolt link, each with its own per-link netdev/subnet, and
 * NCCL selects the device whose GID subnet matches the peer -- exactly as IP
 * routing selects the tb-ch<N> netdev for tbnet.
 *
 * These cases model net_ib's actual requirements (NCCL
 * src/transport/net_ib/init.cc) so a driver change that breaks NCCL's
 * device acceptance or selection trips a test here. Run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <rdma/ib_verbs.h>
#include "../tbv.h"

/* ---- net_ib device-acceptance filter (init.cc: per-port loop) ----
 * For each device port net_ib keeps it only if the port is ACTIVE and the link
 * layer is InfiniBand or Ethernet (RoCE); anything else is skipped. usb4_rdma
 * presents Ethernet + ACTIVE once its rail is up, so net_ib must accept it.
 */
static bool nccl_net_ib_accepts_port(enum ib_port_state state,
				     enum rdma_link_layer ll)
{
	if (state != IB_PORT_ACTIVE)
		return false;
	return ll == IB_LINK_LAYER_INFINIBAND || ll == IB_LINK_LAYER_ETHERNET;
}

static void nccl_accepts_active_roce_rail(struct kunit *test)
{
	/* tbv_query_port reports ACTIVE + tbv_get_link_layer reports Ethernet. */
	KUNIT_EXPECT_TRUE(test,
		nccl_net_ib_accepts_port(IB_PORT_ACTIVE, IB_LINK_LAYER_ETHERNET));
	/* A rail that has not negotiated reports DOWN -> net_ib skips it. */
	KUNIT_EXPECT_FALSE(test,
		nccl_net_ib_accepts_port(IB_PORT_DOWN, IB_LINK_LAYER_ETHERNET));
	/* A non-IB/Ethernet link layer would be skipped -- usb4_rdma must not
	 * present one (the RoCE GID path is what net_ib drives). */
	KUNIT_EXPECT_FALSE(test,
		nccl_net_ib_accepts_port(IB_PORT_ACTIVE,
					 IB_LINK_LAYER_UNSPECIFIED));
}

/* ---- net_ib selects the device whose GID subnet matches the peer ----
 * net_ib addresses a RoCE peer by GID and prefers the device whose GID shares
 * the peer's subnet (NCCL_IB_ADDR_RANGE). On a chain node with two rails this
 * routes each connection onto the correct cabled neighbour -- the ibverbs
 * analogue of tbnet's per-link tb-ch routing. Model that subnet test.
 */
static bool nccl_gid_subnet_match(const u8 a[16], const u8 b[16],
				  unsigned int prefix_bits)
{
	unsigned int full = prefix_bits / 8, rem = prefix_bits % 8;

	if (prefix_bits > 128)
		return false;
	if (full && memcmp(a, b, full))
		return false;
	if (rem) {
		u8 mask = (u8)(0xff << (8 - rem));

		if ((a[full] & mask) != (b[full] & mask))
			return false;
	}
	return true;
}

static void nccl_select_same_link_64(struct kunit *test)
{
	/* both ends of one link share the per-link /64 -> selectable */
	const u8 a[16] = { 0xfd,0,0x99,0,0,2,0,7, 0x5a,0x11,0x22,0xff,0xfe,0xb7,0x75,0xb1 };
	const u8 b[16] = { 0xfd,0,0x99,0,0,2,0,7, 0x12,0x21,0x1b,0x64,0xde,0x7a,0,1 };

	KUNIT_EXPECT_TRUE(test, nccl_gid_subnet_match(a, b, 64));
}

static void nccl_select_other_link_64(struct kunit *test)
{
	/* a peer on a different link (different /64) must NOT select this rail */
	const u8 a[16] = { 0xfd,0,0x99,0,0,2,0,7, 0,0,0,0,0,0,0,1 };
	const u8 c[16] = { 0xfd,0,0x99,0,0,2,0,9, 0,0,0,0,0,0,0,1 };

	KUNIT_EXPECT_FALSE(test, nccl_gid_subnet_match(a, c, 64));
}

static void nccl_select_link_127(struct kunit *test)
{
	/* a /127 link: the two ends share the /127 but are distinct /128s */
	const u8 lo[16] = { 0xfd,0,0x99,1,0,0,0,0, 0,0,0,0,0,0,0,0x08 };
	const u8 hi[16] = { 0xfd,0,0x99,1,0,0,0,0, 0,0,0,0,0,0,0,0x09 };

	KUNIT_EXPECT_TRUE(test, nccl_gid_subnet_match(lo, hi, 127));
	KUNIT_EXPECT_FALSE(test, nccl_gid_subnet_match(lo, hi, 128));
}

static void nccl_select_off_link_127(struct kunit *test)
{
	/* ::8 and ::a are different /127s (::8-9 vs ::a-b) */
	const u8 a[16] = { 0xfd,0,0x99,1,0,0,0,0, 0,0,0,0,0,0,0,0x08 };
	const u8 c[16] = { 0xfd,0,0x99,1,0,0,0,0, 0,0,0,0,0,0,0,0x0a };

	KUNIT_EXPECT_FALSE(test, nccl_gid_subnet_match(a, c, 127));
}

/* ---- two rails must be DISTINCT devices so net_ib does not merge them ----
 * NCCL_IB_MERGE_NICS fuses devices it considers identical; a chain node's two
 * rails must present DISTINCT GIDs (the old shared-LAN-netdev gave both rails
 * one flat GID, which is the bug). tbv_rail_netdev_mac -- the real driver
 * function -- is what makes each rail's RoCE GID distinct.
 */
static void nccl_rails_have_distinct_gids(struct kunit *test)
{
	static const u8 host_uuid[16] = {
		0x7e, 0x12, 0xc9, 0x64, 0x0b, 0xd5, 0x48, 0x93,
		0xa0, 0x8e, 0x27, 0x4c, 0xf1, 0x66, 0x39, 0x0d,
	};
	static const u8 remote0_uuid[16] = {
		0x9f, 0x02, 0x11, 0x5c, 0x3a, 0x77, 0x41, 0xd2,
		0x88, 0x04, 0xe6, 0x2b, 0x19, 0xc5, 0x70, 0x3e,
	};
	static const u8 remote1_uuid[16] = {
		0x1b, 0xe4, 0x9d, 0x08, 0x52, 0xa1, 0x4f, 0x66,
		0x93, 0x7c, 0x0d, 0xf8, 0x24, 0x6a, 0xb1, 0x57,
	};
	u32 h = tbv_host_identity_hash(host_uuid);
	u8 r0[6], r1[6];

	/* one node, two rails toward different peers (see ibdev.c) */
	tbv_rail_netdev_mac(h,
			    tbv_rail_link_identity_hash(remote0_uuid, 1, 0), r0);
	tbv_rail_netdev_mac(h,
			    tbv_rail_link_identity_hash(remote1_uuid, 3, 0), r1);
	KUNIT_EXPECT_NE(test, 0, memcmp(r0, r1, 6));
}

static struct kunit_case tbv_nccl_device_test_cases[] = {
	KUNIT_CASE(nccl_accepts_active_roce_rail),
	KUNIT_CASE(nccl_select_same_link_64),
	KUNIT_CASE(nccl_select_other_link_64),
	KUNIT_CASE(nccl_select_link_127),
	KUNIT_CASE(nccl_select_off_link_127),
	KUNIT_CASE(nccl_rails_have_distinct_gids),
	{}
};

static struct kunit_suite tbv_nccl_device_test_suite = {
	.name = "tbv_nccl_device",
	.test_cases = tbv_nccl_device_test_cases,
};
kunit_test_suite(tbv_nccl_device_test_suite);

MODULE_LICENSE("GPL");
