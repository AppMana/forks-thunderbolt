// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */

#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <rdma/uverbs_ioctl.h>

#include "rxe.h"
#include "rxe_loc.h"
#include "rxe_queue.h"

/*
 * Guards every ucontext's mmaps list and every info's revoked flag. It is
 * file-global rather than per-device because an info can outlive both its
 * ucontext and its rxe_dev: once a ucontext is disassociated, device
 * unregistration no longer waits for userspace (ib_uverbs_remove_one), so a
 * process may still hold the ring mapped after the device is freed, and the
 * last munmap then releases the info with nothing else left to lock.
 *
 * Nesting: rxe_mmap_lock -> rxe->pending_lock.
 */
static DEFINE_SPINLOCK(rxe_mmap_lock);

/* A queue's mmap info becomes findable by rxe_mmap() and revocable. */
void rxe_mmap_info_publish(struct rxe_dev *rxe, struct rxe_mmap_info *ip)
{
	struct rxe_ucontext *uc = to_ruc(ip->context);

	spin_lock_bh(&rxe_mmap_lock);
	list_add(&ip->uc_node, &uc->mmaps);
	spin_lock(&rxe->pending_lock);
	list_add(&ip->pending_mmaps, &rxe->pending_mmaps);
	spin_unlock(&rxe->pending_lock);
	spin_unlock_bh(&rxe_mmap_lock);
}

void rxe_mmap_release(struct kref *ref)
{
	struct rxe_mmap_info *ip = container_of(ref,
					struct rxe_mmap_info, ref);

	/*
	 * Not revoked means rxe_disassociate_ucontext() has not taken this
	 * info off its ucontext yet, and it cannot while we hold the lock, so
	 * the ucontext and its rxe_dev are still alive here. Revoked means
	 * both may be gone and the lists no longer hold the info.
	 */
	spin_lock_bh(&rxe_mmap_lock);
	if (!ip->revoked) {
		struct rxe_dev *rxe = to_rdev(ip->context->device);

		list_del(&ip->uc_node);
		spin_lock(&rxe->pending_lock);
		if (!list_empty(&ip->pending_mmaps))
			list_del(&ip->pending_mmaps);
		spin_unlock(&rxe->pending_lock);
	}
	spin_unlock_bh(&rxe_mmap_lock);

	if (ip->inode)
		iput(ip->inode);
	if (ip->dead_page)
		__free_page(ip->dead_page);
	vfree(ip->obj);		/* buf */
	kfree(ip);
}

/*
 * open and close keep track of how many times the memory region is mapped,
 * to avoid releasing it.
 */
static void rxe_vma_open(struct vm_area_struct *vma)
{
	struct rxe_mmap_info *ip = vma->vm_private_data;

	kref_get(&ip->ref);
}

static void rxe_vma_close(struct vm_area_struct *vma)
{
	struct rxe_mmap_info *ip = vma->vm_private_data;

	kref_put(&ip->ref, rxe_mmap_release);
}

/*
 * The ring is fully populated at mmap time (remap_vmalloc_range), so a fault
 * means the PTEs were zapped: by userspace (MADV_DONTNEED) while the queue is
 * live, in which case the queue page is mapped back, or by revocation, in
 * which case the mapping gets a zeroed page instead of the queue memory --
 * the same contract the core gives its own mmap entries after disassociate
 * (rdma_umap_fault in uverbs_main.c). The process sees an idle ring and
 * -EIO on every verbs call rather than a SIGBUS.
 */
static vm_fault_t rxe_vma_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct rxe_mmap_info *ip = vma->vm_private_data;
	unsigned long off;
	struct page *page;

	off = (vmf->pgoff - (ip->info.offset >> PAGE_SHIFT)) << PAGE_SHIFT;
	if (off >= ip->info.size)
		return VM_FAULT_SIGBUS;

	if (!READ_ONCE(ip->revoked)) {
		/*
		 * Check revoked under the queue page's lock and return with it
		 * held: the core installs the PTE before unlocking, and
		 * rxe_mmap_revoke() lock-sweeps every queue page after setting
		 * revoked and before zapping, so a fault that saw the queue
		 * live has always installed its PTE by the time the zap runs.
		 */
		page = vmalloc_to_page(ip->obj + off);
		lock_page(page);
		if (!READ_ONCE(ip->revoked)) {
			get_page(page);
			vmf->page = page;
			return VM_FAULT_LOCKED;
		}
		unlock_page(page);
	}

	if (!(vma->vm_flags & (VM_WRITE | VM_MAYWRITE))) {
		page = ZERO_PAGE(vmf->address);
	} else {
		page = READ_ONCE(ip->dead_page);
		if (!page) {
			struct page *fresh = alloc_page(GFP_KERNEL | __GFP_ZERO);

			if (!fresh)
				return VM_FAULT_OOM;
			page = cmpxchg(&ip->dead_page, NULL, fresh);
			if (page)
				__free_page(fresh);
			else
				page = fresh;
		}
	}

	get_page(page);
	vmf->page = page;
	return 0;
}

static const struct vm_operations_struct rxe_vm_ops = {
	.open = rxe_vma_open,
	.close = rxe_vma_close,
	.fault = rxe_vma_fault,
};

/**
 * rxe_mmap - create a new mmap region
 * @context: the IB user context of the process making the mmap() call
 * @vma: the VMA to be initialized
 * Return zero if the mmap is OK. Otherwise, return an errno.
 */
int rxe_mmap(struct ib_ucontext *context, struct vm_area_struct *vma)
{
	struct rxe_dev *rxe = to_rdev(context->device);
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;
	struct rxe_mmap_info *ip, *pp;
	int ret;

	/*
	 * Search the device's list of objects waiting for a mmap call.
	 * Normally, this list is very short since a call to create a
	 * CQ, QP, or SRQ is soon followed by a call to mmap().
	 */
	spin_lock_bh(&rxe->pending_lock);
	list_for_each_entry_safe(ip, pp, &rxe->pending_mmaps, pending_mmaps) {
		if (context != ip->context || (__u64)offset != ip->info.offset)
			continue;

		/* Don't allow a mmap larger than the object. */
		if (size > ip->info.size) {
			rxe_dbg_dev(rxe, "mmap region is larger than the object!\n");
			spin_unlock_bh(&rxe->pending_lock);
			ret = -EINVAL;
			goto done;
		}

		goto found_it;
	}
	rxe_dbg_dev(rxe, "unable to find pending mmap info\n");
	spin_unlock_bh(&rxe->pending_lock);
	ret = -EINVAL;
	goto done;

found_it:
	list_del_init(&ip->pending_mmaps);
	spin_unlock_bh(&rxe->pending_lock);

	/* Revocation zaps through this inode's i_mmap tree. A pending info is
	 * mapped at most once (it just left the pending list), so there is one
	 * inode per info; fork and VMA splits inherit the same file.
	 */
	ip->inode = igrab(file_inode(vma->vm_file));
	if (!ip->inode) {
		ret = -ENODEV;
		goto done;
	}

	ret = remap_vmalloc_range(vma, ip->obj, 0);
	if (ret) {
		rxe_dbg_dev(rxe, "err %d from remap_vmalloc_range\n", ret);
		goto done;
	}

	vma->vm_ops = &rxe_vm_ops;
	vma->vm_private_data = ip;
	rxe_vma_open(vma);
done:
	return ret;
}

/*
 * Allocate information for rxe_mmap
 */
struct rxe_mmap_info *rxe_create_mmap_info(struct rxe_dev *rxe, u32 size,
					   struct ib_udata *udata, void *obj)
{
	struct rxe_mmap_info *ip;

	if (!udata)
		return ERR_PTR(-EINVAL);

	ip = kmalloc(sizeof(*ip), GFP_KERNEL);
	if (!ip)
		return ERR_PTR(-ENOMEM);

	size = PAGE_ALIGN(size);

	spin_lock_bh(&rxe->mmap_offset_lock);

	if (rxe->mmap_offset == 0)
		rxe->mmap_offset = ALIGN(PAGE_SIZE, SHMLBA);

	ip->info.offset = rxe->mmap_offset;
	rxe->mmap_offset += ALIGN(size, SHMLBA);

	spin_unlock_bh(&rxe->mmap_offset_lock);

	INIT_LIST_HEAD(&ip->pending_mmaps);
	INIT_LIST_HEAD(&ip->uc_node);
	INIT_LIST_HEAD(&ip->revoke_node);
	ip->revoked = false;
	ip->inode = NULL;
	ip->dead_page = NULL;
	ip->info.size = size;
	ip->context =
		container_of(udata, struct uverbs_attr_bundle, driver_udata)
			->context;
	ip->obj = obj;
	kref_init(&ip->ref);

	return ip;
}

/*
 * Zap every user PTE of an info already marked revoked. The lock sweep waits
 * out faults that read revoked == false before it was set (rxe_vma_fault
 * holds the page lock until the core has installed the PTE); any fault after
 * the sweep sees revoked and maps the dead page, so after the zap no PTE can
 * reach the queue memory again.
 */
static void rxe_mmap_revoke(struct rxe_mmap_info *ip)
{
	unsigned long off;
	struct page *page;

	if (!ip->inode)
		return;

	for (off = 0; off < ip->info.size; off += PAGE_SIZE) {
		page = vmalloc_to_page(ip->obj + off);
		lock_page(page);
		unlock_page(page);
	}
	unmap_mapping_range(ip->inode->i_mapping, ip->info.offset,
			    ip->info.size, 1);
}

/**
 * rxe_disassociate_ucontext - revoke a ucontext's queue mappings
 * @ibuc: the ucontext being torn down on device removal
 *
 * ib_uverbs_remove_one() calls this (via ufile_destroy_ucontext) after it has
 * destroyed the context's objects and instead of waiting for userspace to
 * close the uverbs file. What is left are the mmap infos whose rings are
 * still mapped. Each one is detached from the ucontext and the device, its
 * PTEs are zapped, and later faults land on a zeroed page (rxe_vma_fault).
 * The queue memory itself stays allocated until the last VMA goes away.
 */
void rxe_disassociate_ucontext(struct ib_ucontext *ibuc)
{
	struct rxe_ucontext *uc = to_ruc(ibuc);
	struct rxe_dev *rxe = to_rdev(ibuc->device);
	struct rxe_mmap_info *ip, *next;
	LIST_HEAD(revoke);

	spin_lock_bh(&rxe_mmap_lock);
	list_for_each_entry_safe(ip, next, &uc->mmaps, uc_node) {
		list_del_init(&ip->uc_node);
		spin_lock(&rxe->pending_lock);
		if (!list_empty(&ip->pending_mmaps))
			list_del_init(&ip->pending_mmaps);
		spin_unlock(&rxe->pending_lock);
		WRITE_ONCE(ip->revoked, true);
		/* A zero count is a release already waiting on this lock; it
		 * will see revoked and touch nothing of ours.
		 */
		if (kref_get_unless_zero(&ip->ref))
			list_add(&ip->revoke_node, &revoke);
	}
	spin_unlock_bh(&rxe_mmap_lock);

	list_for_each_entry_safe(ip, next, &revoke, revoke_node) {
		list_del(&ip->revoke_node);
		rxe_mmap_revoke(ip);
		kref_put(&ip->ref, rxe_mmap_release);
	}
}
