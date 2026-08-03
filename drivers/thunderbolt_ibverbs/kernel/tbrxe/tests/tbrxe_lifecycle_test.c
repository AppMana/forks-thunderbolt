// SPDX-License-Identifier: GPL-2.0
/*
 * tbrxe_lifecycle_test.c - module/device lifecycle regressions.
 *
 * Module load/unload itself is not reachable from KUnit, but everything the
 * unload path is made of is:
 *
 *   - the driver id tbrxe registers with, which is the namespace
 *     ib_unregister_driver() sweeps (drivers/infiniband/core/device.c:1639);
 *   - repeated publish/unpublish of the same link (the reload analogue);
 *   - tbrxe_frame_unregister() as a fence: it must unpublish every leftover
 *     link and return with no record, no netdev and no ib_device left, and
 *     the module must be re-registerable afterwards;
 *   - unpublish while another kernel context holds a device reference, which
 *     is what makes the asynchronous teardown ordering observable
 *     (disable_device()'s unreg_completion fence, device.c:1297-1330).
 */

#include <kunit/test.h>
#include <linux/netdevice.h>
#include <rdma/ib_verbs.h>

#include "rxe.h"
#include "tbrxe_test_link.h"

static int lifecycle_register_client(const struct tbframe_client_ops *ops,
				     void *ctx)
{
	return 0;
}

static void lifecycle_unregister_client(void)
{
}

static int lifecycle_alloc_frame(struct tbframe_link *link, u16 len,
				 bool is_ctrl, struct tbframe_frame **frame)
{
	return -ENETDOWN;
}

static int lifecycle_xmit(struct tbframe_link *link,
			  struct tbframe_frame *frame)
{
	return -ENETDOWN;
}

static void lifecycle_frame_free(struct tbframe_link *link,
				 struct tbframe_frame *frame)
{
}

static const char *lifecycle_link_name(const struct tbframe_link *link)
{
	return "lifecyclemock";
}

static void lifecycle_link_info(const struct tbframe_link *link,
				struct tbframe_link_info *info)
{
	memset(info, 0, sizeof(*info));
}

static const struct tbrxe_transport_ops lifecycle_transport = {
	.register_client	= lifecycle_register_client,
	.unregister_client	= lifecycle_unregister_client,
	.alloc_frame		= lifecycle_alloc_frame,
	.xmit			= lifecycle_xmit,
	.frame_free		= lifecycle_frame_free,
	.link_name		= lifecycle_link_name,
	.link_info		= lifecycle_link_info,
};

/* Snapshot a device's GID-anchor netdev name so it can be looked up after
 * the device is gone.
 */
static void lifecycle_ndev_name(struct kunit *test, struct rxe_dev *rxe,
				char name[IFNAMSIZ])
{
	struct net_device *ndev = ib_device_get_netdev(&rxe->ib_dev, 1);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ndev);
	strscpy(name, netdev_name(ndev), IFNAMSIZ);
	dev_put(ndev);
}

static bool lifecycle_ndev_exists(const char *name)
{
	struct net_device *ndev = dev_get_by_name(&init_net, name);

	if (!ndev)
		return false;
	dev_put(ndev);
	return true;
}

/*
 * tbrxe must not register under RDMA_DRIVER_RXE. Sharing the id makes
 * `rmmod rdma_rxe` -- ib_unregister_driver(RDMA_DRIVER_RXE) -- sweep live
 * tbrxe devices, which is what happened on the fleet.
 */
static void tbrxe_lifecycle_owns_driver_id(struct kunit *test)
{
	static int fake_link;
	struct ib_device *found;
	struct net_device *ndev;
	struct rxe_dev *rxe;

	tbrxe_set_transport_ops(&lifecycle_transport);
	tbrxe_test_link_up(&fake_link);

	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);

	KUNIT_EXPECT_EQ(test, (int)rxe->ib_dev.ops.driver_id,
			(int)RDMA_DRIVER_USB4_RDMA);
	KUNIT_EXPECT_NE(test, (int)rxe->ib_dev.ops.driver_id,
			(int)RDMA_DRIVER_RXE);

	/* And the id actually keys device lookup: ours matches, rxe's does
	 * not (device.c:367-381).
	 */
	ndev = ib_device_get_netdev(&rxe->ib_dev, 1);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ndev);

	found = ib_device_get_by_netdev(ndev, RDMA_DRIVER_USB4_RDMA);
	KUNIT_EXPECT_PTR_EQ(test, found, &rxe->ib_dev);
	if (found)
		ib_device_put(found);

	found = ib_device_get_by_netdev(ndev, RDMA_DRIVER_RXE);
	KUNIT_EXPECT_PTR_EQ(test, found, (struct ib_device *)NULL);
	if (found)
		ib_device_put(found);

	dev_put(ndev);
	tbrxe_test_link_down(&fake_link);
}

/*
 * The reload analogue: publish and unpublish the same link repeatedly. Each
 * cycle must leave nothing behind -- no device, no GID-anchor netdev -- and
 * the next cycle must succeed.
 */
static void tbrxe_lifecycle_publish_cycles(struct kunit *test)
{
	static int fake_link;
	char name[IFNAMSIZ];
	struct rxe_dev *rxe;
	int i;

	tbrxe_set_transport_ops(&lifecycle_transport);

	for (i = 0; i < 20; i++) {
		tbrxe_test_link_up(&fake_link);

		rxe = tbrxe_test_dev(&fake_link);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
		lifecycle_ndev_name(test, rxe, name);

		tbrxe_test_link_down(&fake_link);

		KUNIT_ASSERT_PTR_EQ(test, tbrxe_test_dev(&fake_link),
				    (struct rxe_dev *)NULL);
		KUNIT_ASSERT_FALSE(test, lifecycle_ndev_exists(name));
	}
}

/*
 * tbrxe_frame_unregister() is the module-exit fence: called with links still
 * published it must unpublish all of them and return only once every record
 * and netdev is freed. Re-registering afterwards must work -- that is what
 * rmmod-then-insmod does.
 */
static void tbrxe_lifecycle_unregister_sweeps_live_links(struct kunit *test)
{
	static int fake_a, fake_b;
	char name_a[IFNAMSIZ], name_b[IFNAMSIZ];
	struct rxe_dev *rxe;

	tbrxe_set_transport_ops(&lifecycle_transport);
	tbrxe_test_link_up(&fake_a);
	tbrxe_test_link_up(&fake_b);

	rxe = tbrxe_test_dev(&fake_a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
	lifecycle_ndev_name(test, rxe, name_a);
	rxe = tbrxe_test_dev(&fake_b);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
	lifecycle_ndev_name(test, rxe, name_b);

	tbrxe_frame_unregister();

	KUNIT_EXPECT_PTR_EQ(test, tbrxe_test_dev(&fake_a),
			    (struct rxe_dev *)NULL);
	KUNIT_EXPECT_PTR_EQ(test, tbrxe_test_dev(&fake_b),
			    (struct rxe_dev *)NULL);
	KUNIT_EXPECT_FALSE(test, lifecycle_ndev_exists(name_a));
	KUNIT_EXPECT_FALSE(test, lifecycle_ndev_exists(name_b));

	/* insmod again. */
	KUNIT_ASSERT_EQ(test, tbrxe_frame_register(), 0);
	tbrxe_test_link_up(&fake_a);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, tbrxe_test_dev(&fake_a));
	tbrxe_test_link_down(&fake_a);
}

/*
 * Unpublish is asynchronous and must respect the core's reference fence: a
 * held ib_device reference keeps disable_device() waiting
 * (device.c:1297-1330), so the record and netdev survive until it is put.
 * This is the shape of "unplug the cable while userspace still holds the
 * device"; the record must never be freed under the engine.
 */
static void tbrxe_lifecycle_unpublish_waits_for_refs(struct kunit *test)
{
	static int fake_link;
	char name[IFNAMSIZ];
	struct rxe_dev *rxe;

	tbrxe_set_transport_ops(&lifecycle_transport);
	tbrxe_test_link_up(&fake_link);

	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
	lifecycle_ndev_name(test, rxe, name);

	KUNIT_ASSERT_TRUE(test, ib_device_try_get(&rxe->ib_dev));

	/* Raw upcall: tbrxe_test_link_down() would drain and deadlock on the
	 * reference this test is holding on purpose.
	 */
	tbrxe_frame_client_ops()->link_down(NULL,
					    (struct tbframe_link *)&fake_link,
					    TBFRAME_DOWN_CLOSED);

	/* Already unfindable by the transport, but not yet freed. */
	KUNIT_EXPECT_PTR_EQ(test, tbrxe_test_dev(&fake_link),
			    (struct rxe_dev *)NULL);
	KUNIT_EXPECT_TRUE(test, lifecycle_ndev_exists(name));

	ib_device_put(&rxe->ib_dev);
	tbrxe_frame_drain();

	KUNIT_EXPECT_FALSE(test, lifecycle_ndev_exists(name));
}

static struct kunit_case tbrxe_lifecycle_cases[] = {
	KUNIT_CASE(tbrxe_lifecycle_owns_driver_id),
	KUNIT_CASE(tbrxe_lifecycle_publish_cycles),
	KUNIT_CASE(tbrxe_lifecycle_unregister_sweeps_live_links),
	KUNIT_CASE(tbrxe_lifecycle_unpublish_waits_for_refs),
	{}
};

static struct kunit_suite tbrxe_lifecycle_suite = {
	.name = "tbrxe_lifecycle",
	.test_cases = tbrxe_lifecycle_cases,
};

kunit_test_suites(&tbrxe_lifecycle_suite);
