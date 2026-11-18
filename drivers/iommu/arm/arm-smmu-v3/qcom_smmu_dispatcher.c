// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <asm/arm-smmu-v3-common.h>
#include <asm/kvm_pkvm_module.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm.h>

#include <linux/atomic.h>
#include <linux/moduleparam.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/kvm_host.h>
#include "smmuv2_nesting.h"
#include "arm_smmu_v3_nested.h"

#ifdef MODULE
static unsigned long                   pkvm_module_token;

#define ksym_ref_addr_nvhe(x) \
	((typeof(kvm_nvhe_sym(x))*)(pkvm_el2_mod_va(&kvm_nvhe_sym(x), pkvm_module_token)))
#else
#define ksym_ref_addr_nvhe(x) \
	((typeof(kvm_nvhe_sym(x))*)(kern_hyp_va(lm_alias(&kvm_nvhe_sym(x)))))
#endif

int kvm_nvhe_sym(qcom_smmu_nesting_init_module)(const struct pkvm_module_ops *ops);
extern struct kvm_iommu_ops kvm_nvhe_sym(qcom_smmu_hyp_nesting_ops);

static int atomic_pages;
module_param(atomic_pages, int, 0);

static int smmu_alloc_atomic_mc(struct kvm_hyp_memcache *atomic_mc)
{
	int ret;
	int nr_smmus = 2; /* assumption for now */

	if (!atomic_pages) {
		struct device_node *np = NULL;
		u64 total_ram = 0;
		unsigned long mmio_pages;
		int regions = 0;

		/*
		 * Allocate pages to cover mapping with PAGE_SIZE for all memory
		 * Then allocate extra for 4GB of MMIO.
		 * Add 64 extra pages as we map the rest with first level blocks.
		 */
		while ((np = of_find_node_by_name(np, "memory"))) {
			u64 addr, size;
			int i = 0;

			while (!of_property_read_reg(np, i++, &addr, &size)) {
				unsigned long reg_pages;

				if (!size)
					continue;

				reg_pages = __hyp_pgtable_max_pages(size >> PAGE_SHIFT);
				atomic_pages += reg_pages;
				total_ram += size;
				regions++;
				pr_info("smmuv3: DT memory[%d] [0x%llx-0x%llx] (%llu MiB) -> +%lu atomic_pages\n",
					regions - 1, addr, addr + size - 1,
					size >> 20, reg_pages);
			}
		}

		if (!atomic_pages) {
			atomic_pages = __hyp_pgtable_max_pages((32ULL * SZ_1G) >> PAGE_SHIFT);
			pr_warn("smmuv3: No DT memory nodes found, falling back to 32 GiB (%d atomic_pages)\n",
				atomic_pages);
		}

		mmio_pages = __hyp_pgtable_max_pages((4ULL * SZ_1G) >> PAGE_SHIFT) + 64;
		pr_info("smmuv3: DT total RAM %llu MiB across %d regions (%d pages) + 4 GiB MMIO/extra (%lu pages) = %lu atomic_pages\n",
			total_ram >> 20, regions, atomic_pages, mmio_pages,
			atomic_pages + mmio_pages);
		atomic_pages += mmio_pages;
	} else {
		pr_info("smmuv3: Using module_param atomic_pages = %d\n", atomic_pages);
	}

	/* For PGD*/
	ret = topup_hyp_memcache(atomic_mc, 1, 3);
	if (ret)
		return ret;

	/* For STEs. */
	ret = topup_hyp_memcache(atomic_mc, nr_smmus, 10);
		if (ret)
			return ret;

	/* For command queue. */
	ret = topup_hyp_memcache(atomic_mc, nr_smmus, 8);
	if (ret)
		return ret;

	/* For PGD. */
	ret = topup_hyp_memcache(atomic_mc, nr_smmus, 3);
	if (ret)
		return ret;

	/*For L2 ptrs */;
	ret = topup_hyp_memcache(atomic_mc, 50, 2);
	if (ret)
		return ret;

	ret = topup_hyp_memcache(atomic_mc, atomic_pages, 0);
	if (ret)
		return ret;

	pr_info("smmuv3: Allocated %d atomic_pages (%lu MiB) for stage-2 pgtable (memcache chunks: %lu)\n",
		atomic_pages, ((unsigned long)atomic_pages << PAGE_SHIFT) / SZ_1M,
		atomic_mc->nr_pages);

	return 0;
}

static int qcom_smmu_nesting_init(void)
{
	int ret = 0;
	struct kvm_hyp_memcache atomic_mc;

	init_hyp_memcache(&atomic_mc);

	ret = smmu_alloc_atomic_mc(&atomic_mc);
	if (ret)
		return ret;

	pr_info("alloc atomic suceeded\n");

	ret = kvm_arm_smmu_v3_init_drv();
	if (ret) {
		pr_err("Failed to load SMMUv3 IOMMU EL1 module %d\n", ret);
		return ret;
	}

	smmuv2_nesting_init();

#ifdef MODULE
	ret = pkvm_load_el2_module(kvm_nvhe_sym(qcom_smmu_nesting_init_module),
				   &pkvm_module_token);

	if (ret) {
		pr_err("Failed to load SMMUv3 IOMMU EL2 module: %d\n", ret);
		return ret;
	}
#endif

	ret = kvm_iommu_init_hyp(ksym_ref_addr_nvhe(qcom_smmu_hyp_nesting_ops),
				 &atomic_mc);

	if (ret) {
		pr_err("Failed to init hyp iommu ops: %d\n", ret);
		return ret;
	}

	ret = smmuv2_post_boot_init();
	if (ret) {
		pr_err("Failed to initialize SMMUv2 post boot: %d\n", ret);
		return ret;
	}

	ret = kvm_arm_smmu_v3_post_init();
	if (ret) {
		pr_err("Failed to initialize SMMUv3 post boot: %d\n", ret);
		return ret;
	}

	return ret;
}

static pkvm_handle_t qcom_smmu_get_iommu_id(struct device_node *np)
{
	return (pkvm_handle_t)0;
}

static void qcom_smmu_nesting_remove(void)
{
}

struct kvm_iommu_driver qcom_smmu_nesting_ops = {
	.init_driver = qcom_smmu_nesting_init,
	.remove_driver = qcom_smmu_nesting_remove,
	.get_iommu_id_by_of = qcom_smmu_get_iommu_id,
};

static int qcom_smmu_nesting_register(void)
{
	return kvm_iommu_register_driver(&qcom_smmu_nesting_ops);
}

/*
 * Register must be run before de-privliage before kvm_iommu_init_driver
 * for module case, it should be loaded using pKVM early loading which
 * loads it before this point.
 * For builtin drivers we use core_initcall
 */
subsys_initcall(qcom_smmu_nesting_register);
MODULE_LICENSE("GPL");
