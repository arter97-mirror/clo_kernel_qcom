# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)

obj-$(CONFIG_ARM_SMMU) += arm_smmu.o
arm_smmu-objs += arm-smmu-impl.o arm-smmu.o
arm_smmu-$(CONFIG_ARM_SMMU_NVIDIA) += arm-smmu-nvidia.o
arm_smmu-$(CONFIG_ARM_SMMU_QCOM) += arm-smmu-qcom.o arm-smmu-qcom-pm.o
arm_smmu-$(CONFIG_ARM_SMMU_QCOM_DEBUG) += arm-smmu-qcom-debug.o

