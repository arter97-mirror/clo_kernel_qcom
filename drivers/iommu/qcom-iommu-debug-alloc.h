/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __DRIVERS_IOMMU_QCOM_IOMMU_DEBUG_ALLOC_H__
#define __DRIVERS_IOMMU_QCOM_IOMMU_DEBUG_ALLOC_H__

#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/types.h>

struct iommu_debug_allocator_ops;

enum iommu_debug_mem_type {
	IOMMU_DEBUG_MEM_CONTIG,
	IOMMU_DEBUG_MEM_SGT,
};

struct iommu_debug_mem {
	struct list_head list;
	const struct iommu_debug_allocator_ops *ops;
	size_t size;
	struct sg_table sgt;
	struct page *page;
	enum iommu_debug_mem_type type;
	dma_addr_t dma_addr;	/* DMA address for mapped memory */
	bool dma_mapped;	/* Whether memory is DMA mapped */
};

struct iommu_debug_usecase_device {
	struct device *dev;
	/* Protects memory-related fields*/
	struct mutex mem_lock;
	struct list_head mem_list;
	u32 nr_mem_regions;
	size_t total_memory;
	const struct iommu_debug_allocator_ops *ops;
};

struct iommu_debug_allocator_ops {
	int (*alloc_contig)(struct iommu_debug_usecase_device *dev,
			struct iommu_debug_mem *mem, size_t size);
	void (*free_contig)(struct iommu_debug_usecase_device *dev, struct iommu_debug_mem *mem);
	int (*alloc_sgt)(struct iommu_debug_usecase_device *dev, struct iommu_debug_mem *mem,
			size_t size, size_t chunk_size);
	void (*free_sgt)(struct iommu_debug_usecase_device *dev, struct iommu_debug_mem *mem);
};

/* Function declarations */
struct iommu_debug_mem *iommu_debug_mem_alloc_contig(struct iommu_debug_usecase_device *dev,
						  size_t size);
struct iommu_debug_mem *iommu_debug_mem_alloc_sgt(struct iommu_debug_usecase_device *dev,
						  size_t size, size_t chunk_size);
void iommu_debug_mem_free(struct iommu_debug_usecase_device *dev, struct iommu_debug_mem *mem);

extern const struct iommu_debug_allocator_ops standard_allocator_ops;
extern const struct iommu_debug_allocator_ops secure_allocator_ops;

#endif /* __DRIVERS_IOMMU_QCOM_IOMMU_DEBUG_ALLOC_H__ */
