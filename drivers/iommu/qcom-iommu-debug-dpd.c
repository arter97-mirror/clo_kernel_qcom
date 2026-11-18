// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include "qcom-iommu-debug.h"

/**
 * iommu_debug_find_dpd_proxy_usecase - Find index of first child node compatible
 * with "qcom,testcase-dpd-proxy"
 * @ddev: debug device
 *
 * Returns the index of the first child devicetree node of ddev->self which is
 * compatible with "qcom,testcase-dpd-proxy", or -1 if not found.
 */
static int iommu_debug_find_dpd_proxy_usecase(struct iommu_debug_device *ddev)
{
	struct device_node *child;
	int child_nr = 0;

	for_each_child_of_node(ddev->self->of_node, child) {
		if (of_device_is_compatible(child, "qcom,testcase-dpd-proxy")) {
			of_node_put(child);
			return child_nr;
		}
		child_nr++;
	}

	return -1;
}

/**
 * test_large_num_objects
 * @s: sequence file for output
 * @ddev: debug device
 *
 * Returns 0 on success, negative error code on failure
 */
static int test_large_num_objects(struct seq_file *s, struct iommu_debug_device *ddev)
{
	struct iommu_debug_usecase_device *udev;
	struct iommu_debug_mem *mem, *tmp;
	int usecase_idx;
	int i, allocated_count = 0;
	int ret = 0;
	int max_objects = 16000;

	seq_puts(s, "Large #of objects test\n");
	seq_puts(s, "========================================\n");

	/* Find the DPD proxy usecase */
	usecase_idx = iommu_debug_find_dpd_proxy_usecase(ddev);
	if (usecase_idx < 0) {
		seq_puts(s, "FAILED - No DPD proxy usecase found\n");
		return -ENODEV;
	}

	/* Switch to the DPD proxy usecase */
	mutex_lock(&ddev->state_lock);
	if (!iommu_debug_switch_usecase(ddev, usecase_idx)) {
		mutex_unlock(&ddev->state_lock);
		seq_puts(s, "FAILED - Could not switch to DPD proxy usecase\n");
		return -EINVAL;
	}

	/* Get the usecase device */
	udev = dev_get_drvdata(ddev->test_dev);
	if (!udev) {
		mutex_unlock(&ddev->state_lock);
		seq_puts(s, "FAILED - Could not get usecase device data\n");
		return -EINVAL;
	}

	seq_puts(s, "Successfully switched to DPD proxy usecase\n");
	seq_printf(s, "Allocating 4KB objects until reaching %d total objects...\n", max_objects);

	for (i = 0; i < max_objects; i++) {
		mem = iommu_debug_mem_alloc_contig(udev, SZ_4K);
		if (IS_ERR_OR_NULL(mem)) {
			seq_printf(s, "Allocation failed at object %d\n", i);
			ret = -ENOMEM;
			break;
		}

		/* DMA map the allocated memory */
		mem->dma_addr = dma_map_page(udev->dev, mem->page, 0, mem->size, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(udev->dev, mem->dma_addr)) {
			seq_printf(s, "DMA mapping failed at object %d\n", i);
			/* Free the allocated memory since DMA mapping failed */
			iommu_debug_mem_free(udev, mem);
			ret = -ENOMEM;
			break;
		}
		mem->dma_mapped = true;

		allocated_count++;

		/* Print progress every 1000 allocations */
		if ((i + 1) % 1000 == 0) {
			seq_printf(s, "Allocated and DMA mapped %d objects (%lu MB total)\n",
				   i + 1, ((i + 1UL) * SZ_4K) / SZ_1M);
		}
	}

	if (allocated_count == max_objects) {
		seq_printf(s, "SUCCESS - Allocated and DMA mapped all %d objects (%lu MB total)\n",
			   max_objects, ((unsigned long)max_objects * SZ_4K) / SZ_1M);
	} else {
		seq_printf(s, "PARTIAL SUCCESS - Allocated and DMA mapped %d objects\n",
			allocated_count);
	}

	/* Create temporary list for objects to free */
	LIST_HEAD(temp_free_list);

	/* Move objects allocated during this test to temporary list */
	mutex_lock(&udev->mem_lock);
	list_splice_init(&udev->mem_list, &temp_free_list);
	mutex_unlock(&udev->mem_lock);

	list_for_each_entry_safe(mem, tmp, &temp_free_list, list)
		iommu_debug_mem_free(udev, mem);

	mutex_unlock(&ddev->state_lock);
	seq_puts(s, "Large #of objects test completed\n\n");

	return ret;
}

/**
 * test_large_14mb_objects
 * @s: sequence file for output
 * @ddev: debug device
 *
 * Returns 0 on success, negative error code on failure
 */
static int test_max_limit_secure_memory(struct seq_file *s, struct iommu_debug_device *ddev)
{
	struct iommu_debug_usecase_device *udev;
	struct iommu_debug_mem *mem, *tmp;
	int usecase_idx;
	int i, allocated_count = 0;
	int ret = 0;
	size_t object_size = 2 * SZ_1M;
	size_t target_total = 2UL * SZ_1G;
	int max_objects = target_total / object_size;

	seq_printf(s, "Test secure memory limit (target: %zx)\n", target_total);
	seq_puts(s, "========================================\n");

	/* Find the DPD proxy usecase */
	usecase_idx = iommu_debug_find_dpd_proxy_usecase(ddev);
	if (usecase_idx < 0) {
		seq_puts(s, "FAILED - No DPD proxy usecase found\n");
		return -ENODEV;
	}

	/* Switch to the DPD proxy usecase */
	mutex_lock(&ddev->state_lock);
	if (!iommu_debug_switch_usecase(ddev, usecase_idx)) {
		mutex_unlock(&ddev->state_lock);
		seq_puts(s, "FAILED - Could not switch to DPD proxy usecase\n");
		return -EINVAL;
	}

	/* Get the usecase device */
	udev = dev_get_drvdata(ddev->test_dev);
	if (!udev) {
		mutex_unlock(&ddev->state_lock);
		seq_puts(s, "FAILED - Could not get usecase device data\n");
		return -EINVAL;
	}

	seq_puts(s, "Successfully switched to DPD proxy usecase\n");
	seq_printf(s, "Allocating objects of size %zx until reaching %zx total (%d objects)...\n",
		object_size, target_total, max_objects);

	for (i = 0; i < max_objects; i++) {
		mem = iommu_debug_mem_alloc_sgt(udev, object_size, SZ_4K);
		if (IS_ERR_OR_NULL(mem)) {
			seq_printf(s, "Allocation failed at object %d\n", i);
			ret = -ENOMEM;
			break;
		}

		ret = dma_map_sgtable(udev->dev, &mem->sgt, DMA_BIDIRECTIONAL, 0);
		if (ret) {
			seq_printf(s, "DMA mapping failed at object %d: %d\n", i, ret);
			/* Free the allocated memory since DMA mapping failed */
			iommu_debug_mem_free(udev, mem);
			ret = -ENOMEM;
			break;
		}
		mem->dma_mapped = true;

		allocated_count++;

		/* Print progress every 10 allocations */
		if ((i + 1) % 10 == 0) {
			seq_printf(s, "Allocated and DMA mapped %d objects (%lu MB total)\n",
				   i + 1, ((i + 1UL) * object_size) / SZ_1M);
		}
	}

	if (allocated_count == max_objects) {
		seq_printf(s, "SUCCESS - Allocated and DMA mapped all %d objects (%lu MB total)\n",
			   max_objects, ((unsigned long)max_objects * object_size) / SZ_1M);
	} else {
		seq_printf(s, "PARTIAL SUCCESS - Allocated and DMA mapped %d objects (%lu MB total)\n",
			   allocated_count, ((unsigned long)allocated_count * object_size) / SZ_1M);
	}

	/* Create temporary list for objects to free */
	LIST_HEAD(temp_free_list);

	/* Move objects allocated during this test to temporary list */
	mutex_lock(&udev->mem_lock);
	list_splice_init(&udev->mem_list, &temp_free_list);
	mutex_unlock(&udev->mem_lock);

	list_for_each_entry_safe(mem, tmp, &temp_free_list, list)
		iommu_debug_mem_free(udev, mem);

	mutex_unlock(&ddev->state_lock);
	seq_puts(s, "Max limit secure memory test completed\n\n");

	return ret;
}

/**
 * test_iommu_bypass_vms_loop - Test secure memory allocation for a specific VMID and size
 * @s: sequence file for output
 * @ddev: debug device
 * @vmid: secure memory VMID to test
 * @size: size of memory to allocate
 *
 * Returns 0 on success, negative error code on failure
 */
static int test_iommu_bypass_vms_loop(struct seq_file *s,
				      struct iommu_debug_device *ddev,
				      u32 vmid,
				      size_t size)
{
	struct qcom_scm_vmperm vmperm_set[] = {
		{.vmid = vmid, .perm = QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE},
	};
	struct qcom_scm_vmperm vmperm_unset[] = {
		{.vmid = QCOM_SCM_VMID_HLOS, .perm = QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE
		| QCOM_SCM_PERM_EXEC},
	};
	u64 srcvm_set = BIT(QCOM_SCM_VMID_HLOS);
	u64 srcvm_unset = BIT(vmid);
	dma_addr_t dma_handle;
	void *cpu_addr;
	phys_addr_t phys;
	int ret;
	const char *vmid_name;
	const char *size_str;

	/* Get human-readable names */
	switch (vmid) {
	case QCOM_SCM_VMID_ADSP_HEAP:
		vmid_name = "ADSP_HEAP";
		break;
	case QCOM_SCM_VMID_LPASS:
		vmid_name = "LPASS";
		break;
	default:
		vmid_name = "UNKNOWN";
		break;
	}

	switch (size) {
	case SZ_8K:
		size_str = "8K";
		break;
	case SZ_64K:
		size_str = "64K";
		break;
	case SZ_2M:
		size_str = "2M";
		break;
	default:
		size_str = "UNKNOWN";
		break;
	}

	seq_printf(s, "Testing %s allocation for VMID %s (%u): ", size_str, vmid_name, vmid);
	pr_info("Testing %s allocation for VMID %s (%u)\n", size_str, vmid_name, vmid);

	/* Allocate contiguous memory using DMA API */
	cpu_addr = dma_alloc_attrs(ddev->self, size, &dma_handle, GFP_KERNEL, 0);
	if (!cpu_addr) {
		seq_puts(s, "FAILED - dma_alloc_attrs failed\n");
		return -ENOMEM;
	}

	phys = dma_to_phys(ddev->self, dma_handle);
	/* Make memory secure */
	ret = qcom_scm_assign_mem(phys, size, &srcvm_set, vmperm_set, 1);
	if (ret) {
		seq_printf(s, "FAILED - qcom_scm_assign_mem to secure failed: %d\n", ret);
		dma_free_attrs(ddev->self, size, cpu_addr, dma_handle, 0);
		return ret;
	}

	/* Return memory to HLOS */
	ret = qcom_scm_assign_mem(phys, size, &srcvm_unset, vmperm_unset, 1);
	if (ret) {
		seq_printf(s, "FAILED - qcom_scm_assign_mem to HLOS failed: %d\n", ret);
		/* Intentional leak on failure */
		return ret;
	}

	/* Free the memory */
	dma_free_attrs(ddev->self, size, cpu_addr, dma_handle, 0);
	seq_puts(s, "SUCCESS\n");
	return 0;
}

/* Suspect this vmids require 32 bit addresses */
static int test_iommu_bypass_vms(struct seq_file *s, struct iommu_debug_device *ddev,
				u32 *vmids, u32 nr)
{
	size_t sizes[] = {SZ_8K, SZ_64K, SZ_2M};
	int i, j;
	int ret = 0;
	int total_tests = 0;
	int passed_tests = 0;

	seq_puts(s, "Iommu Bypass Secure Memory Test\n");
	seq_puts(s, "========================================\n\n");

	for (i = 0; i < nr; i++) {
		u32 vmid = vmids[i];

		for (j = 0; j < ARRAY_SIZE(sizes); j++) {
			size_t size = sizes[j];

			total_tests++;

			ret = test_iommu_bypass_vms_loop(s, ddev, vmid, size);
			if (!ret)
				passed_tests++;
		}
		seq_puts(s, "\n");
	}

	seq_printf(s, "Test completed - %d/%d tests passed\n", passed_tests, total_tests);

	return total_tests == passed_tests ? 0 : -EINVAL;
}

/* Individual test show functions */
static int iommu_bypass_vms_show(struct seq_file *s, void *ignored)
{
	struct iommu_debug_device *ddev = s->private;
	int ret;
	u32 vmids[] = {QCOM_SCM_VMID_ADSP_HEAP, QCOM_SCM_VMID_LPASS};

	ret = test_iommu_bypass_vms(s, ddev, vmids, ARRAY_SIZE(vmids));
	seq_printf(s, "\nTest result: %s\n", ret == 0 ? "PASSED" : "FAILED");
	return 0;
}
/* Individual test file operations */
static int iommu_bypass_vms_open(struct inode *inode, struct file *file)
{
	return single_open(file, iommu_bypass_vms_show, inode->i_private);
}

static const struct file_operations iommu_bypass_vms_fops = {
	.open	 = iommu_bypass_vms_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

static int large_num_objects_show(struct seq_file *s, void *ignored)
{
	struct iommu_debug_device *ddev = s->private;
	int ret;

	ret = test_large_num_objects(s, ddev);
	seq_printf(s, "Test result: %s\n", ret == 0 ? "PASSED" : "FAILED");

	return 0;
}

static int large_num_objects_open(struct inode *inode, struct file *file)
{
	return single_open(file, large_num_objects_show, inode->i_private);
}

static const struct file_operations large_num_objects_fops = {
	.open	 = large_num_objects_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

static int max_limit_secure_memory_show(struct seq_file *s, void *ignored)
{
	struct iommu_debug_device *ddev = s->private;
	int ret;

	ret = test_max_limit_secure_memory(s, ddev);
	seq_printf(s, "Test result: %s\n", ret == 0 ? "PASSED" : "FAILED");

	return 0;
}

static int max_limit_secure_memory_open(struct inode *inode, struct file *file)
{
	return single_open(file, max_limit_secure_memory_show, inode->i_private);
}

static const struct file_operations max_limit_secure_memory_fops = {
	.open	 = max_limit_secure_memory_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

void iommu_debug_debugfs_setup_dpd(struct iommu_debug_device *ddev)
{
	struct dentry *dpd_proxy2_dir;

	/* Create dpd-proxy2 directory */
	dpd_proxy2_dir = debugfs_create_dir("dpd-proxy2", ddev->root_dir);
	if (IS_ERR_OR_NULL(dpd_proxy2_dir))
		return;

	/* Create individual test files within the directory */
	debugfs_create_file("iommu_bypass_vms", 0400, dpd_proxy2_dir, ddev, &iommu_bypass_vms_fops);
	debugfs_create_file("max_limit_secure_memory", 0400, dpd_proxy2_dir, ddev,
			&max_limit_secure_memory_fops);
	debugfs_create_file("large_num_objects", 0400, dpd_proxy2_dir, ddev,
				&large_num_objects_fops);
}
