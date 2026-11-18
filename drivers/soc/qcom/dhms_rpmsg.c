// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone DHMS RPMSG Driver
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 */

#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/delay.h>

#include "dhms_rpmsg.h"

/* Module-level context pointer — defined here, declared extern in dhms_rpmsg.h */
struct dhms_device *g_dhms_dev;
static DEFINE_MUTEX(g_dhms_dev_lock); /* protects g_dhms_dev */

/* ================================================================
 * Core buffer operations
 * ================================================================
 */

static void dhms_buf_free_resources(struct dhms_buffer_entry *buf)
{
	if (buf->sgt) {
		dma_buf_unmap_attachment_unlocked(buf->attach, buf->sgt,
						  DMA_BIDIRECTIONAL);
		buf->sgt = NULL;
	}
	if (buf->attach) {
		dma_buf_detach(buf->dmabuf, buf->attach);
		buf->attach = NULL;
	}
	if (buf->dmabuf) {
		dma_buf_put(buf->dmabuf);
		buf->dmabuf = NULL;
	}
}

/* ================================================================
 * Multi-Processor Registration
 * ================================================================
 */

/**
 * dhms_register_processor() - Register a new remote processor
 * @ctx: DHMS context
 * @name: Processor name (e.g., "mcu","soccp", "adsp", "cdsp")
 * @rpdev: RPMSG device handle for this processor
 *
 * This function is called from RPMSG probe when a new processor connects.
 * It creates a processor context and adds it to the processors list.
 *
 * Returns: 0 on success, negative errno on failure
 */
static int dhms_register_processor(struct dhms_device *ctx,
				   const char *name,
				   struct rpmsg_device *rpdev)
{
	struct dhms_processor_ctx *proc;
	unsigned long flags;

	if (!ctx || !name || !rpdev) {
		pr_err("DHMS: Invalid parameters to register_processor\n");
		return -EINVAL;
	}

	proc = kzalloc(sizeof(*proc), GFP_KERNEL);
	if (!proc)
		return -ENOMEM;

	strscpy(proc->name, name, sizeof(proc->name));
	proc->rpdev = rpdev;
	proc->state = PROCESSOR_ONLINE;
	proc->dhms_dev = ctx;
	atomic_set(&proc->buffer_count, 0);

	spin_lock_irqsave(&ctx->processor_lock, flags);
	list_add_tail(&proc->node, &ctx->processors);
	ctx->num_processors++;
	spin_unlock_irqrestore(&ctx->processor_lock, flags);

	dev_info(ctx->dma_dev, "DHMS: Processor '%s' registered (total: %d)\n",
		 name, ctx->num_processors);
	wake_up_interruptible(&ctx->wait_queue);

	return 0;
}

/**
 * dhms_unregister_processor() - Unregister a remote processor
 * @ctx: DHMS context
 * @rpdev: RPMSG device handle to unregister
 *
 * This function is called from RPMSG remove when a processor disconnects.
 * It removes the processor from the list and frees resources.
 */
static void dhms_unregister_processor(struct dhms_device *ctx,
				      struct rpmsg_device *rpdev)
{
	struct dhms_processor_ctx *proc, *tmp;
	unsigned long flags;
	int buffer_count;

	if (!ctx || !rpdev)
		return;

	spin_lock_irqsave(&ctx->processor_lock, flags);
	list_for_each_entry_safe(proc, tmp, &ctx->processors, node) {
		if (proc->rpdev == rpdev) {
			list_del(&proc->node);
			ctx->num_processors--;
			buffer_count = atomic_read(&proc->buffer_count);
			spin_unlock_irqrestore(&ctx->processor_lock, flags);

			dev_info(ctx->dma_dev,
				 "DHMS: Processor '%s' unregistered (%d buffers, %d remaining)\n",
				 proc->name, buffer_count, ctx->num_processors);

			kfree(proc);
			wake_up_interruptible(&ctx->wait_queue);
			return;
		}
	}
	spin_unlock_irqrestore(&ctx->processor_lock, flags);

	dev_info(ctx->dma_dev, "DHMS: Processor not found for unregister\n");
}

/* ================================================================
 * Multi-Processor Buffer Operations
 * ================================================================
 */

/**
 * dhms_cleanup_processor_refs() - Free all processor refs for a buffer
 * @buf: Buffer entry whose processor refs should be freed
 *
 * This function is called on error during import or during buffer release.
 * It frees all processor reference structures associated with the buffer.
 */
static void dhms_cleanup_processor_refs(struct dhms_buffer_entry *buf)
{
	struct dhms_processor_entry *ref, *tmp;

	if (!buf)
		return;

	spin_lock(&buf->ref_lock);
	list_for_each_entry_safe(ref, tmp, &buf->processor_refs, node) {
		list_del(&ref->node);
		kfree(ref);
	}
	buf->num_processors = 0;
	spin_unlock(&buf->ref_lock);
}

/**
 * dhms_map_dmabuf() - Map DMA-BUF and perform IOMMU mapping
 * @ctx: DHMS context
 * @fd: DMA-BUF file descriptor from userspace
 * @flags: Caller-supplied flags
 * @buf_out: Output buffer entry populated with mapping info on success
 *
 * This function performs DMA-BUF get/attach/map and returns the IOVA address.
 * It allocates a dhms_buffer_entry, attaches the DMA-BUF to the DMA device,
 * and maps it to obtain the IOMMU IOVA.
 *
 * Returns: 0 on success, negative errno on failure
 */
static int dhms_map_dmabuf(struct dhms_device *ctx, int fd, uint32_t flags,
			   struct dhms_buffer_entry **buf_out)
{
	struct dhms_buffer_entry *buf;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t iova;
	int ret;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err_put_dmabuf;
	}

	attach = dma_buf_attach(dmabuf, ctx->dma_dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto err_free_buf;
	}

	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_detach;
	}

	iova = sg_dma_address(sgt->sgl);

	buf->fd = fd;
	buf->dmabuf = dmabuf;
	buf->attach = attach;
	buf->sgt = sgt;
	buf->iova = iova;
	buf->size = dmabuf->size;
	buf->flags = flags;
	*buf_out = buf;
	return 0;

err_detach:
	dma_buf_detach(dmabuf, attach);
err_free_buf:
	kfree(buf);
err_put_dmabuf:
	dma_buf_put(dmabuf);
	return ret;
}

/**
 * dhms_setup_buffer_bookkeeping() - Setup buffer bookkeeping and processor refs
 * @ctx: DHMS context
 * @buf: Buffer entry to setup
 *
 * This function allocates a client_id from the IDR, initializes the buffer
 * entry fields, and creates processor reference entries for all currently
 * online processors.
 *
 * Returns: 0 on success, negative errno on failure
 */
static int dhms_setup_buffer_bookkeeping(struct dhms_device *ctx,
					 struct dhms_buffer_entry *buf)
{
	struct dhms_processor_ctx *proc;
	struct dhms_processor_entry *ref;
	unsigned long flags;
	uint64_t client_id;
	int ret;

	spin_lock_irqsave(&ctx->lock, flags);
	ret = idr_alloc(&ctx->buf_idr, buf, 1, 0, GFP_ATOMIC);
	if (ret < 0) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return ret;
	}
	client_id = (uint64_t)ret;
	spin_unlock_irqrestore(&ctx->lock, flags);

	buf->client_id = client_id;
	INIT_LIST_HEAD(&buf->processor_refs);
	spin_lock_init(&buf->ref_lock);
	buf->num_processors = 0;
	atomic_set(&buf->acks_received, 0);
	INIT_LIST_HEAD(&buf->release_node);
	dev_dbg(ctx->dma_dev,
		"DHMS: DMA-BUF map and bookkeeping successful for client_id %llu IOVA = 0x%llx\n",
		buf->client_id, buf->iova);

	spin_lock_irqsave(&ctx->processor_lock, flags);
	list_for_each_entry(proc, &ctx->processors, node) {
		if (proc->state != PROCESSOR_ONLINE)
			continue;

		ref = kzalloc(sizeof(*ref), GFP_ATOMIC);
		if (!ref) {
			spin_unlock_irqrestore(&ctx->processor_lock, flags);
			ret = -ENOMEM;
			goto err_cleanup_refs;
		}

		strscpy(ref->processor_name, proc->name,
			sizeof(ref->processor_name));
		ref->rpdev = proc->rpdev;
		ref->ack_state = DHMS_ACK_PENDING;

		spin_lock(&buf->ref_lock);
		list_add_tail(&ref->node, &buf->processor_refs);
		buf->num_processors++;
		spin_unlock(&buf->ref_lock);

		atomic_inc(&proc->buffer_count);
	}
	spin_unlock_irqrestore(&ctx->processor_lock, flags);

	if (buf->num_processors == 0) {
		dev_warn(ctx->dma_dev,
			 "DHMS: Buffer %llu created with no processors online - broadcast will be skipped\n",
			 client_id);
	}

	return 0;

err_cleanup_refs:
	dhms_cleanup_processor_refs(buf);
	spin_lock_irqsave(&ctx->lock, flags);
	idr_remove(&ctx->buf_idr, client_id);
	spin_unlock_irqrestore(&ctx->lock, flags);
	return ret;
}

/**
 * dhms_broadcast_import() - Broadcast IMPORT message to all processors
 * @ctx: DHMS context
 * @buf: Buffer entry to broadcast
 *
 * This function sends an IMPORT message to all registered processors in a
 * fire-and-forget manner. ACKs are captured asynchronously in the RPMSG
 * callback without blocking the caller.
 *
 * Returns: Number of successful sends, negative errno on total failure
 */
static int dhms_broadcast_import(struct dhms_device *ctx,
				 struct dhms_buffer_entry *buf)
{
	struct dhms_processor_entry *ref;
	struct dhms_glink_msg msg = {0};
	int send_count = 0;
	int ret;

	if (buf->num_processors == 0) {
		dev_warn(ctx->dma_dev,
			 "DHMS: Buffer %llu - no processors online, skipping IMPORT broadcast\n",
			 buf->client_id);
		return 0;
	}

	msg.msg_type = DHMS_MSG_IMPORT;
	msg.client_id = buf->client_id;
	msg.iova = (uint64_t)buf->iova;
	msg.size = (uint64_t)buf->size;
	msg.flags = buf->flags;

	spin_lock(&buf->ref_lock);
	list_for_each_entry(ref, &buf->processor_refs, node) {
		struct rpmsg_device *rpdev = ref->rpdev;

		if (!rpdev) {
			dev_warn(ctx->dma_dev,
				 "DHMS: Skipping IMPORT to %s — channel down (SSR)\n",
				 ref->processor_name);
			ref->ack_state = DHMS_ACK_NODEV;
			continue;
		}

		spin_unlock(&buf->ref_lock);
		ret = rpmsg_send(rpdev->ept, &msg, sizeof(msg));
		spin_lock(&buf->ref_lock);

		if (ret) {
			dev_err(ctx->dma_dev,
				"DHMS: Failed to send IMPORT to %s: %d (continuing)\n",
				ref->processor_name, ret);
			ref->ack_state = DHMS_ACK_NODEV;
		} else {
			send_count++;
			dev_dbg(ctx->dma_dev,
				"DHMS: Sending msg_type=%u (IMPORT) to %s client_id=%llu iova=0x%llx\n",
				DHMS_MSG_IMPORT, ref->processor_name,
				buf->client_id, (uint64_t)buf->iova);
		}
	}
	spin_unlock(&buf->ref_lock);

	if (send_count == 0) {
		dev_err(ctx->dma_dev, "DHMS: Failed to send to any processor\n");
		return -EIO;
	}

	return send_count;
}

/**
 * dhms_import_dmabuf_multi() - Import buffer to all processors
 * @ctx: DHMS context
 * @fd: DMA-BUF file descriptor from userspace
 * @flags: Caller-supplied flags forwarded to remote
 * @client_id_out: On success, receives the IDR-allocated client ID
 *
 * This function is the main entry point for buffer import. It orchestrates
 * DMA-BUF mapping via dhms_map_dmabuf(), bookkeeping setup via
 * dhms_setup_buffer_bookkeeping(), and IMPORT broadcast via
 * dhms_broadcast_import().
 *
 * Returns: 0 on success, negative errno on failure
 */
static int dhms_import_dmabuf_multi(struct dhms_device *ctx,
				    int fd,
				    uint32_t flags,
				    uint64_t *client_id_out)
{
	struct dhms_buffer_entry *buf = NULL;
	unsigned long irqflags;
	int ret;

	ret = dhms_map_dmabuf(ctx, fd, flags, &buf);
	if (ret) {
		dev_err(ctx->dma_dev, "DHMS: dhms_map_dmabuf failed: %d\n", ret);
		return ret;
	}

	ret = dhms_setup_buffer_bookkeeping(ctx, buf);
	if (ret) {
		dev_err(ctx->dma_dev, "DHMS: dhms_setup_buffer_bookkeeping failed: %d\n", ret);
		goto err_free_mapping;
	}

	ret = dhms_broadcast_import(ctx, buf);
	if (ret < 0) {
		dev_err(ctx->dma_dev, "DHMS: dhms_broadcast_import failed: %d\n", ret);
		goto err_cleanup_bookkeeping;
	}

	*client_id_out = buf->client_id;
	return 0;

err_cleanup_bookkeeping:
	dhms_cleanup_processor_refs(buf);
	spin_lock_irqsave(&ctx->lock, irqflags);
	idr_remove(&ctx->buf_idr, buf->client_id);
	spin_unlock_irqrestore(&ctx->lock, irqflags);
err_free_mapping:
	dhms_buf_free_resources(buf);
	kfree(buf);
	return ret;
}

/* ================================================================
 * Multi-Processor Buffer Release
 * ================================================================
 */

/**
 * dhms_send_release_to_all() - Send RELEASE message to all processors
 * @ctx: DHMS context
 * @buf: Buffer entry with processor refs
 *
 * This function sends a RELEASE message to all processors that hold a
 * reference to this buffer in a fire-and-forget manner. RELEASE_ACKs are
 * captured asynchronously in the RPMSG callback for bookkeeping.
 *
 * Returns: Number of successful sends
 */
static int dhms_send_release_to_all(struct dhms_device *ctx,
				    struct dhms_buffer_entry *buf)
{
	struct dhms_processor_entry *ref;
	struct dhms_glink_msg msg = {0};
	int send_count = 0;
	int ret;

	msg.msg_type = DHMS_MSG_RELEASE;
	msg.client_id = buf->client_id;
	msg.iova = (uint64_t)buf->iova;
	msg.size = (uint64_t)buf->size;
	msg.flags = buf->flags;

	spin_lock(&buf->ref_lock);
	list_for_each_entry(ref, &buf->processor_refs, node) {
		struct rpmsg_device *rpdev = ref->rpdev;

		if (!rpdev) {
			dev_warn(ctx->dma_dev,
				 "DHMS: Skipping RELEASE to %s — channel down (SSR)\n",
				 ref->processor_name);
			ref->ack_state = DHMS_ACK_NODEV;
			continue;
		}

		spin_unlock(&buf->ref_lock);
		ret = rpmsg_send(rpdev->ept, &msg, sizeof(msg));
		spin_lock(&buf->ref_lock);

		if (ret) {
			dev_err(ctx->dma_dev,
				"DHMS: Failed to send RELEASE to %s: %d (continuing)\n",
				ref->processor_name, ret);
			ref->ack_state = DHMS_ACK_NODEV;
		} else {
			send_count++;
			dev_dbg(ctx->dma_dev,
				"DHMS: RELEASE broadcast to %s for client_id=%llu\n",
				ref->processor_name, buf->client_id);
		}
	}
	spin_unlock(&buf->ref_lock);

	return send_count;
}

/**
 * dhms_release_dmabuf_multi() - Release buffer from all processors
 * @ctx: DHMS context
 * @buf: Buffer entry to release
 *
 * This function implements a two-phase buffer release. Phase 1 broadcasts
 * RELEASE to all processors, immediately frees DMA-BUF resources, and moves
 * the buffer entry to ctx->pending_releases for ACK tracking. Phase 2 is
 * handled by dhms_handle_release_ack() which frees the struct once all
 * processors have acknowledged.
 *
 * Phase 1 (this function):
 *   - Broadcast RELEASE to all processors over GLink.
 *   - Immediately free the DMA-BUF resources (unmap IOVA, detach, put).
 *     The IOMMU mapping is torn down right away — the remote processors
 *     are notified but we do not wait for their ACK before unmapping.
 *   - Move the lightweight dhms_buffer_entry (now with NULL dma-buf
 *     pointers, but still holding client_id + processor_refs) into
 *     ctx->pending_releases for ACK tracking.
 *
 * Phase 2 (dhms_handle_release_ack):
 *   - When every processor sends RELEASE_ACK, the entry is removed from
 *     pending_releases, processor_refs are freed, and the struct is kfree'd.
 *
 * If no processors are registered (or all sends fail), the buffer is
 * freed entirely in this function — no ACKs will ever arrive.
 *
 * NOTE: The buffer must already be removed from ctx->buf_idr by the
 * caller before this function is invoked.
 */
static void dhms_release_dmabuf_multi(struct dhms_device *ctx,
				      struct dhms_buffer_entry *buf)
{
	unsigned long flags;
	int send_count;

	if (!buf)
		return;

	if (buf->num_processors == 0) {
		dev_warn(ctx->dma_dev,
			"DHMS: Buffer %llu has no processor refs, freeing directly\n",
			buf->client_id);
		dhms_buf_free_resources(buf);
		kfree(buf);
		return;
	}

	send_count = dhms_send_release_to_all(ctx, buf);

	/*
	 * WORKAROUND: Add a 1ms delay before tearing down the IOMMU mapping.
	 * TODO: Remove once LMCU firmware  add fix.
	 *
	 */
	if (send_count > 0)
		usleep_range(1000, 1500);

	dhms_buf_free_resources(buf);

	if (send_count > 0) {
		spin_lock_irqsave(&ctx->lock, flags);
		list_add_tail(&buf->release_node, &ctx->pending_releases);
		spin_unlock_irqrestore(&ctx->lock, flags);
		dev_dbg(ctx->dma_dev,
			 "DHMS: Buffer %llu resources freed; RELEASE sent to %d processors — ACKs pending\n",
			 buf->client_id, send_count);
	} else {
		dev_err(ctx->dma_dev,
			 "DHMS: Buffer %llu — failed to send RELEASE to any processor, freeing struct\n",
			 buf->client_id);
		dhms_cleanup_processor_refs(buf);
		kfree(buf);
	}
}

/* ================================================================
 * File operations  (/dev/dhms)
 * ================================================================
 */

/**
 * dhms_fops_open() - Allocate per-fd context on open()
 * @inode: Inode of the character device
 * @filp: File pointer for the opened file descriptor
 *
 * This function allocates and initializes a dhms_file_ctx for each open()
 * call, enabling per-fd buffer tracking and automatic cleanup on close.
 *
 * Returns: 0 on success, -ENOMEM on allocation failure
 */
static int dhms_fops_open(struct inode *inode, struct file *filp)
{
	struct dhms_device *ctx = container_of(inode->i_cdev, struct dhms_device, cdev);
	struct dhms_file_ctx *fctx;

	fctx = kzalloc(sizeof(*fctx), GFP_KERNEL);
	if (!fctx)
		return -ENOMEM;

	fctx->ctx = ctx;
	INIT_LIST_HEAD(&fctx->buf_list);
	mutex_init(&fctx->lock);
	filp->private_data = fctx;

	return 0;
}

/**
 * dhms_fops_release() - Called when userspace closes the fd
 * @inode: Inode (unused)
 * @filp: File pointer carrying the per-fd dhms_file_ctx
 *
 * This function releases all buffers imported by this fd that have not been
 * explicitly unmapped. For each buffer it removes it from the IDR and calls
 * dhms_release_dmabuf_multi() to broadcast RELEASE to all processors and
 * free the IOVA immediately. This ensures no buffer leaks when a process
 * exits without calling DHMS_IOC_UNMAP.
 *
 * Returns: 0 always
 */
static int dhms_fops_release(struct inode *inode, struct file *filp)
{
	struct dhms_file_ctx *fctx = filp->private_data;
	struct dhms_device *ctx  = fctx->ctx;
	struct dhms_buf_node *node, *tmp_node;
	struct dhms_buffer_entry *buf;
	unsigned long flags;

	mutex_lock(&fctx->lock);
	list_for_each_entry_safe(node, tmp_node, &fctx->buf_list, node) {
		spin_lock_irqsave(&ctx->lock, flags);
		buf = idr_find(&ctx->buf_idr, node->client_id);
		if (buf)
			idr_remove(&ctx->buf_idr, node->client_id);
		spin_unlock_irqrestore(&ctx->lock, flags);
		if (buf)
			dhms_release_dmabuf_multi(ctx, buf);
		list_del(&node->node);
		kfree(node);
	}
	mutex_unlock(&fctx->lock);
	kfree(fctx);
	return 0;
}

/**
 * dhms_fops_poll() - Poll support for /dev/dhms
 * @filp: File pointer
 * @wait: Poll table
 *
 * This function allows userspace to use poll()/select()/epoll() to detect
 * driver removal. It returns EPOLLHUP|EPOLLERR when the platform device is
 * being removed so userspace can close the fd gracefully. No readable or
 * writable events are generated as all operations are synchronous IOCTLs.
 *
 * Returns: 0 normally, EPOLLHUP|EPOLLERR on driver removal
 */
static __poll_t dhms_fops_poll(struct file *filp, poll_table *wait)
{
	struct dhms_file_ctx *fctx = filp->private_data;
	struct dhms_device *ctx  = fctx->ctx;

	poll_wait(filp, &ctx->wait_queue, wait);
	if (atomic_read(&ctx->is_removing))
		return EPOLLHUP | EPOLLERR;
	return 0;
}

/**
 * dhms_fops_ioctl() - Handle userspace IOCTL requests
 * @filp: File pointer (carries per-fd dhms_file_ctx)
 * @cmd: IOCTL command (DHMS_IOC_MAP or DHMS_IOC_UNMAP)
 * @arg: Userspace pointer to command-specific struct
 *
 * This function dispatches DHMS_IOC_MAP and DHMS_IOC_UNMAP IOCTL commands.
 * MAP imports a DMA-BUF, assigns a client_id, and broadcasts IMPORT to all
 * processors. UNMAP releases the buffer and broadcasts RELEASE.
 *
 * Returns: 0 on success, negative errno on failure
 */
static long dhms_fops_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct dhms_file_ctx *fctx = filp->private_data;
	struct dhms_device *ctx  = fctx->ctx;
	unsigned long flags;
	int ret  = 0;

	switch (cmd) {
	case DHMS_IOC_MAP: {
		struct dhms_dmabuf_import_data req;
		struct dhms_buf_node *buf_node;
		uint64_t client_id;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		buf_node = kzalloc(sizeof(*buf_node), GFP_KERNEL);
		if (!buf_node)
			return -ENOMEM;

		ret = dhms_import_dmabuf_multi(ctx, req.dmabuf_fd, req.flags, &client_id);
		if (ret) {
			dev_err(ctx->dma_dev, "DHMS: dhms_import_dmabuf_multi failed: %d\n", ret);
			kfree(buf_node);
			return ret;
		}
		buf_node->client_id = client_id;
		mutex_lock(&fctx->lock);
		list_add_tail(&buf_node->node, &fctx->buf_list);
		mutex_unlock(&fctx->lock);
		req.client_id = client_id;
		if (copy_to_user((void __user *)arg, &req, sizeof(req)))
			return -EFAULT;
		break;
	}
	case DHMS_IOC_UNMAP: {
		struct dhms_dmabuf_import_data req;
		struct dhms_buffer_entry *buf;
		struct dhms_buf_node *node, *tmp_node;
		bool found = false;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		mutex_lock(&fctx->lock);
		list_for_each_entry_safe(node, tmp_node, &fctx->buf_list, node) {
			if (node->client_id == req.client_id) {
				list_del(&node->node);
				kfree(node);
				found = true;
				break;
			}
		}
		mutex_unlock(&fctx->lock);

		if (!found)
			return -EINVAL;

		spin_lock_irqsave(&ctx->lock, flags);
		buf = idr_find(&ctx->buf_idr, req.client_id);
		if (buf)
			idr_remove(&ctx->buf_idr, req.client_id);
		spin_unlock_irqrestore(&ctx->lock, flags);

		if (!buf) {
			dev_err(ctx->dma_dev,
				 "DHMS: UNMAP client_id=%llu already released by remote processor\n",
				 req.client_id);
			return -ENOENT;
		}

		dhms_release_dmabuf_multi(ctx, buf);
		break;
	}
	default:
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static const struct file_operations dhms_fops = {
	.owner          = THIS_MODULE,
	.open           = dhms_fops_open,
	.release        = dhms_fops_release,
	.unlocked_ioctl = dhms_fops_ioctl,
	.compat_ioctl   = dhms_fops_ioctl,
	.poll           = dhms_fops_poll,
};

/* ================================================================
 * RPMSG callback (Multi-Processor ACK Handling)
 * ================================================================
 */

/**
 * dhms_update_processor_ref_ack() - Update ACK state for a processor_ref
 * @ref: Processor reference to update
 * @msg: Message containing status
 * @buf: Buffer entry (for acks_received counter)
 * @rpdev: RPMSG device (for logging)
 *
 * This function updates the ACK state of a processor reference entry to
 * DHMS_ACK_SUCCESS and increments the buffer's acks_received counter.
 */
static void dhms_update_processor_ref_ack(struct dhms_processor_entry *ref,
					  const struct dhms_glink_msg *msg,
					  struct dhms_buffer_entry *buf,
					  struct rpmsg_device *rpdev)
{
	if (ref->ack_state != DHMS_ACK_PENDING)
		return;
	ref->ack_state = DHMS_ACK_SUCCESS;
	atomic_inc(&buf->acks_received);
	dev_dbg(&rpdev->dev,
		"DHMS: IMPORT_ACK from %s for client_id=%llu\n",
		ref->processor_name, msg->client_id);
}

/**
 * dhms_handle_import_ack_multi() - Handle IMPORT_ACK in multi-processor mode
 * @buf: Buffer entry
 * @rpdev: RPMSG device that sent the ACK
 * @msg: The IMPORT_ACK message
 *
 * This function finds the processor reference entry matching @rpdev and
 * updates its ACK state via dhms_update_processor_ref_ack().
 *
 */
static void dhms_handle_import_ack_multi(struct dhms_buffer_entry *buf,
					 struct rpmsg_device *rpdev,
					 const struct dhms_glink_msg *msg)
{
	struct dhms_processor_entry *ref;

	spin_lock(&buf->ref_lock);
	list_for_each_entry(ref, &buf->processor_refs, node) {
		if (ref->rpdev == rpdev) {
			dhms_update_processor_ref_ack(ref, msg, buf, rpdev);
			break;
		}
	}
	spin_unlock(&buf->ref_lock);
}

/**
 * dhms_handle_import_ack() - Handle IMPORT_ACK message from a processor
 * @ctx: DHMS context
 * @rpdev: RPMSG device (identifies which processor sent the ACK)
 * @msg: The IMPORT_ACK message
 *
 * This function looks up the buffer by client_id and updates the ACK state
 * for the processor that sent the IMPORT_ACK.
 */
static void dhms_handle_import_ack(struct dhms_device *ctx,
				   struct rpmsg_device *rpdev,
				   const struct dhms_glink_msg *msg)
{
	struct dhms_buffer_entry *buf;
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);
	buf = idr_find(&ctx->buf_idr, msg->client_id);
	spin_unlock_irqrestore(&ctx->lock, flags);
	if (!buf) {
		dev_err(&rpdev->dev,
			 "DHMS: IMPORT_ACK for unknown client_id=%llu\n",
			 msg->client_id);
		return;
	}
	dhms_handle_import_ack_multi(buf, rpdev, msg);
}

/**
 * dhms_update_release_processor_ref() - Update RELEASE_ACK state for a processor_ref
 * @ref: Processor reference to update
 * @msg: Message containing status
 * @rpdev: RPMSG device (for logging)
 *
 * This function updates the ACK state of a processor reference entry to
 * DHMS_ACK_SUCCESS upon receiving a RELEASE_ACK from the remote processor.
 */
static void dhms_update_release_processor_ref(struct dhms_processor_entry *ref,
					      const struct dhms_glink_msg *msg,
					      struct rpmsg_device *rpdev)
{
	if (ref->ack_state != DHMS_ACK_PENDING)
		return;
	ref->ack_state = DHMS_ACK_SUCCESS;
	dev_dbg(&rpdev->dev,
		"DHMS: RELEASE_ACK from %s for client_id=%llu\n",
		ref->processor_name, msg->client_id);
}

/**
 * dhms_handle_release_ack_multi() - Handle RELEASE_ACK in multi-processor mode
 * @buf: Buffer entry
 * @rpdev: RPMSG device that sent the ACK
 * @msg: The RELEASE_ACK message
 *
 * This function finds the processor reference entry matching @rpdev and
 * updates its RELEASE_ACK state via dhms_update_release_processor_ref().
 *
 */
static void dhms_handle_release_ack_multi(struct dhms_buffer_entry *buf,
					  struct rpmsg_device *rpdev,
					  const struct dhms_glink_msg *msg)
{
	struct dhms_processor_entry *ref;

	spin_lock(&buf->ref_lock);
	list_for_each_entry(ref, &buf->processor_refs, node) {
		if (ref->rpdev == rpdev) {
			dhms_update_release_processor_ref(ref, msg, rpdev);
			break;
		}
	}
	spin_unlock(&buf->ref_lock);
}

/**
 * dhms_find_buffer_in_pending_releases() - Find buffer in pending_releases list
 * @ctx: DHMS context
 * @client_id: Client ID to search for
 *
 * This function searches the pending_releases list for a buffer entry with
 * a matching client_id. The caller must hold ctx->lock.
 *
 * Returns: Buffer entry if found, NULL otherwise
 */
static struct dhms_buffer_entry *dhms_find_buffer_in_pending_releases(
	struct dhms_device *ctx,
	uint64_t client_id)
{
	struct dhms_buffer_entry *buf;

	list_for_each_entry(buf, &ctx->pending_releases, release_node) {
		if (buf->client_id == client_id)
			return buf;
	}
	return NULL;
}

/**
 * dhms_handle_release_req() - Handle DHMS_MSG_RELEASE message from a processor
 * @ctx: DHMS context
 * @rpdev: RPMSG device (identifies which processor sent the release request)
 * @msg: The DHMS_MSG_RELEASE message
 *
 * This function handles a remote RELEASE request by removing the buffer from
 * the IDR, freeing its DMA-BUF resources, and broadcasting RELEASE_ACK to
 * all processors that held the buffer.
 */
static int dhms_handle_release_req(struct dhms_device *ctx,
				   struct rpmsg_device *rpdev,
				   const struct dhms_glink_msg *msg)
{
	struct dhms_buffer_entry *buf;
	struct dhms_processor_entry *ref;
	struct dhms_glink_msg ack = {0};
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&ctx->lock, flags);
	buf = idr_find(&ctx->buf_idr, msg->client_id);
	if (buf) {
		idr_remove(&ctx->buf_idr, msg->client_id);
		if (!list_empty(&buf->release_node))
			list_del_init(&buf->release_node);
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (!buf) {
		dev_warn(&rpdev->dev,
			 "DHMS: Remote RELEASE for already-removed client_id=%llu\n",
			 msg->client_id);
		goto send_ack_single;
	}

	/*
	 * WORKAROUND: Add a 1ms delay before tearing down the IOMMU mapping.
	 * TODO: Remove once LMCU firmware adds fix
	 *
	 */
	usleep_range(1000, 1500);

	dhms_buf_free_resources(buf);

	dev_dbg(&rpdev->dev,
		 "DHMS: Remote RELEASE handled for client_id=%llu — broadcasting ACK\n",
		 msg->client_id);

	ack.msg_type  = DHMS_MSG_RELEASE_ACK;
	ack.client_id = msg->client_id;

	spin_lock(&buf->ref_lock);
	list_for_each_entry(ref, &buf->processor_refs, node) {
		struct rpmsg_device *ref_rpdev = ref->rpdev;

		spin_unlock(&buf->ref_lock);
		ret = rpmsg_send(ref_rpdev->ept, &ack, sizeof(ack));
		spin_lock(&buf->ref_lock);

		if (ret)
			dev_err(&rpdev->dev,
				"DHMS: Failed to send RELEASE_ACK to %s: %d\n",
				ref->processor_name, ret);
		else
			dev_dbg(&rpdev->dev,
				 "DHMS: RELEASE_ACK sent to %s for client_id=%llu\n",
				 ref->processor_name, msg->client_id);
	}
	spin_unlock(&buf->ref_lock);

	dhms_cleanup_processor_refs(buf);
	kfree(buf);

	return 0;

send_ack_single:
	ack.msg_type  = DHMS_MSG_RELEASE_ACK;
	ack.client_id = msg->client_id;
	ack.status    = DHMS_STATUS_CLIENT_ALREADY_RELEASED;
	dev_err(&rpdev->dev,
		"DHMS: Sending error RELEASE_ACK for already-released client_id=%llu\n",
		msg->client_id);
	ret = rpmsg_send(rpdev->ept, &ack, sizeof(ack));
	if (ret)
		dev_err(&rpdev->dev,
			"DHMS: Failed to send RELEASE_ACK: %d\n", ret);
	return ret;
}

/**
 * dhms_handle_release_ack() - Handle RELEASE_ACK message from a processor
 * @ctx: DHMS context
 * @rpdev: RPMSG device (identifies which processor sent the ACK)
 * @msg: The RELEASE_ACK message
 *
 * This function finds the buffer in pending_releases and updates the ACK
 * state for the processor that sent the RELEASE_ACK. When all processors
 * have acknowledged, the buffer struct is freed.
 */
static void dhms_handle_release_ack(struct dhms_device *ctx,
				    struct rpmsg_device *rpdev,
				    const struct dhms_glink_msg *msg)
{
	struct dhms_buffer_entry *buf;
	struct dhms_processor_entry *ref;
	unsigned long flags;
	bool all_acked = true;

	spin_lock_irqsave(&ctx->lock, flags);
	buf = dhms_find_buffer_in_pending_releases(ctx, msg->client_id);
	if (!buf) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return;
	}
	dhms_handle_release_ack_multi(buf, rpdev, msg);

	spin_lock(&buf->ref_lock);
	list_for_each_entry(ref, &buf->processor_refs, node) {
		if (ref->ack_state == DHMS_ACK_PENDING) {
			all_acked = false;
			break;
		}
	}
	spin_unlock(&buf->ref_lock);

	if (all_acked)
		list_del(&buf->release_node);

	spin_unlock_irqrestore(&ctx->lock, flags);

	if (!all_acked)
		return;

	dev_dbg(ctx->dma_dev,
		 "DHMS: All RELEASE_ACKs received for buffer %llu — freeing struct\n",
		 buf->client_id);
	dhms_cleanup_processor_refs(buf);
	kfree(buf);
}

/**
 * dhms_handle_query_req() - Handle QUERY_REQ message from a processor
 * @ctx: DHMS context
 * @rpdev: RPMSG device (identifies which processor sent the query)
 * @msg: The QUERY_REQ message
 *
 * This function looks up the buffer by client_id and sends a QUERY_RSP
 * message back on the same RPMSG channel that sent the query.
 */
static int dhms_handle_query_req(struct dhms_device *ctx,
				  struct rpmsg_device *rpdev,
				  const struct dhms_glink_msg *msg)
{
	struct dhms_buffer_entry *buf;
	struct dhms_glink_msg resp = {0};
	unsigned long flags;
	int ret;

	resp.msg_type = DHMS_MSG_QUERY_RSP;
	resp.client_id = msg->client_id;

	spin_lock_irqsave(&ctx->lock, flags);
	buf = idr_find(&ctx->buf_idr, msg->client_id);
	if (!buf) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		dev_err(&rpdev->dev,
			"DHMS: QUERY_REQ for unknown/released client_id=%llu, sending error RSP\n",
			msg->client_id);
		resp.status = DHMS_STATUS_CLIENT_NOT_FOUND;
		ret = rpmsg_send(rpdev->ept, &resp, sizeof(resp));
		if (ret)
			dev_err(&rpdev->dev,
				"DHMS: Failed to send error QUERY_RSP client_id=%llu ret=%d\n",
				msg->client_id, ret);
		return -EINVAL;
	}

	resp.iova = (uint64_t)buf->iova;
	resp.size = (uint64_t)buf->size;
	resp.flags = buf->flags;
	resp.status = DHMS_STATUS_OK;

	spin_unlock_irqrestore(&ctx->lock, flags);
	ret = rpmsg_send(rpdev->ept, &resp, sizeof(resp));
	if (ret)
		dev_err(&rpdev->dev,
			"DHMS: QUERY_RSP send failed client_id=%llu ret=%d\n",
			msg->client_id, ret);
	else
		dev_dbg(&rpdev->dev,
			"DHMS: Sending msg_type=%u (QUERY_RSP) client_id=%llu iova=0x%llx\n",
			resp.msg_type, resp.client_id, resp.iova);
	return 0;
}

/**
 * dhms_handle_query_rsp_ack() - Handle QUERY_RSP_ACK message from a processor
 * @rpdev: RPMSG device (identifies which processor sent the ACK)
 * @msg: The QUERY_RSP_ACK message
 *
 * This function logs the QUERY_RSP_ACK received from a remote processor
 * confirming that the QUERY_RSP was received successfully.
 */
static void dhms_handle_query_rsp_ack(struct rpmsg_device *rpdev,
				      const struct dhms_glink_msg *msg)
{
	dev_dbg(&rpdev->dev,
		"DHMS: QUERY_RSP_ACK received from processor for client_id=%llu\n",
		msg->client_id);
}

/**
 * dhms_rpmsg_callback() - Handle messages from remote processors
 * @rpdev: RPMSG device (identifies which processor sent the message)
 * @data: Message data
 * @len: Message length
 * @priv: Private data
 * @src: Source address
 *
 * This function is the main dispatcher for RPMSG messages. It validates the
 * message length and routes each message to the appropriate handler based on
 * the msg_type field.
 *
 * Returns: 0 on success
 */
static int dhms_rpmsg_callback(struct rpmsg_device *rpdev, void *data,
			       int len, void *priv, u32 src)
{
	struct dhms_device *ctx = dev_get_drvdata(&rpdev->dev);
	const struct dhms_glink_msg *msg = data;
	int ret = 0;

	if (!ctx || !data || len < (int)sizeof(*msg))
		return 0;

	switch (msg->msg_type) {
	case DHMS_MSG_RELEASE:
		ret = dhms_handle_release_req(ctx, rpdev, msg);
		if (ret < 0)
			dev_err(&rpdev->dev, "DHMS: dhms_handle_release_req failed with %d\n", ret);
		break;
	case DHMS_MSG_IMPORT_ACK:
		dhms_handle_import_ack(ctx, rpdev, msg);
		break;
	case DHMS_MSG_RELEASE_ACK:
		dhms_handle_release_ack(ctx, rpdev, msg);
		break;
	case DHMS_MSG_QUERY_REQ:
		ret = dhms_handle_query_req(ctx, rpdev, msg);
		if (ret)
			dev_err(&rpdev->dev, "DHMS: dhms_handle_query_req failed with %d\n", ret);
		break;
	case DHMS_MSG_QUERY_RSP_ACK:
		dhms_handle_query_rsp_ack(rpdev, msg);
		break;
	default:
		dev_warn(&rpdev->dev, "DHMS: unknown msg_type=%u\n",
			msg->msg_type);
		break;
	}
	return 0;
}

/**
 * dhms_extract_processor_name() - Extract processor name from channel name
 * @channel_name: RPMSG channel name (e.g., "dhms", "dhms_lmcu")
 * @proc_name: Output buffer for processor name
 * @size: Size of output buffer
 *
 * This function extracts the processor name from the RPMSG channel name by
 * taking the substring after the first underscore (e.g., "dhms_lmcu" -> "lmcu").
 */
static void dhms_extract_processor_name(const char *channel_name,
					char *proc_name, size_t size)
{
	const char *underscore;

	if (!channel_name || !proc_name || !size)
		return;
	underscore = strchr(channel_name, '_');
	if (underscore && strlen(underscore + 1) > 0)
		strscpy(proc_name, underscore + 1, size);
}

/* ================================================================
 * RPMSG probe / remove (Multi-Processor Support)
 * ================================================================
 */

/**
 * dhms_rpmsg_probe() - Called when a GLink channel connects
 * @rpdev: RPMSG device representing the GLink channel
 *
 * This function is called when a new GLink channel connects. Each channel is
 * point-to-point between the AP and one remote processor. It extracts the
 * processor name from the channel name and registers the processor with the
 * DHMS context.
 */
static int dhms_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct dhms_device *ctx;
	char proc_name[32];
	int ret;

	mutex_lock(&g_dhms_dev_lock);
	ctx = g_dhms_dev;
	mutex_unlock(&g_dhms_dev_lock);

	if (!ctx) {
		dev_err(&rpdev->dev,
			"DHMS: platform device not probed yet\n");
		return -ENODEV;
	}
	dhms_extract_processor_name(rpdev->id.name, proc_name, sizeof(proc_name));

	dev_set_drvdata(&rpdev->dev, ctx);

	ret = dhms_register_processor(ctx, proc_name, rpdev);
	if (ret) {
		dev_err(&rpdev->dev,
			"DHMS: Failed to register processor '%s' (channel: %s): %d\n",
			proc_name, rpdev->id.name, ret);
		return ret;
	}

	dev_info(&rpdev->dev,
		 "DHMS: Processor '%s' connected via GLink channel '%s'\n",
		 proc_name, rpdev->id.name);

	return 0;
}

/**
 * dhms_invalidate_processor_refs_for_rpdev() - Clear stale rpdev in buffer entries on SSR
 * @ctx: DHMS context
 * @rpdev: The rpmsg_device being torn down
 *
 * FIX for use-after-free:
 *
 * This function walks all active buffers (ctx->buf_idr) and pending-release
 * buffers, and for every dhms_processor_entry whose ref->rpdev matches the
 * dying channel, sets ref->rpdev = NULL and ref->ack_state = DHMS_ACK_NODEV.
 *
 */
static void dhms_invalidate_processor_refs_for_rpdev(struct dhms_device *ctx,
						      struct rpmsg_device *rpdev)
{
	struct dhms_buffer_entry *buf;
	struct dhms_processor_entry *ref;
	unsigned long flags;
	int id;

	spin_lock_irqsave(&ctx->lock, flags);

	idr_for_each_entry(&ctx->buf_idr, buf, id) {
		spin_lock(&buf->ref_lock);
		list_for_each_entry(ref, &buf->processor_refs, node) {
			if (ref->rpdev == rpdev) {
				ref->rpdev     = NULL;
				ref->ack_state = DHMS_ACK_NODEV;
			}
		}
		spin_unlock(&buf->ref_lock);
	}

	list_for_each_entry(buf, &ctx->pending_releases, release_node) {
		spin_lock(&buf->ref_lock);
		list_for_each_entry(ref, &buf->processor_refs, node) {
			if (ref->rpdev == rpdev) {
				ref->rpdev     = NULL;
				ref->ack_state = DHMS_ACK_NODEV;
			}
		}
		spin_unlock(&buf->ref_lock);
	}

	spin_unlock_irqrestore(&ctx->lock, flags);
}

static void dhms_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct dhms_device *ctx = dev_get_drvdata(&rpdev->dev);

	if (!ctx)
		return;

	dhms_invalidate_processor_refs_for_rpdev(ctx, rpdev);

	dhms_unregister_processor(ctx, rpdev);
	dev_info(&rpdev->dev, "DHMS: Processor disconnected from GLink\n");
}

/**
 * RPMSG Device ID Table
 *
 * Each entry represents a separate GLink channel to one remote processor.
 *
 * Channel naming convention:
 *   - "dhms_mcu0"  -> LMCU remote processor 1
 *   - "dhms_mcu1"  -> LMCU remote processor 2
 *
 * The driver will extract the processor name from the channel name.
 */
static const struct rpmsg_device_id dhms_rpmsg_id_table[] = {
	{ .name = "dhms_mcu0" },
	{ }
};
MODULE_DEVICE_TABLE(rpmsg, dhms_rpmsg_id_table);

static struct rpmsg_driver dhms_rpmsg_driver = {
	.probe    = dhms_rpmsg_probe,
	.remove   = dhms_rpmsg_remove,
	.callback = dhms_rpmsg_callback,
	.id_table = dhms_rpmsg_id_table,
	.drv      = { .name = "dhms_rpmsg" },
};

/* ================================================================
 * Platform driver  (compatible = "qcom,dhms-rpmsg")
 * ================================================================
 */

static int dhms_platform_probe(struct platform_device *pdev)
{
	struct dhms_device *ctx;
	int ret;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dma_dev = &pdev->dev;
	if (!dev_iommu_fwspec_get(ctx->dma_dev)) {
		dev_err(&pdev->dev, "DHMS: Device has no IOMMU configuration\n");
		return -EINVAL;
	}

	strscpy(ctx->devname, "dhms", sizeof(ctx->devname));

	spin_lock_init(&ctx->lock);
	idr_init(&ctx->buf_idr);
	INIT_LIST_HEAD(&ctx->pending_releases);
	init_waitqueue_head(&ctx->wait_queue);
	atomic_set(&ctx->is_removing, 0);

	INIT_LIST_HEAD(&ctx->processors);
	spin_lock_init(&ctx->processor_lock);
	ctx->num_processors = 0;

	ret = alloc_chrdev_region(&ctx->devt, 0, 1, ctx->devname);
	if (ret) {
		dev_err(&pdev->dev, "DHMS: alloc chardev region failed\n");
		return ret;
	}

	ctx->class = class_create(ctx->devname);
	if (IS_ERR(ctx->class)) {
		ret = PTR_ERR(ctx->class);
		goto err_unregister_chrdev;
	}

	cdev_init(&ctx->cdev, &dhms_fops);
	ctx->cdev.owner = THIS_MODULE;
	ret = cdev_add(&ctx->cdev, ctx->devt, 1);
	if (ret) {
		dev_err(&pdev->dev, "DHMS: char dev add failed\n");
		goto err_class_destroy;
	}

	ctx->chardev = device_create(ctx->class, &pdev->dev,
				     ctx->devt, NULL, "%s", ctx->devname);
	if (IS_ERR(ctx->chardev)) {
		ret = PTR_ERR(ctx->chardev);
		goto err_cdev_del;
	}

	mutex_lock(&g_dhms_dev_lock);
	g_dhms_dev = ctx;
	mutex_unlock(&g_dhms_dev_lock);
	platform_set_drvdata(pdev, ctx);

	ret = register_rpmsg_driver(&dhms_rpmsg_driver);
	if (ret) {
		dev_err(&pdev->dev,
			"DHMS: register_rpmsg_driver failed: %d\n", ret);
		goto err_device_destroy;
	}

	dev_info(&pdev->dev, "DHMS: /dev/%s ready\n", ctx->devname);

	return 0;

err_device_destroy:
	device_destroy(ctx->class, ctx->devt);
	mutex_lock(&g_dhms_dev_lock);
	g_dhms_dev = NULL;
	mutex_unlock(&g_dhms_dev_lock);
err_cdev_del:
	cdev_del(&ctx->cdev);
err_class_destroy:
	class_destroy(ctx->class);
err_unregister_chrdev:
	unregister_chrdev_region(ctx->devt, 1);
	return ret;
}

/**
 * dhms_complete_all_processor_refs() - Complete all processor ACKs for a buffer
 * @buf: Buffer entry with processor refs
 *
 * This function is called during driver removal to mark all pending ACKs as
 * DHMS_ACK_NODEV, unblocking any threads waiting for responses from remote
 * processors that are no longer reachable.
 */
static void dhms_complete_all_processor_refs(struct dhms_buffer_entry *buf)
{
	struct dhms_processor_entry *ref;

	if (!buf || buf->num_processors == 0)
		return;

	spin_lock(&buf->ref_lock);
	list_for_each_entry(ref, &buf->processor_refs, node) {
		if (ref->ack_state == DHMS_ACK_PENDING)
			ref->ack_state = DHMS_ACK_NODEV;
	}
	spin_unlock(&buf->ref_lock);
}

/**
 * dhms_platform_remove() - Platform driver remove callback
 * @pdev: Platform device
 *
 * This function is called when the DHMS platform device is being removed.
 * It signals removal to poll() waiters, unregisters all RPMSG channels,
 * completes all pending ACKs, frees all buffer resources, and cleans up
 * the character device nodes.
 */
static void dhms_platform_remove(struct platform_device *pdev)
{
	struct dhms_device *ctx = platform_get_drvdata(pdev);
	struct dhms_buffer_entry *buf, *tmp_buf;
	struct dhms_processor_ctx *proc, *tmp_proc;
	LIST_HEAD(cleanup_list);
	unsigned long flags;
	int id;

	if (!ctx) {
		dev_err(&pdev->dev, "DHMS:CTX is null\n");
		return;
	}

	atomic_set(&ctx->is_removing, 1);
	wake_up_interruptible(&ctx->wait_queue);

	unregister_rpmsg_driver(&dhms_rpmsg_driver);

	spin_lock_irqsave(&ctx->lock, flags);

	idr_for_each_entry(&ctx->buf_idr, buf, id) {
		idr_remove(&ctx->buf_idr, id);
		dhms_complete_all_processor_refs(buf);

		list_add_tail(&buf->release_node, &cleanup_list);
	}
	idr_destroy(&ctx->buf_idr);

	list_for_each_entry_safe(buf, tmp_buf, &ctx->pending_releases, release_node) {
		dhms_complete_all_processor_refs(buf);
		list_del(&buf->release_node);
		list_add_tail(&buf->release_node, &cleanup_list);
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	spin_lock_irqsave(&ctx->processor_lock, flags);
	list_for_each_entry_safe(proc, tmp_proc, &ctx->processors, node) {
		list_del(&proc->node);
		dev_err(&pdev->dev,
			 "DHMS: Cleaning up straggler processor '%s'\n",
			 proc->name);
		kfree(proc);
	}
	ctx->num_processors = 0;
	spin_unlock_irqrestore(&ctx->processor_lock, flags);

	list_for_each_entry_safe(buf, tmp_buf, &cleanup_list, release_node) {
		list_del(&buf->release_node);

		dhms_cleanup_processor_refs(buf);

		dhms_buf_free_resources(buf);
		kfree(buf);
	}

	device_destroy(ctx->class, ctx->devt);
	cdev_del(&ctx->cdev);
	class_destroy(ctx->class);
	unregister_chrdev_region(ctx->devt, 1);
	mutex_lock(&g_dhms_dev_lock);
	g_dhms_dev = NULL;
	mutex_unlock(&g_dhms_dev_lock);

	dev_info(&pdev->dev, "DHMS: /dev/%s removed\n", ctx->devname);
}

static const struct of_device_id dhms_rpmsg_of_match[] = {
	{ .compatible = "qcom,dhms-rpmsg" },
	{ }
};
MODULE_DEVICE_TABLE(of, dhms_rpmsg_of_match);

static struct platform_driver dhms_platform_driver = {
	.probe  = dhms_platform_probe,
	.remove = dhms_platform_remove,
	.driver = {
		.name           = "dhms_rpmsg",
		.of_match_table = dhms_rpmsg_of_match,
	},
};

module_platform_driver(dhms_platform_driver);

MODULE_DESCRIPTION("DHMS RPMSG Service Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(DMA_BUF);
