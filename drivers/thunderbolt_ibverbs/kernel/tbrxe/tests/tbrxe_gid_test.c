// SPDX-License-Identifier: GPL-2.0
/*
 * tbrxe_gid_test.c - regression: the GID identity split that hung the
 * first two-node traffic (appmana-023/025, "Failed allocating skb").
 *
 * Peers exchange tbrxe's SELF GID out of band (perftest bootstrap), but the
 * requester's link lookup matches the dgid against peer GIDs derived from
 * the tbframe HELLO gid_eui64. If the self GID is derived from any identity
 * OTHER than the one tbframe advertised in our own HELLO
 * (tbframe_link_info.local_gid_eui64), the two derivations disagree, the
 * dgid never matches any link, rxe_init_packet() returns NULL and the
 * requester parks forever.
 *
 * The contract here: tbrxe_query_gid(index 0) must equal
 * tbrxe_gid_from_eui64(local_gid_eui64) - exactly what the peer derives
 * from our HELLO - and a requester alloc for a dgid derived from the peer's
 * HELLO must find the link and return a packet.
 */

#include <kunit/test.h>
#include <rdma/ib_verbs.h>

#include "rxe.h"
#include "rxe_loc.h"
#include "tbrxe_test_link.h"

static u8 gid_mock_buf[TBFRAME_MAX_FRAME];
static struct tbframe_frame gid_mock_frame;
static atomic_t gid_mock_allocs;

static int gid_mock_register_client(const struct tbframe_client_ops *ops,
				    void *ctx)
{
	return 0;
}

static void gid_mock_unregister_client(void)
{
}

static int gid_mock_alloc_frame(struct tbframe_link *link, u16 len,
				bool is_ctrl, struct tbframe_frame **frame)
{
	atomic_inc(&gid_mock_allocs);
	gid_mock_frame.data = gid_mock_buf;
	gid_mock_frame.len = len;
	*frame = &gid_mock_frame;
	return 0;
}

static int gid_mock_xmit(struct tbframe_link *link,
			 struct tbframe_frame *frame)
{
	return 0;
}

static void gid_mock_frame_free(struct tbframe_link *link,
				struct tbframe_frame *frame)
{
}

static const char *gid_mock_link_name(const struct tbframe_link *link)
{
	return "gidmock";
}

static void gid_mock_link_info(const struct tbframe_link *link,
			       struct tbframe_link_info *info)
{
	memset(info, 0, sizeof(*info));
}

static const struct tbrxe_transport_ops gid_mock_transport = {
	.register_client	= gid_mock_register_client,
	.unregister_client	= gid_mock_unregister_client,
	.alloc_frame		= gid_mock_alloc_frame,
	.xmit			= gid_mock_xmit,
	.frame_free		= gid_mock_frame_free,
	.link_name		= gid_mock_link_name,
	.link_info		= gid_mock_link_info,
};

static void tbrxe_gid_identity_matches_hello(struct kunit *test)
{
	static int fake_link;
	union ib_gid self, expect, peer;
	struct rxe_pkt_info pkt = {};
	struct rxe_av av = {};
	struct rxe_dev *rxe;
	struct sk_buff *skb;

	rxe = tbrxe_get_dev();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);

	atomic_set(&gid_mock_allocs, 0);
	tbrxe_set_transport_ops(&gid_mock_transport);
	tbrxe_test_link_up(&fake_link);

	/* End-to-end agreement: the GID we publish at index 0 (what
	 * userspace hands the peer through the perftest bootstrap) must be
	 * the GID the peer derives from OUR HELLO.
	 */
	KUNIT_ASSERT_EQ(test, tbrxe_query_gid(rxe, 0, &self), 0);
	tbrxe_gid_from_eui64(TBRXE_TEST_LOCAL_EUI64, &expect);
	KUNIT_EXPECT_MEMEQ(test, self.raw, expect.raw, sizeof(self.raw));

	/* Requester alloc for a dgid equal to what the peer's HELLO
	 * advertised: the link lookup must hit and produce a packet, not
	 * NULL ("Failed allocating skb" park).
	 */
	tbrxe_gid_from_eui64(TBRXE_TEST_PEER_EUI64, &peer);
	memcpy(&av.grh.dgid, &peer, sizeof(peer));
	skb = rxe_init_packet(rxe, &av, 128, &pkt);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, skb);
	KUNIT_EXPECT_EQ(test, 1, atomic_read(&gid_mock_allocs));
	if (!IS_ERR_OR_NULL(skb))
		kfree_skb(skb);

	tbrxe_test_link_down(&fake_link);
	tbrxe_set_transport_ops(NULL);
}

static struct kunit_case tbrxe_gid_cases[] = {
	KUNIT_CASE(tbrxe_gid_identity_matches_hello),
	{}
};

static struct kunit_suite tbrxe_gid_suite = {
	.name = "tbrxe_gid",
	.test_cases = tbrxe_gid_cases,
};

kunit_test_suites(&tbrxe_gid_suite);
