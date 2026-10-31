/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared fake-link helpers for the tbrxe KUnit suites: drive the tbframe
 * client upcalls directly (there is no tbframe module in the KUnit kernel).
 *
 * Per-link device model (wire-spec section 8): every link_up publishes an
 * ib_device for that link, bound to a GID-anchor netdev; a terminal
 * link_down (CLOSED) unpublishes it. Tests reach the device with
 * tbrxe_link_device() and its netdev with ib_device_get_netdev().
 */
#ifndef TBRXE_TEST_LINK_H
#define TBRXE_TEST_LINK_H

#include <kunit/test.h>
#include <linux/delay.h>
#include <linux/inetdevice.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <rdma/ib_cache.h>

#include "../tbrxe_frame.h"

/* The identity this side "advertised in its own HELLO" and the peer's. */
#define TBRXE_TEST_LOCAL_EUI64	0x005cdcfffe4360e9ull
#define TBRXE_TEST_PEER_EUI64	0x00a1b2fffec3d4e5ull

static inline void
tbrxe_test_link_up_window_ids(void *fake_link, u16 data_window,
			      u64 local_eui64, u64 peer_eui64)
{
	struct tbframe_link_info info = {
		.gid_eui64	= peer_eui64,
		.local_gid_eui64 = local_eui64,
		.rx_ring_entries = data_window + TBFRAME_CTRL_RESERVE,
		.data_window	= data_window,
		.max_payload	= TBFRAME_MAX_FRAME,
		.width		= 1,
		.speed		= 20,
		.remote_name	= "kunitpeer",
	};

	tbrxe_frame_client_ops()->link_up(NULL, fake_link, &info);
}

static inline void tbrxe_test_link_up_window(void *fake_link, u16 data_window)
{
	tbrxe_test_link_up_window_ids(fake_link, data_window,
				      TBRXE_TEST_LOCAL_EUI64,
				      TBRXE_TEST_PEER_EUI64);
}

static inline void tbrxe_test_link_up(void *fake_link)
{
	tbrxe_test_link_up_window(fake_link, 1984);
}

/*
 * Terminal link_down + fence. Unpublishing is asynchronous
 * (ib_unregister_device_queued), so tests that want the device, its netdev
 * and the link record actually gone on return must drain.
 */
static inline void tbrxe_test_link_down(void *fake_link)
{
	tbrxe_frame_client_ops()->link_down(NULL, fake_link,
					    TBFRAME_DOWN_CLOSED);
	tbrxe_frame_drain();
}

static inline struct rxe_dev *tbrxe_test_dev(void *fake_link)
{
	return tbrxe_link_device(fake_link);
}

/* A non-local wire destination: the peer identity's link-local. */
static inline void tbrxe_test_peer_gid(union ib_gid *gid)
{
	memset(gid, 0, sizeof(*gid));
	gid->raw[0] = 0xfe;
	gid->raw[1] = 0x80;
	put_unaligned_be64(TBRXE_TEST_PEER_EUI64, &gid->raw[8]);
}

/* Bring the device's GID-anchor netdev up so IPv6 addrconf creates the
 * identity link-local address, then wait for ib_core to turn it into a
 * GID table entry. Returns 0 and fills @gid/@gid_index on success.
 */
static inline int tbrxe_test_gid_eui64(struct rxe_dev *rxe, union ib_gid *gid,
				       u8 *gid_index, u64 local_eui64)
{
	struct ib_device *ibdev = &rxe->ib_dev;
	const struct ib_gid_attr *attr;
	struct net_device *ndev;
	int i;

	ndev = ib_device_get_netdev(ibdev, 1);
	if (!ndev)
		return -ENODEV;

	rtnl_lock();
	dev_open(ndev, NULL);
	rtnl_unlock();

	memset(gid, 0, sizeof(*gid));
	gid->raw[0] = 0xfe;
	gid->raw[1] = 0x80;
	put_unaligned_be64(local_eui64, &gid->raw[8]);

	for (i = 0; i < 500; i++) {
		attr = rdma_find_gid_by_port(ibdev, gid,
					     IB_GID_TYPE_ROCE_UDP_ENCAP,
					     1, NULL);
		if (!IS_ERR(attr)) {
			*gid_index = attr->index;
			rdma_put_gid_attr(attr);
			dev_put(ndev);
			return 0;
		}
		msleep(10);
	}
	dev_put(ndev);
	return -ETIMEDOUT;
}

static inline int tbrxe_test_gid(struct rxe_dev *rxe, union ib_gid *gid,
				 u8 *gid_index)
{
	return tbrxe_test_gid_eui64(rxe, gid, gid_index,
				    TBRXE_TEST_LOCAL_EUI64);
}

#endif /* TBRXE_TEST_LINK_H */
