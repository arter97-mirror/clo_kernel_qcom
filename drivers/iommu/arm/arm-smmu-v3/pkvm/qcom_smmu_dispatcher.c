// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <linux/io-pgtable-arm.h>

#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>
#include <nvhe/trap_handler.h>
#include <module/nvhe/trace.h>
#include "smmuv2_nesting.h"
//#include "smmuv3_nesting.h"
#include "arm_smmu_v3.h"
#include "qcom_smmu_dispatcher.h"

/* Registered SMMU drivers */
#define MAX_SMMU_DRIVERS 4
static struct smmu_vendor_driver *registered_drivers[MAX_SMMU_DRIVERS];
static int num_registered_drivers;
const struct pkvm_module_ops	*mod_ops;

struct smmu_nested_domain idmapped_domain;

/**
 * smmu_vendor_register_driver - Register an SMMU driver
 * @driver: SMMU driver to register
 *
 * Returns: 0 on success, negative error code on failure
 */
int smmu_vendor_register_driver(struct smmu_vendor_driver *driver)
{
	if (!driver || !driver->callbacks || num_registered_drivers >= MAX_SMMU_DRIVERS)
		return -EINVAL;

	registered_drivers[num_registered_drivers] = driver;
	num_registered_drivers++;

	driver->vdata->vops = mod_ops;
	driver->vdata->vdomain = &idmapped_domain;

	return 0;
}

/**
 * smmu_vendor_unregister_driver - Unregister an SMMU driver
 * @driver: SMMU driver to unregister
 *
 * Returns: 0 on success, negative error code on failure
 */
int smmu_vendor_unregister_driver(struct smmu_vendor_driver *driver)
{
	int i, j;

	if (!driver)
		return -EINVAL;

	for (i = 0; i < num_registered_drivers; i++) {
		if (registered_drivers[i] == driver) {
			/* Shift remaining drivers down */
			for (j = i; j < num_registered_drivers - 1; j++)
				registered_drivers[j] = registered_drivers[j + 1];
			registered_drivers[num_registered_drivers - 1] = NULL;
			num_registered_drivers--;
			return 0;
		}
	}

	return -ENOENT;

	return 0;
}

static void smmu_tlb_flush_all(void *cookie)
{
	int i;

	for (i = 0; i < num_registered_drivers; i++) {
		if (!registered_drivers[i]->callbacks->tlb_ops) {
			mod_ops->puts("WARN!! Possible stale TLB entries, tlb ops not implemented");
			continue;
		}

		if (registered_drivers[i]->callbacks->tlb_ops->tlb_flush_all)
			registered_drivers[i]->callbacks->tlb_ops->tlb_flush_all(cookie);
	}
}

static void smmu_tlb_flush_walk(unsigned long iova, size_t size,
				size_t granule, void *cookie)
{
	int i;

	for (i = 0; i < num_registered_drivers; i++) {
		if (!registered_drivers[i]->callbacks->tlb_ops) {
			mod_ops->puts("WARN!! Possible stale TLB entries, tlb ops not implemented");
			continue;
		}

		if (registered_drivers[i]->callbacks->tlb_ops->tlb_flush_walk)
			registered_drivers[i]->callbacks->tlb_ops->tlb_flush_walk(iova, size,
										  granule, cookie);
	}
}

static void smmu_tlb_add_page(struct iommu_iotlb_gather *gather,
			      unsigned long iova, size_t granule,
			      void *cookie)
{
	int i;

	for (i = 0; i < num_registered_drivers; i++) {
		if (!registered_drivers[i]->callbacks->tlb_ops) {
			mod_ops->puts("WARN!! Possible stale TLB entries, tlb ops not implemented");
			continue;
		}

		if (registered_drivers[i]->callbacks->tlb_ops->tlb_add_page)
			registered_drivers[i]->callbacks->tlb_ops->tlb_add_page(gather, iova,
										granule, cookie);
	}
}

static const struct iommu_flush_ops smmu_tlb_ops = {
	.tlb_flush_all	= smmu_tlb_flush_all,
	.tlb_flush_walk = smmu_tlb_flush_walk,
	.tlb_add_page	= smmu_tlb_add_page,
};

static int qcom_smmu_nestinc_init_pgt(struct smmu_nested_domain *smmu_domain)
{
	int i, ret = 0;

	/* TBD: populate after probing all SMMUs */
	struct io_pgtable_cfg cfg = (struct io_pgtable_cfg) {
		.fmt = ARM_64_LPAE_S2,
		.pgsize_bitmap = SZ_4K | SZ_2M | SZ_1G,
		.ias = 48,
		.oas = 48,
		.coherent_walk = true,
		.tlb = &smmu_tlb_ops,
	};

	/* Configure the page table with best effort. */
	for (i = 0; i < num_registered_drivers; i++) {
		struct io_pgtable_cfg ncfg;

		ncfg.coherent_walk = true;

		if (registered_drivers[i]->callbacks->get_cfg) {
			registered_drivers[i]->callbacks->get_cfg(&ncfg);

			cfg.ias = min(cfg.ias, ncfg.ias);
			cfg.oas = min(cfg.oas, ncfg.oas);
			if (!ncfg.coherent_walk)
				cfg.coherent_walk = false;
		}
	}

	/* At least PAGE_SIZE must be supported by all SMMUs*/
	if ((cfg.pgsize_bitmap & PAGE_SIZE) == 0)
		return -EINVAL;

	smmu_domain->domain.priv = &idmapped_domain;
	smmu_domain->pgtable = kvm_arm_io_pgtable_alloc(&cfg, &smmu_domain->domain,
							&ret, true);
	if (ret)
		return ret;

	WARN_ON(!smmu_domain->pgtable);

	WARN_ON(kvm_iommu_snapshot_host_stage2(&smmu_domain->domain));

	return ret;
}

static struct kvm_hyp_iommu *qcom_nesting_id_to_iommu(pkvm_handle_t smmu_id)
{
	return NULL;
}

static int qcom_smmu_nesting_alloc_domain(struct kvm_hyp_iommu_domain *domain, int type)
{
	struct smmu_nested_guest_domain *guest_domain;

	if (type >= KVM_ARM_SMMU_DOMAIN_MAX)
		return -EINVAL;

	guest_domain = hyp_alloc(sizeof(*guest_domain));
	if (!guest_domain)
		return -ENOMEM;

	/*
	 * The SMMU version (v2/v3) is not known here, so the page table and
	 * the SMMU binding are deferred to attach_dev, which supplies the
	 * version-specific TLB ops. Keep them unset until then.
	 */
	guest_domain->domain = domain;
	guest_domain->iommu = NULL;
	guest_domain->cb_idx = -1;
	guest_domain->pgtable = NULL;
	hyp_spin_lock_init(&guest_domain->pgt_lock);

	domain->priv = guest_domain;

	return 0;
}

static void qcom_smmu_nesting_free_domain(struct kvm_hyp_iommu_domain *domain)
{
	struct smmu_nested_guest_domain *guest_domain = domain->priv;

	if (!guest_domain)
		return;

	/* Page table is allocated at attach_dev; NULL if never attached. */
	if (guest_domain->pgtable)
		kvm_arm_io_pgtable_free(guest_domain->pgtable);

	/*
	 * TODO: force_free_domain can free a domain with attached devices
	 * (i.e. no detach_dev). Reclaim the guest CB/SMR pool slots here when
	 * guest_domain->iommu != NULL, else a crashing pVM leaks them.
	 */

	hyp_free(guest_domain);
	domain->priv = NULL;
}

static int qcom_smmu_nesting_detach_dev(struct kvm_hyp_iommu *iommu,
					struct kvm_hyp_iommu_domain *domain,
					u32 sid, u32 pasid)
{
	return 0;
}

static int qcom_smmu_nesting_attach_dev(struct kvm_hyp_iommu *iommu,
					struct kvm_hyp_iommu_domain *domain,
					u32 endpoint_id, u32 pasid, u32 pasid_bits,
					unsigned long flags)
{
	return 0;
}

static int qcom_smmu_nesting_map_pages(struct kvm_hyp_iommu_domain *domain,
				       unsigned long iova, phys_addr_t paddr,
				       size_t pgsize, size_t pgcount, int prot,
				       size_t *total_mapped)
{
	return 0;
}

static size_t qcom_smmu_nesting_unmap_pages(struct kvm_hyp_iommu_domain *domain,
					    unsigned long iova,
					    size_t pgsize, size_t pgcount,
					    struct iommu_iotlb_gather *iotlb_gather)
{
	return 0;
}

static int qcom_smmu_nesting_init(void)
{
	int ret, i = 0;

	ret = smmuv3_hyp_nesting_init();
	if (ret)
		return ret;

	ret = smmuv2_hyp_nesting_init();
	if (ret)
		return ret;

	ret =  qcom_smmu_nestinc_init_pgt(&idmapped_domain);
	if (ret)
		return ret;

	for (i = 0; i < num_registered_drivers; i++) {
		if (registered_drivers[i]->callbacks->post_init) {
			ret = registered_drivers[i]->callbacks->post_init();
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int qcom_smmu_nesting_suspend(struct kvm_hyp_iommu *iommu)
{
	return 0;
}

static int qcom_smmu_nesting_resume(struct kvm_hyp_iommu *iommu)
{
	return 0;
}

static size_t smmu_pgsize_idmap(size_t size, u64 paddr, size_t pgsize_bitmap)
{
	size_t pgsizes;

	/* Remove page sizes that are larger than the current size */
	pgsizes = pgsize_bitmap & GENMASK_ULL(__fls(size), 0);

	/* Remove page sizes that the address is not aligned to. */
	if (likely(paddr))
		pgsizes &= GENMASK_ULL(__ffs(paddr), 0);

	WARN_ON(!pgsizes);

	/* Return the larget page size that fits. */
	return BIT(__fls(pgsizes));
}

static void qcom_smmu_nesting_idmap(struct kvm_hyp_iommu_domain *domain,
				    phys_addr_t start, phys_addr_t end, int prot)
{
	size_t size;
	size_t pgsize, pgcount;
	size_t mapped, unmapped;
	int ret;
	struct smmu_nested_domain *smmu_domain = &idmapped_domain;
	struct io_pgtable *pgtable = smmu_domain->pgtable;
	unsigned long pgsize_bitmap = pgtable->cfg.pgsize_bitmap;
	struct iommu_iotlb_gather gather;

	end = min(end, BIT(pgtable->cfg.oas));
	if (start >= end)
		return;
	size = end - start;

	if (prot) {
		if (!(prot & IOMMU_MMIO) && pgtable->cfg.coherent_walk)
			prot |= IOMMU_CACHE;

		while (size) {
			mapped = 0;
			pgsize = smmu_pgsize_idmap(size, start, pgsize_bitmap);
			pgcount = size / pgsize;

			ret = pgtable->ops.map_pages(&pgtable->ops, start, start,
						     pgsize, pgcount, prot, 0, &mapped);
			/*
			 * Failing to map isn't compromising security. Make sure we don't crash for
			 * that.
			 */
			if (ret || !mapped)
				return;

			size -= mapped;
			start += mapped;
		}
	} else {
		while (size) {
			iommu_iotlb_gather_init(&gather);
			pgsize = smmu_pgsize_idmap(size, start, pgsize_bitmap);
			pgcount = size / pgsize;
			unmapped = pgtable->ops.unmap_pages(&pgtable->ops, start,
							    pgsize, pgcount, &gather);
			if (!unmapped)
				break;

			size -= unmapped;
			start += unmapped;
		}
		/* Some memory were not unmapped. */
		WARN_ON(size);
	}
}

bool qcom_dispatcher_dabt_handler(struct user_pt_regs *regs, u64 esr, u64 addr)
{
	int i;
	bool ret = false;

	for (i = 0; i < num_registered_drivers; i++) {
		if (registered_drivers[i]->callbacks->dabt_hdl) {
			ret = registered_drivers[i]->callbacks->dabt_hdl(regs, esr, addr);
			if (ret)
				return ret;
		}
	}

	return ret;
}

struct kvm_iommu_ops qcom_smmu_hyp_nesting_ops = {
	.init				= qcom_smmu_nesting_init,
	.get_iommu_by_id		= qcom_nesting_id_to_iommu,
	.alloc_domain			= qcom_smmu_nesting_alloc_domain,
	.free_domain			= qcom_smmu_nesting_free_domain,
	.dabt_handler                   = qcom_dispatcher_dabt_handler,
	.attach_dev			= qcom_smmu_nesting_attach_dev,
	.detach_dev			= qcom_smmu_nesting_detach_dev,
	.map_pages			= qcom_smmu_nesting_map_pages,
	.unmap_pages			= qcom_smmu_nesting_unmap_pages,
	.suspend			= qcom_smmu_nesting_suspend,
	.resume				= qcom_smmu_nesting_resume,
	.host_stage2_idmap		= qcom_smmu_nesting_idmap,
};

#ifdef MODULE
int qcom_smmu_nesting_init_module(const struct pkvm_module_ops *ops)
{
	mod_ops = ops;

	return 0;
}
#endif
