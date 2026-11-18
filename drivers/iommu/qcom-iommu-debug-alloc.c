// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#define pr_fmt(fmt) "iommu-debug-alloc: %s: " fmt, __func__

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <soc/qcom/secure_buffer.h>
#include "qcom-iommu-debug.h"

/**
 * iommu_debug_alloc_phoney - Fake allocate contiguous memory
 * @dev: Usecase device
 * @mem: Pre-allocated memory object to populate
 * @size: Size to allocate
 *
 * Returns 0 on success, negative error code on failure
 * Returns a fake page. This is useful if a large contiguous
 * memory region is desired and for maintaining identical
 * behavior to legacy systems.
 */
static int iommu_debug_alloc_phoney(struct iommu_debug_usecase_device *dev,
				struct iommu_debug_mem *mem, size_t size)
{
	phys_addr_t phys = 0x80000000;

	mem->page = phys_to_page(phys);
	return 0;
}

static void iommu_debug_free_phoney(struct iommu_debug_usecase_device *dev,
				struct iommu_debug_mem *mem)
{
}

/**
 * iommu_debug_alloc_contig_impl - Allocate contiguous memory
 * @dev: Usecase device
 * @mem: Pre-allocated memory object to populate
 * @size: Size to allocate
 *
 * Returns 0 on success, negative error code on failure
 */
static int iommu_debug_alloc_contig_impl(struct iommu_debug_usecase_device *dev,
					struct iommu_debug_mem *mem, size_t size)
{
	struct page *page;
	int order;

	size = PAGE_ALIGN(size);

	order = get_order(size);
	if (order > MAX_PAGE_ORDER) {
		dev_err(dev->dev, "Size too large for contiguous allocation\n");
		return -ENOMEM;
	}

	/* Allocate contiguous pages */
	page = alloc_pages(GFP_KERNEL, order);
	if (!page) {
		dev_err(dev->dev, "Failed to allocate %zu bytes\n", size);
		return -ENOMEM;
	}

	mem->page = page;
	dev_dbg(dev->dev, "Allocated contiguous memory: %zu bytes\n", size);
	return 0;
}

/**
 * iommu_debug_free_contig_impl - Free contiguous memory
 * @dev: Usecase device
 * @mem: Memory object to free
 */
static void iommu_debug_free_contig_impl(struct iommu_debug_usecase_device *dev,
					struct iommu_debug_mem *mem)
{
	if (mem->type != IOMMU_DEBUG_MEM_CONTIG) {
		dev_err(dev->dev, "Memory type mismatch\n");
		return;
	}

	/* Free the contiguous pages */
	if (mem->page) {
		__free_pages(mem->page, get_order(mem->size));
		mem->page = NULL;
	}

	dev_dbg(dev->dev, "Freed contiguous memory: %zu bytes\n", mem->size);
}

/**
 * iommu_debug_alloc_sgt_impl - Allocate scatter-gather memory
 * @dev: Usecase device
 * @mem: Pre-allocated memory object to populate
 * @size: Total size to allocate
 * @chunk_size: Size of each chunk
 *
 * Returns 0 on success, negative error code on failure
 */
static int iommu_debug_alloc_sgt_impl(struct iommu_debug_usecase_device *dev,
				struct iommu_debug_mem *mem,
				size_t size, size_t chunk_size)
{
	struct sg_table *table;
	struct scatterlist *sg;
	struct page *page;
	unsigned int nents;
	int i, j, ret, order;

	size = PAGE_ALIGN(size);
	chunk_size = PAGE_ALIGN(chunk_size);

	if (!size || !chunk_size) {
		dev_err(dev->dev, "Invalid size parameters\n");
		return -EINVAL;
	}

	if (chunk_size > size) {
		dev_err(dev->dev, "Chunk size cannot be larger than total size\n");
		return -EINVAL;
	}

	/* Calculate number of entries needed */
	nents = DIV_ROUND_UP(size, chunk_size);

	order = get_order(chunk_size);
	if (order > MAX_PAGE_ORDER) {
		dev_err(dev->dev, "Chunk size too large for sg table\n");
		return -ENOMEM;
	}

	table = &mem->sgt;
	ret = sg_alloc_table(table, nents, GFP_KERNEL);
	if (ret) {
		dev_err(dev->dev, "Failed to allocate sg table\n");
		return -ENOMEM;
	}

	for_each_sgtable_sg(table, sg, i) {
		page = alloc_pages(GFP_KERNEL, order);
		if (!page) {
			dev_err(dev->dev, "Failed to allocate page for chunk %d\n", i);
			goto free_pages;
		}
		sg_set_page(sg, page, chunk_size, 0);
	}

	dev_dbg(dev->dev, "Allocated scatter-gather memory: %zu bytes in %zu byte chunks\n",
		size, chunk_size);
	return 0;

free_pages:
	for_each_sg(table->sgl, sg, i, j) {
		if (sg_page(sg))
			__free_pages(sg_page(sg), get_order(sg->length));
	}
	sg_free_table(table);
	return -ENOMEM;
}

/**
 * iommu_debug_free_sgt_impl - Free scatter-gather memory
 * @dev: Usecase device
 * @mem: Memory object to free
 */
static void iommu_debug_free_sgt_impl(struct iommu_debug_usecase_device *dev,
				struct iommu_debug_mem *mem)
{
	struct sg_table *table;
	struct scatterlist *sg;
	int i;

	if (mem->type != IOMMU_DEBUG_MEM_SGT) {
		dev_err(dev->dev, "Memory type mismatch\n");
		return;
	}

	table = &mem->sgt;
	for_each_sgtable_sg(table, sg, i)
		__free_pages(sg_page(sg), get_order(sg->length));
	sg_free_table(table);

	dev_dbg(dev->dev, "Freed scatter-gather memory: %zu bytes\n", mem->size);
}

/**
 * iommu_debug_alloc_contig_secure - Allocate secure contiguous memory
 * @dev: Usecase device
 * @mem: Pre-allocated memory object to populate
 * @size: Size to allocate
 *
 * Returns 0 on success, negative error code on failure
 */
static int iommu_debug_alloc_contig_secure(struct iommu_debug_usecase_device *dev,
					struct iommu_debug_mem *mem, size_t size)
{
	struct qcom_scm_vmperm vmperm_set[] = {
		{.vmid = QCOM_SCM_VMID_CP_PIXEL, .perm = QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE},
	};
	u64 srcvm = BIT(QCOM_SCM_VMID_HLOS);
	phys_addr_t phys;
	int ret;

	/* First allocate memory using standard implementation */
	ret = iommu_debug_alloc_contig_impl(dev, mem, size);
	if (ret) {
		dev_err(dev->dev, "Failed to allocate contiguous memory for secure allocation\n");
		return ret;
	}

	/* Get physical address */
	phys = page_to_phys(mem->page);

	/* Make memory secure using qcom_scm_assign_mem */
	ret = qcom_scm_assign_mem(phys, size, &srcvm, vmperm_set, 1);
	if (ret) {
		dev_err(dev->dev, "Failed to make contiguous memory secure: %d\n", ret);
		/* Clean up allocated memory */
		iommu_debug_free_contig_impl(dev, mem);
		return ret;
	}

	dev_dbg(dev->dev, "Allocated secure contiguous memory: %zu bytes at %pa\n", size, &phys);
	return 0;
}

/**
 * iommu_debug_free_contig_secure - Free secure contiguous memory
 * @dev: Usecase device
 * @mem: Memory object to free
 */
static void iommu_debug_free_contig_secure(struct iommu_debug_usecase_device *dev,
					struct iommu_debug_mem *mem)
{
	struct qcom_scm_vmperm vmperm_unset[] = {
		{.vmid = QCOM_SCM_VMID_HLOS, .perm = QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE
		| QCOM_SCM_PERM_EXEC},
	};
	u64 srcvm = BIT(QCOM_SCM_VMID_CP_PIXEL);
	phys_addr_t phys;
	int ret;

	if (mem->type != IOMMU_DEBUG_MEM_CONTIG) {
		dev_err(dev->dev, "Memory type mismatch\n");
		return;
	}

	if (!mem->page) {
		dev_err(dev->dev, "No page to free\n");
		return;
	}

	/* Get physical address */
	phys = page_to_phys(mem->page);

	/* Return memory to HLOS */
	ret = qcom_scm_assign_mem(phys, mem->size, &srcvm, vmperm_unset, 1);
	if (ret) {
		dev_err(dev->dev, "Failed to return secure memory to HLOS: %d\n", ret);
		/* Continue with freeing anyway to avoid memory leak */
	}

	/* Free the memory using standard implementation */
	iommu_debug_free_contig_impl(dev, mem);

	dev_dbg(dev->dev, "Freed secure contiguous memory: %zu bytes\n", mem->size);
}

/**
 * iommu_debug_alloc_sgt_secure - Allocate secure scatter-gather memory
 * @dev: Usecase device
 * @mem: Pre-allocated memory object to populate
 * @size: Total size to allocate
 * @chunk_size: Size of each chunk
 *
 * Returns 0 on success, negative error code on failure
 */
static int iommu_debug_alloc_sgt_secure(struct iommu_debug_usecase_device *dev,
					struct iommu_debug_mem *mem,
					size_t size, size_t chunk_size)
{
	int vmids_set[] = {QCOM_SCM_VMID_CP_PIXEL};
	int perms_set[] = {QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE};
	int vmids_unset[] = {QCOM_SCM_VMID_HLOS};
	int ret;

	/* First allocate memory using standard implementation */
	ret = iommu_debug_alloc_sgt_impl(dev, mem, size, chunk_size);
	if (ret) {
		dev_err(dev->dev, "Failed to allocate scatter-gather memory for secure allocation\n");
		return ret;
	}

	/* Make memory secure using hyp_assign_table */
	ret = hyp_assign_table(&mem->sgt, vmids_unset, 1, vmids_set, perms_set, 1);
	if (ret) {
		dev_err(dev->dev, "Failed to make scatter-gather memory secure: %d\n", ret);
		/* Clean up allocated memory */
		iommu_debug_free_sgt_impl(dev, mem);
		return ret;
	}

	dev_dbg(dev->dev, "Allocated secure scatter-gather memory: %zu bytes in %zu byte chunks\n",
		size, chunk_size);
	return 0;
}

/**
 * iommu_debug_free_sgt_secure - Free secure scatter-gather memory
 * @dev: Usecase device
 * @mem: Memory object to free
 */
static void iommu_debug_free_sgt_secure(struct iommu_debug_usecase_device *dev,
					struct iommu_debug_mem *mem)
{
	int vmids_set[] = {QCOM_SCM_VMID_CP_PIXEL};
	int vmids_unset[] = {QCOM_SCM_VMID_HLOS};
	int perms_unset[] = {QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE | QCOM_SCM_PERM_EXEC};
	int ret;

	if (mem->type != IOMMU_DEBUG_MEM_SGT) {
		dev_err(dev->dev, "Memory type mismatch\n");
		return;
	}

	/* Return memory to HLOS */
	ret = hyp_assign_table(&mem->sgt, vmids_set, 1, vmids_unset, perms_unset, 1);
	if (ret) {
		dev_err(dev->dev,
			"Failed to return secure scatter-gather memory to HLOS: %d\n", ret);
		/* Continue with freeing anyway to avoid memory leak */
	}

	/* Free the memory using standard implementation */
	iommu_debug_free_sgt_impl(dev, mem);

	dev_dbg(dev->dev, "Freed secure scatter-gather memory: %zu bytes\n", mem->size);
}

/* Default allocator operations */
static const struct iommu_debug_allocator_ops default_allocator_ops = {
	.alloc_contig = iommu_debug_alloc_phoney,
	.free_contig = iommu_debug_free_phoney,
	.alloc_sgt = iommu_debug_alloc_sgt_impl,
	.free_sgt = iommu_debug_free_sgt_impl,
};

const struct iommu_debug_allocator_ops standard_allocator_ops = {
	.alloc_contig = iommu_debug_alloc_contig_impl,
	.free_contig = iommu_debug_free_contig_impl,
	.alloc_sgt = iommu_debug_alloc_sgt_impl,
	.free_sgt = iommu_debug_free_sgt_impl,
};

const struct iommu_debug_allocator_ops secure_allocator_ops = {
	.alloc_contig = iommu_debug_alloc_contig_secure,
	.free_contig = iommu_debug_free_contig_secure,
	.alloc_sgt = iommu_debug_alloc_sgt_secure,
	.free_sgt = iommu_debug_free_sgt_secure,
};

/**
 * iommu_debug_mem_alloc_contig - Allocate physical contiguous memory
 * @dev: Usecase device
 * @size: Size to allocate
 *
 * Returns allocated memory object or NULL on failure
 */
struct iommu_debug_mem *iommu_debug_mem_alloc_contig(struct iommu_debug_usecase_device *dev,
						size_t size)
{
	struct iommu_debug_mem *mem;
	int ret;

	size = PAGE_ALIGN(size);
	if (!size) {
		dev_err(dev->dev, "Invalid size\n");
		return NULL;
	}

	/* Allocate memory object */
	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return ERR_PTR(-ENOMEM);

	/* Initialize common fields */
	INIT_LIST_HEAD(&mem->list);
	mem->ops = dev->ops ? dev->ops : &default_allocator_ops;
	mem->size = size;
	mem->type = IOMMU_DEBUG_MEM_CONTIG;
	mem->dma_addr = DMA_MAPPING_ERROR;
	mem->dma_mapped = false;

	/* Call allocator implementation */
	ret = mem->ops->alloc_contig(dev, mem, size);
	if (ret) {
		dev_err(dev->dev, "Failed to allocate contiguous memory\n");
		kfree(mem);
		return NULL;
	}

	/* Add to device's memory list */
	mutex_lock(&dev->mem_lock);
	list_add_tail(&mem->list, &dev->mem_list);
	dev->nr_mem_regions++;
	dev->total_memory += size;
	mutex_unlock(&dev->mem_lock);

	return mem;
}

/**
 * iommu_debug_mem_alloc_sgt - Allocate scatter-gather memory
 * @dev: Usecase device
 * @size: Total size to allocate
 * @chunk_size: Size of each chunk
 *
 * Returns allocated memory object or NULL on failure
 */
struct iommu_debug_mem *iommu_debug_mem_alloc_sgt(struct iommu_debug_usecase_device *dev,
						  size_t size, size_t chunk_size)
{
	struct iommu_debug_mem *mem;
	int ret;

	size = PAGE_ALIGN(size);
	chunk_size = PAGE_ALIGN(chunk_size);

	/* Allocate memory object */
	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return ERR_PTR(-ENOMEM);

	/* Initialize common fields */
	INIT_LIST_HEAD(&mem->list);
	mem->ops = dev->ops ? dev->ops : &default_allocator_ops;
	mem->size = size;
	mem->type = IOMMU_DEBUG_MEM_SGT;
	mem->dma_addr = DMA_MAPPING_ERROR;
	mem->dma_mapped = false;

	/* Call allocator implementation */
	ret = mem->ops->alloc_sgt(dev, mem, size, chunk_size);
	if (ret) {
		dev_err(dev->dev, "Failed to allocate scatter-gather memory\n");
		kfree(mem);
		return NULL;
	}

	/* Add to device's memory list */
	mutex_lock(&dev->mem_lock);
	list_add_tail(&mem->list, &dev->mem_list);
	dev->nr_mem_regions++;
	dev->total_memory += size;
	mutex_unlock(&dev->mem_lock);

	return mem;
}

/**
 * iommu_debug_mem_free - Free allocated memory
 * @dev: Usecase device
 * @mem: Memory object to free
 */
void iommu_debug_mem_free(struct iommu_debug_usecase_device *dev, struct iommu_debug_mem *mem)
{
	/* Remove from device's memory list */
	mutex_lock(&dev->mem_lock);
	list_del(&mem->list);
	dev->nr_mem_regions--;
	dev->total_memory -= mem->size;
	mutex_unlock(&dev->mem_lock);

	/* Unmap DMA mapping if it exists */
	if (mem->dma_mapped) {
		switch (mem->type) {
		case IOMMU_DEBUG_MEM_CONTIG:
			dma_unmap_page(dev->dev, mem->dma_addr, mem->size, DMA_BIDIRECTIONAL);
			break;
		case IOMMU_DEBUG_MEM_SGT:
			dma_unmap_sgtable(dev->dev, &mem->sgt, DMA_BIDIRECTIONAL, 0);
			break;
		default:
			dev_err(dev->dev, "Unknown memory type for DMA unmapping: %d\n", mem->type);
			break;
		}
		mem->dma_mapped = false;
		mem->dma_addr = DMA_MAPPING_ERROR;
	}

	/* Call appropriate free function */
	switch (mem->type) {
	case IOMMU_DEBUG_MEM_CONTIG:
		mem->ops->free_contig(dev, mem);
		break;
	case IOMMU_DEBUG_MEM_SGT:
		mem->ops->free_sgt(dev, mem);
		break;
	default:
		dev_err(dev->dev, "Unknown memory type: %d\n", mem->type);
		break;
	}

	/* Free the memory object itself */
	kfree(mem);
}
