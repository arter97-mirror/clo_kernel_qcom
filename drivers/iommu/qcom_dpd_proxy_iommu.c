// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#define pr_fmt(fmt) "%s: " fmt,  __func__

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/maple_tree.h>
#include <linux/of.h>
#include <linux/qtee_shmbridge.h>
#include <linux/firmware/qcom/si_object.h>
#include <linux/iommu.h>
#include <linux/qcom_dpd_proxy.h>
#include <linux/pci.h>
#include <linux/adreno-smmu-priv.h>
#include "drivers/iommu/dma-iommu.h"
#include "qcom_dpd_proxy_tee.h"
#include "arm/arm-smmu/arm-smmu.h"

#define MSI_IOVA_BASE                   0x8000000
#define MSI_IOVA_LENGTH                 0x100000

struct dpd_smmu {
	struct device *dev;
	struct iommu_device iommu;
	struct mutex streams_lock;
	struct xarray streams;
	struct iommu_domain_geometry geometry;
	u64 pgsize_bitmap;
	struct si_object *service;
	struct si_object *env;
};

struct dpd_smmu_domain {
	struct dpd_smmu *smmu;

	/* Protected by mappings_lock */
	struct mutex mappings_lock;
	struct maple_tree mappings;

	bool attached;
	struct device *dev;
	/* Sharing domains across multiple devices is not supported */
	u32 si_domain_id;
	struct adreno_smmu_fault_info adreno_cfi;
	struct iommu_domain domain;
};

struct dpd_smmu_cbo {
	struct si_object si_cbo;
	struct dpd_smmu *smmu;
};
static struct dpd_smmu_cbo *__cbo_priv;

/*
 * Used primarily for iommu_iova_to_phys.
 * Also for attaching/detaching domains.
 */
struct dpd_mapping {
	struct dpd_scatterlist *dpd_sg;
	unsigned long iova;
	u32 prot;
};

struct dpd_map_walk {
	struct iommu_map_cookie_sg cookie;
	struct dpd_scatterlist *dpd_sg;
	struct sg_page_iter piter;
	size_t offset;
	size_t mapped;
	unsigned long iova;
	int prot;
	int error;
};

#define to_smmu_domain(d) container_of((d), struct dpd_smmu_domain, domain)

/* from arm-smmu-v3 */
static struct dpd_smmu_domain *
to_smmu_domain_devices(struct iommu_domain *domain)
{
	/* The domain can be NULL only when processing the first attach */
	if (!domain)
		return NULL;
	if (domain->type & __IOMMU_DOMAIN_PAGING)
		return to_smmu_domain(domain);
	return NULL;
}

static struct dpd_mapping *add_mapping(struct dpd_smmu_domain *smmu_domain,
					struct dpd_scatterlist *dpd_sg,
					unsigned long iova, int prot)
{
	struct dpd_mapping *mapping;
	int ret;

	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping)
		return NULL;

	get_si_object(dpd_sg->shm);
	mapping->dpd_sg = dpd_sg;
	mapping->iova = iova;
	mapping->prot = prot;

	mutex_lock(&smmu_domain->mappings_lock);
	ret = mtree_insert_range(&smmu_domain->mappings, iova,
			iova + dpd_sg->size - 1, mapping, GFP_KERNEL);
	mutex_unlock(&smmu_domain->mappings_lock);
	if (ret) {
		dev_err(smmu_domain->smmu->dev, "Domain %u: mtree_insert_range failed: %d. start=%lx, end=%zx\n",
			smmu_domain->si_domain_id, ret,
			iova, iova + dpd_sg->size);
		put_si_object(dpd_sg->shm);
		kfree(mapping);
		return NULL;
	}

	return mapping;
}

static void __free_dpd_mapping(struct dpd_mapping *mapping)
{
	put_si_object(mapping->dpd_sg->shm);
	kfree(mapping);
}

/* Caller must hold mappings lock */
static int detach_mappings(struct dpd_smmu_domain *smmu_domain, unsigned long iova_end)
{
	MA_STATE(mas, &smmu_domain->mappings, 0, 0);
	struct dpd_mapping *mapping;
	int ret = 0;

	if (!smmu_domain->attached)
		return 0;

	mas_for_each(&mas, mapping, iova_end) {
		ret = dpd_svc_unmap(mapping->dpd_sg, smmu_domain->si_domain_id,
				mapping->iova);
		if (ret) {
			dev_warn(smmu_domain->smmu->dev, "%s: %s failed\n",
				 dev_name(smmu_domain->dev), __func__);
			return ret;
		}
	}

	smmu_domain->attached = false;
	return 0;
}

static u32 prot_to_svc_flags(u32 prot)
{
	u32 flags = 0;

	if (prot & IOMMU_READ)
		flags |= IMM_F_READ;
	if (prot & IOMMU_WRITE)
		flags |= IMM_F_WRITE;

	return flags;
}

/* Caller must hold mappings lock */
static int attach_mappings(struct dpd_smmu_domain *smmu_domain)
{
	MA_STATE(mas, &smmu_domain->mappings, 0, 0);
	struct dpd_mapping *mapping;
	int ret = 0;

	if (smmu_domain->attached)
		return 0;

	/* Set early for detach_mappings error case */
	smmu_domain->attached = true;
	mas_for_each(&mas, mapping, ULONG_MAX) {
		ret = dpd_svc_map(mapping->dpd_sg, smmu_domain->si_domain_id,
				  prot_to_svc_flags(mapping->prot),
				  mapping->iova);
		if (ret) {
			detach_mappings(smmu_domain, mapping->iova - 1);
			return ret;
		}
	}

	return 0;
}

static struct iommu_domain *dpd_smmu_domain_alloc_paging(struct device *dev)
{
	struct dpd_smmu_domain *smmu_domain;
	struct dpd_smmu *smmu = container_of(dev->iommu->iommu_dev, struct dpd_smmu, iommu);
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	smmu_domain = kzalloc(sizeof(*smmu_domain), GFP_KERNEL);
	if (!smmu_domain)
		return ERR_PTR(-ENOMEM);

	mutex_init(&smmu_domain->mappings_lock);
	mt_init(&smmu_domain->mappings);
	smmu_domain->smmu = smmu;
	smmu_domain->dev = dev;
	smmu_domain->si_domain_id = fwspec->ids[0];
	smmu_domain->domain.pgsize_bitmap = smmu->pgsize_bitmap;
	smmu_domain->domain.geometry = smmu->geometry;

	return &smmu_domain->domain;
}

static void dpd_smmu_domain_free_paging(struct iommu_domain *domain)
{
	struct dpd_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct dpd_mapping *mapping;

	MA_STATE(mas, &smmu_domain->mappings, 0, 0);

	mutex_lock(&smmu_domain->mappings_lock);
	if (smmu_domain->attached)
		detach_mappings(smmu_domain, ULONG_MAX);

	mas_for_each(&mas, mapping, ULONG_MAX) {
		dev_dbg(smmu_domain->dev, "Cleanup mappings @ %lx\n", mas.index);
		__free_dpd_mapping(mapping);
	}
	mtree_destroy(&smmu_domain->mappings);
	mutex_unlock(&smmu_domain->mappings_lock);

	kfree(smmu_domain);
}

static struct dpd_smmu *dpd_smmu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev = bus_find_device_by_fwnode(&platform_bus_type, fwnode);

	put_device(dev);
	return dev ? dev_get_drvdata(dev) : NULL;
}

static struct iommu_device *dpd_smmu_probe_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct dpd_smmu *smmu;
	int ret;

	smmu = dpd_smmu_get_by_fwnode(fwspec->iommu_fwnode);
	if (!smmu)
		return ERR_PTR(-ENODEV);

	if (fwspec->num_ids > 1) {
		dev_err(dev, "Max one iommu-id\n");
		return ERR_PTR(-EINVAL);
	}

	mutex_lock(&smmu->streams_lock);
	ret = xa_insert(&smmu->streams, fwspec->ids[0], dev, GFP_KERNEL);
	mutex_unlock(&smmu->streams_lock);
	if (ret) {
		dev_err(dev, "ID %d is already registered\n", fwspec->ids[0]);
		return ERR_PTR(ret);
	}

	return &smmu->iommu;
}

static void dpd_smmu_release_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct dpd_smmu *smmu = container_of(dev->iommu->iommu_dev, struct dpd_smmu, iommu);
	int i;

	if (!fwspec)
		return;

	mutex_lock(&smmu->streams_lock);
	for (i = 0; i < fwspec->num_ids; i++)
		xa_erase(&smmu->streams, fwspec->ids[i]);
	mutex_unlock(&smmu->streams_lock);
}

static struct iommu_group *dpd_smmu_device_group(struct device *dev)
{
	struct iommu_group *group;

	if (dev_is_pci(dev))
		group = pci_device_group(dev);
	else
		group = generic_device_group(dev);

	return group;
}

static int dpd_smmu_of_xlate(struct device *dev, const struct of_phandle_args *args)
{
	return iommu_fwspec_add_ids(dev, args->args, 1);
}

static void dpd_smmu_get_resv_regions(struct device *dev,
			       struct list_head *head)
{
	struct iommu_resv_region *region;
	int prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;

	region = iommu_alloc_resv_region(MSI_IOVA_BASE, MSI_IOVA_LENGTH,
					 prot, IOMMU_RESV_SW_MSI, GFP_KERNEL);
	if (!region)
		return;

	list_add_tail(&region->list, head);

	iommu_dma_get_resv_regions(dev, head);
}

static void adreno_set_fault_info(struct iommu_domain *domain, struct imm_fault_info *cfi)
{
	struct dpd_smmu_domain *smmu_domain;
	struct adreno_smmu_fault_info *adreno_cfi;

	if (!domain)
		return;

	smmu_domain = to_smmu_domain(domain);
	adreno_cfi = &smmu_domain->adreno_cfi;
	adreno_cfi->far = cfi->far;
	adreno_cfi->ttbr0 = cfi->ttbr0;
	/* TEE does not provide contextidr */
	adreno_cfi->contextidr = U32_MAX;
	adreno_cfi->fsr = cfi->fsr;
	adreno_cfi->fsynr0 = cfi->fsynr0;
	adreno_cfi->fsynr1 = cfi->fsynr1;
	adreno_cfi->cbfrsynra = cfi->cbfrsynra;
}

static void adreno_get_fault_info(const void *cookie, struct adreno_smmu_fault_info *info)
{
	const struct dpd_smmu_domain *smmu_domain = cookie;

	*info = smmu_domain->adreno_cfi;
}

static bool device_is_adreno(struct device *dev)
{
	const char *name = "gfx3d_secure";

	if (!dev->of_node)
		return false;

	if (of_node_name_eq(dev->of_node, name))
		return true;
	return false;
}

static void setup_adreno_callbacks(struct dpd_smmu_domain *smmu_domain, struct device *dev)
{
	struct adreno_smmu_priv *p = dev_get_drvdata(dev);

	if (!p || !device_is_adreno(dev))
		return;

	p->cookie = smmu_domain;
	p->get_fault_info = adreno_get_fault_info;
	dev_dbg(smmu_domain->smmu->dev, "%s: Setup of adreno_smmu_priv complete\n",
		dev_name(dev));
}

static int dpd_smmu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct dpd_smmu_domain *prev_domain =
		to_smmu_domain_devices(iommu_get_domain_for_dev(dev));
	struct dpd_smmu_domain *smmu_domain = to_smmu_domain(domain);
	int ret;

	if (prev_domain) {
		mutex_lock(&prev_domain->mappings_lock);
		ret = detach_mappings(prev_domain, ULONG_MAX);
		mutex_unlock(&prev_domain->mappings_lock);
		if (ret)
			return ret;
	}

	mutex_lock(&smmu_domain->mappings_lock);
	ret = attach_mappings(smmu_domain);
	mutex_unlock(&smmu_domain->mappings_lock);

	setup_adreno_callbacks(smmu_domain, dev);

	return ret;
}

/*
 * Releases held resources.
 */
static void dpd_map_walk_reset(struct dpd_map_walk *w)
{
	if (w->dpd_sg)
		put_si_object(w->dpd_sg->shm);
	memset(w, 0, sizeof(*w));
}

/*
 * Translates the physical address ranges provided by the iommu framework
 * into smcinvoke memory objects (MO). If [paddr, paddr + size] is a subset of
 * a MO, the current offset within the MO is preserved and used by the
 * subsequent call to dpd_map_walk(). When the end of a MO is reached,
 * a iommu map command is submitted over smcinvoke.
 *
 * Mapping partial smcinvoke memory objects is not allowed.
 */
static int dpd_map_walk(struct dpd_map_walk *w, phys_addr_t paddr, size_t pgsize, size_t pgcount)
{
	struct dpd_smmu_domain *smmu_domain = to_smmu_domain(w->cookie.domain);
	struct dpd_smmu *smmu = smmu_domain->smmu;
	struct dpd_scatterlist *dpd_sg;
	size_t size;
	struct page *p, *page_end;
	int ret;

	if (!smmu_domain->attached) {
		dev_err(smmu->dev, "%s: Mapping to inactive domain\n",
			dev_name(smmu_domain->dev));
		return -EINVAL;
	}

	if (w->error)
		return w->error;

	if (check_mul_overflow(pgsize, pgcount, &size)) {
		w->error = -EINVAL;
		return w->error;
	}

	p = phys_to_page(paddr);
	page_end = phys_to_page(paddr + size);

	while (p < page_end) {
		if (!w->dpd_sg) {
			unsigned long pfn = page_to_pfn(p);

			/* Acquires reference */
			w->dpd_sg = dpd_mtree_lookup(pfn);
			if (!w->dpd_sg) {
				dev_dbg(smmu->dev, "Not protected memory: pfn:%lx\n", pfn);
				w->error = -ERANGE;
				return w->error;
			}

			__sg_page_iter_start(&w->piter, w->dpd_sg->sgt.sgl,
					w->dpd_sg->sgt.orig_nents, 0);
			w->offset = 0;
			__sg_page_iter_next(&w->piter);
		}
		dpd_sg = w->dpd_sg;

		do {
			struct page *p_sg = sg_page_iter_page(&w->piter);

			if (p >= page_end)
				return 0;

			if (p != p_sg) {
				dev_err(smmu->dev, "Partial si_object: expected pfn:%lx (%zx/%zx) have: %lx\n",
					page_to_pfn(p), w->offset, dpd_sg->size,
					page_to_pfn(p_sg));
				w->error = -ERANGE;
				return w->error;
			}
			w->offset += PAGE_SIZE;
			p++;
		} while (__sg_page_iter_next(&w->piter));

		/* Reached end of current dpd_sg. Start the map operation */
		if (!add_mapping(smmu_domain, dpd_sg, w->iova, w->prot)) {
			w->error = -ENOMEM;
			return w->error;
		}

		/* Mapping now holds the refcount; drop ours */
		put_si_object(w->dpd_sg->shm);
		w->dpd_sg = NULL;

		ret = dpd_svc_map(dpd_sg, smmu_domain->si_domain_id,
				prot_to_svc_flags(w->prot), w->iova);

		if (ret) {
			struct dpd_mapping *mapping;

			mutex_lock(&smmu_domain->mappings_lock);
			mapping = mtree_erase(&smmu_domain->mappings, w->iova);
			mutex_unlock(&smmu_domain->mappings_lock);
			__free_dpd_mapping(mapping);
			w->error = ret;
			return ret;
		}

		w->mapped += dpd_sg->size;
		w->iova += dpd_sg->size;
	}

	return 0;
}

/*
 * This driver heavily depends on Android's custom iommu_map_sg ops:
 * alloc_cookie_sg/add_deferred_map_sg/consume_deferred_map_sg in order
 * to convert the contiguous physical addresses the iommu framework provides
 * into smcinvoke memory objects.
 */
static struct iommu_map_cookie_sg *
dpd_alloc_cookie_sg(unsigned long iova, int prot, unsigned int nents, gfp_t gfp) {
	struct dpd_map_walk *w;

	w = kzalloc(sizeof(*w), gfp);
	if (!w)
		return NULL;

	w->prot = prot;
	w->iova = iova;
	return &w->cookie;
}

static int dpd_add_deferred_map_sg(struct iommu_map_cookie_sg *cookie,
				   phys_addr_t paddr, size_t pgsize, size_t pgcount)
{
	int ret = 0;
	struct dpd_map_walk *w = container_of(cookie, struct dpd_map_walk, cookie);

	ret = dpd_map_walk(w, paddr, pgsize, pgcount);

	/*
	 * Android 6.12 calls ops->consume_deferred_map_sg if
	 * ops->add_deferred_map_sg fails.
	 * Therefore return error here.
	 */
	return ret;
}

static size_t dpd_consume_deferred_map_sg(struct iommu_map_cookie_sg *cookie)
{
	struct dpd_map_walk *w = container_of(cookie, struct dpd_map_walk, cookie);
	struct dpd_smmu_domain *smmu_domain = to_smmu_domain(cookie->domain);
	struct dpd_smmu *smmu = smmu_domain->smmu;
	size_t mapped = w->mapped;

	if (!w->error && w->dpd_sg) {
		dev_err(smmu->dev, "Expected map of entire si_object. Only %zx/%zx mapped\n",
			w->offset, w->dpd_sg->size);
	}

	dpd_map_walk_reset(w);
	kfree(w);
	return mapped;
}

static int dpd_smmu_map_pages(struct iommu_domain *domain, unsigned long iova,
			      phys_addr_t paddr, size_t pgsize, size_t pgcount,
			      int prot, gfp_t gfp, size_t *_mapped)
{
	struct iommu_map_cookie_sg *cookie;
	struct dpd_smmu_domain *smmu_domain = to_smmu_domain(domain);

	if (!smmu_domain->attached) {
		dev_err(smmu_domain->smmu->dev, "%s: Mapping to inactive domain\n",
			dev_name(smmu_domain->dev));
		return -EINVAL;
	}

	cookie = dpd_alloc_cookie_sg(iova, prot, 1, GFP_KERNEL);
	if (!cookie)
		return -ENOMEM;

	cookie->domain = domain;

	dpd_add_deferred_map_sg(cookie, paddr, pgsize, pgcount);
	*_mapped = dpd_consume_deferred_map_sg(cookie);

	if (*_mapped != pgsize * pgcount)
		return -EINVAL;
	return 0;
}

static size_t dpd_smmu_unmap_pages(struct iommu_domain *domain, unsigned long iova,
				   size_t pgsize, size_t pgcount,
				   struct iommu_iotlb_gather *gather)
{
	struct dpd_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct dpd_smmu *smmu = smmu_domain->smmu;
	size_t size;
	struct dpd_mapping *mapping;
	unsigned long end;
	size_t unmapped = 0;
	int ret;

	if (!smmu_domain->attached) {
		dev_err(smmu->dev, "%s: Unmapping from inactive domain\n",
			dev_name(smmu_domain->dev));
		return unmapped;
	}

	if (check_mul_overflow(pgsize, pgcount, &size))
		return unmapped;

	end = iova + size;

	mutex_lock(&smmu_domain->mappings_lock);
	while (iova < end) {
		mapping = mtree_load(&smmu_domain->mappings, iova);
		if (!mapping) {
			dev_err(smmu->dev, "No mapping at %lx\n", iova);
			goto out;
		}

		if (iova != mapping->iova || end < mapping->iova + mapping->dpd_sg->size) {
			dev_err(smmu->dev, "Domain %d: Can't unmap partial si_object. Request %lx-%lx expected %lx-%lx\n",
				smmu_domain->si_domain_id, iova, end,
				mapping->iova,
				mapping->iova + mapping->dpd_sg->size);
			goto out;
		}

		ret = dpd_svc_unmap(mapping->dpd_sg, smmu_domain->si_domain_id,
				    mapping->iova);
		if (ret)
			goto out;

		mtree_erase(&smmu_domain->mappings, iova);
		unmapped += mapping->dpd_sg->size;
		iova += mapping->dpd_sg->size;
		__free_dpd_mapping(mapping);
	}
out:
	mutex_unlock(&smmu_domain->mappings_lock);
	return unmapped;
}

static phys_addr_t
dpd_smmu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	struct dpd_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct dpd_mapping *mapping;
	struct sg_page_iter piter;
	unsigned long pgoffset;
	phys_addr_t phys = 0;

	mutex_lock(&smmu_domain->mappings_lock);
	mapping = mtree_load(&smmu_domain->mappings, iova);
	if (!mapping)
		goto out;

	/*
	 * hyp_assign() requires the segments of its sg_table to be aligned to the
	 * iommu-granularity. Therefore, dma-iommu did not modify the offset/sizes
	 * of the sg_table before passing it to the iommu framework as it might do
	 * in the general case.
	 */
	pgoffset = (iova - mapping->iova) >> PAGE_SHIFT;
	__sg_page_iter_start(&piter, mapping->dpd_sg->sgt.sgl,
		mapping->dpd_sg->sgt.orig_nents, pgoffset);

	if (__sg_page_iter_next(&piter))
		phys = page_to_phys(sg_page_iter_page(&piter)) +
		       (iova & (PAGE_SIZE - 1));
out:
	mutex_unlock(&smmu_domain->mappings_lock);
	return phys;
}

static const struct iommu_ops dpd_smmu_ops = {
	.domain_alloc_paging    = dpd_smmu_domain_alloc_paging,
	.probe_device		= dpd_smmu_probe_device,
	.release_device		= dpd_smmu_release_device,
	.device_group		= dpd_smmu_device_group,
	.of_xlate		= dpd_smmu_of_xlate,
	.get_resv_regions	= dpd_smmu_get_resv_regions,
	.owner			= THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev			= dpd_smmu_attach_dev,
		.map_pages			= dpd_smmu_map_pages,
		.unmap_pages			= dpd_smmu_unmap_pages,
		.alloc_cookie_sg		= dpd_alloc_cookie_sg,
		.add_deferred_map_sg		= dpd_add_deferred_map_sg,
		.consume_deferred_map_sg	= dpd_consume_deferred_map_sg,
		.iova_to_phys			= dpd_smmu_iova_to_phys,
		.free				= dpd_smmu_domain_free_paging,
	}
};

static void si_cbo_release(struct si_object *object)
{
	WARN(true, "Should never be called because TEE does not support cbo unregister\n");
}

static int si_cbo_dispatch(unsigned int context_id, struct si_object *object,
			unsigned long op, struct si_arg args[])
{
	struct dpd_smmu_cbo *cbo;
	struct dpd_smmu *smmu;
	struct imm_fault_info *cfi;
	struct device *client;
	struct iommu_domain *domain = NULL;
	static DEFINE_RATELIMIT_STATE(_rs,
				      DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	int ret, flags;

	cbo = container_of(object, struct dpd_smmu_cbo, si_cbo);
	smmu = cbo->smmu;
	if (SI_OBJECT_OP_METHOD_ID(op) != IMM_CBO_METHOD_NOTIFY_FAULT) {
		dev_err(smmu->dev, "Unexpected method: %ld\n", op);
		return -EINVAL;
	}
	if (args[0].type != SI_AT_IB) {
		dev_err(smmu->dev, "Unexpected args[0] type: %d\n", args[0].type);
		return -EINVAL;
	}
	if (args[0].b.size != sizeof(struct imm_fault_info)) {
		dev_err(smmu->dev, "Unexpected args[0] size: %#lx\n", args[0].b.size);
		return -EINVAL;
	}
	if (args[1].type != SI_AT_END) {
		dev_err(smmu->dev, "Unexpected args[1] type: %d\n", args[1].type);
		return -EINVAL;
	}

	cfi = (struct imm_fault_info *)args[0].b.addr;
	flags = cfi->fsynr0 & ARM_SMMU_CB_FSYNR0_WNR ? IOMMU_FAULT_WRITE : IOMMU_FAULT_READ;

	mutex_lock(&smmu->streams_lock);
	client = xa_load(&smmu->streams, cfi->eVM);
	mutex_unlock(&smmu->streams_lock);

	ret = -ENOSYS;
	if (client) {
		domain = iommu_get_domain_for_dev(client);
		if (domain) {
			/*
			 * Adreno device retrieve additional data via a get_fault_info callback
			 * Ensure it is set before calling report_iommu_fault.
			 */
			if (device_is_adreno(client))
				adreno_set_fault_info(domain, cfi);
			ret = report_iommu_fault(domain, smmu->dev, cfi->far, flags);
		}
	}
	/* Clients should return -ENOSYS for default fault handling */
	if (ret != -ENOSYS || __ratelimit(&_rs))
		return 0;

	/* Ported from arm_smmu_print_context_fault_info */
	dev_err(smmu->dev,
		"Unhandled context fault: fsr=0x%x, iova=0x%08llx, fsynr=0x%x, fsynr1=0x%x, cbfrsynra=0x%x, client:%s, Vmid: %d\n",
		cfi->fsr, cfi->far, cfi->fsynr0, cfi->fsynr1, cfi->cbfrsynra,
		client ? dev_name(client) : "Unknown", cfi->eVM);

	dev_err(smmu->dev, "FSR    = %08x [%s%sFormat=%u%s%s%s%s%s%s%s%s], SID=0x%x\n",
		cfi->fsr,
		(cfi->fsr & ARM_SMMU_CB_FSR_MULTI)  ? "MULTI " : "",
		(cfi->fsr & ARM_SMMU_CB_FSR_SS)     ? "SS " : "",
		(u32)FIELD_GET(ARM_SMMU_CB_FSR_FORMAT, cfi->fsr),
		(cfi->fsr & ARM_SMMU_CB_FSR_UUT)    ? " UUT" : "",
		(cfi->fsr & ARM_SMMU_CB_FSR_ASF)    ? " ASF" : "",
		(cfi->fsr & ARM_SMMU_CB_FSR_TLBLKF) ? " TLBLKF" : "",
		(cfi->fsr & ARM_SMMU_CB_FSR_TLBMCF) ? " TLBMCF" : "",
		(cfi->fsr & ARM_SMMU_CB_FSR_EF)     ? " EF" : "",
		(cfi->fsr & ARM_SMMU_CB_FSR_PF)     ? " PF" : "",
		(cfi->fsr & ARM_SMMU_CB_FSR_AFF)    ? " AFF" : "",
		(cfi->fsr & ARM_SMMU_CB_FSR_TF)     ? " TF" : "",
		cfi->cbfrsynra);

	dev_err(smmu->dev, "FSYNR0 = %08x [%s%s%s%s%s%s PLVL=%u]\n",
		cfi->fsynr0,
		(cfi->fsynr0 & ARM_SMMU_CB_FSYNR0_AFR) ? " AFR" : "",
		(cfi->fsynr0 & ARM_SMMU_CB_FSYNR0_PTWF) ? " PTWF" : "",
		(cfi->fsynr0 & ARM_SMMU_CB_FSYNR0_NSATTR) ? " NSATTR" : "",
		(cfi->fsynr0 & ARM_SMMU_CB_FSYNR0_IND) ? " IND" : "",
		(cfi->fsynr0 & ARM_SMMU_CB_FSYNR0_PNU) ? " PNU" : "",
		(cfi->fsynr0 & ARM_SMMU_CB_FSYNR0_WNR) ? " WNR" : "",
		(u32)FIELD_GET(ARM_SMMU_CB_FSYNR0_PLVL, cfi->fsynr0));

	if (domain) {
		phys_addr_t phys = iommu_iova_to_phys(domain, cfi->far);

		dev_err(smmu->dev, "SW table walk: %#llx -> %pa\n",
			cfi->far, &phys);
	}

	return 0;
}

struct si_object_operations dpd_smmu_si_cbo_ops = {
	.release = si_cbo_release,
	.dispatch = si_cbo_dispatch,
};

/*
 * TEE does not support unregistering this callback object.
 * No lock; expect only single device.
 */
static struct dpd_smmu_cbo *dpd_smmu_setup_cbo(struct dpd_smmu *smmu)
{
	struct dpd_smmu_cbo *cbo;
	int ret;

	if (__cbo_priv) {
		if (__cbo_priv->smmu) {
			dev_err(smmu->dev, "cbo is already setup\n");
			return NULL;
		}
		__cbo_priv->smmu = smmu;
		return __cbo_priv;
	}

	cbo = kzalloc(sizeof(*cbo), GFP_KERNEL);
	if (!cbo)
		return NULL;

	ret = init_si_object_user(&cbo->si_cbo, SI_OT_CB_OBJECT, &dpd_smmu_si_cbo_ops,
				  "dpd_smmu_cbo");
	if (ret) {
		dev_err(smmu->dev, "init_si_object_user for cbo failed with %d\n", ret);
		goto err_init_cbo;
	}

	cbo->smmu = smmu;
	ret = dpd_svc_register_cbo(&cbo->si_cbo);
	if (ret) {
		dev_err(smmu->dev, "init_si_object_user for cbo failed with %d\n", ret);
		goto err_register_cbo;
	}

	__cbo_priv = cbo;
	return cbo;

err_register_cbo:
	put_si_object(&cbo->si_cbo);
err_init_cbo:
	kfree(cbo);
	return NULL;
}

static void dpd_smmu_teardown_cbo(void)
{
	if (__cbo_priv)
		__cbo_priv->smmu = NULL;
}

static int dpd_smmu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dpd_smmu *smmu;
	int ret;
	struct si_object_invoke_ctx oic;
	char *msg = "";

	smmu = devm_kzalloc(dev, sizeof(*smmu), GFP_KERNEL);
	if (!smmu)
		return dev_err_probe(dev, -ENOMEM, "Failed to allocate smmu structure");

	mutex_init(&smmu->streams_lock);
	xa_init(&smmu->streams);
	smmu->dev = dev;

	ret = si_core_get_client_env(&oic, &smmu->env);
	if (ret) {
		msg = "si_core_get_client_env failed";
		goto err_env;
	}

	ret = si_core_client_env_open(&oic, smmu->env, CSecureMemoryManager_UID, &smmu->service);
	if (ret) {
		msg = "si_core_client_env_open failed";
		goto err_service;
	}

	/* Temporarily optional for build compatibility */
	dpd_smmu_setup_cbo(smmu);

	/* Hardcoded values. Ideally could query these from the TEE service */
	smmu->pgsize_bitmap = (1 << PAGE_SHIFT);
	smmu->geometry = (struct iommu_domain_geometry) {
		.aperture_start = 0,
		.aperture_end = 1ULL << 48,
		.force_aperture = true,
	};

	dev_set_drvdata(dev, smmu);

	ret = iommu_device_sysfs_add(&smmu->iommu, dev, NULL, "dpd_smmu");
	if (ret) {
		msg = "iommu_device_sysfs_add failed";
		goto err_sysfs;  /* Both service and env need cleanup */
	}

	ret = iommu_device_register(&smmu->iommu, &dpd_smmu_ops, dev);
	if (ret) {
		msg = "Failed to register iommu";
		goto err_register;  /* Cleanup sysfs, service, and env */
	}

	return 0;

err_register:
	iommu_device_sysfs_remove(&smmu->iommu);
err_sysfs:
	dpd_smmu_teardown_cbo();
	put_si_object(smmu->service);
err_service:
	put_si_object(smmu->env);
err_env:
	return dev_err_probe(dev, ret, "%s", msg);
}

static void dpd_smmu_remove(struct platform_device *pdev)
{
	struct dpd_smmu *smmu = platform_get_drvdata(pdev);

	iommu_device_unregister(&smmu->iommu);
	iommu_device_sysfs_remove(&smmu->iommu);

	dpd_smmu_teardown_cbo();
	put_si_object(smmu->service);
	put_si_object(smmu->env);
}

static const struct of_device_id dpd_smmu_of_match[] = {
	{ .compatible = "qcom,dpd-smmu" },
	{}
};

static struct platform_driver dpd_smmu_driver = {
	.driver	= {
		.name			= "dpd-smmu",
		.of_match_table		= dpd_smmu_of_match,
		.suppress_bind_attrs	= true,
	},
	.probe	= dpd_smmu_probe,
	.remove	= dpd_smmu_remove,
};

module_platform_driver(dpd_smmu_driver);
MODULE_DESCRIPTION("QTI DPD SMMU Driver");
MODULE_LICENSE("GPL");
