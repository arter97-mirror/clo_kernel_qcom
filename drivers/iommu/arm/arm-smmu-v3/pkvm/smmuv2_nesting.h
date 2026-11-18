/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SMMUv2 Nested Virtualization Header
 *
 * This file defines structures and constants for SMMUv2 nested virtualization
 * support in the pKVM hypervisor. It includes definitions for Stream Matching
 * Registers (SMR), Context Banks (CB), and the main nested SMMU structure.
 */
#ifndef _SMMUV2_NESTING_H
#define _SMMUV2_NESTING_H
#include <kvm/iommu.h>

/* Global address offset mask for SMMUv2 */
#define SMMU_V2_GLB_ADDR_OFFSET_MASK   0x0000FFFFU
/* VMID for host S2 context bank */
#define HOST_S2_VMID 0x3
/* Maximum number of context banks and CBARs per SMMU */
#define ARM_SMMU_MAX_CBS 128
#define ARM_SMMU_MAX_CBARS ARM_SMMU_MAX_CBS
/* Maximum number of SMRs and S2CRs per SMMU */
#define ARM_SMMU_MAX_SMRS 256
#define ARM_SMMU_MAX_S2CRS ARM_SMMU_MAX_SMRS

#define ARM_SMMU_MAX_HANDOFF_SMRS 8

/* Guest pVMs resource limits
 * Actual counts are determined dynamically from the pkvm_assignable_devices
 * DT node, stored in smmu_v2_nested.num_guest_cb / num_guest_smr.
 */
/* upper bound: S2 CBs reservable for guest pVMs */
#define ARM_SMMU_MAX_GUEST_S2_CB	16
/* upper bound: SMRs reservable for guest pVMs */
#define ARM_SMMU_MAX_GUEST_SMRS		16

/* Guest SMR pool entry is unallocated */
#define ARM_SMMU_GUEST_SMR_FREE		0
/* Guest CB pool entry is unallocated*/
#define ARM_SMMU_GUEST_CB_FREE		0

/**
 * struct smmu_v2_nested - SMMUv2 nested virtualization structure
 * @iommu: Base IOMMU structure
 * @base_pa: Base physical address
 * @base_va: Base virtual address
 * @size: Size of the SMMU region
 * @ias: Input Address Size
 * @oas: Output Address Size
 * @pgsize_bitmap: Page size bitmap
 * @cr0: Control Register 0 value
 * @num_smr: Number of Stream Matching Registers
 * @num_cb: Number of Context Banks
 * @smr_pool: Pool of SMR information structures
 * @cb_pool: Pool of Context Bank information structures
 */
struct smmu_v2_nested {
	struct kvm_hyp_iommu iommu;
	u64 base_pa;
	u64 base_va;
	u64 host_cb_base;
	u64 host_gr1_base;
	u32 size;
	u32 ias;
	u32 oas;
	u32 pgsize_bitmap;
	u32 cr0;
	u32 irq_s2_cb; /* Context bank s2 fault irq */
	u32 total_num_smr; /* Total SMR allocation for NS (Host+Guest) */
	u32 num_smr;  /* SMR allocation for Host */
	u32 num_s2cr; /* S2CR allocation for Host */
	u32 num_cbar; /* CBAR allocation for Host */
	u32 num_cb;   /* CB allocation for Host */
	u32 num_handoff_smrs;  /* Number of Host Handoff SMRs */
	u32 pgshift;  /* Page size 4KB or 64KB  */
	u32 numpage;
	u32 host_s2_cb_idx;  /* Index of reserved host S2 context bank */
	u32 smr_pool[ARM_SMMU_MAX_SMRS];
	u32 s2cr_pool[ARM_SMMU_MAX_S2CRS];
	u32 cbar_pool[ARM_SMMU_MAX_CBS];
	u32 handoff_smrs[ARM_SMMU_MAX_HANDOFF_SMRS];
	u32 guest_smr_base;  /* First reserved SMR/S2CR index for guest pVMs */
	u32 guest_cb_base;   /* First reserved CB index for guest pVMs */
	u32 num_guest_smr;   /* SMR allocation for Guest */
	u32 num_guest_cb;    /* S2 CB allocation for Guest (unique group_ids) */
	/* SMR/S2CR pool for guest pVMs - ARM_SMMU_GUEST_SMR_FREE, else cb_idx */
	u32 guest_smr_pool[ARM_SMMU_MAX_GUEST_SMRS];
	/* CB pool for guest pVMs - ARM_SMMU_GUEST_CB_FREE, else VMID/domain_id */
	u32 guest_cb_pool[ARM_SMMU_MAX_GUEST_S2_CB];
#define ARM_SMMU_FEAT_COHERENT_WALK	(1 << 0)
#define ARM_SMMU_FEAT_STREAM_MATCH	(1 << 1)
#define ARM_SMMU_FEAT_TRANS_S1_V2	(1 << 2)
#define ARM_SMMU_FEAT_TRANS_S2_V2	(1 << 3)
#define ARM_SMMU_FEAT_TRANS_NESTED	(1 << 4)
#define ARM_SMMU_FEAT_TRANS_OPS		(1 << 5)
#define ARM_SMMU_FEAT_VMID16		(1 << 6)
#define ARM_SMMU_FEAT_FMT_AARCH64_4K	(1 << 7)
#define ARM_SMMU_FEAT_FMT_AARCH64_16K	(1 << 8)
#define ARM_SMMU_FEAT_FMT_AARCH64_64K	(1 << 9)
#define ARM_SMMU_FEAT_FMT_AARCH32_L	(1 << 10)
#define ARM_SMMU_FEAT_FMT_AARCH32_S	(1 << 11)
#define ARM_SMMU_FEAT_EXIDS		(1 << 12)
	u32				features;
};

int smmuv2_hyp_nesting_init(void);
int smmuv2_nesting_init(void);
int smmuv2_post_boot_init(void);

#endif /* _SMMUV2_NESTING_H */
