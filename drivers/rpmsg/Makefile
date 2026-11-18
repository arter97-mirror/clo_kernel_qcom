# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)

obj-$(CONFIG_QCOM_GLINK_PKT) += glink_pkt.o

obj-$(CONFIG_RPMSG_QCOM_GLINK) += qcom_glink.o
qcom_glink-objs += qcom_glink_memshare.o qcom_glink_native.o qcom_glink_ssr.o

obj-$(CONFIG_RPMSG_QCOM_GLINK_SMEM) += qcom_glink_smem.o

obj-$(CONFIG_RPMSG_QCOM_GLINK_SPSS) += qcom_glink_spss.o

obj-$(CONFIG_RPMSG_QCOM_SMD) += qcom_smd.o

obj-$(CONFIG_RPMSG_QCOM_GLINK_CMA) += qcom_glink_cma.o
qcom_glink_cma-objs += virtio_glink_cma.o qcom_glink_cma_core.o

obj-$(CONFIG_RPMSG_QCOM_GLINK_RPM) += qcom_glink_rpm.o

obj-$(CONFIG_RPMSG_QCOM_GLINK_SLATECOM) += qcom_glink_slatecom.o

obj-$(CONFIG_MSM_RPM_SMD) += rpm-smd.o

obj-$(CONFIG_MSM_RPM_SMD) += rpm_dump.o

