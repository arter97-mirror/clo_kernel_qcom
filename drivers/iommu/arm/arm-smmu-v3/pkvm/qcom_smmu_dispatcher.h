/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#ifndef __SMMU_VENDOR_COMMON_H__
#define __SMMU_VENDOR_COMMON_H__

#include <kvm/iommu.h>
#include <linux/io-pgtable-arm.h>

#if defined(__KVM_NVHE_HYPERVISOR__) && defined(MODULE)
#include <asm/kvm_pkvm_module.h>

extern const struct pkvm_module_ops *smmu_vendor_ops;
extern struct smmu_nested_domain idmapped_domain;

/* Forward declarations */
struct smmu_vendor_driver;

struct smmu_nested_domain {
	struct kvm_hyp_iommu_domain     domain;
	struct io_pgtable               *pgtable;
};

/*
 * Common guest (pVM) domain container, version-agnostic.
 * Allocated by the dispatcher at alloc_domain; domain->priv points here.
 * The io_pgtable and the SMMU binding are version-specific and are filled
 * in by the vendor attach_dev callback (which supplies the v2/v3 TLB ops
 * when allocating the page table), so they are NULL/unset until a device
 * is attached.
 *
 * v1 assumption: a guest domain spans a single SMMU instance (@iommu).
 * Multi-SMMU domains would replace @iommu/@cb_idx with a per-domain SMMU
 * list.
 */
/*
 * smmu_nested_guest_domain:
 * @domain: back-ptr; domain_id is the SMMUv3 VMID (S2) / ASID (S1)
 * @iommu: owning SMMU, NULL until attach
 * @cb_idx: SMMUv2 guest S2 CB, -1 until attach
 * @pgt_lock: serializes map/unmap on @pgtable
 * @pgtable: S2 page table, NULL until attach
 */
struct smmu_nested_guest_domain {
	struct kvm_hyp_iommu_domain	*domain;
	struct kvm_hyp_iommu		*iommu;
	int				cb_idx;
	hyp_spinlock_t			pgt_lock;
	struct io_pgtable		*pgtable;
};

/* SMMU vendor driver callbacks - implemented by specific SMMU versions */
struct smmu_vendor_callbacks {
	const struct iommu_flush_ops *tlb_ops;
	void (*get_cfg)(struct io_pgtable_cfg *ncfg);
	int (*post_init)(void);
	bool (*dabt_hdl)(struct user_pt_regs *regs, u64 esr, u64 addr);
	struct kvm_hyp_iommu *(*get_iommu_by_id)(pkvm_handle_t smmu_id);
	int (*get_iommu_token_by_id)(pkvm_handle_t smmu_id, u64 *out_token);
	int (*attach_dev)(struct kvm_hyp_iommu *iommu,
			  struct kvm_hyp_iommu_domain *domain,
			  u32 sid, u32 pasid, u32 pasid_bits, unsigned long flags);
	int (*detach_dev)(struct kvm_hyp_iommu *iommu,
			  struct kvm_hyp_iommu_domain *domain,
			  u32 sid, u32 pasid);
};

/* SMMU vendor driver structure */
struct smmu_vendor_driver {
	const char *name;
	const struct smmu_vendor_callbacks *callbacks;
	struct smmu_vendor_data *vdata;
	void *priv_data;  /* Driver-specific private data */
};

/* SMMU vendor data */
struct smmu_vendor_data {
	const struct pkvm_module_ops *vops;
	struct smmu_nested_domain *vdomain;
};

/* Common vendor module functions */
int smmu_vendor_register_driver(struct smmu_vendor_driver *driver);
int smmu_vendor_unregister_driver(struct smmu_vendor_driver *driver);
int smmu_vendor_init_module(const struct pkvm_module_ops *ops);
#endif

#if defined(__KVM_NVHE_HYPERVISOR__) && defined(MODULE)

#include <asm/kvm_pkvm_module.h>

extern const struct pkvm_module_ops  *mod_ops;

#undef memset
#undef memcpy
#undef udelay
#undef kern_hyp_va
#undef kvm_flush_dcache_to_poc

#define CALL_FROM_OPS(fn, ...)                  (mod_ops->fn(__VA_ARGS__))

/* Needs alternatives which is not supported at the moment. */
#undef cmpxchg64_relaxed
#define cmpxchg64_relaxed                       __ll_sc__cmpxchg_case_64
#define hyp_free(x)                             CALL_FROM_OPS(hyp_free, x)
#define hyp_alloc_errno()                       CALL_FROM_OPS(hyp_alloc_errno)
#define hyp_alloc(x)                            CALL_FROM_OPS(hyp_alloc, x)
#define kvm_iommu_donate_pages(x, y)            CALL_FROM_OPS(iommu_donate_pages, x, y)
#define kvm_iommu_reclaim_pages(x, y)           CALL_FROM_OPS(iommu_reclaim_pages, x, y)
#define hyp_virt_to_phys(x)                     CALL_FROM_OPS(hyp_pa, x)
#define hyp_phys_to_virt(x)                     CALL_FROM_OPS(hyp_va, x)
#define memcpy(x, y, z)                         CALL_FROM_OPS(memcpy, x, y, z)
#define kvm_iommu_init_device(x)                CALL_FROM_OPS(iommu_init_device, x)
#define pkvm_udelay(x)                          CALL_FROM_OPS(udelay, x)
#define kvm_flush_dcache_to_poc(x, y)           CALL_FROM_OPS(flush_dcache_to_poc, x, y)
#define hyp_alloc_missing_donations()           CALL_FROM_OPS(hyp_alloc_missing_donations)
#define ___pkvm_host_donate_hyp_prot(x, y, z, w) CALL_FROM_OPS(host_donate_hyp_prot, x, y, z, w)
#define ___pkvm_host_donate_hyp(x, y, z)         CALL_FROM_OPS(host_donate_hyp, x, y, z)
#define kern_hyp_va(x)                          \
		((void *)CALL_FROM_OPS(kern_hyp_va, (unsigned long)x))
#define __pkvm_host_donate_hyp(x, y)            CALL_FROM_OPS(host_donate_hyp, x, y, false)
#define __pkvm_hyp_donate_host(x, y)            CALL_FROM_OPS(hyp_donate_host, x, y)
#define __pkvm_host_share_hyp(x)                CALL_FROM_OPS(host_share_hyp, x)
#define hyp_pin_shared_mem(x, y)                CALL_FROM_OPS(pin_shared_mem, x, y)
#define __pkvm_host_unshare_hyp(x)              CALL_FROM_OPS(host_unshare_hyp, x)
#define hyp_unpin_shared_mem(x, y)              CALL_FROM_OPS(unpin_shared_mem, x, y)
#define kvm_iommu_iotlb_gather_add_page(x, y, z, w) \
		CALL_FROM_OPS(iommu_iotlb_gather_add_page, x, y, z, w)
#define iommu_pkvm_unuse_dma(x, y)              CALL_FROM_OPS(pkvm_unuse_dma, x, y)
#define kvm_iommu_donate_pages_atomic(x)        CALL_FROM_OPS(iommu_donate_pages_atomic, x)
#define kvm_iommu_reclaim_pages_atomic(x, y)    CALL_FROM_OPS(iommu_reclaim_pages_atomic, x, y)
#define kvm_iommu_snapshot_host_stage2(x)       CALL_FROM_OPS(iommu_snapshot_host_stage2, x)

#endif
#endif /* __SMMU_VENDOR_COMMON_H__ */
