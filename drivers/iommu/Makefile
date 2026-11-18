# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)

# Subdirectories
obj-y += arm/arm-smmu-v3/
obj-y += arm/arm-smmu/

obj-$(CONFIG_QTI_IOMMU_SUPPORT) += iommu-logger.o

obj-$(CONFIG_QCOM_LAZY_MAPPING) += msm_dma_iommu_mapping.o

obj-$(CONFIG_QCOM_IOMMU_DEBUG) += qcom_iommu_debug.o
qcom_iommu_debug-objs += qcom-iommu-debug-user.o qcom-iommu-debug.o qcom-iommu-debug-alloc.o

obj-$(CONFIG_QCOM_IOMMU_UTIL) += qcom_iommu_util.o
qcom_iommu_util-objs += qcom-iommu-util.o
qcom_iommu_util-$(CONFIG_IOMMU_IO_PGTABLE_FAST) += qcom-dma-iommu-generic.o io-pgtable-fast.o dma-mapping-fast.o
qcom_iommu_util-$(CONFIG_IOMMU_IO_PGTABLE_LPAE) += qcom-io-pgtable-arm.o qcom-io-pgtable-alloc.o

obj-$(CONFIG_VIRTIO_IOMMU) += virtio-iommu.o

obj-$(CONFIG_QCOM_DPD_PROXY) += qcom_dpd_proxy_iommu.o

obj-$(CONFIG_QCOM_DPD_PROXY) += qcom_dpd_proxy.o

