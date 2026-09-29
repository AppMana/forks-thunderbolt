// SPDX-License-Identifier: GPL-2.0
/*
 * tbrxe_uverbs_test.c - unpublish while userspace holds the device.
 *
 * Observed on a daisy chain under an RDMA training job: one host rebooted,
 * both of its neighbours unpublished their rail's ib_device, and on each the
 * unregister worker parked in
 *
 *   ib_unregister_work -> __ib_unregister_device -> disable_device
 *     -> remove_client_context -> ib_uverbs_remove_one -> wait_for_completion
 *
 * because a training process still had the uverbs device open with its queues
 * mmapped. Without .disassociate_ucontext, ib_uverbs_remove_one() waits for
 * every open uverbs file to be closed (uverbs_main.c, wait_clients); the
 * process never closed it, and with hung_task_panic set khungtaskd took
 * both hosts down 122 s later.
 *
 * This suite drives that exact state from KUnit: a kthread with a real
 * user mm opens the device's uverbs char device, allocates a ucontext and a
 * CQ through the write() ABI the rdma-core provider uses, mmaps the CQ ring
 * MAP_SHARED, and then the link goes down. The contract:
 *
 *   - the unpublish completes while the file is still open and mapped;
 *   - the user mapping no longer reaches the queue memory: it is zapped and
 *     re-faults onto a zeroed page, as the core does for its own mmap
 *     entries (rdma_umap_fault);
 *   - further verbs commands on the orphaned file fail with -EIO;
 *   - closing the file and unmapping afterwards, when the device and its
 *     rxe_dev are already freed, is safe.
 */

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/mman.h>
#include <linux/namei.h>
#include <linux/uaccess.h>
#include <net/netlink.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_user_rxe.h>

#include "../../../infiniband/core/core_priv.h"

#include "rxe.h"
#include "tbrxe_test_link.h"

#define UVERBS_TEST_NODE	"/tbrxe-uverbs-test"
#define UVERBS_UNPUBLISH_WAIT_MS	3000
#define UVERBS_MARKER		0xa5

static int uverbs_register_client(const struct tbframe_client_ops *ops,
				  void *ctx)
{
	return 0;
}

static void uverbs_unregister_client(void)
{
}

static int uverbs_alloc_frame(struct tbframe_link *link, u16 len,
			      bool is_ctrl, struct tbframe_frame **frame)
{
	return -ENETDOWN;
}

static int uverbs_xmit(struct tbframe_link *link, struct tbframe_frame *frame)
{
	return -ENETDOWN;
}

static void uverbs_frame_free(struct tbframe_link *link,
			      struct tbframe_frame *frame)
{
}

static const char *uverbs_link_name(const struct tbframe_link *link)
{
	return "uverbsmock";
}

static void uverbs_link_info(const struct tbframe_link *link,
			     struct tbframe_link_info *info)
{
	memset(info, 0, sizeof(*info));
}

static const struct tbrxe_transport_ops uverbs_transport = {
	.register_client	= uverbs_register_client,
	.unregister_client	= uverbs_unregister_client,
	.alloc_frame		= uverbs_alloc_frame,
	.xmit			= uverbs_xmit,
	.frame_free		= uverbs_frame_free,
	.link_name		= uverbs_link_name,
	.link_info		= uverbs_link_info,
};

/* The uverbs char device ib_core created for @ibdev. */
static int uverbs_devt(struct ib_device *ibdev, dev_t *devt)
{
	struct ib_client_nl_info info = { .port = -1 };
	int ret;

	info.nl_msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!info.nl_msg)
		return -ENOMEM;
	ret = ib_get_client_nl_info(ibdev, "uverbs", &info);
	nlmsg_free(info.nl_msg);
	if (ret)
		return ret;
	*devt = info.cdev->devt;
	put_device(info.cdev);
	return 0;
}

/* There is no devtmpfs in the KUnit kernel; make the node on rootfs. */
static struct file *uverbs_open(struct kunit *test, dev_t devt)
{
	struct dentry *dentry;
	struct path path;
	int ret;

	dentry = kern_path_create(AT_FDCWD, UVERBS_TEST_NODE, &path, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dentry);
	ret = vfs_mknod(mnt_idmap(path.mnt), d_inode(path.dentry), dentry,
			S_IFCHR | 0600, devt);
	done_path_create(&path, dentry);
	KUNIT_ASSERT_EQ(test, ret, 0);

	return filp_open(UVERBS_TEST_NODE, O_RDWR, 0);
}

static void uverbs_unlink_node(void)
{
	struct dentry *dentry;
	struct path parent;

	dentry = kern_path_locked(UVERBS_TEST_NODE, &parent);
	if (IS_ERR(dentry))
		return;
	vfs_unlink(mnt_idmap(parent.mnt), d_inode(parent.dentry), dentry, NULL);
	inode_unlock(d_inode(parent.dentry));
	dput(dentry);
	path_put(&parent);
}

/* One legacy write() command: header + request in @ubuf, response after. */
static ssize_t uverbs_cmd(struct file *filp, void __user *ubuf, u32 command,
			  const void *req, size_t req_len, size_t resp_len,
			  void __user *uresp)
{
	struct ib_uverbs_cmd_hdr hdr = {
		.command = command,
		.in_words = (sizeof(hdr) + req_len) / 4,
		.out_words = resp_len / 4,
	};
	loff_t pos = 0;

	if (copy_to_user(ubuf, &hdr, sizeof(hdr)) ||
	    copy_to_user(ubuf + sizeof(hdr), req, req_len))
		return -EFAULT;
	return vfs_write(filp, ubuf, sizeof(hdr) + req_len, &pos);
}

struct uverbs_drain_ctx {
	struct completion done;
};

static int uverbs_drain_thread(void *data)
{
	struct uverbs_drain_ctx *ctx = data;

	tbrxe_frame_drain();
	complete(&ctx->done);
	return 0;
}

static void tbrxe_uverbs_unpublish_revokes_open_context(struct kunit *test)
{
	static int fake_link;
	struct ib_uverbs_get_context_resp ctx_resp;
	struct ib_uverbs_get_context ctx_cmd;
	struct ib_uverbs_create_cq cq_cmd;
	struct {
		struct ib_uverbs_create_cq_resp core;
		struct rxe_create_cq_resp drv;
	} cq_resp;
	struct uverbs_drain_ctx drain;
	struct task_struct *task;
	unsigned long ubuf, ring;
	u8 bytes[64], marker[64];
	struct rxe_dev *rxe;
	struct file *filp;
	unsigned long done;
	dev_t devt;
	ssize_t n;
	int i;

	tbrxe_set_transport_ops(&uverbs_transport);
	tbrxe_test_link_up(&fake_link);
	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
	KUNIT_ASSERT_EQ(test, uverbs_devt(&rxe->ib_dev, &devt), 0);

	/* A user address space for commands and responses. */
	ubuf = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE,
			     PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_NE(test, ubuf, 0UL);

	filp = uverbs_open(test, devt);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, filp);

	memset(&ctx_cmd, 0, sizeof(ctx_cmd));
	ctx_cmd.response = ubuf + PAGE_SIZE / 2;
	n = uverbs_cmd(filp, (void __user *)ubuf, IB_USER_VERBS_CMD_GET_CONTEXT,
		       &ctx_cmd, sizeof(ctx_cmd), sizeof(ctx_resp),
		       (void __user *)ctx_cmd.response);
	KUNIT_ASSERT_GT(test, n, 0);
	KUNIT_ASSERT_EQ(test, copy_from_user(&ctx_resp,
					     (void __user *)ctx_cmd.response,
					     sizeof(ctx_resp)), 0UL);

	memset(&cq_cmd, 0, sizeof(cq_cmd));
	cq_cmd.response = ubuf + PAGE_SIZE / 2;
	cq_cmd.cqe = 16;
	cq_cmd.comp_channel = -1;
	n = uverbs_cmd(filp, (void __user *)ubuf, IB_USER_VERBS_CMD_CREATE_CQ,
		       &cq_cmd, sizeof(cq_cmd), sizeof(cq_resp),
		       (void __user *)cq_cmd.response);
	KUNIT_ASSERT_GT(test, n, 0);
	KUNIT_ASSERT_EQ(test, copy_from_user(&cq_resp,
					     (void __user *)cq_cmd.response,
					     sizeof(cq_resp)), 0UL);
	KUNIT_ASSERT_GT(test, cq_resp.drv.mi.size, 0U);

	/* The provider's view of the ring: MAP_SHARED at the mmap cookie. */
	ring = kunit_vm_mmap(test, filp, 0, cq_resp.drv.mi.size,
			     PROT_READ | PROT_WRITE, MAP_SHARED,
			     cq_resp.drv.mi.offset);
	KUNIT_ASSERT_TRUE(test, ring && !IS_ERR_VALUE(ring));

	/* Write past the queue header and read it back through the mapping. */
	memset(marker, UVERBS_MARKER, sizeof(marker));
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)(ring + PAGE_SIZE / 2),
					   marker, sizeof(marker)), 0UL);
	KUNIT_ASSERT_EQ(test, copy_from_user(bytes,
					     (void __user *)(ring + PAGE_SIZE / 2),
					     sizeof(bytes)), 0UL);
	KUNIT_ASSERT_EQ(test, memcmp(bytes, marker, sizeof(bytes)), 0);

	/* The neighbour goes away: terminal link_down, queued unpublish. */
	tbrxe_frame_client_ops()->link_down(NULL,
					    (struct tbframe_link *)&fake_link,
					    TBFRAME_DOWN_CLOSED);

	init_completion(&drain.done);
	task = kthread_run(uverbs_drain_thread, &drain, "tbrxe-uverbs-drain");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, task);
	done = wait_for_completion_timeout(&drain.done,
				msecs_to_jiffies(UVERBS_UNPUBLISH_WAIT_MS));

	/* The contract: userspace keeping the file open and mapped must not
	 * hold the unregister worker hostage.
	 */
	KUNIT_EXPECT_NE_MSG(test, done, 0UL,
			    "unpublish blocked in ib_uverbs_remove_one while the uverbs file was open and mapped");

	if (done) {
		/* The ring is revoked: the mapping still exists, but re-faults
		 * onto a zeroed page instead of the queue memory.
		 */
		KUNIT_EXPECT_EQ(test,
				copy_from_user(bytes,
					       (void __user *)(ring + PAGE_SIZE / 2),
					       sizeof(bytes)), 0UL);
		for (i = 0; i < sizeof(bytes); i++)
			if (bytes[i])
				break;
		KUNIT_EXPECT_EQ_MSG(test, i, (int)sizeof(bytes),
				    "revoked mapping still reads the queue memory");

		/* And the orphaned file refuses further verbs. */
		n = uverbs_cmd(filp, (void __user *)ubuf,
			       IB_USER_VERBS_CMD_CREATE_CQ, &cq_cmd,
			       sizeof(cq_cmd), sizeof(cq_resp),
			       (void __user *)cq_cmd.response);
		KUNIT_EXPECT_EQ(test, n, (ssize_t)-EIO);
	}

	/* Userspace exits: unmap, close the async fd and the device file.
	 * On GREEN this runs after the rxe_dev is already freed; on RED it
	 * is what finally lets the blocked unpublish finish.
	 */
	vm_munmap(ring, cq_resp.drv.mi.size);
	close_fd(ctx_resp.async_fd);
	fput(filp);
	uverbs_unlink_node();

	if (!done)
		KUNIT_ASSERT_NE(test,
				wait_for_completion_timeout(&drain.done,
							    msecs_to_jiffies(10000)),
				0UL);
	KUNIT_EXPECT_PTR_EQ(test, tbrxe_test_dev(&fake_link),
			    (struct rxe_dev *)NULL);
	tbrxe_set_transport_ops(NULL);
}

static struct kunit_case tbrxe_uverbs_cases[] = {
	KUNIT_CASE(tbrxe_uverbs_unpublish_revokes_open_context),
	{}
};

static struct kunit_suite tbrxe_uverbs_suite = {
	.name = "tbrxe_uverbs",
	.test_cases = tbrxe_uverbs_cases,
};

kunit_test_suites(&tbrxe_uverbs_suite);
