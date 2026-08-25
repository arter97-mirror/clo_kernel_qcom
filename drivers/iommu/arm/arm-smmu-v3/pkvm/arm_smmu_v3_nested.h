/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_ARM_SMMU_V3_H
#define __KVM_ARM_SMMU_V3_H

#include <asm/kvm_asm.h>

#ifdef __KVM_NVHE_HYPERVISOR__
#include <nvhe/spinlock.h>
#endif

#include "arm-smmu-v3.h"

/*
 * Parameters from the trusted host:
 * @mmio_addr		base address of the SMMU registers
 * @mmio_size		size of the registers resource
 * @features		Features of SMMUv3, subset of the main driver
 * @strtab_dma		Phys address of stream table
 * @strtab_size		Stream table size
 *
 * Other members are filled and used at runtime by the SMMU driver.
 * @base		Virtual address of SMMU registers
 * @ias			IPA size
 * @oas			PA size
 * @pgsize_bitmap	Supported page sizes
 * @sid_bits		Max number of SID bits supported
 * @lock		Lock to protect SMMU
 * @cmdq		CMDQ as observed by HW
 * @cmdq_host		Host view of the CMDQ, only q_base and llq used.
 * @cr0			Last value of CR0
 * @host_ste_cfg	Host stream table config
 * @host_ste_base	Host stream table base
 * @strtab_cfg		Stream table as seen by HW
 * @evtq_base		Host evtq base reg
 * @priq_base		Host priq base reg
 */
struct hyp_arm_smmu_v3_device {
	phys_addr_t		mmio_addr;
	size_t			mmio_size;
	void __iomem		*base;
	u32			features;
	unsigned long		ias;
	unsigned long		oas;
	unsigned long		pgsize_bitmap;
	unsigned int		sid_bits;
#ifdef __KVM_NVHE_HYPERVISOR__
	hyp_spinlock_t		lock;
#else
	u32			lock;
#endif
	struct arm_smmu_queue	cmdq;
	struct arm_smmu_queue	cmdq_host;
	u32			cr0;
	dma_addr_t		strtab_dma;
	size_t			strtab_size;
	u64			host_ste_cfg;
	u64			host_ste_base;
	struct arm_smmu_strtab_cfg strtab_cfg;
	unsigned long           evtq_base;
	unsigned long           priq_base;
	/* Order smmu.lock => host_mmu.lock => smmu.hw_lock */
	u32			hw_lock;
	bool			active;
};

extern size_t kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count);
#define kvm_hyp_arm_smmu_v3_count kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_count)

extern struct hyp_arm_smmu_v3_device *kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus);
#define kvm_hyp_arm_smmu_v3_smmus kvm_nvhe_sym(kvm_hyp_arm_smmu_v3_smmus)

#ifdef __KVM_NVHE_HYPERVISOR__
static inline hyp_spinlock_t *kvm_smmu_get_hw_lock(struct hyp_arm_smmu_v3_device *smmu)
{
		/* See struct kvm_hyp_iommu */
		BUILD_BUG_ON(sizeof(smmu->hw_lock) != sizeof(hyp_spinlock_t));
			return (hyp_spinlock_t *)(&smmu->hw_lock);
}

static inline void kvm_smmu_hw_lock_init(struct hyp_arm_smmu_v3_device *smmu)
{
		hyp_spin_lock_init(kvm_smmu_get_hw_lock(smmu));
}

static inline void kvm_smmu_hw_lock(struct hyp_arm_smmu_v3_device *smmu)
{
		hyp_spin_lock(kvm_smmu_get_hw_lock(smmu));
}

static inline void kvm_smmu_hw_unlock(struct hyp_arm_smmu_v3_device *smmu)
{
		hyp_spin_unlock(kvm_smmu_get_hw_lock(smmu));
}
#endif /* __KVM_NVHE_HYPERVISOR__ */
#ifdef MODULE
int smmu_init_hyp_module(const struct pkvm_module_ops *ops);
#endif

int kvm_arm_smmu_v3_init_drv(void);
int kvm_arm_smmu_v3_post_init(void);

#endif /* __KVM_ARM_SMMU_V3_H */
