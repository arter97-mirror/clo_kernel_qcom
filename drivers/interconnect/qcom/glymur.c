// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 */

#include <linux/device.h>
#include <linux/interconnect.h>
#include <linux/interconnect-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <dt-bindings/interconnect/qcom,glymur-rpmh.h>

#include "icc-rpmh.h"
#include "qnoc-qos.h"

enum {
	VOTER_IDX_HLOS,
};

static const struct regmap_config icc_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
};

static struct qcom_icc_qosbox qxm_crypto_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xb000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qxm_crypto = {
	.name = "qxm_crypto",
	.id = MASTER_CRYPTO,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qxm_crypto_qos,
	.num_links = 1,
	.links = { SLAVE_A1NOC_SNOC },
};

static struct qcom_icc_qosbox qxm_soccp_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xe000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qxm_soccp = {
	.name = "qxm_soccp",
	.id = MASTER_SOCCP_PROC,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qxm_soccp_qos,
	.num_links = 1,
	.links = { SLAVE_A1NOC_SNOC },
};

static struct qcom_icc_qosbox xm_qdss_etr_0_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xc000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_qdss_etr_0 = {
	.name = "xm_qdss_etr_0",
	.id = MASTER_QDSS_ETR,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_qdss_etr_0_qos,
	.num_links = 1,
	.links = { SLAVE_A1NOC_SNOC },
};

static struct qcom_icc_qosbox xm_qdss_etr_1_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xd000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_qdss_etr_1 = {
	.name = "xm_qdss_etr_1",
	.id = MASTER_QDSS_ETR_1,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_qdss_etr_1_qos,
	.num_links = 1,
	.links = { SLAVE_A1NOC_SNOC },
};

static struct qcom_icc_qosbox xm_ufs_mem_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xa000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_ufs_mem = {
	.name = "xm_ufs_mem",
	.id = MASTER_UFS_MEM,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_ufs_mem_qos,
	.num_links = 1,
	.links = { SLAVE_A2NOC_SNOC },
};

static struct qcom_icc_qosbox xm_usb3_2_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x8000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_usb3_2 = {
	.name = "xm_usb3_2",
	.id = MASTER_USB3_2,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_usb3_2_qos,
	.num_links = 1,
	.links = { SLAVE_A2NOC_SNOC },
};

static struct qcom_icc_qosbox xm_usb4_2_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x9000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_usb4_2 = {
	.name = "xm_usb4_2",
	.id = MASTER_USB4_2,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_usb4_2_qos,
	.num_links = 1,
	.links = { SLAVE_A2NOC_SNOC },
};

static struct qcom_icc_qosbox qhm_qspi_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x10000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qhm_qspi = {
	.name = "qhm_qspi",
	.id = MASTER_QSPI_0,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qhm_qspi_qos,
	.num_links = 1,
	.links = { SLAVE_A3NOC_SNOC },
};

static struct qcom_icc_qosbox qhm_qup0_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x11000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qhm_qup0 = {
	.name = "qhm_qup0",
	.id = MASTER_QUP_0,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qhm_qup0_qos,
	.num_links = 1,
	.links = { SLAVE_A3NOC_SNOC },
};

static struct qcom_icc_qosbox qhm_qup1_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x12000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qhm_qup1 = {
	.name = "qhm_qup1",
	.id = MASTER_QUP_1,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qhm_qup1_qos,
	.num_links = 1,
	.links = { SLAVE_A3NOC_SNOC },
};

static struct qcom_icc_qosbox qhm_qup2_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x13000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qhm_qup2 = {
	.name = "qhm_qup2",
	.id = MASTER_QUP_2,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qhm_qup2_qos,
	.num_links = 1,
	.links = { SLAVE_A3NOC_SNOC },
};

static struct qcom_icc_node qxm_sp = {
	.name = "qxm_sp",
	.id = MASTER_SP,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_A3NOC_SNOC },
};

static struct qcom_icc_qosbox xm_sdc2_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x18000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_sdc2 = {
	.name = "xm_sdc2",
	.id = MASTER_SDCC_2,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_sdc2_qos,
	.num_links = 1,
	.links = { SLAVE_A3NOC_SNOC },
};

static struct qcom_icc_qosbox xm_sdc4_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x14000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_sdc4 = {
	.name = "xm_sdc4",
	.id = MASTER_SDCC_4,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_sdc4_qos,
	.num_links = 1,
	.links = { SLAVE_A3NOC_SNOC },
};

static struct qcom_icc_qosbox xm_usb2_0_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x15000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_usb2_0 = {
	.name = "xm_usb2_0",
	.id = MASTER_USB2,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_usb2_0_qos,
	.num_links = 1,
	.links = { SLAVE_A3NOC_SNOC },
};

static struct qcom_icc_qosbox xm_usb3_mp_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x16000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_usb3_mp = {
	.name = "xm_usb3_mp",
	.id = MASTER_USB3_MP,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_usb3_mp_qos,
	.num_links = 1,
	.links = { SLAVE_A3NOC_SNOC },
};

static struct qcom_icc_qosbox xm_usb3_0_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xa000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_usb3_0 = {
	.name = "xm_usb3_0",
	.id = MASTER_USB3_0,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_usb3_0_qos,
	.num_links = 1,
	.links = { SLAVE_A4NOC_HSCNOC },
};

static struct qcom_icc_qosbox xm_usb3_1_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xb000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_usb3_1 = {
	.name = "xm_usb3_1",
	.id = MASTER_USB3_1,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_usb3_1_qos,
	.num_links = 1,
	.links = { SLAVE_A4NOC_HSCNOC },
};

static struct qcom_icc_qosbox xm_usb4_0_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xc000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_usb4_0 = {
	.name = "xm_usb4_0",
	.id = MASTER_USB4_0,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_usb4_0_qos,
	.num_links = 1,
	.links = { SLAVE_A4NOC_HSCNOC },
};

static struct qcom_icc_qosbox xm_usb4_1_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xd000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node xm_usb4_1 = {
	.name = "xm_usb4_1",
	.id = MASTER_USB4_1,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_usb4_1_qos,
	.num_links = 1,
	.links = { SLAVE_A4NOC_HSCNOC },
};

static struct qcom_icc_node qup0_core_master = {
	.name = "qup0_core_master",
	.id = MASTER_QUP_CORE_0,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_QUP_CORE_0 },
};

static struct qcom_icc_node qup1_core_master = {
	.name = "qup1_core_master",
	.id = MASTER_QUP_CORE_1,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_QUP_CORE_1 },
};

static struct qcom_icc_node qup2_core_master = {
	.name = "qup2_core_master",
	.id = MASTER_QUP_CORE_2,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_QUP_CORE_2 },
};

static struct qcom_icc_node qsm_cfg = {
	.name = "qsm_cfg",
	.id = MASTER_CNOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 51,
	.links = { SLAVE_AHB2PHY_SOUTH, SLAVE_AHB2PHY_NORTH,
		   SLAVE_AHB2PHY_2, SLAVE_AHB2PHY_3,
		   SLAVE_AV1_ENC_CFG, SLAVE_CAMERA_CFG,
		   SLAVE_CLK_CTL, SLAVE_CRYPTO_0_CFG,
		   SLAVE_DISPLAY_CFG, SLAVE_GFX3D_CFG,
		   SLAVE_IMEM_CFG, SLAVE_PCIE_0_CFG,
		   SLAVE_PCIE_1_CFG, SLAVE_PCIE_2_CFG,
		   SLAVE_PCIE_3A_CFG, SLAVE_PCIE_3B_CFG,
		   SLAVE_PCIE_4_CFG, SLAVE_PCIE_5_CFG,
		   SLAVE_PCIE_6_CFG, SLAVE_PCIE_RSCC,
		   SLAVE_PDM, SLAVE_PRNG,
		   SLAVE_QDSS_CFG, SLAVE_QSPI_0,
		   SLAVE_QUP_0, SLAVE_QUP_1,
		   SLAVE_QUP_2, SLAVE_SDCC_2,
		   SLAVE_SDCC_4, SLAVE_SMMUV3_CFG,
		   SLAVE_TCSR, SLAVE_TLMM,
		   SLAVE_UFS_MEM_CFG, SLAVE_USB2,
		   SLAVE_USB3_0, SLAVE_USB3_1,
		   SLAVE_USB3_2, SLAVE_USB3_MP,
		   SLAVE_USB4_0, SLAVE_USB4_1,
		   SLAVE_USB4_2, SLAVE_VENUS_CFG,
		   SLAVE_CNOC_PCIE_SLAVE_EAST_CFG, SLAVE_CNOC_PCIE_SLAVE_WEST_CFG,
		   SLAVE_LPASS_QTB_CFG, SLAVE_CNOC_MNOC_CFG,
		   SLAVE_NSP_QTB_CFG, SLAVE_PCIE_EAST_ANOC_CFG,
		   SLAVE_PCIE_WEST_ANOC_CFG, SLAVE_QDSS_STM,
		   SLAVE_TCU },
};

static struct qcom_icc_node qnm_hscnoc_cnoc = {
	.name = "qnm_hscnoc_cnoc",
	.id = MASTER_HSCNOC_CNOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 8,
	.links = { SLAVE_AOSS, SLAVE_IPC_ROUTER_CFG,
		   SLAVE_SOCCP, SLAVE_TME_CFG,
		   SLAVE_APPSS, SLAVE_CNOC_CFG,
		   SLAVE_BOOT_IMEM, SLAVE_IMEM },
};

static struct qcom_icc_qosbox alm_gpu_tcu_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x933000 },
	.config = &(struct qos_config) {
		.prio = 1,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node alm_gpu_tcu = {
	.name = "alm_gpu_tcu",
	.id = MASTER_GPU_TCU,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &alm_gpu_tcu_qos,
	.num_links = 2,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC },
};

static struct qcom_icc_qosbox alm_pcie_qtc_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x51f000 },
	.config = &(struct qos_config) {
		.prio = 3,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node alm_pcie_qtc = {
	.name = "alm_pcie_qtc",
	.id = MASTER_PCIE_TCU,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &alm_pcie_qtc_qos,
	.num_links = 2,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC },
};

static struct qcom_icc_qosbox alm_sys_tcu_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x51f080 },
	.config = &(struct qos_config) {
		.prio = 6,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node alm_sys_tcu = {
	.name = "alm_sys_tcu",
	.id = MASTER_SYS_TCU,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &alm_sys_tcu_qos,
	.num_links = 2,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC },
};

static struct qcom_icc_node chm_apps = {
	.name = "chm_apps",
	.id = MASTER_APPSS_PROC,
	.channels = 6,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 4,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC,
		   SLAVE_PCIE_EAST, SLAVE_PCIE_WEST },
};

static struct qcom_icc_qosbox qnm_aggre_noc_east_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x934000 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_aggre_noc_east = {
	.name = "qnm_aggre_noc_east",
	.id = MASTER_AGGRE_NOC_EAST,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_aggre_noc_east_qos,
	.num_links = 4,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC,
		   SLAVE_PCIE_EAST, SLAVE_PCIE_WEST },
};

static struct qcom_icc_qosbox qnm_gpu_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 4,
	.offsets = { 0x935000, 0x936000, 0x937000, 0x938000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_gpu = {
	.name = "qnm_gpu",
	.id = MASTER_GFX3D,
	.channels = 4,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_gpu_qos,
	.num_links = 4,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC,
		   SLAVE_PCIE_EAST, SLAVE_PCIE_WEST },
};

static struct qcom_icc_qosbox qnm_lpass_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x939000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node qnm_lpass = {
	.name = "qnm_lpass",
	.id = MASTER_LPASS_GEM_NOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_lpass_qos,
	.num_links = 4,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC,
		   SLAVE_PCIE_EAST, SLAVE_PCIE_WEST },
};

static struct qcom_icc_qosbox qnm_mnoc_hf_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 2,
	.offsets = { 0x721000, 0x721080 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node qnm_mnoc_hf = {
	.name = "qnm_mnoc_hf",
	.id = MASTER_MNOC_HF_MEM_NOC,
	.channels = 2,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_mnoc_hf_qos,
	.num_links = 4,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC,
		   SLAVE_PCIE_EAST, SLAVE_PCIE_WEST },
};

static struct qcom_icc_qosbox qnm_mnoc_sf_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 2,
	.offsets = { 0x721100, 0x721180 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node qnm_mnoc_sf = {
	.name = "qnm_mnoc_sf",
	.id = MASTER_MNOC_SF_MEM_NOC,
	.channels = 2,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_mnoc_sf_qos,
	.num_links = 4,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC,
		   SLAVE_PCIE_EAST, SLAVE_PCIE_WEST },
};

static struct qcom_icc_qosbox qnm_nsp_noc_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 4,
	.offsets = { 0x816000, 0x816080, 0x816100, 0x816180 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_nsp_noc = {
	.name = "qnm_nsp_noc",
	.id = MASTER_COMPUTE_NOC,
	.channels = 4,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_nsp_noc_qos,
	.num_links = 4,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC,
		   SLAVE_PCIE_EAST, SLAVE_PCIE_WEST },
};

static struct qcom_icc_qosbox qnm_pcie_east_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x93a000 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_pcie_east = {
	.name = "qnm_pcie_east",
	.id = MASTER_PCIE_EAST,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_pcie_east_qos,
	.num_links = 2,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC },
};

static struct qcom_icc_qosbox qnm_pcie_west_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x721200 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_pcie_west = {
	.name = "qnm_pcie_west",
	.id = MASTER_PCIE_WEST,
	.channels = 1,
	.buswidth = 64,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_pcie_west_qos,
	.num_links = 2,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC },
};

static struct qcom_icc_qosbox qnm_snoc_sf_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x51f100 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_snoc_sf = {
	.name = "qnm_snoc_sf",
	.id = MASTER_SNOC_SF_MEM_NOC,
	.channels = 1,
	.buswidth = 64,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_snoc_sf_qos,
	.num_links = 4,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC,
		   SLAVE_PCIE_EAST, SLAVE_PCIE_WEST },
};

static struct qcom_icc_node qxm_wlan_q6 = {
	.name = "qxm_wlan_q6",
	.id = MASTER_WLAN_Q6,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 4,
	.links = { SLAVE_HSCNOC_CNOC, SLAVE_LLCC,
		   SLAVE_PCIE_EAST, SLAVE_PCIE_WEST },
};

static struct qcom_icc_qosbox xm_gic_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x33000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node xm_gic = {
	.name = "xm_gic",
	.id = MASTER_GIC,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_gic_qos,
	.num_links = 1,
	.links = { SLAVE_LLCC },
};

static struct qcom_icc_node qnm_lpiaon_noc = {
	.name = "qnm_lpiaon_noc",
	.id = MASTER_LPIAON_NOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_LPASS_GEM_NOC },
};

static struct qcom_icc_node qnm_lpass_lpinoc = {
	.name = "qnm_lpass_lpinoc",
	.id = MASTER_LPASS_LPINOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_LPIAON_NOC_LPASS_AG_NOC },
};

static struct qcom_icc_node qnm_lpinoc_dsp_qns4m = {
	.name = "qnm_lpinoc_dsp_qns4m",
	.id = MASTER_LPASS_PROC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_LPICX_NOC_LPIAON_NOC },
};

static struct qcom_icc_node llcc_mc = {
	.name = "llcc_mc",
	.id = MASTER_LLCC,
	.channels = 12,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_EBI1 },
};

static struct qcom_icc_qosbox qnm_av1_enc_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x30000 },
	.config = &(struct qos_config) {
		.prio = 4,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_av1_enc = {
	.name = "qnm_av1_enc",
	.id = MASTER_AV1_ENC,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_av1_enc_qos,
	.num_links = 1,
	.links = { SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_qosbox qnm_camnoc_hf_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 2,
	.offsets = { 0x29000, 0x2a000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node qnm_camnoc_hf = {
	.name = "qnm_camnoc_hf",
	.id = MASTER_CAMNOC_HF,
	.channels = 2,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_camnoc_hf_qos,
	.num_links = 1,
	.links = { SLAVE_MNOC_HF_MEM_NOC },
};

static struct qcom_icc_qosbox qnm_camnoc_icp_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x2b000 },
	.config = &(struct qos_config) {
		.prio = 4,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_camnoc_icp = {
	.name = "qnm_camnoc_icp",
	.id = MASTER_CAMNOC_ICP,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_camnoc_icp_qos,
	.num_links = 1,
	.links = { SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_qosbox qnm_camnoc_sf_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 2,
	.offsets = { 0x2c000, 0x2d000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node qnm_camnoc_sf = {
	.name = "qnm_camnoc_sf",
	.id = MASTER_CAMNOC_SF,
	.channels = 2,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_camnoc_sf_qos,
	.num_links = 1,
	.links = { SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_qosbox qnm_eva_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x34000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node qnm_eva = {
	.name = "qnm_eva",
	.id = MASTER_EVA,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_eva_qos,
	.num_links = 1,
	.links = { SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_qosbox qnm_mdp_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 2,
	.offsets = { 0x2e000, 0x2f000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node qnm_mdp = {
	.name = "qnm_mdp",
	.id = MASTER_MDP,
	.channels = 2,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_mdp_qos,
	.num_links = 1,
	.links = { SLAVE_MNOC_HF_MEM_NOC },
};

static struct qcom_icc_node qnm_vapss_hcp = {
	.name = "qnm_vapss_hcp",
	.id = MASTER_CDSP_HCP,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_qosbox qnm_video_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 4,
	.offsets = { 0x31000, 0x32000, 0x37000, 0x38000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 1,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node qnm_video = {
	.name = "qnm_video",
	.id = MASTER_VIDEO,
	.channels = 4,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_video_qos,
	.num_links = 1,
	.links = { SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_qosbox qnm_video_cv_cpu_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x33000 },
	.config = &(struct qos_config) {
		.prio = 4,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_video_cv_cpu = {
	.name = "qnm_video_cv_cpu",
	.id = MASTER_VIDEO_CV_PROC,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_video_cv_cpu_qos,
	.num_links = 1,
	.links = { SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_qosbox qnm_video_v_cpu_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x35000 },
	.config = &(struct qos_config) {
		.prio = 4,
		.urg_fwd = 1,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_video_v_cpu = {
	.name = "qnm_video_v_cpu",
	.id = MASTER_VIDEO_V_PROC,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_video_v_cpu_qos,
	.num_links = 1,
	.links = { SLAVE_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_node qsm_mnoc_cfg = {
	.name = "qsm_mnoc_cfg",
	.id = MASTER_CNOC_MNOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_SERVICE_MNOC },
};

static struct qcom_icc_node xm_cpucp = {
	.name = "xm_cpucp",
	.id = MASTER_CPUCP,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 2,
	.links = { SLAVE_NSINOC_SYSTEM_NOC, SLAVE_SERVICE_NSINOC },
};

static struct qcom_icc_node qnm_nsp = {
	.name = "qnm_nsp",
	.id = MASTER_CDSP_PROC,
	.channels = 4,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_NSP0_HSC_NOC },
};

static struct qcom_icc_node xm_mem_sp = {
	.name = "xm_mem_sp",
	.id = MASTER_OOBMSS_SP_PROC,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_OOBMSS_SNOC },
};

static struct qcom_icc_node qsm_pcie_east_anoc_cfg = {
	.name = "qsm_pcie_east_anoc_cfg",
	.id = MASTER_PCIE_EAST_ANOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_SERVICE_PCIE_EAST_AGGRE_NOC },
};

static struct qcom_icc_qosbox xm_pcie_0_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xb000 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 0,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node xm_pcie_0 = {
	.name = "xm_pcie_0",
	.id = MASTER_PCIE_0,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_pcie_0_qos,
	.num_links = 1,
	.links = { SLAVE_PCIE_EAST_MEM_NOC },
};

static struct qcom_icc_qosbox xm_pcie_1_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xc000 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 0,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node xm_pcie_1 = {
	.name = "xm_pcie_1",
	.id = MASTER_PCIE_1,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_pcie_1_qos,
	.num_links = 1,
	.links = { SLAVE_PCIE_EAST_MEM_NOC },
};

static struct qcom_icc_qosbox xm_pcie_5_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xd000 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 0,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node xm_pcie_5 = {
	.name = "xm_pcie_5",
	.id = MASTER_PCIE_5,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_pcie_5_qos,
	.num_links = 1,
	.links = { SLAVE_PCIE_EAST_MEM_NOC },
};

static struct qcom_icc_node qnm_hscnoc_pcie_east = {
	.name = "qnm_hscnoc_pcie_east",
	.id = MASTER_HSCNOC_PCIE_EAST,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 3,
	.links = { SLAVE_PCIE_0, SLAVE_PCIE_1,
		   SLAVE_PCIE_5 },
};

static struct qcom_icc_node qsm_cnoc_pcie_east_slave_cfg = {
	.name = "qsm_cnoc_pcie_east_slave_cfg",
	.id = MASTER_CNOC_PCIE_EAST_SLAVE_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 2,
	.links = { SLAVE_HSCNOC_PCIE_EAST_MS_MPU_CFG, SLAVE_SERVICE_PCIE_EAST },
};

static struct qcom_icc_node qsm_pcie_west_anoc_cfg = {
	.name = "qsm_pcie_west_anoc_cfg",
	.id = MASTER_PCIE_WEST_ANOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_SERVICE_PCIE_WEST_AGGRE_NOC },
};

static struct qcom_icc_qosbox xm_pcie_2_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xd000 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 0,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node xm_pcie_2 = {
	.name = "xm_pcie_2",
	.id = MASTER_PCIE_2,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_pcie_2_qos,
	.num_links = 1,
	.links = { SLAVE_PCIE_WEST_MEM_NOC },
};

static struct qcom_icc_qosbox xm_pcie_3a_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xd200 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 0,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node xm_pcie_3a = {
	.name = "xm_pcie_3a",
	.id = MASTER_PCIE_3A,
	.channels = 1,
	.buswidth = 64,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_pcie_3a_qos,
	.num_links = 1,
	.links = { SLAVE_PCIE_WEST_MEM_NOC },
};

static struct qcom_icc_qosbox xm_pcie_3b_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xd400 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 0,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node xm_pcie_3b = {
	.name = "xm_pcie_3b",
	.id = MASTER_PCIE_3B,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_pcie_3b_qos,
	.num_links = 1,
	.links = { SLAVE_PCIE_WEST_MEM_NOC },
};

static struct qcom_icc_qosbox xm_pcie_4_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xd600 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 0,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node xm_pcie_4 = {
	.name = "xm_pcie_4",
	.id = MASTER_PCIE_4,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_pcie_4_qos,
	.num_links = 1,
	.links = { SLAVE_PCIE_WEST_MEM_NOC },
};

static struct qcom_icc_qosbox xm_pcie_6_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0xd800 },
	.config = &(struct qos_config) {
		.prio = 2,
		.urg_fwd = 0,
		.prio_fwd_disable = 0,
	},
};

static struct qcom_icc_node xm_pcie_6 = {
	.name = "xm_pcie_6",
	.id = MASTER_PCIE_6,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &xm_pcie_6_qos,
	.num_links = 1,
	.links = { SLAVE_PCIE_WEST_MEM_NOC },
};

static struct qcom_icc_node qnm_hscnoc_pcie_west = {
	.name = "qnm_hscnoc_pcie_west",
	.id = MASTER_HSCNOC_PCIE_WEST,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 5,
	.links = { SLAVE_PCIE_2, SLAVE_PCIE_3A,
		   SLAVE_PCIE_3B, SLAVE_PCIE_4,
		   SLAVE_PCIE_6 },
};

static struct qcom_icc_node qsm_cnoc_pcie_west_slave_cfg = {
	.name = "qsm_cnoc_pcie_west_slave_cfg",
	.id = MASTER_CNOC_PCIE_WEST_SLAVE_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 2,
	.links = { SLAVE_HSCNOC_PCIE_WEST_MS_MPU_CFG, SLAVE_SERVICE_PCIE_WEST },
};

static struct qcom_icc_node qnm_aggre1_noc = {
	.name = "qnm_aggre1_noc",
	.id = MASTER_A1NOC_SNOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_SNOC_GEM_NOC_SF },
};

static struct qcom_icc_node qnm_aggre2_noc = {
	.name = "qnm_aggre2_noc",
	.id = MASTER_A2NOC_SNOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_SNOC_GEM_NOC_SF },
};

static struct qcom_icc_node qnm_aggre3_noc = {
	.name = "qnm_aggre3_noc",
	.id = MASTER_A3NOC_SNOC,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { SLAVE_SNOC_GEM_NOC_SF },
};

static struct qcom_icc_qosbox qnm_nsi_noc_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x1c000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_nsi_noc = {
	.name = "qnm_nsi_noc",
	.id = MASTER_NSINOC_SNOC,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_nsi_noc_qos,
	.num_links = 1,
	.links = { SLAVE_SNOC_GEM_NOC_SF },
};

static struct qcom_icc_qosbox qnm_oobmss_qos = {
	.regs = icc_qnoc_qos_regs[ICC_QNOC_QOSGEN_TYPE_RPMH],
	.num_ports = 1,
	.offsets = { 0x1b000 },
	.config = &(struct qos_config) {
		.prio = 0,
		.urg_fwd = 0,
		.prio_fwd_disable = 1,
	},
};

static struct qcom_icc_node qnm_oobmss = {
	.name = "qnm_oobmss",
	.id = MASTER_OOBMSS,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.qosbox = &qnm_oobmss_qos,
	.num_links = 1,
	.links = { SLAVE_SNOC_GEM_NOC_SF },
};

static struct qcom_icc_node qns_a1noc_snoc = {
	.name = "qns_a1noc_snoc",
	.id = SLAVE_A1NOC_SNOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_A1NOC_SNOC },
};

static struct qcom_icc_node qns_a2noc_snoc = {
	.name = "qns_a2noc_snoc",
	.id = SLAVE_A2NOC_SNOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_A2NOC_SNOC },
};

static struct qcom_icc_node qns_a3noc_snoc = {
	.name = "qns_a3noc_snoc",
	.id = SLAVE_A3NOC_SNOC,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_A3NOC_SNOC },
};

static struct qcom_icc_node qns_a4noc_hscnoc = {
	.name = "qns_a4noc_hscnoc",
	.id = SLAVE_A4NOC_HSCNOC,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_AGGRE_NOC_EAST },
};

static struct qcom_icc_node qup0_core_slave = {
	.name = "qup0_core_slave",
	.id = SLAVE_QUP_CORE_0,
	.channels = 1,
	.buswidth = 4,
	.init_peak = INT_MAX,
	.init_avg  = INT_MAX,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qup1_core_slave = {
	.name = "qup1_core_slave",
	.id = SLAVE_QUP_CORE_1,
	.channels = 1,
	.buswidth = 4,
	.init_peak = INT_MAX,
	.init_avg  = INT_MAX,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qup2_core_slave = {
	.name = "qup2_core_slave",
	.id = SLAVE_QUP_CORE_2,
	.channels = 1,
	.buswidth = 4,
	.init_peak = INT_MAX,
	.init_avg  = INT_MAX,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_ahb2phy0 = {
	.name = "qhs_ahb2phy0",
	.id = SLAVE_AHB2PHY_SOUTH,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_ahb2phy1 = {
	.name = "qhs_ahb2phy1",
	.id = SLAVE_AHB2PHY_NORTH,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_ahb2phy2 = {
	.name = "qhs_ahb2phy2",
	.id = SLAVE_AHB2PHY_2,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_ahb2phy3 = {
	.name = "qhs_ahb2phy3",
	.id = SLAVE_AHB2PHY_3,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_av1_enc_cfg = {
	.name = "qhs_av1_enc_cfg",
	.id = SLAVE_AV1_ENC_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_camera_cfg = {
	.name = "qhs_camera_cfg",
	.id = SLAVE_CAMERA_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_clk_ctl = {
	.name = "qhs_clk_ctl",
	.id = SLAVE_CLK_CTL,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_crypto0_cfg = {
	.name = "qhs_crypto0_cfg",
	.id = SLAVE_CRYPTO_0_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_display_cfg = {
	.name = "qhs_display_cfg",
	.id = SLAVE_DISPLAY_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_gpuss_cfg = {
	.name = "qhs_gpuss_cfg",
	.id = SLAVE_GFX3D_CFG,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_imem_cfg = {
	.name = "qhs_imem_cfg",
	.id = SLAVE_IMEM_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_pcie0_cfg = {
	.name = "qhs_pcie0_cfg",
	.id = SLAVE_PCIE_0_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_pcie1_cfg = {
	.name = "qhs_pcie1_cfg",
	.id = SLAVE_PCIE_1_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_pcie2_cfg = {
	.name = "qhs_pcie2_cfg",
	.id = SLAVE_PCIE_2_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_pcie3a_cfg = {
	.name = "qhs_pcie3a_cfg",
	.id = SLAVE_PCIE_3A_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_pcie3b_cfg = {
	.name = "qhs_pcie3b_cfg",
	.id = SLAVE_PCIE_3B_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_pcie4_cfg = {
	.name = "qhs_pcie4_cfg",
	.id = SLAVE_PCIE_4_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_pcie5_cfg = {
	.name = "qhs_pcie5_cfg",
	.id = SLAVE_PCIE_5_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_pcie6_cfg = {
	.name = "qhs_pcie6_cfg",
	.id = SLAVE_PCIE_6_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_pcie_rscc = {
	.name = "qhs_pcie_rscc",
	.id = SLAVE_PCIE_RSCC,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_pdm = {
	.name = "qhs_pdm",
	.id = SLAVE_PDM,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_prng = {
	.name = "qhs_prng",
	.id = SLAVE_PRNG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_qdss_cfg = {
	.name = "qhs_qdss_cfg",
	.id = SLAVE_QDSS_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_qspi = {
	.name = "qhs_qspi",
	.id = SLAVE_QSPI_0,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_qup0 = {
	.name = "qhs_qup0",
	.id = SLAVE_QUP_0,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_qup1 = {
	.name = "qhs_qup1",
	.id = SLAVE_QUP_1,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_qup2 = {
	.name = "qhs_qup2",
	.id = SLAVE_QUP_2,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_sdc2 = {
	.name = "qhs_sdc2",
	.id = SLAVE_SDCC_2,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_sdc4 = {
	.name = "qhs_sdc4",
	.id = SLAVE_SDCC_4,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_smmuv3_cfg = {
	.name = "qhs_smmuv3_cfg",
	.id = SLAVE_SMMUV3_CFG,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_tcsr = {
	.name = "qhs_tcsr",
	.id = SLAVE_TCSR,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_tlmm = {
	.name = "qhs_tlmm",
	.id = SLAVE_TLMM,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_ufs_mem_cfg = {
	.name = "qhs_ufs_mem_cfg",
	.id = SLAVE_UFS_MEM_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_usb2_0_cfg = {
	.name = "qhs_usb2_0_cfg",
	.id = SLAVE_USB2,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_usb3_0_cfg = {
	.name = "qhs_usb3_0_cfg",
	.id = SLAVE_USB3_0,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_usb3_1_cfg = {
	.name = "qhs_usb3_1_cfg",
	.id = SLAVE_USB3_1,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_usb3_2_cfg = {
	.name = "qhs_usb3_2_cfg",
	.id = SLAVE_USB3_2,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_usb3_mp_cfg = {
	.name = "qhs_usb3_mp_cfg",
	.id = SLAVE_USB3_MP,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_usb4_0_cfg = {
	.name = "qhs_usb4_0_cfg",
	.id = SLAVE_USB4_0,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_usb4_1_cfg = {
	.name = "qhs_usb4_1_cfg",
	.id = SLAVE_USB4_1,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_usb4_2_cfg = {
	.name = "qhs_usb4_2_cfg",
	.id = SLAVE_USB4_2,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_venus_cfg = {
	.name = "qhs_venus_cfg",
	.id = SLAVE_VENUS_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qss_cnoc_pcie_slave_east_cfg = {
	.name = "qss_cnoc_pcie_slave_east_cfg",
	.id = SLAVE_CNOC_PCIE_SLAVE_EAST_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_CNOC_PCIE_EAST_SLAVE_CFG },
};

static struct qcom_icc_node qss_cnoc_pcie_slave_west_cfg = {
	.name = "qss_cnoc_pcie_slave_west_cfg",
	.id = SLAVE_CNOC_PCIE_SLAVE_WEST_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_CNOC_PCIE_WEST_SLAVE_CFG },
};

static struct qcom_icc_node qss_lpass_qtb_cfg = {
	.name = "qss_lpass_qtb_cfg",
	.id = SLAVE_LPASS_QTB_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qss_mnoc_cfg = {
	.name = "qss_mnoc_cfg",
	.id = SLAVE_CNOC_MNOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_CNOC_MNOC_CFG },
};

static struct qcom_icc_node qss_nsp_qtb_cfg = {
	.name = "qss_nsp_qtb_cfg",
	.id = SLAVE_NSP_QTB_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qss_pcie_east_anoc_cfg = {
	.name = "qss_pcie_east_anoc_cfg",
	.id = SLAVE_PCIE_EAST_ANOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_PCIE_EAST_ANOC_CFG },
};

static struct qcom_icc_node qss_pcie_west_anoc_cfg = {
	.name = "qss_pcie_west_anoc_cfg",
	.id = SLAVE_PCIE_WEST_ANOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_PCIE_WEST_ANOC_CFG },
};

static struct qcom_icc_node xs_qdss_stm = {
	.name = "xs_qdss_stm",
	.id = SLAVE_QDSS_STM,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node xs_sys_tcu_cfg = {
	.name = "xs_sys_tcu_cfg",
	.id = SLAVE_TCU,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_aoss = {
	.name = "qhs_aoss",
	.id = SLAVE_AOSS,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_ipc_router = {
	.name = "qhs_ipc_router",
	.id = SLAVE_IPC_ROUTER_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_soccp = {
	.name = "qhs_soccp",
	.id = SLAVE_SOCCP,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_tme_cfg = {
	.name = "qhs_tme_cfg",
	.id = SLAVE_TME_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qns_apss = {
	.name = "qns_apss",
	.id = SLAVE_APPSS,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qss_cfg = {
	.name = "qss_cfg",
	.id = SLAVE_CNOC_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_CNOC_CFG },
};

static struct qcom_icc_node qxs_boot_imem = {
	.name = "qxs_boot_imem",
	.id = SLAVE_BOOT_IMEM,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qxs_imem = {
	.name = "qxs_imem",
	.id = SLAVE_IMEM,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qns_hscnoc_cnoc = {
	.name = "qns_hscnoc_cnoc",
	.id = SLAVE_HSCNOC_CNOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_HSCNOC_CNOC },
};

static struct qcom_icc_node qns_llcc = {
	.name = "qns_llcc",
	.id = SLAVE_LLCC,
	.channels = 12,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_LLCC },
};

static struct qcom_icc_node qns_pcie_east = {
	.name = "qns_pcie_east",
	.id = SLAVE_PCIE_EAST,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_HSCNOC_PCIE_EAST },
};

static struct qcom_icc_node qns_pcie_west = {
	.name = "qns_pcie_west",
	.id = SLAVE_PCIE_WEST,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_HSCNOC_PCIE_WEST },
};

static struct qcom_icc_node qns_lpass_ag_noc_gemnoc = {
	.name = "qns_lpass_ag_noc_gemnoc",
	.id = SLAVE_LPASS_GEM_NOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_LPASS_GEM_NOC },
};

static struct qcom_icc_node qns_lpass_aggnoc = {
	.name = "qns_lpass_aggnoc",
	.id = SLAVE_LPIAON_NOC_LPASS_AG_NOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_LPIAON_NOC },
};

static struct qcom_icc_node qns_lpi_aon_noc = {
	.name = "qns_lpi_aon_noc",
	.id = SLAVE_LPICX_NOC_LPIAON_NOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_LPASS_LPINOC },
};

static struct qcom_icc_node ebi = {
	.name = "ebi",
	.id = SLAVE_EBI1,
	.channels = 12,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qns_mem_noc_hf = {
	.name = "qns_mem_noc_hf",
	.id = SLAVE_MNOC_HF_MEM_NOC,
	.channels = 2,
	.buswidth = 32,
	.init_peak = INT_MAX,
	.init_avg  = INT_MAX,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_MNOC_HF_MEM_NOC },
};

static struct qcom_icc_node qns_mem_noc_sf = {
	.name = "qns_mem_noc_sf",
	.id = SLAVE_MNOC_SF_MEM_NOC,
	.channels = 2,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_MNOC_SF_MEM_NOC },
};

static struct qcom_icc_node srvc_mnoc = {
	.name = "srvc_mnoc",
	.id = SLAVE_SERVICE_MNOC,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qns_system_noc = {
	.name = "qns_system_noc",
	.id = SLAVE_NSINOC_SYSTEM_NOC,
	.channels = 1,
	.buswidth = 8,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_NSINOC_SNOC },
};

static struct qcom_icc_node srvc_nsinoc = {
	.name = "srvc_nsinoc",
	.id = SLAVE_SERVICE_NSINOC,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qns_nsp_hscnoc = {
	.name = "qns_nsp_hscnoc",
	.id = SLAVE_NSP0_HSC_NOC,
	.channels = 4,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_COMPUTE_NOC },
};

static struct qcom_icc_node qns_oobmss_snoc = {
	.name = "qns_oobmss_snoc",
	.id = SLAVE_OOBMSS_SNOC,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_OOBMSS },
};

static struct qcom_icc_node qns_pcie_east_mem_noc = {
	.name = "qns_pcie_east_mem_noc",
	.id = SLAVE_PCIE_EAST_MEM_NOC,
	.channels = 1,
	.buswidth = 32,
	.init_peak = INT_MAX,
	.init_avg  = INT_MAX,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_PCIE_EAST },
};

static struct qcom_icc_node srvc_pcie_east_aggre_noc = {
	.name = "srvc_pcie_east_aggre_noc",
	.id = SLAVE_SERVICE_PCIE_EAST_AGGRE_NOC,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_hscnoc_pcie_east_ms_mpu_cfg = {
	.name = "qhs_hscnoc_pcie_east_ms_mpu_cfg",
	.id = SLAVE_HSCNOC_PCIE_EAST_MS_MPU_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node srvc_pcie_east = {
	.name = "srvc_pcie_east",
	.id = SLAVE_SERVICE_PCIE_EAST,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node xs_pcie_0 = {
	.name = "xs_pcie_0",
	.id = SLAVE_PCIE_0,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node xs_pcie_1 = {
	.name = "xs_pcie_1",
	.id = SLAVE_PCIE_1,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node xs_pcie_5 = {
	.name = "xs_pcie_5",
	.id = SLAVE_PCIE_5,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qns_pcie_west_mem_noc = {
	.name = "qns_pcie_west_mem_noc",
	.id = SLAVE_PCIE_WEST_MEM_NOC,
	.channels = 1,
	.buswidth = 64,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_PCIE_WEST },
};

static struct qcom_icc_node srvc_pcie_west_aggre_noc = {
	.name = "srvc_pcie_west_aggre_noc",
	.id = SLAVE_SERVICE_PCIE_WEST_AGGRE_NOC,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qhs_hscnoc_pcie_west_ms_mpu_cfg = {
	.name = "qhs_hscnoc_pcie_west_ms_mpu_cfg",
	.id = SLAVE_HSCNOC_PCIE_WEST_MS_MPU_CFG,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node srvc_pcie_west = {
	.name = "srvc_pcie_west",
	.id = SLAVE_SERVICE_PCIE_WEST,
	.channels = 1,
	.buswidth = 4,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node xs_pcie_2 = {
	.name = "xs_pcie_2",
	.id = SLAVE_PCIE_2,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node xs_pcie_3a = {
	.name = "xs_pcie_3a",
	.id = SLAVE_PCIE_3A,
	.channels = 1,
	.buswidth = 64,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node xs_pcie_3b = {
	.name = "xs_pcie_3b",
	.id = SLAVE_PCIE_3B,
	.channels = 1,
	.buswidth = 32,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node xs_pcie_4 = {
	.name = "xs_pcie_4",
	.id = SLAVE_PCIE_4,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node xs_pcie_6 = {
	.name = "xs_pcie_6",
	.id = SLAVE_PCIE_6,
	.channels = 1,
	.buswidth = 16,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 0,
};

static struct qcom_icc_node qns_gemnoc_sf = {
	.name = "qns_gemnoc_sf",
	.id = SLAVE_SNOC_GEM_NOC_SF,
	.channels = 1,
	.buswidth = 64,
	.noc_ops = &qcom_qnoc4_ops,
	.num_links = 1,
	.links = { MASTER_SNOC_SF_MEM_NOC },
};

static struct qcom_icc_bcm bcm_acv = {
	.name = "ACV",
	.voter_idx = VOTER_IDX_HLOS,
	.perf_mode_mask = 0x2,
	.type = QCOM_ICC_BCM_TYPE_MASK,
	.num_nodes = 1,
	.nodes = { &ebi },
};

static struct qcom_icc_bcm bcm_ce0 = {
	.name = "CE0",
	.voter_idx = VOTER_IDX_HLOS,
	.num_nodes = 1,
	.nodes = { &qxm_crypto },
};

static struct qcom_icc_bcm bcm_cn0 = {
	.name = "CN0",
	.voter_idx = VOTER_IDX_HLOS,
	.enable_mask = 0x1,
	.keepalive = true,
	.num_nodes = 60,
	.nodes = { &qsm_cfg, &qhs_ahb2phy0,
		   &qhs_ahb2phy1, &qhs_ahb2phy2,
		   &qhs_ahb2phy3, &qhs_av1_enc_cfg,
		   &qhs_camera_cfg, &qhs_clk_ctl,
		   &qhs_crypto0_cfg, &qhs_gpuss_cfg,
		   &qhs_imem_cfg, &qhs_pcie0_cfg,
		   &qhs_pcie1_cfg, &qhs_pcie2_cfg,
		   &qhs_pcie3a_cfg, &qhs_pcie3b_cfg,
		   &qhs_pcie4_cfg, &qhs_pcie5_cfg,
		   &qhs_pcie6_cfg, &qhs_pcie_rscc,
		   &qhs_pdm, &qhs_prng,
		   &qhs_qdss_cfg, &qhs_qspi,
		   &qhs_qup0, &qhs_qup1,
		   &qhs_qup2, &qhs_sdc2,
		   &qhs_sdc4, &qhs_smmuv3_cfg,
		   &qhs_tcsr, &qhs_tlmm,
		   &qhs_ufs_mem_cfg, &qhs_usb2_0_cfg,
		   &qhs_usb3_0_cfg, &qhs_usb3_1_cfg,
		   &qhs_usb3_2_cfg, &qhs_usb3_mp_cfg,
		   &qhs_usb4_0_cfg, &qhs_usb4_1_cfg,
		   &qhs_usb4_2_cfg, &qhs_venus_cfg,
		   &qss_cnoc_pcie_slave_east_cfg, &qss_cnoc_pcie_slave_west_cfg,
		   &qss_lpass_qtb_cfg, &qss_mnoc_cfg,
		   &qss_nsp_qtb_cfg, &qss_pcie_east_anoc_cfg,
		   &qss_pcie_west_anoc_cfg, &xs_qdss_stm,
		   &xs_sys_tcu_cfg, &qnm_hscnoc_cnoc,
		   &qhs_aoss, &qhs_ipc_router,
		   &qhs_soccp, &qhs_tme_cfg,
		   &qns_apss, &qss_cfg,
		   &qxs_boot_imem, &qxs_imem },
};

static struct qcom_icc_bcm bcm_cn1 = {
	.name = "CN1",
	.voter_idx = VOTER_IDX_HLOS,
	.num_nodes = 1,
	.nodes = { &qhs_display_cfg },
};

static struct qcom_icc_bcm bcm_co0 = {
	.name = "CO0",
	.voter_idx = VOTER_IDX_HLOS,
	.enable_mask = 0x1,
	.num_nodes = 2,
	.nodes = { &qnm_nsp, &qns_nsp_hscnoc },
};

static struct qcom_icc_bcm bcm_lp0 = {
	.name = "LP0",
	.voter_idx = VOTER_IDX_HLOS,
	.num_nodes = 2,
	.nodes = { &qnm_lpass_lpinoc, &qns_lpass_aggnoc },
};

static struct qcom_icc_bcm bcm_mc0 = {
	.name = "MC0",
	.voter_idx = VOTER_IDX_HLOS,
	.keepalive = true,
	.num_nodes = 1,
	.nodes = { &ebi },
};

static struct qcom_icc_bcm bcm_mm0 = {
	.name = "MM0",
	.voter_idx = VOTER_IDX_HLOS,
	.num_nodes = 1,
	.nodes = { &qns_mem_noc_hf },
};

static struct qcom_icc_bcm bcm_mm1 = {
	.name = "MM1",
	.voter_idx = VOTER_IDX_HLOS,
	.enable_mask = 0x1,
	.num_nodes = 11,
	.nodes = { &qnm_av1_enc, &qnm_camnoc_hf,
		   &qnm_camnoc_icp, &qnm_camnoc_sf,
		   &qnm_eva, &qnm_mdp,
		   &qnm_vapss_hcp, &qnm_video,
		   &qnm_video_cv_cpu, &qnm_video_v_cpu,
		   &qns_mem_noc_sf },
};

static struct qcom_icc_bcm bcm_qup0 = {
	.name = "QUP0",
	.voter_idx = VOTER_IDX_HLOS,
	.vote_scale = 1,
	.num_nodes = 1,
	.nodes = { &qup0_core_slave },
};

static struct qcom_icc_bcm bcm_qup1 = {
	.name = "QUP1",
	.voter_idx = VOTER_IDX_HLOS,
	.vote_scale = 1,
	.num_nodes = 1,
	.nodes = { &qup1_core_slave },
};

static struct qcom_icc_bcm bcm_qup2 = {
	.name = "QUP2",
	.voter_idx = VOTER_IDX_HLOS,
	.vote_scale = 1,
	.num_nodes = 1,
	.nodes = { &qup2_core_slave },
};

static struct qcom_icc_bcm bcm_sh0 = {
	.name = "SH0",
	.voter_idx = VOTER_IDX_HLOS,
	.keepalive = true,
	.num_nodes = 1,
	.nodes = { &qns_llcc },
};

static struct qcom_icc_bcm bcm_sh1 = {
	.name = "SH1",
	.voter_idx = VOTER_IDX_HLOS,
	.enable_mask = 0x1,
	.num_nodes = 18,
	.nodes = { &alm_gpu_tcu, &alm_pcie_qtc,
		   &alm_sys_tcu, &chm_apps,
		   &qnm_aggre_noc_east, &qnm_gpu,
		   &qnm_lpass, &qnm_mnoc_hf,
		   &qnm_mnoc_sf, &qnm_nsp_noc,
		   &qnm_pcie_east, &qnm_pcie_west,
		   &qnm_snoc_sf, &qxm_wlan_q6,
		   &xm_gic, &qns_hscnoc_cnoc,
		   &qns_pcie_east, &qns_pcie_west },
};

static struct qcom_icc_bcm bcm_sn0 = {
	.name = "SN0",
	.voter_idx = VOTER_IDX_HLOS,
	.keepalive = true,
	.num_nodes = 1,
	.nodes = { &qns_gemnoc_sf },
};

static struct qcom_icc_bcm bcm_sn1 = {
	.name = "SN1",
	.voter_idx = VOTER_IDX_HLOS,
	.enable_mask = 0x1,
	.num_nodes = 1,
	.nodes = { &qnm_oobmss },
};

static struct qcom_icc_bcm bcm_sn2 = {
	.name = "SN2",
	.voter_idx = VOTER_IDX_HLOS,
	.num_nodes = 1,
	.nodes = { &qnm_aggre1_noc },
};

static struct qcom_icc_bcm bcm_sn3 = {
	.name = "SN3",
	.voter_idx = VOTER_IDX_HLOS,
	.num_nodes = 1,
	.nodes = { &qnm_aggre2_noc },
};

static struct qcom_icc_bcm bcm_sn4 = {
	.name = "SN4",
	.voter_idx = VOTER_IDX_HLOS,
	.num_nodes = 1,
	.nodes = { &qnm_aggre3_noc },
};

static struct qcom_icc_bcm bcm_sn5 = {
	.name = "SN5",
	.voter_idx = VOTER_IDX_HLOS,
	.num_nodes = 1,
	.nodes = { &qns_a4noc_hscnoc },
};

static struct qcom_icc_bcm bcm_sn6 = {
	.name = "SN6",
	.voter_idx = VOTER_IDX_HLOS,
	.num_nodes = 4,
	.nodes = { &qns_pcie_east_mem_noc, &qnm_hscnoc_pcie_east,
		   &qns_pcie_west_mem_noc, &qnm_hscnoc_pcie_west },
};

static struct qcom_icc_bcm *aggre1_noc_bcms[] = {
	&bcm_ce0,
};

static struct qcom_icc_node *aggre1_noc_nodes[] = {
	[MASTER_CRYPTO] = &qxm_crypto,
	[MASTER_SOCCP_PROC] = &qxm_soccp,
	[MASTER_QDSS_ETR] = &xm_qdss_etr_0,
	[MASTER_QDSS_ETR_1] = &xm_qdss_etr_1,
	[SLAVE_A1NOC_SNOC] = &qns_a1noc_snoc,
};

static char *aggre1_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_aggre1_noc = {
	.config = &icc_regmap_config,
	.nodes = aggre1_noc_nodes,
	.num_nodes = ARRAY_SIZE(aggre1_noc_nodes),
	.bcms = aggre1_noc_bcms,
	.num_bcms = ARRAY_SIZE(aggre1_noc_bcms),
	.voters = aggre1_noc_voters,
	.num_voters = ARRAY_SIZE(aggre1_noc_voters),
};

static struct qcom_icc_bcm *aggre2_noc_bcms[] = {
};

static struct qcom_icc_node *aggre2_noc_nodes[] = {
	[MASTER_UFS_MEM] = &xm_ufs_mem,
	[MASTER_USB3_2] = &xm_usb3_2,
	[MASTER_USB4_2] = &xm_usb4_2,
	[SLAVE_A2NOC_SNOC] = &qns_a2noc_snoc,
};

static char *aggre2_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_aggre2_noc = {
	.config = &icc_regmap_config,
	.nodes = aggre2_noc_nodes,
	.num_nodes = ARRAY_SIZE(aggre2_noc_nodes),
	.bcms = aggre2_noc_bcms,
	.num_bcms = ARRAY_SIZE(aggre2_noc_bcms),
	.voters = aggre2_noc_voters,
	.num_voters = ARRAY_SIZE(aggre2_noc_voters),
};

static struct qcom_icc_bcm *aggre3_noc_bcms[] = {
};

static struct qcom_icc_node *aggre3_noc_nodes[] = {
	[MASTER_QSPI_0] = &qhm_qspi,
	[MASTER_QUP_0] = &qhm_qup0,
	[MASTER_QUP_1] = &qhm_qup1,
	[MASTER_QUP_2] = &qhm_qup2,
	[MASTER_SP] = &qxm_sp,
	[MASTER_SDCC_2] = &xm_sdc2,
	[MASTER_SDCC_4] = &xm_sdc4,
	[MASTER_USB2] = &xm_usb2_0,
	[MASTER_USB3_MP] = &xm_usb3_mp,
	[SLAVE_A3NOC_SNOC] = &qns_a3noc_snoc,
};

static char *aggre3_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_aggre3_noc = {
	.config = &icc_regmap_config,
	.nodes = aggre3_noc_nodes,
	.num_nodes = ARRAY_SIZE(aggre3_noc_nodes),
	.bcms = aggre3_noc_bcms,
	.num_bcms = ARRAY_SIZE(aggre3_noc_bcms),
	.voters = aggre3_noc_voters,
	.num_voters = ARRAY_SIZE(aggre3_noc_voters),
};

static struct qcom_icc_bcm *aggre4_noc_bcms[] = {
	&bcm_sn5,
};

static struct qcom_icc_node *aggre4_noc_nodes[] = {
	[MASTER_USB3_0] = &xm_usb3_0,
	[MASTER_USB3_1] = &xm_usb3_1,
	[MASTER_USB4_0] = &xm_usb4_0,
	[MASTER_USB4_1] = &xm_usb4_1,
	[SLAVE_A4NOC_HSCNOC] = &qns_a4noc_hscnoc,
};

static char *aggre4_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_aggre4_noc = {
	.config = &icc_regmap_config,
	.nodes = aggre4_noc_nodes,
	.num_nodes = ARRAY_SIZE(aggre4_noc_nodes),
	.bcms = aggre4_noc_bcms,
	.num_bcms = ARRAY_SIZE(aggre4_noc_bcms),
	.voters = aggre4_noc_voters,
	.num_voters = ARRAY_SIZE(aggre4_noc_voters),
};

static struct qcom_icc_bcm *clk_virt_bcms[] = {
	&bcm_qup0,
	&bcm_qup1,
	&bcm_qup2,
};

static struct qcom_icc_node *clk_virt_nodes[] = {
	[MASTER_QUP_CORE_0] = &qup0_core_master,
	[MASTER_QUP_CORE_1] = &qup1_core_master,
	[MASTER_QUP_CORE_2] = &qup2_core_master,
	[SLAVE_QUP_CORE_0] = &qup0_core_slave,
	[SLAVE_QUP_CORE_1] = &qup1_core_slave,
	[SLAVE_QUP_CORE_2] = &qup2_core_slave,
};

static char *clk_virt_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_clk_virt = {
	.config = &icc_regmap_config,
	.nodes = clk_virt_nodes,
	.num_nodes = ARRAY_SIZE(clk_virt_nodes),
	.bcms = clk_virt_bcms,
	.num_bcms = ARRAY_SIZE(clk_virt_bcms),
	.voters = clk_virt_voters,
	.num_voters = ARRAY_SIZE(clk_virt_voters),
};

static struct qcom_icc_bcm *cnoc_cfg_bcms[] = {
	&bcm_cn0,
	&bcm_cn1,
};

static struct qcom_icc_node *cnoc_cfg_nodes[] = {
	[MASTER_CNOC_CFG] = &qsm_cfg,
	[SLAVE_AHB2PHY_SOUTH] = &qhs_ahb2phy0,
	[SLAVE_AHB2PHY_NORTH] = &qhs_ahb2phy1,
	[SLAVE_AHB2PHY_2] = &qhs_ahb2phy2,
	[SLAVE_AHB2PHY_3] = &qhs_ahb2phy3,
	[SLAVE_AV1_ENC_CFG] = &qhs_av1_enc_cfg,
	[SLAVE_CAMERA_CFG] = &qhs_camera_cfg,
	[SLAVE_CLK_CTL] = &qhs_clk_ctl,
	[SLAVE_CRYPTO_0_CFG] = &qhs_crypto0_cfg,
	[SLAVE_DISPLAY_CFG] = &qhs_display_cfg,
	[SLAVE_GFX3D_CFG] = &qhs_gpuss_cfg,
	[SLAVE_IMEM_CFG] = &qhs_imem_cfg,
	[SLAVE_PCIE_0_CFG] = &qhs_pcie0_cfg,
	[SLAVE_PCIE_1_CFG] = &qhs_pcie1_cfg,
	[SLAVE_PCIE_2_CFG] = &qhs_pcie2_cfg,
	[SLAVE_PCIE_3A_CFG] = &qhs_pcie3a_cfg,
	[SLAVE_PCIE_3B_CFG] = &qhs_pcie3b_cfg,
	[SLAVE_PCIE_4_CFG] = &qhs_pcie4_cfg,
	[SLAVE_PCIE_5_CFG] = &qhs_pcie5_cfg,
	[SLAVE_PCIE_6_CFG] = &qhs_pcie6_cfg,
	[SLAVE_PCIE_RSCC] = &qhs_pcie_rscc,
	[SLAVE_PDM] = &qhs_pdm,
	[SLAVE_PRNG] = &qhs_prng,
	[SLAVE_QDSS_CFG] = &qhs_qdss_cfg,
	[SLAVE_QSPI_0] = &qhs_qspi,
	[SLAVE_QUP_0] = &qhs_qup0,
	[SLAVE_QUP_1] = &qhs_qup1,
	[SLAVE_QUP_2] = &qhs_qup2,
	[SLAVE_SDCC_2] = &qhs_sdc2,
	[SLAVE_SDCC_4] = &qhs_sdc4,
	[SLAVE_SMMUV3_CFG] = &qhs_smmuv3_cfg,
	[SLAVE_TCSR] = &qhs_tcsr,
	[SLAVE_TLMM] = &qhs_tlmm,
	[SLAVE_UFS_MEM_CFG] = &qhs_ufs_mem_cfg,
	[SLAVE_USB2] = &qhs_usb2_0_cfg,
	[SLAVE_USB3_0] = &qhs_usb3_0_cfg,
	[SLAVE_USB3_1] = &qhs_usb3_1_cfg,
	[SLAVE_USB3_2] = &qhs_usb3_2_cfg,
	[SLAVE_USB3_MP] = &qhs_usb3_mp_cfg,
	[SLAVE_USB4_0] = &qhs_usb4_0_cfg,
	[SLAVE_USB4_1] = &qhs_usb4_1_cfg,
	[SLAVE_USB4_2] = &qhs_usb4_2_cfg,
	[SLAVE_VENUS_CFG] = &qhs_venus_cfg,
	[SLAVE_CNOC_PCIE_SLAVE_EAST_CFG] = &qss_cnoc_pcie_slave_east_cfg,
	[SLAVE_CNOC_PCIE_SLAVE_WEST_CFG] = &qss_cnoc_pcie_slave_west_cfg,
	[SLAVE_LPASS_QTB_CFG] = &qss_lpass_qtb_cfg,
	[SLAVE_CNOC_MNOC_CFG] = &qss_mnoc_cfg,
	[SLAVE_NSP_QTB_CFG] = &qss_nsp_qtb_cfg,
	[SLAVE_PCIE_EAST_ANOC_CFG] = &qss_pcie_east_anoc_cfg,
	[SLAVE_PCIE_WEST_ANOC_CFG] = &qss_pcie_west_anoc_cfg,
	[SLAVE_QDSS_STM] = &xs_qdss_stm,
	[SLAVE_TCU] = &xs_sys_tcu_cfg,
};

static char *cnoc_cfg_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_cnoc_cfg = {
	.config = &icc_regmap_config,
	.nodes = cnoc_cfg_nodes,
	.num_nodes = ARRAY_SIZE(cnoc_cfg_nodes),
	.bcms = cnoc_cfg_bcms,
	.num_bcms = ARRAY_SIZE(cnoc_cfg_bcms),
	.voters = cnoc_cfg_voters,
	.num_voters = ARRAY_SIZE(cnoc_cfg_voters),
};

static struct qcom_icc_bcm *cnoc_main_bcms[] = {
	&bcm_cn0,
};

static struct qcom_icc_node *cnoc_main_nodes[] = {
	[MASTER_HSCNOC_CNOC] = &qnm_hscnoc_cnoc,
	[SLAVE_AOSS] = &qhs_aoss,
	[SLAVE_IPC_ROUTER_CFG] = &qhs_ipc_router,
	[SLAVE_SOCCP] = &qhs_soccp,
	[SLAVE_TME_CFG] = &qhs_tme_cfg,
	[SLAVE_APPSS] = &qns_apss,
	[SLAVE_CNOC_CFG] = &qss_cfg,
	[SLAVE_BOOT_IMEM] = &qxs_boot_imem,
	[SLAVE_IMEM] = &qxs_imem,
};

static char *cnoc_main_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_cnoc_main = {
	.config = &icc_regmap_config,
	.nodes = cnoc_main_nodes,
	.num_nodes = ARRAY_SIZE(cnoc_main_nodes),
	.bcms = cnoc_main_bcms,
	.num_bcms = ARRAY_SIZE(cnoc_main_bcms),
	.voters = cnoc_main_voters,
	.num_voters = ARRAY_SIZE(cnoc_main_voters),
};

static struct qcom_icc_bcm *hscnoc_bcms[] = {
	&bcm_sh0,
	&bcm_sh1,
};

static struct qcom_icc_node *hscnoc_nodes[] = {
	[MASTER_GPU_TCU] = &alm_gpu_tcu,
	[MASTER_PCIE_TCU] = &alm_pcie_qtc,
	[MASTER_SYS_TCU] = &alm_sys_tcu,
	[MASTER_APPSS_PROC] = &chm_apps,
	[MASTER_AGGRE_NOC_EAST] = &qnm_aggre_noc_east,
	[MASTER_GFX3D] = &qnm_gpu,
	[MASTER_LPASS_GEM_NOC] = &qnm_lpass,
	[MASTER_MNOC_HF_MEM_NOC] = &qnm_mnoc_hf,
	[MASTER_MNOC_SF_MEM_NOC] = &qnm_mnoc_sf,
	[MASTER_COMPUTE_NOC] = &qnm_nsp_noc,
	[MASTER_PCIE_EAST] = &qnm_pcie_east,
	[MASTER_PCIE_WEST] = &qnm_pcie_west,
	[MASTER_SNOC_SF_MEM_NOC] = &qnm_snoc_sf,
	[MASTER_WLAN_Q6] = &qxm_wlan_q6,
	[MASTER_GIC] = &xm_gic,
	[SLAVE_HSCNOC_CNOC] = &qns_hscnoc_cnoc,
	[SLAVE_LLCC] = &qns_llcc,
	[SLAVE_PCIE_EAST] = &qns_pcie_east,
	[SLAVE_PCIE_WEST] = &qns_pcie_west,
};

static char *hscnoc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_hscnoc = {
	.config = &icc_regmap_config,
	.nodes = hscnoc_nodes,
	.num_nodes = ARRAY_SIZE(hscnoc_nodes),
	.bcms = hscnoc_bcms,
	.num_bcms = ARRAY_SIZE(hscnoc_bcms),
	.voters = hscnoc_voters,
	.num_voters = ARRAY_SIZE(hscnoc_voters),
};

static struct qcom_icc_bcm *lpass_ag_noc_bcms[] = {
};

static struct qcom_icc_node *lpass_ag_noc_nodes[] = {
	[MASTER_LPIAON_NOC] = &qnm_lpiaon_noc,
	[SLAVE_LPASS_GEM_NOC] = &qns_lpass_ag_noc_gemnoc,
};

static char *lpass_ag_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_lpass_ag_noc = {
	.config = &icc_regmap_config,
	.nodes = lpass_ag_noc_nodes,
	.num_nodes = ARRAY_SIZE(lpass_ag_noc_nodes),
	.bcms = lpass_ag_noc_bcms,
	.num_bcms = ARRAY_SIZE(lpass_ag_noc_bcms),
	.voters = lpass_ag_noc_voters,
	.num_voters = ARRAY_SIZE(lpass_ag_noc_voters),
};

static struct qcom_icc_bcm *lpass_lpiaon_noc_bcms[] = {
	&bcm_lp0,
};

static struct qcom_icc_node *lpass_lpiaon_noc_nodes[] = {
	[MASTER_LPASS_LPINOC] = &qnm_lpass_lpinoc,
	[SLAVE_LPIAON_NOC_LPASS_AG_NOC] = &qns_lpass_aggnoc,
};

static char *lpass_lpiaon_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_lpass_lpiaon_noc = {
	.config = &icc_regmap_config,
	.nodes = lpass_lpiaon_noc_nodes,
	.num_nodes = ARRAY_SIZE(lpass_lpiaon_noc_nodes),
	.bcms = lpass_lpiaon_noc_bcms,
	.num_bcms = ARRAY_SIZE(lpass_lpiaon_noc_bcms),
	.voters = lpass_lpiaon_noc_voters,
	.num_voters = ARRAY_SIZE(lpass_lpiaon_noc_voters),
};

static struct qcom_icc_bcm *lpass_lpicx_noc_bcms[] = {
};

static struct qcom_icc_node *lpass_lpicx_noc_nodes[] = {
	[MASTER_LPASS_PROC] = &qnm_lpinoc_dsp_qns4m,
	[SLAVE_LPICX_NOC_LPIAON_NOC] = &qns_lpi_aon_noc,
};

static char *lpass_lpicx_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_lpass_lpicx_noc = {
	.config = &icc_regmap_config,
	.nodes = lpass_lpicx_noc_nodes,
	.num_nodes = ARRAY_SIZE(lpass_lpicx_noc_nodes),
	.bcms = lpass_lpicx_noc_bcms,
	.num_bcms = ARRAY_SIZE(lpass_lpicx_noc_bcms),
	.voters = lpass_lpicx_noc_voters,
	.num_voters = ARRAY_SIZE(lpass_lpicx_noc_voters),
};

static struct qcom_icc_bcm *mc_virt_bcms[] = {
	&bcm_acv,
	&bcm_mc0,
};

static struct qcom_icc_node *mc_virt_nodes[] = {
	[MASTER_LLCC] = &llcc_mc,
	[SLAVE_EBI1] = &ebi,
};

static char *mc_virt_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_mc_virt = {
	.config = &icc_regmap_config,
	.nodes = mc_virt_nodes,
	.num_nodes = ARRAY_SIZE(mc_virt_nodes),
	.bcms = mc_virt_bcms,
	.num_bcms = ARRAY_SIZE(mc_virt_bcms),
	.voters = mc_virt_voters,
	.num_voters = ARRAY_SIZE(mc_virt_voters),
};

static struct qcom_icc_bcm *mmss_noc_bcms[] = {
	&bcm_mm0,
	&bcm_mm1,
};

static struct qcom_icc_node *mmss_noc_nodes[] = {
	[MASTER_AV1_ENC] = &qnm_av1_enc,
	[MASTER_CAMNOC_HF] = &qnm_camnoc_hf,
	[MASTER_CAMNOC_ICP] = &qnm_camnoc_icp,
	[MASTER_CAMNOC_SF] = &qnm_camnoc_sf,
	[MASTER_EVA] = &qnm_eva,
	[MASTER_MDP] = &qnm_mdp,
	[MASTER_CDSP_HCP] = &qnm_vapss_hcp,
	[MASTER_VIDEO] = &qnm_video,
	[MASTER_VIDEO_CV_PROC] = &qnm_video_cv_cpu,
	[MASTER_VIDEO_V_PROC] = &qnm_video_v_cpu,
	[MASTER_CNOC_MNOC_CFG] = &qsm_mnoc_cfg,
	[SLAVE_MNOC_HF_MEM_NOC] = &qns_mem_noc_hf,
	[SLAVE_MNOC_SF_MEM_NOC] = &qns_mem_noc_sf,
	[SLAVE_SERVICE_MNOC] = &srvc_mnoc,
};

static char *mmss_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_mmss_noc = {
	.config = &icc_regmap_config,
	.nodes = mmss_noc_nodes,
	.num_nodes = ARRAY_SIZE(mmss_noc_nodes),
	.bcms = mmss_noc_bcms,
	.num_bcms = ARRAY_SIZE(mmss_noc_bcms),
	.voters = mmss_noc_voters,
	.num_voters = ARRAY_SIZE(mmss_noc_voters),
};

static struct qcom_icc_bcm *nsinoc_bcms[] = {
};

static struct qcom_icc_node *nsinoc_nodes[] = {
	[MASTER_CPUCP] = &xm_cpucp,
	[SLAVE_NSINOC_SYSTEM_NOC] = &qns_system_noc,
	[SLAVE_SERVICE_NSINOC] = &srvc_nsinoc,
};

static char *nsinoc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_nsinoc = {
	.config = &icc_regmap_config,
	.nodes = nsinoc_nodes,
	.num_nodes = ARRAY_SIZE(nsinoc_nodes),
	.bcms = nsinoc_bcms,
	.num_bcms = ARRAY_SIZE(nsinoc_bcms),
	.voters = nsinoc_voters,
	.num_voters = ARRAY_SIZE(nsinoc_voters),
};

static struct qcom_icc_bcm *nsp_noc_bcms[] = {
	&bcm_co0,
};

static struct qcom_icc_node *nsp_noc_nodes[] = {
	[MASTER_CDSP_PROC] = &qnm_nsp,
	[SLAVE_NSP0_HSC_NOC] = &qns_nsp_hscnoc,
};

static char *nsp_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_nsp_noc = {
	.config = &icc_regmap_config,
	.nodes = nsp_noc_nodes,
	.num_nodes = ARRAY_SIZE(nsp_noc_nodes),
	.bcms = nsp_noc_bcms,
	.num_bcms = ARRAY_SIZE(nsp_noc_bcms),
	.voters = nsp_noc_voters,
	.num_voters = ARRAY_SIZE(nsp_noc_voters),
};

static struct qcom_icc_bcm *oobm_ss_noc_bcms[] = {
};

static struct qcom_icc_node *oobm_ss_noc_nodes[] = {
	[MASTER_OOBMSS_SP_PROC] = &xm_mem_sp,
	[SLAVE_OOBMSS_SNOC] = &qns_oobmss_snoc,
};

static char *oobm_ss_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_oobm_ss_noc = {
	.config = &icc_regmap_config,
	.nodes = oobm_ss_noc_nodes,
	.num_nodes = ARRAY_SIZE(oobm_ss_noc_nodes),
	.bcms = oobm_ss_noc_bcms,
	.num_bcms = ARRAY_SIZE(oobm_ss_noc_bcms),
	.voters = oobm_ss_noc_voters,
	.num_voters = ARRAY_SIZE(oobm_ss_noc_voters),
};

static struct qcom_icc_bcm *pcie_east_anoc_bcms[] = {
	&bcm_sn6,
};

static struct qcom_icc_node *pcie_east_anoc_nodes[] = {
	[MASTER_PCIE_EAST_ANOC_CFG] = &qsm_pcie_east_anoc_cfg,
	[MASTER_PCIE_0] = &xm_pcie_0,
	[MASTER_PCIE_1] = &xm_pcie_1,
	[MASTER_PCIE_5] = &xm_pcie_5,
	[SLAVE_PCIE_EAST_MEM_NOC] = &qns_pcie_east_mem_noc,
	[SLAVE_SERVICE_PCIE_EAST_AGGRE_NOC] = &srvc_pcie_east_aggre_noc,
};

static char *pcie_east_anoc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_pcie_east_anoc = {
	.config = &icc_regmap_config,
	.nodes = pcie_east_anoc_nodes,
	.num_nodes = ARRAY_SIZE(pcie_east_anoc_nodes),
	.bcms = pcie_east_anoc_bcms,
	.num_bcms = ARRAY_SIZE(pcie_east_anoc_bcms),
	.voters = pcie_east_anoc_voters,
	.num_voters = ARRAY_SIZE(pcie_east_anoc_voters),
};

static struct qcom_icc_bcm *pcie_east_slv_noc_bcms[] = {
	&bcm_sn6,
};

static struct qcom_icc_node *pcie_east_slv_noc_nodes[] = {
	[MASTER_HSCNOC_PCIE_EAST] = &qnm_hscnoc_pcie_east,
	[MASTER_CNOC_PCIE_EAST_SLAVE_CFG] = &qsm_cnoc_pcie_east_slave_cfg,
	[SLAVE_HSCNOC_PCIE_EAST_MS_MPU_CFG] = &qhs_hscnoc_pcie_east_ms_mpu_cfg,
	[SLAVE_SERVICE_PCIE_EAST] = &srvc_pcie_east,
	[SLAVE_PCIE_0] = &xs_pcie_0,
	[SLAVE_PCIE_1] = &xs_pcie_1,
	[SLAVE_PCIE_5] = &xs_pcie_5,
};

static char *pcie_east_slv_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_pcie_east_slv_noc = {
	.config = &icc_regmap_config,
	.nodes = pcie_east_slv_noc_nodes,
	.num_nodes = ARRAY_SIZE(pcie_east_slv_noc_nodes),
	.bcms = pcie_east_slv_noc_bcms,
	.num_bcms = ARRAY_SIZE(pcie_east_slv_noc_bcms),
	.voters = pcie_east_slv_noc_voters,
	.num_voters = ARRAY_SIZE(pcie_east_slv_noc_voters),
};

static struct qcom_icc_bcm *pcie_west_anoc_bcms[] = {
	&bcm_sn6,
};

static struct qcom_icc_node *pcie_west_anoc_nodes[] = {
	[MASTER_PCIE_WEST_ANOC_CFG] = &qsm_pcie_west_anoc_cfg,
	[MASTER_PCIE_2] = &xm_pcie_2,
	[MASTER_PCIE_3A] = &xm_pcie_3a,
	[MASTER_PCIE_3B] = &xm_pcie_3b,
	[MASTER_PCIE_4] = &xm_pcie_4,
	[MASTER_PCIE_6] = &xm_pcie_6,
	[SLAVE_PCIE_WEST_MEM_NOC] = &qns_pcie_west_mem_noc,
	[SLAVE_SERVICE_PCIE_WEST_AGGRE_NOC] = &srvc_pcie_west_aggre_noc,
};

static char *pcie_west_anoc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_pcie_west_anoc = {
	.config = &icc_regmap_config,
	.nodes = pcie_west_anoc_nodes,
	.num_nodes = ARRAY_SIZE(pcie_west_anoc_nodes),
	.bcms = pcie_west_anoc_bcms,
	.num_bcms = ARRAY_SIZE(pcie_west_anoc_bcms),
	.voters = pcie_west_anoc_voters,
	.num_voters = ARRAY_SIZE(pcie_west_anoc_voters),
};

static struct qcom_icc_bcm *pcie_west_slv_noc_bcms[] = {
	&bcm_sn6,
};

static struct qcom_icc_node *pcie_west_slv_noc_nodes[] = {
	[MASTER_HSCNOC_PCIE_WEST] = &qnm_hscnoc_pcie_west,
	[MASTER_CNOC_PCIE_WEST_SLAVE_CFG] = &qsm_cnoc_pcie_west_slave_cfg,
	[SLAVE_HSCNOC_PCIE_WEST_MS_MPU_CFG] = &qhs_hscnoc_pcie_west_ms_mpu_cfg,
	[SLAVE_SERVICE_PCIE_WEST] = &srvc_pcie_west,
	[SLAVE_PCIE_2] = &xs_pcie_2,
	[SLAVE_PCIE_3A] = &xs_pcie_3a,
	[SLAVE_PCIE_3B] = &xs_pcie_3b,
	[SLAVE_PCIE_4] = &xs_pcie_4,
	[SLAVE_PCIE_6] = &xs_pcie_6,
};

static char *pcie_west_slv_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_pcie_west_slv_noc = {
	.config = &icc_regmap_config,
	.nodes = pcie_west_slv_noc_nodes,
	.num_nodes = ARRAY_SIZE(pcie_west_slv_noc_nodes),
	.bcms = pcie_west_slv_noc_bcms,
	.num_bcms = ARRAY_SIZE(pcie_west_slv_noc_bcms),
	.voters = pcie_west_slv_noc_voters,
	.num_voters = ARRAY_SIZE(pcie_west_slv_noc_voters),
};

static struct qcom_icc_bcm *system_noc_bcms[] = {
	&bcm_sn0,
	&bcm_sn1,
	&bcm_sn2,
	&bcm_sn3,
	&bcm_sn4,
};

static struct qcom_icc_node *system_noc_nodes[] = {
	[MASTER_A1NOC_SNOC] = &qnm_aggre1_noc,
	[MASTER_A2NOC_SNOC] = &qnm_aggre2_noc,
	[MASTER_A3NOC_SNOC] = &qnm_aggre3_noc,
	[MASTER_NSINOC_SNOC] = &qnm_nsi_noc,
	[MASTER_OOBMSS] = &qnm_oobmss,
	[SLAVE_SNOC_GEM_NOC_SF] = &qns_gemnoc_sf,
};

static char *system_noc_voters[] = {
	[VOTER_IDX_HLOS] = "hlos",
};

static struct qcom_icc_desc glymur_system_noc = {
	.config = &icc_regmap_config,
	.nodes = system_noc_nodes,
	.num_nodes = ARRAY_SIZE(system_noc_nodes),
	.bcms = system_noc_bcms,
	.num_bcms = ARRAY_SIZE(system_noc_bcms),
	.voters = system_noc_voters,
	.num_voters = ARRAY_SIZE(system_noc_voters),
};

static int qnoc_probe(struct platform_device *pdev)
{
	int ret;

	if (device_is_compatible(&pdev->dev, "qcom,mahua-mc_virt")) {
		llcc_mc.channels = 8;
		ebi.channels = 8;
	} else if (device_is_compatible(&pdev->dev, "qcom,mahua-hscnoc")) {
		qns_llcc.channels = 8;
		chm_apps.channels = 4;
		qnm_pcie_west.buswidth = 32;
		hscnoc_nodes[MASTER_WLAN_Q6] = NULL;
	} else if (device_is_compatible(&pdev->dev, "qcom,mahua-pcie_west_anoc")) {
		qns_pcie_west_mem_noc.buswidth = 32;
		pcie_west_anoc_nodes[MASTER_PCIE_3A] = NULL;
	} else if (device_is_compatible(&pdev->dev, "qcom,mahua-cnoc_cfg")) {
		cnoc_cfg_nodes[SLAVE_PCIE_3A_CFG] = NULL;
	} else if (device_is_compatible(&pdev->dev, "qcom,mahua-pcie_west_slv_noc")) {
		pcie_west_slv_noc_nodes[SLAVE_PCIE_3A] = NULL;
	}

	ret = qcom_icc_rpmh_probe(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to register ICC provider\n");

	dev_info(&pdev->dev, "Registered ICC provider\n");

	return ret;
}

static const struct of_device_id qnoc_of_match[] = {
	{ .compatible = "qcom,glymur-aggre1_noc",
	  .data = &glymur_aggre1_noc},
	{ .compatible = "qcom,glymur-aggre2_noc",
	  .data = &glymur_aggre2_noc},
	{ .compatible = "qcom,glymur-aggre3_noc",
	  .data = &glymur_aggre3_noc},
	{ .compatible = "qcom,glymur-aggre4_noc",
	  .data = &glymur_aggre4_noc},
	{ .compatible = "qcom,glymur-clk_virt",
	  .data = &glymur_clk_virt},
	{ .compatible = "qcom,glymur-cnoc_cfg",
	  .data = &glymur_cnoc_cfg},
	{ .compatible = "qcom,mahua-cnoc_cfg",
	  .data = &glymur_cnoc_cfg},
	{ .compatible = "qcom,glymur-cnoc_main",
	  .data = &glymur_cnoc_main},
	{ .compatible = "qcom,glymur-hscnoc",
	  .data = &glymur_hscnoc},
	{ .compatible = "qcom,mahua-hscnoc",
	  .data = &glymur_hscnoc},
	{ .compatible = "qcom,glymur-lpass_ag_noc",
	  .data = &glymur_lpass_ag_noc},
	{ .compatible = "qcom,glymur-lpass_lpiaon_noc",
	  .data = &glymur_lpass_lpiaon_noc},
	{ .compatible = "qcom,glymur-lpass_lpicx_noc",
	  .data = &glymur_lpass_lpicx_noc},
	{ .compatible = "qcom,glymur-mc_virt",
	  .data = &glymur_mc_virt},
	{ .compatible = "qcom,mahua-mc_virt",
	  .data = &glymur_mc_virt},
	{ .compatible = "qcom,glymur-mmss_noc",
	  .data = &glymur_mmss_noc},
	{ .compatible = "qcom,glymur-nsinoc",
	  .data = &glymur_nsinoc},
	{ .compatible = "qcom,glymur-nsp_noc",
	  .data = &glymur_nsp_noc},
	{ .compatible = "qcom,glymur-oobm_ss_noc",
	  .data = &glymur_oobm_ss_noc},
	{ .compatible = "qcom,glymur-pcie_east_anoc",
	  .data = &glymur_pcie_east_anoc},
	{ .compatible = "qcom,glymur-pcie_east_slv_noc",
	  .data = &glymur_pcie_east_slv_noc},
	{ .compatible = "qcom,glymur-pcie_west_anoc",
	  .data = &glymur_pcie_west_anoc},
	{ .compatible = "qcom,mahua-pcie_west_anoc",
	  .data = &glymur_pcie_west_anoc},
	{ .compatible = "qcom,glymur-pcie_west_slv_noc",
	  .data = &glymur_pcie_west_slv_noc},
	{ .compatible = "qcom,mahua-pcie_west_slv_noc",
	  .data = &glymur_pcie_west_slv_noc},
	{ .compatible = "qcom,glymur-system_noc",
	  .data = &glymur_system_noc},
	{ }
};
MODULE_DEVICE_TABLE(of, qnoc_of_match);

static struct platform_driver qnoc_driver = {
	.probe = qnoc_probe,
	.remove = qcom_icc_rpmh_remove,
	.driver = {
		.name = "qnoc-glymur",
		.of_match_table = qnoc_of_match,
		.sync_state = icc_sync_state,
	},
};

static int __init qnoc_driver_init(void)
{
	return platform_driver_register(&qnoc_driver);
}
core_initcall(qnoc_driver_init);

MODULE_DESCRIPTION("GLYMUR NoC driver");
MODULE_LICENSE("GPL");
