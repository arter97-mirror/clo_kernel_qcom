// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/ctype.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <uapi/misc/named_dmabuf.h>

#define CREATE_TRACE_POINTS
#include "named_dmabuf_trace.h"

MODULE_IMPORT_NS(DMA_BUF);

/* Max buffers per instance (ctrl cdev + up to 255 named buffers) */
#define NAMED_DMABUF_MAX_MINORS	256
#define NAMED_DMABUF_INST_NAME_LEN	64

static struct class *named_dmabuf_class;

/* Per-instance state (one per DT node / platform device) */

struct named_dmabuf_inst {
	/* Per-instance chrdev region and minor allocator */
	dev_t devt;
	struct ida minor_ida;

	/* Control cdev at /dev/named_dmabuf_<name>/ctrl */
	struct cdev ctrl_cdev;
	dev_t ctrl_devno;
	int ctrl_minor;

	/* Instance name (from DT "instance-name" or device name fallback) */
	char inst_name[NAMED_DMABUF_INST_NAME_LEN];

	/* Platform device; needed for dma_buf_attach in carveout validation */
	struct device *dev;

	/* Carveout backing this instance; 0 = no carveout (phase 1 behavior) */
	phys_addr_t carveout_base;
	size_t      carveout_size;

	/* Per-instance buffer registry */
	struct mutex registry_lock;
	struct list_head registry;

	/*
	 * Taken for read by every ioctl; taken for write by remove() after
	 * the ctrl cdev is destroyed, to drain all in-flight ioctls before
	 * tearing down the instance.
	 */
	struct rw_semaphore ioctl_rwsem;
	bool removing;
};

/* Per-buffer state */

struct named_dmabuf {
	struct kobject kobj;	/* lifetime anchor; freed via kobj release */
	struct kref refcount;
	char name[NAMED_DMABUF_NAME_LEN];
	bool rdonly;
	bool ready;
	bool dying;		/* set under registry_lock before list_del in destroy */
	bool carveout_validated; /* true = carveout_offset below is meaningful */
	u64  carveout_offset; /* bytes from inst->carveout_base */
	kuid_t creator_uid;
	struct dma_buf *dmabuf;
	struct cdev cdev;
	dev_t devno;
	int minor;
	struct list_head node;
	struct named_dmabuf_inst *inst;
};

/* Name validation */

static bool named_dmabuf_name_valid(const char *name, size_t len)
{
	size_t i;

	if (len == 0)
		return false;

	for (i = 0; i < len; i++) {
		char c = name[i];

		if (!isalnum(c) && c != '-' && c != '_' && c != '.')
			return false;
	}
	return true;
}

/* Registry helpers -- must be called with inst->registry_lock held */

static struct named_dmabuf *
__registry_lookup(struct named_dmabuf_inst *inst, const char *name,
		  bool ready_only)
{
	struct named_dmabuf *b;
	size_t name_len = strnlen(name, NAMED_DMABUF_NAME_LEN);

	list_for_each_entry(b, &inst->registry, node) {
		if (ready_only && !b->ready)
			continue;

		if (strnlen(b->name, NAMED_DMABUF_NAME_LEN) == name_len &&
		    !memcmp(b->name, name, name_len))
			return b;
	}
	return NULL;
}

static struct named_dmabuf *
__registry_lookup_by_buf(struct named_dmabuf_inst *inst, struct dma_buf *dmabuf)
{
	struct named_dmabuf *b;

	list_for_each_entry(b, &inst->registry, node) {
		if (b->dmabuf == dmabuf)
			return b;
	}
	return NULL;
}

/*
 * Look up a ready, non-dying entry by name and bump its kref atomically under
 * the lock.  Returns the entry with an elevated kref, or NULL if not found,
 * not yet ready, or already dying.  Callers must kref_put on all exit paths.
 */
static struct named_dmabuf *
__registry_acquire(struct named_dmabuf_inst *inst, const char *name)
{
	struct named_dmabuf *b;

	mutex_lock(&inst->registry_lock);
	b = __registry_lookup(inst, name, true);
	if (b) {
		if (!READ_ONCE(b->dying))
			kref_get(&b->refcount);
		else
			b = NULL;
	}
	mutex_unlock(&inst->registry_lock);
	return b;
}

/* Reference counting */

/*
 * kobj.release is the true destructor.  We use a separate kobject (not the
 * cdev's own kobject) as the parent of the cdev, so that:
 *   1. cdev's kobject uses ktype_cdev_default, which calls cdev_purge() on
 *      release — clearing all cached inode->i_cdev pointers before the cdev
 *      memory goes away.
 *   2. After cdev_purge, cdev puts its parent (our b->kobj), which eventually
 *      reaches zero and calls named_dmabuf_kobj_release → kfree(b).
 *
 * Refcount lifecycle:
 *   kobject_init  → kobj.refcount = 1  (initial ref, dropped by named_dmabuf_release)
 *   cdev_add      → kobj.refcount += 1  (cdev's parent ref, dropped by cdev_del)
 *   kref gates logical destruction; when kref → 0, named_dmabuf_release drops
 *   the initial kobj ref; kfree fires only after the cdev parent ref also drops.
 */
static void named_dmabuf_kobj_release(struct kobject *kobj)
{
	struct named_dmabuf *b = container_of(kobj, struct named_dmabuf, kobj);

	kfree(b);
}

static const struct kobj_type named_dmabuf_ktype = {
	.release = named_dmabuf_kobj_release,
};

static void named_dmabuf_release(struct kref *ref)
{
	struct named_dmabuf *b = container_of(ref, struct named_dmabuf, refcount);

	/*
	 * kref hits zero only after both the registry's ref and all open-fd
	 * refs have been dropped, so no concurrent open path can be calling
	 * get_dma_buf(b->dmabuf) at this point — safe to release.
	 */
	dma_buf_put(b->dmabuf);
	kobject_put(&b->kobj);
}

static void named_dmabuf_unregister_cdev(struct named_dmabuf *b)
{
	struct named_dmabuf_inst *inst = b->inst;

	device_destroy(named_dmabuf_class, b->devno);
	cdev_del(&b->cdev);
	ida_free(&inst->minor_ida, b->minor);
}

/* cdev file operations -- consumers open /dev/named_dmabuf_<inst>/<name> */

static int named_dmabuf_cdev_open(struct inode *inode, struct file *filp)
{
	struct named_dmabuf *b;
	int ret;

	b = container_of(inode->i_cdev, struct named_dmabuf, cdev);

	if (!kref_get_unless_zero(&b->refcount)) {
		trace_named_dmabuf_open_err(b->name, -ENODEV);
		return -ENODEV;
	}

	/*
	 * Reject opens on a logically-destroyed entry.  dying is set under
	 * registry_lock before list_del in named_dmabuf_destroy, so any open
	 * that incremented kref after list_del will see it here.
	 */
	if (READ_ONCE(b->dying) || !READ_ONCE(b->ready)) {
		ret = -ENODEV;
		goto err_put;
	}

	if (READ_ONCE(b->rdonly) && (filp->f_flags & O_ACCMODE) != O_RDONLY) {
		ret = -EACCES;
		goto err_put;
	}

	get_dma_buf(READ_ONCE(b->dmabuf));
	filp->private_data = b;
	trace_named_dmabuf_open(b->name, READ_ONCE(b->rdonly));
	return 0;

err_put:
	trace_named_dmabuf_open_err(b->name, ret);
	kref_put(&b->refcount, named_dmabuf_release);
	return ret;
}

static int named_dmabuf_cdev_release(struct inode *inode, struct file *filp)
{
	struct named_dmabuf *b = filp->private_data;

	trace_named_dmabuf_release(b->name);
	dma_buf_put(READ_ONCE(b->dmabuf));
	kref_put(&b->refcount, named_dmabuf_release);
	return 0;
}

static int named_dmabuf_cdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct named_dmabuf *b = filp->private_data;

	if (READ_ONCE(b->rdonly) && (vma->vm_flags & VM_WRITE))
		return -EACCES;

	trace_named_dmabuf_mmap(b->name, READ_ONCE(b->rdonly));
	return dma_buf_mmap(READ_ONCE(b->dmabuf), vma, 0);
}

static const struct file_operations named_cdev_fops = {
	.owner		= THIS_MODULE,
	.open		= named_dmabuf_cdev_open,
	.release	= named_dmabuf_cdev_release,
	.mmap		= named_dmabuf_cdev_mmap,
	.llseek		= noop_llseek,
};

/* Core create / destroy */

/*
 * Register @dmabuf under @name for this instance.  Takes a dma_buf * directly;
 * the caller is responsible for calling dma_buf_get() or constructing the buf.
 * The registry duplicate check and list_add happen before cdev_add so that no
 * open() can race against the check.  The entry is inserted with ready=false
 * and only becomes visible to consumers after device_create succeeds.  The
 * driver holds one dmabuf ref for the lifetime of the registry entry.
 */
static int __named_dmabuf_register(struct named_dmabuf_inst *inst,
				   const char *name, struct dma_buf *dmabuf,
				   bool rdonly, bool carveout_validated,
				   u64 carveout_offset)
{
	struct named_dmabuf *b;
	struct device *dev;
	int ret;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b) {
		dma_buf_put(dmabuf);
		return -ENOMEM;
	}

	kobject_init(&b->kobj, &named_dmabuf_ktype);
	kref_init(&b->refcount);
	strscpy(b->name, name, sizeof(b->name));
	WRITE_ONCE(b->dmabuf, dmabuf);
	WRITE_ONCE(b->rdonly, rdonly);
	b->carveout_validated = carveout_validated;
	b->carveout_offset    = carveout_offset;
	b->creator_uid = current_uid();
	b->inst = inst;

	b->minor = ida_alloc_max(&inst->minor_ida, NAMED_DMABUF_MAX_MINORS - 1, GFP_KERNEL);
	if (b->minor < 0) {
		ret = b->minor;
		goto err_free_no_ida;
	}
	b->devno = MKDEV(MAJOR(inst->devt), b->minor);

	cdev_init(&b->cdev, &named_cdev_fops);
	b->cdev.owner = THIS_MODULE;
	/*
	 * Make b->kobj the parent of the cdev kobject.  When the last consumer
	 * fd closes, cdev's kobject release (ktype_cdev_default) calls
	 * cdev_purge() to clear all inode->i_cdev back-pointers, then puts the
	 * parent (b->kobj).  Only after that final put does named_dmabuf_kobj_release
	 * fire and kfree(b).
	 */
	b->cdev.kobj.parent = &b->kobj;

	/* Check removing, duplicates, and insert atomically. */
	mutex_lock(&inst->registry_lock);
	if (READ_ONCE(inst->removing)) {
		mutex_unlock(&inst->registry_lock);
		ret = -ENODEV;
		goto err_free;
	}
	if (__registry_lookup(inst, name, false) ||
	    __registry_lookup_by_buf(inst, dmabuf)) {
		mutex_unlock(&inst->registry_lock);
		ret = -EEXIST;
		goto err_free;
	}
	list_add(&b->node, &inst->registry);
	mutex_unlock(&inst->registry_lock);

	ret = cdev_add(&b->cdev, b->devno, 1);
	if (ret)
		goto err_list;

	dev = device_create(named_dmabuf_class, NULL, b->devno, b,
			    "named_dmabuf_%s/%s", inst->inst_name, name);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err_cdev;
	}

	mutex_lock(&inst->registry_lock);
	WRITE_ONCE(b->ready, true);
	mutex_unlock(&inst->registry_lock);

	trace_named_dmabuf_create(name, rdonly);
	return 0;

err_cdev:
	cdev_del(&b->cdev);
err_list:
	mutex_lock(&inst->registry_lock);
	list_del(&b->node);
	mutex_unlock(&inst->registry_lock);
err_free:
	ida_free(&inst->minor_ida, b->minor);
err_free_no_ida:
	kref_put(&b->refcount, named_dmabuf_release);
	trace_named_dmabuf_create_err(name, ret);
	return ret;
}

/*
 * Validate that @dmabuf is physically contiguous and falls within @inst's
 * carveout.  On success writes the byte offset from carveout_base into
 * @offset.  The attach/map/unmap/detach sequence is validation-only; the
 * registry entry holds a normal dmabuf ref, not a persistent attachment.
 */
static int named_dmabuf_validate_carveout(struct named_dmabuf_inst *inst,
					  struct dma_buf *dmabuf, u64 *offset)
{
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	phys_addr_t pa;
	size_t size;
	int ret = 0;

	attach = dma_buf_attach(dmabuf, inst->dev);
	if (IS_ERR(attach))
		return PTR_ERR(attach);

	/*
	 * The carveout is no-map: it has no kernel VA in the linear map, so
	 * any SWIOTLB bounce attempt will fault.  Tell the DMA layer we can
	 * consume the physical address directly (peer2peer) and skip the CPU
	 * cache-sync that would also dereference the unmapped VA.
	 */
	attach->peer2peer      = true;
	attach->dma_map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_detach;
	}

	if (sgt->orig_nents != 1) {
		ret = -EINVAL;
		goto err_unmap;
	}

	pa   = sg_dma_address(sgt->sgl);
	size = sg_dma_len(sgt->sgl);

	if (pa < inst->carveout_base ||
	    size > inst->carveout_size ||
	    pa - inst->carveout_base > inst->carveout_size - size) {
		ret = -ERANGE;
		goto err_unmap;
	}

	*offset = pa - inst->carveout_base;

err_unmap:
	dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(dmabuf, attach);
	return ret;
}

/*
 * IOC_CREATE path: get the dmabuf from userspace fd, optionally validate
 * against the carveout, then register via __named_dmabuf_register.
 */
static int named_dmabuf_create(struct named_dmabuf_inst *inst,
				const char *name, int dmabuf_fd, bool rdonly)
{
	struct dma_buf *dmabuf;
	bool validated = false;
	u64 offset = 0;
	int ret;

	dmabuf = dma_buf_get(dmabuf_fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	if (inst->carveout_size) {
		ret = named_dmabuf_validate_carveout(inst, dmabuf, &offset);
		if (ret) {
			dma_buf_put(dmabuf);
			return ret;
		}
		validated = true;
	}

	return __named_dmabuf_register(inst, name, dmabuf, rdonly,
				       validated, offset);
}

/*
 * Unregister @name from the instance registry.  Requires the caller to be the
 * creator (matched by UID) or hold CAP_SYS_ADMIN.  Drops the driver's dmabuf
 * ref and the registry kref; existing consumer fds remain valid because each
 * open holds its own dmabuf ref and kref.
 */
static int named_dmabuf_destroy(struct named_dmabuf_inst *inst, const char *name)
{
	struct named_dmabuf *b;

	mutex_lock(&inst->registry_lock);
	b = __registry_lookup(inst, name, true);
	if (!b) {
		mutex_unlock(&inst->registry_lock);
		trace_named_dmabuf_destroy_err(name, -ENOENT);
		return -ENOENT;
	}

	if (!uid_eq(current_uid(), b->creator_uid) &&
	    !ns_capable(&init_user_ns, CAP_SYS_ADMIN)) {
		mutex_unlock(&inst->registry_lock);
		trace_named_dmabuf_destroy_err(name, -EPERM);
		return -EPERM;
	}

	/*
	 * Mark dying before list_del so that any open() that races past
	 * kref_get_unless_zero sees the flag and bails.
	 */
	WRITE_ONCE(b->dying, true);
	list_del(&b->node);
	mutex_unlock(&inst->registry_lock);

	named_dmabuf_unregister_cdev(b);
	/* dma_buf_put deferred to named_dmabuf_release — see kref comment */
	kref_put(&b->refcount, named_dmabuf_release);
	trace_named_dmabuf_destroy(name);
	return 0;
}

/* In-driver raw dmabuf exporter — used only for IOC_IMPORT_REMOTE */

struct ndmabuf_raw_buf {
	phys_addr_t paddr;
	size_t      size;
};

static struct sg_table *ndmabuf_raw_map(struct dma_buf_attachment *attach,
					enum dma_data_direction dir)
{
	struct ndmabuf_raw_buf *raw = attach->dmabuf->priv;
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	/*
	 * No-map carveout: the hypervisor has already established the mapping.
	 * Set dma address and length directly; there is no valid struct page for
	 * this memory so we skip sg_set_page to avoid pfn_to_page on an unmapped
	 * PFN, which would produce a garbage page pointer on platforms where the
	 * carveout lies outside the kernel memmap.
	 */
	sg_dma_address(sgt->sgl) = raw->paddr;
	sg_dma_len(sgt->sgl)     = raw->size;

	return sgt;
}

static void ndmabuf_raw_unmap(struct dma_buf_attachment *attach,
			      struct sg_table *sgt,
			      enum dma_data_direction dir)
{
	sg_free_table(sgt);
	kfree(sgt);
}

static int ndmabuf_raw_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct ndmabuf_raw_buf *raw = dmabuf->priv;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > raw->size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start,
			       PHYS_PFN(raw->paddr), size,
			       vma->vm_page_prot);
}

static void ndmabuf_raw_release(struct dma_buf *dmabuf)
{
	kfree(dmabuf->priv);
}

static const struct dma_buf_ops ndmabuf_raw_ops = {
	.map_dma_buf   = ndmabuf_raw_map,
	.unmap_dma_buf = ndmabuf_raw_unmap,
	.mmap          = ndmabuf_raw_mmap,
	.release       = ndmabuf_raw_release,
};

static struct dma_buf *ndmabuf_raw_create(phys_addr_t paddr, size_t size)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct ndmabuf_raw_buf *raw;
	struct dma_buf *dmabuf;

	raw = kzalloc(sizeof(*raw), GFP_KERNEL);
	if (!raw)
		return ERR_PTR(-ENOMEM);

	raw->paddr = paddr;
	raw->size  = size;

	exp_info.ops   = &ndmabuf_raw_ops;
	exp_info.size  = size;
	exp_info.flags = O_RDWR;
	exp_info.priv  = raw;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf))
		kfree(raw);
	return dmabuf;
}

/* Control device ioctl handlers */

/*
 * IOC_CREATE: validate the userspace struct, then register the supplied
 * dmabuf fd under the given name via named_dmabuf_create().
 */
static int named_dmabuf_ioctl_create(struct named_dmabuf_inst *inst,
				     unsigned long arg)
{
	struct named_dmabuf_create c;

	if (copy_from_user(&c, (void __user *)arg, sizeof(c)))
		return -EFAULT;

	c.name[NAMED_DMABUF_NAME_LEN - 1] = '\0';
	if (!named_dmabuf_name_valid(c.name, strnlen(c.name, NAMED_DMABUF_NAME_LEN)))
		return -EINVAL;

	if (c.flags & ~NAMED_DMABUF_F_MASK)
		return -EINVAL;

	if (c.dmabuf_fd < 0)
		return -EBADF;

	return named_dmabuf_create(inst, c.name, c.dmabuf_fd,
				   !!(c.flags & NAMED_DMABUF_F_RDONLY));
}

/*
 * IOC_RECOVER_FD: re-issue a dmabuf fd for a buffer the caller originally
 * created.  Restricted to the creator UID or CAP_SYS_ADMIN.  Acquires a fresh
 * dmabuf ref that is transferred to the new fd; the registry's own ref is
 * untouched.
 */
static int named_dmabuf_ioctl_recover_fd(struct named_dmabuf_inst *inst,
					 unsigned long arg)
{
	struct named_dmabuf_recover e;
	struct named_dmabuf *b;
	struct dma_buf *dmabuf;
	bool rdonly;
	int fd;

	if (copy_from_user(&e, (void __user *)arg, sizeof(e)))
		return -EFAULT;

	e.name[NAMED_DMABUF_NAME_LEN - 1] = '\0';
	if (!named_dmabuf_name_valid(e.name, strnlen(e.name, NAMED_DMABUF_NAME_LEN)))
		return -EINVAL;

	if (e._pad != 0)
		return -EINVAL;

	b = __registry_acquire(inst, e.name);
	if (!b) {
		trace_named_dmabuf_recover_err(e.name, -ENOENT);
		return -ENOENT;
	}

	if (!uid_eq(current_uid(), b->creator_uid) &&
	    !ns_capable(&init_user_ns, CAP_SYS_ADMIN)) {
		kref_put(&b->refcount, named_dmabuf_release);
		trace_named_dmabuf_recover_err(e.name, -EPERM);
		return -EPERM;
	}

	dmabuf = READ_ONCE(b->dmabuf);
	rdonly = READ_ONCE(b->rdonly);
	get_dma_buf(dmabuf);
	fd = dma_buf_fd(dmabuf, O_CLOEXEC | (rdonly ? O_RDONLY : O_RDWR));
	if (fd < 0) {
		kref_put(&b->refcount, named_dmabuf_release);
		dma_buf_put(dmabuf);
		trace_named_dmabuf_recover_err(e.name, fd);
		return fd;
	}
	e.dmabuf_fd = fd;
	if (copy_to_user((void __user *)arg, &e, sizeof(e))) {
		close_fd(fd);
		kref_put(&b->refcount, named_dmabuf_release);
		trace_named_dmabuf_recover_err(e.name, -EFAULT);
		return -EFAULT;
	}
	kref_put(&b->refcount, named_dmabuf_release);
	trace_named_dmabuf_recover(e.name, fd, rdonly);
	return 0;
}

/*
 * IOC_DESTROY: validate the name and delegate to named_dmabuf_destroy().
 */
static int named_dmabuf_ioctl_destroy(struct named_dmabuf_inst *inst,
				      unsigned long arg)
{
	struct named_dmabuf_destroy d;

	if (copy_from_user(&d, (void __user *)arg, sizeof(d)))
		return -EFAULT;

	d.name[NAMED_DMABUF_NAME_LEN - 1] = '\0';
	if (!named_dmabuf_name_valid(d.name, strnlen(d.name, NAMED_DMABUF_NAME_LEN)))
		return -EINVAL;

	return named_dmabuf_destroy(inst, d.name);
}

/*
 * IOC_GET_EXPORT_INFO: return the carveout export tuple for a named buffer.
 */
static int named_dmabuf_ioctl_get_export_info(struct named_dmabuf_inst *inst,
					      unsigned long arg)
{
	struct named_dmabuf_export_info info;
	struct named_dmabuf *b;
	struct dma_buf *dmabuf;
	int err = 0;

	if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
		return -EFAULT;

	info.name[NAMED_DMABUF_NAME_LEN - 1] = '\0';
	if (!named_dmabuf_name_valid(info.name,
				     strnlen(info.name, NAMED_DMABUF_NAME_LEN)))
		return -EINVAL;

	b = __registry_acquire(inst, info.name);
	if (!b)
		return -ENOENT;

	if (!b->carveout_validated) {
		kref_put(&b->refcount, named_dmabuf_release);
		return -EOPNOTSUPP;
	}

	if (!uid_eq(current_uid(), b->creator_uid) &&
	    !ns_capable(&init_user_ns, CAP_SYS_ADMIN)) {
		kref_put(&b->refcount, named_dmabuf_release);
		return -EPERM;
	}

	dmabuf = READ_ONCE(b->dmabuf);
	memcpy(info.inst_name, inst->inst_name, sizeof(info.inst_name));
	info.offset = b->carveout_offset;
	info.size   = dmabuf->size;

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		err = -EFAULT;
	kref_put(&b->refcount, named_dmabuf_release);
	return err;
}

/*
 * IOC_IMPORT_REMOTE: validate an export tuple, construct a raw dmabuf from
 * the carveout PA, and register it under the given name.
 */
static int named_dmabuf_ioctl_import_remote(struct named_dmabuf_inst *inst,
					    unsigned long arg)
{
	struct named_dmabuf_import imp;
	struct dma_buf *dmabuf;
	int ret;

	if (copy_from_user(&imp, (void __user *)arg, sizeof(imp)))
		return -EFAULT;

	imp.name[NAMED_DMABUF_NAME_LEN - 1]      = '\0';
	imp.inst_name[NAMED_DMABUF_NAME_LEN - 1] = '\0';

	if (!named_dmabuf_name_valid(imp.name,
				     strnlen(imp.name, NAMED_DMABUF_NAME_LEN)))
		return -EINVAL;

	if (!inst->carveout_size)
		return -ENODEV;

	if (memcmp(imp.inst_name, inst->inst_name, NAMED_DMABUF_NAME_LEN))
		return -EINVAL;

	if (imp.size == 0 ||
	    imp.offset >= inst->carveout_size ||
	    imp.size   > inst->carveout_size - imp.offset)
		return -ERANGE;

	if (!PAGE_ALIGNED(imp.offset) || !PAGE_ALIGNED(imp.size))
		return -EINVAL;

	if (imp.flags & ~NAMED_DMABUF_F_MASK)
		return -EINVAL;

	if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	dmabuf = ndmabuf_raw_create(inst->carveout_base + imp.offset, imp.size);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	ret = __named_dmabuf_register(inst, imp.name, dmabuf,
				      !!(imp.flags & NAMED_DMABUF_F_RDONLY),
				      true, imp.offset);

	return ret;
}

/* Control device ioctl dispatch */

static long named_dmabuf_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	struct named_dmabuf_inst *inst = filp->private_data;
	long ret;

	if (!down_read_trylock(&inst->ioctl_rwsem))
		return -ENODEV;

	switch (cmd) {
	case NAMED_DMABUF_IOC_CREATE:
		ret = named_dmabuf_ioctl_create(inst, arg);
		break;
	case NAMED_DMABUF_IOC_RECOVER_FD:
		ret = named_dmabuf_ioctl_recover_fd(inst, arg);
		break;
	case NAMED_DMABUF_IOC_DESTROY:
		ret = named_dmabuf_ioctl_destroy(inst, arg);
		break;
	case NAMED_DMABUF_IOC_GET_EXPORT_INFO:
		ret = named_dmabuf_ioctl_get_export_info(inst, arg);
		break;
	case NAMED_DMABUF_IOC_IMPORT_REMOTE:
		ret = named_dmabuf_ioctl_import_remote(inst, arg);
		break;
	default:
		ret = -ENOTTY;
	}

	up_read(&inst->ioctl_rwsem);
	return ret;
}

static int named_dmabuf_ctrl_open(struct inode *inode, struct file *filp)
{
	struct named_dmabuf_inst *inst =
		container_of(inode->i_cdev, struct named_dmabuf_inst, ctrl_cdev);

	filp->private_data = inst;
	return 0;
}

static const struct file_operations named_dmabuf_ctrl_fops = {
	.owner		= THIS_MODULE,
	.open		= named_dmabuf_ctrl_open,
	.unlocked_ioctl	= named_dmabuf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= named_dmabuf_ioctl,
#endif
	.llseek		= noop_llseek,
};

/* Platform driver probe / remove */

/*
 * Allocate and initialize a named_dmabuf_inst for this DT node.  The instance
 * name comes from the optional "instance-name" DT property; falls back to the
 * platform device name.  Creates the control cdev at
 * /dev/named_dmabuf_<inst_name>/ctrl.
 */
static int named_dmabuf_probe(struct platform_device *pdev)
{
	struct named_dmabuf_inst *inst;
	const char *inst_name = NULL;
	struct device_node *np;
	struct reserved_mem *rmem;
	struct device *dev;
	int ret;

	inst = devm_kzalloc(&pdev->dev, sizeof(*inst), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	inst->dev = &pdev->dev;

	mutex_init(&inst->registry_lock);
	INIT_LIST_HEAD(&inst->registry);
	ida_init(&inst->minor_ida);
	init_rwsem(&inst->ioctl_rwsem);

	of_property_read_string(pdev->dev.of_node, "instance-name", &inst_name);
	if (inst_name)
		strscpy(inst->inst_name, inst_name, sizeof(inst->inst_name));
	else
		strscpy(inst->inst_name, pdev->name, sizeof(inst->inst_name));

	np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (np) {
		rmem = of_reserved_mem_lookup(np);
		of_node_put(np);
		if (!rmem) {
			dev_err(&pdev->dev, "memory-region not found\n");
			ida_destroy(&inst->minor_ida);
			return -EINVAL;
		}
		inst->carveout_base = rmem->base;
		inst->carveout_size = rmem->size;
		dev_info(&pdev->dev, "carveout %pa size %zu\n",
			 &inst->carveout_base, inst->carveout_size);

		/*
		 * The carveout can sit above 4 GB.  Set a 64-bit DMA mask so
		 * dma_capable() returns true for the carveout PA and the DMA
		 * layer never routes it through SWIOTLB.
		 */
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
		if (ret) {
			dev_err(&pdev->dev, "failed to set 64-bit DMA mask: %d\n", ret);
			ida_destroy(&inst->minor_ida);
			return ret;
		}
	}

	ret = alloc_chrdev_region(&inst->devt, 0, NAMED_DMABUF_MAX_MINORS, inst->inst_name);
	if (ret) {
		ida_destroy(&inst->minor_ida);
		return ret;
	}

	inst->ctrl_minor = ida_alloc_max(&inst->minor_ida, NAMED_DMABUF_MAX_MINORS - 1, GFP_KERNEL);
	if (inst->ctrl_minor < 0) {
		ret = inst->ctrl_minor;
		goto err_chrdev;
	}
	inst->ctrl_devno = MKDEV(MAJOR(inst->devt), inst->ctrl_minor);

	cdev_init(&inst->ctrl_cdev, &named_dmabuf_ctrl_fops);
	inst->ctrl_cdev.owner = THIS_MODULE;
	ret = cdev_add(&inst->ctrl_cdev, inst->ctrl_devno, 1);
	if (ret)
		goto err_ida;

	dev = device_create(named_dmabuf_class, &pdev->dev, inst->ctrl_devno, inst,
			    "named_dmabuf_%s/ctrl", inst->inst_name);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err_cdev;
	}

	platform_set_drvdata(pdev, inst);
	return 0;

err_cdev:
	cdev_del(&inst->ctrl_cdev);
err_ida:
	ida_free(&inst->minor_ida, inst->ctrl_minor);
err_chrdev:
	unregister_chrdev_region(inst->devt, NAMED_DMABUF_MAX_MINORS);
	ida_destroy(&inst->minor_ida);
	return ret;
}

/*
 * Tear down all registered buffers and the instance infrastructure.  Destroys
 * each registry entry under the lock, then cleans up the instance resources.
 * Drops the driver's dmabuf ref for each remaining buffer so the underlying
 * memory can be freed even if consumers still hold fds.
 */
static void named_dmabuf_remove(struct platform_device *pdev)
{
	struct named_dmabuf_inst *inst = platform_get_drvdata(pdev);
	struct named_dmabuf *b, *tmp;

	/*
	 * Destroy the ctrl cdev first so no new ioctls can arrive after this
	 * point.  Any ioctl that entered before device_destroy holds a read
	 * lock on ioctl_rwsem; down_write below drains them.
	 */
	device_destroy(named_dmabuf_class, inst->ctrl_devno);
	cdev_del(&inst->ctrl_cdev);

	/*
	 * Drain all in-flight ioctls before touching the registry.  After
	 * down_write returns no ioctl is running, and ioctl_rwsem will never
	 * be taken for read again because the ctrl cdev is already gone.
	 */
	down_write(&inst->ioctl_rwsem);
	up_write(&inst->ioctl_rwsem);

	mutex_lock(&inst->registry_lock);
	WRITE_ONCE(inst->removing, true);
	list_for_each_entry_safe(b, tmp, &inst->registry, node) {
		WRITE_ONCE(b->dying, true);
		list_del(&b->node);
		named_dmabuf_unregister_cdev(b);
		/* dma_buf_put deferred to named_dmabuf_release via kref */
		kref_put(&b->refcount, named_dmabuf_release);
	}
	mutex_unlock(&inst->registry_lock);

	ida_free(&inst->minor_ida, inst->ctrl_minor);
	unregister_chrdev_region(inst->devt, NAMED_DMABUF_MAX_MINORS);
	ida_destroy(&inst->minor_ida);
}

static const struct of_device_id named_dmabuf_of_match[] = {
	{ .compatible = "qcom,named-dmabuf" },
	{}
};
MODULE_DEVICE_TABLE(of, named_dmabuf_of_match);

static struct platform_driver named_dmabuf_driver = {
	.probe	= named_dmabuf_probe,
	.remove	= named_dmabuf_remove,
	.driver	= {
		.name		= "named-dmabuf",
		.of_match_table	= named_dmabuf_of_match,
	},
};

static int __init named_dmabuf_init(void)
{
	int ret;

	named_dmabuf_class = class_create("named_dmabuf");
	if (IS_ERR(named_dmabuf_class))
		return PTR_ERR(named_dmabuf_class);

	ret = platform_driver_register(&named_dmabuf_driver);
	if (ret)
		class_destroy(named_dmabuf_class);
	return ret;
}

static void __exit named_dmabuf_exit(void)
{
	platform_driver_unregister(&named_dmabuf_driver);
	class_destroy(named_dmabuf_class);
}

module_init(named_dmabuf_init);
module_exit(named_dmabuf_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Named dma-buf registry — per-instance platform driver");
