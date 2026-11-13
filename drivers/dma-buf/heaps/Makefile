# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)

obj-$(CONFIG_DMABUF_HEAPS_SYSTEM) += system_heap.o

obj-$(CONFIG_QCOM_DMABUF_HEAPS) += qcom_dma_heaps.o
qcom_dma_heaps-objs += qcom_dma_heap.o qcom_dt_parser.o qcom_sg_ops.o
qcom_dma_heaps-$(CONFIG_QCOM_DMABUF_HEAPS_SYSTEM) += qcom_system_heap.o qcom_dynamic_page_pool.o
qcom_dma_heaps-$(CONFIG_QCOM_DMABUF_HEAPS_SYSTEM_SECURE) += qcom_secure_system_heap.o qcom_dynamic_page_pool.o qcom_dma_heap_secure_utils.o
qcom_dma_heaps-$(CONFIG_QCOM_DMABUF_HEAPS_CMA) += qcom_cma_heap.o
qcom_dma_heaps-$(CONFIG_QCOM_DMABUF_HEAPS_CARVEOUT) += qcom_carveout_heap.o qcom_dma_heap_secure_utils.o
qcom_dma_heaps-$(CONFIG_QCOM_DMABUF_HEAPS_UBWCP) += qcom_ubwcp_heap.o
qcom_dma_heaps-$(CONFIG_QCOM_DMABUF_HEAPS_TVM_CARVEOUT) += qcom_tvm_carveout_heap.o
qcom_dma_heaps-$(CONFIG_QCOM_DMABUF_HEAPS_SYSTEM_MOVABLE) += qcom_system_movable_heap.o

obj-$(CONFIG_QCOM_DMABUF_HEAPS_DEFERRED_FREE) += deferred-free-helper.o

obj-$(CONFIG_QCOM_NAMED_DMABUF) += named_dmabuf.o
