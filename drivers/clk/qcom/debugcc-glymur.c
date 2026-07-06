// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#define pr_fmt(fmt) "clk: %s: " fmt, __func__

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "clk-debug.h"
#include "common.h"

static struct measure_clk_data debug_mux_priv = {
	.ctl_reg = 0x87040,
	.status_reg = 0x87044,
	.xo_div4_cbcr = 0x87008,
};

static const char *const apss_cc_debug_mux_parent_names[] = {
	"measure_only_ncc0_clk",
	"measure_only_ncc1_clk",
	"measure_only_ncc2_clk",
};

static int apss_cc_debug_mux_sels[] = {
	0x15,		/* measure_only_ncc0_clk */
	0x16,		/* measure_only_ncc1_clk */
	0x17,		/* measure_only_ncc2_clk */
};

static struct clk_debug_mux apss_cc_debug_mux = {
	.priv = &debug_mux_priv,
	.debug_offset = 0x0,
	.post_div_offset = 0x4,
	.cbcr_offset = 0x8,
	.src_sel_mask = 0x1FF,
	.src_sel_shift = 0,
	.post_div_mask = 0xF,
	.post_div_shift = 0,
	.post_div_val = 4,
	.mux_sels = apss_cc_debug_mux_sels,
	.num_mux_sels = ARRAY_SIZE(apss_cc_debug_mux_sels),
	.hw.init = &(const struct clk_init_data){
		.name = "apss_cc_debug_mux",
		.ops = &clk_debug_mux_ops,
		.parent_names = apss_cc_debug_mux_parent_names,
		.num_parents = ARRAY_SIZE(apss_cc_debug_mux_parent_names),
	},
};

static const char *const cam_cc_debug_mux_parent_names[] = {
	"cam_cc_bps_ahb_clk",
	"cam_cc_bps_clk",
	"cam_cc_bps_fast_ahb_clk",
	"cam_cc_camnoc_axi_nrt_clk",
	"cam_cc_camnoc_axi_rt_clk",
	"cam_cc_camnoc_dcd_xo_clk",
	"cam_cc_camnoc_xo_clk",
	"cam_cc_cci_0_clk",
	"cam_cc_cci_1_clk",
	"cam_cc_core_ahb_clk",
	"cam_cc_cpas_ahb_clk",
	"cam_cc_cpas_bps_clk",
	"cam_cc_cpas_fast_ahb_clk",
	"cam_cc_cpas_ife_0_clk",
	"cam_cc_cpas_ife_1_clk",
	"cam_cc_cpas_ife_lite_clk",
	"cam_cc_cpas_ipe_nps_clk",
	"cam_cc_csi0phytimer_clk",
	"cam_cc_csi1phytimer_clk",
	"cam_cc_csi4phytimer_clk",
	"cam_cc_csid_clk",
	"cam_cc_csid_csiphy_rx_clk",
	"cam_cc_csiphy0_clk",
	"cam_cc_csiphy1_clk",
	"cam_cc_csiphy4_clk",
	"cam_cc_icp_ahb_clk",
	"cam_cc_icp_clk",
	"cam_cc_ife_0_clk",
	"cam_cc_ife_0_dsp_clk",
	"cam_cc_ife_0_fast_ahb_clk",
	"cam_cc_ife_1_clk",
	"cam_cc_ife_1_dsp_clk",
	"cam_cc_ife_1_fast_ahb_clk",
	"cam_cc_ife_lite_ahb_clk",
	"cam_cc_ife_lite_clk",
	"cam_cc_ife_lite_cphy_rx_clk",
	"cam_cc_ife_lite_csid_clk",
	"cam_cc_ipe_nps_ahb_clk",
	"cam_cc_ipe_nps_clk",
	"cam_cc_ipe_nps_fast_ahb_clk",
	"cam_cc_ipe_pps_clk",
	"cam_cc_ipe_pps_fast_ahb_clk",
	"cam_cc_jpeg_clk",
	"cam_cc_mclk0_clk",
	"cam_cc_mclk1_clk",
	"cam_cc_mclk2_clk",
	"cam_cc_mclk3_clk",
	"cam_cc_mclk4_clk",
	"cam_cc_mclk5_clk",
	"cam_cc_mclk6_clk",
	"cam_cc_mclk7_clk",
	"cam_cc_qdss_debug_clk",
	"cam_cc_qdss_debug_xo_clk",
	"measure_only_cam_cc_gdsc_clk",
	"measure_only_cam_cc_sleep_clk",
};

static int cam_cc_debug_mux_sels[] = {
	0x11,		/* cam_cc_bps_ahb_clk */
	0x12,		/* cam_cc_bps_clk */
	0x10,		/* cam_cc_bps_fast_ahb_clk */
	0x34,		/* cam_cc_camnoc_axi_nrt_clk */
	0x33,		/* cam_cc_camnoc_axi_rt_clk */
	0x36,		/* cam_cc_camnoc_dcd_xo_clk */
	0x37,		/* cam_cc_camnoc_xo_clk */
	0x2E,		/* cam_cc_cci_0_clk */
	0x2F,		/* cam_cc_cci_1_clk */
	0x3A,		/* cam_cc_core_ahb_clk */
	0x30,		/* cam_cc_cpas_ahb_clk */
	0x13,		/* cam_cc_cpas_bps_clk */
	0x31,		/* cam_cc_cpas_fast_ahb_clk */
	0x1C,		/* cam_cc_cpas_ife_0_clk */
	0x21,		/* cam_cc_cpas_ife_1_clk */
	0x26,		/* cam_cc_cpas_ife_lite_clk */
	0x15,		/* cam_cc_cpas_ipe_nps_clk */
	0x9,		/* cam_cc_csi0phytimer_clk */
	0xC,		/* cam_cc_csi1phytimer_clk */
	0xE,		/* cam_cc_csi4phytimer_clk */
	0x32,		/* cam_cc_csid_clk */
	0xB,		/* cam_cc_csid_csiphy_rx_clk */
	0xA,		/* cam_cc_csiphy0_clk */
	0xD,		/* cam_cc_csiphy1_clk */
	0xF,		/* cam_cc_csiphy4_clk */
	0x2D,		/* cam_cc_icp_ahb_clk */
	0x2C,		/* cam_cc_icp_clk */
	0x1B,		/* cam_cc_ife_0_clk */
	0x1D,		/* cam_cc_ife_0_dsp_clk */
	0x1F,		/* cam_cc_ife_0_fast_ahb_clk */
	0x20,		/* cam_cc_ife_1_clk */
	0x22,		/* cam_cc_ife_1_dsp_clk */
	0x24,		/* cam_cc_ife_1_fast_ahb_clk */
	0x29,		/* cam_cc_ife_lite_ahb_clk */
	0x25,		/* cam_cc_ife_lite_clk */
	0x28,		/* cam_cc_ife_lite_cphy_rx_clk */
	0x27,		/* cam_cc_ife_lite_csid_clk */
	0x18,		/* cam_cc_ipe_nps_ahb_clk */
	0x14,		/* cam_cc_ipe_nps_clk */
	0x19,		/* cam_cc_ipe_nps_fast_ahb_clk */
	0x16,		/* cam_cc_ipe_pps_clk */
	0x1A,		/* cam_cc_ipe_pps_fast_ahb_clk */
	0x2A,		/* cam_cc_jpeg_clk */
	0x1,		/* cam_cc_mclk0_clk */
	0x2,		/* cam_cc_mclk1_clk */
	0x3,		/* cam_cc_mclk2_clk */
	0x4,		/* cam_cc_mclk3_clk */
	0x5,		/* cam_cc_mclk4_clk */
	0x6,		/* cam_cc_mclk5_clk */
	0x7,		/* cam_cc_mclk6_clk */
	0x8,		/* cam_cc_mclk7_clk */
	0x38,		/* cam_cc_qdss_debug_clk */
	0x39,		/* cam_cc_qdss_debug_xo_clk */
	0x3B,		/* measure_only_cam_cc_gdsc_clk */
	0x3C,		/* measure_only_cam_cc_sleep_clk */
};

static struct clk_debug_mux cam_cc_debug_mux = {
	.priv = &debug_mux_priv,
	.debug_offset = 0x16000,
	.post_div_offset = 0x14004,
	.cbcr_offset = 0x14008,
	.src_sel_mask = 0xFF,
	.src_sel_shift = 0,
	.post_div_mask = 0xF,
	.post_div_shift = 0,
	.post_div_val = 4,
	.mux_sels = cam_cc_debug_mux_sels,
	.num_mux_sels = ARRAY_SIZE(cam_cc_debug_mux_sels),
	.hw.init = &(const struct clk_init_data){
		.name = "cam_cc_debug_mux",
		.ops = &clk_debug_mux_ops,
		.parent_names = cam_cc_debug_mux_parent_names,
		.num_parents = ARRAY_SIZE(cam_cc_debug_mux_parent_names),
	},
};

static const char *const disp_cc_debug_mux_parent_names[] = {
	"disp_cc_esync0_clk",
	"disp_cc_esync1_clk",
	"disp_cc_mdss_accu_shift_clk",
	"disp_cc_mdss_ahb1_clk",
	"disp_cc_mdss_ahb_clk",
	"disp_cc_mdss_byte0_clk",
	"disp_cc_mdss_byte0_intf_clk",
	"disp_cc_mdss_byte1_clk",
	"disp_cc_mdss_byte1_intf_clk",
	"disp_cc_mdss_dptx0_aux_clk",
	"disp_cc_mdss_dptx0_link_clk",
	"disp_cc_mdss_dptx0_link_dpin_clk",
	"disp_cc_mdss_dptx0_link_intf_clk",
	"disp_cc_mdss_dptx0_pixel0_clk",
	"disp_cc_mdss_dptx0_pixel1_clk",
	"disp_cc_mdss_dptx0_usb_router_link_intf_clk",
	"disp_cc_mdss_dptx1_aux_clk",
	"disp_cc_mdss_dptx1_link_clk",
	"disp_cc_mdss_dptx1_link_dpin_clk",
	"disp_cc_mdss_dptx1_link_intf_clk",
	"disp_cc_mdss_dptx1_pixel0_clk",
	"disp_cc_mdss_dptx1_pixel1_clk",
	"disp_cc_mdss_dptx1_usb_router_link_intf_clk",
	"disp_cc_mdss_dptx2_aux_clk",
	"disp_cc_mdss_dptx2_link_clk",
	"disp_cc_mdss_dptx2_link_dpin_clk",
	"disp_cc_mdss_dptx2_link_intf_clk",
	"disp_cc_mdss_dptx2_pixel0_clk",
	"disp_cc_mdss_dptx2_pixel1_clk",
	"disp_cc_mdss_dptx2_usb_router_link_intf_clk",
	"disp_cc_mdss_dptx3_aux_clk",
	"disp_cc_mdss_dptx3_link_clk",
	"disp_cc_mdss_dptx3_link_dpin_clk",
	"disp_cc_mdss_dptx3_link_intf_clk",
	"disp_cc_mdss_dptx3_pixel0_clk",
	"disp_cc_mdss_esc0_clk",
	"disp_cc_mdss_esc1_clk",
	"disp_cc_mdss_mdp1_clk",
	"disp_cc_mdss_mdp_clk",
	"disp_cc_mdss_mdp_lut1_clk",
	"disp_cc_mdss_mdp_lut_clk",
	"disp_cc_mdss_non_gdsc_ahb_clk",
	"disp_cc_mdss_pclk0_clk",
	"disp_cc_mdss_pclk1_clk",
	"disp_cc_mdss_pclk2_clk",
	"disp_cc_mdss_rscc_ahb_clk",
	"disp_cc_mdss_rscc_vsync_clk",
	"disp_cc_mdss_vsync1_clk",
	"disp_cc_mdss_vsync_clk",
	"disp_cc_osc_clk",
	"measure_only_disp_cc_sleep_clk",
	"measure_only_disp_cc_xo_clk",
};

static int disp_cc_debug_mux_sels[] = {
	0x37,		/* disp_cc_esync0_clk */
	0x38,		/* disp_cc_esync1_clk */
	0x4C,		/* disp_cc_mdss_accu_shift_clk */
	0x3C,		/* disp_cc_mdss_ahb1_clk */
	0x35,		/* disp_cc_mdss_ahb_clk */
	0x15,		/* disp_cc_mdss_byte0_clk */
	0x16,		/* disp_cc_mdss_byte0_intf_clk */
	0x17,		/* disp_cc_mdss_byte1_clk */
	0x18,		/* disp_cc_mdss_byte1_intf_clk */
	0x21,		/* disp_cc_mdss_dptx0_aux_clk */
	0x1B,		/* disp_cc_mdss_dptx0_link_clk */
	0x54,		/* disp_cc_mdss_dptx0_link_dpin_clk */
	0x1D,		/* disp_cc_mdss_dptx0_link_intf_clk */
	0x1F,		/* disp_cc_mdss_dptx0_pixel0_clk */
	0x20,		/* disp_cc_mdss_dptx0_pixel1_clk */
	0x1C,		/* disp_cc_mdss_dptx0_usb_router_link_intf_clk */
	0x28,		/* disp_cc_mdss_dptx1_aux_clk */
	0x24,		/* disp_cc_mdss_dptx1_link_clk */
	0x55,		/* disp_cc_mdss_dptx1_link_dpin_clk */
	0x26,		/* disp_cc_mdss_dptx1_link_intf_clk */
	0x22,		/* disp_cc_mdss_dptx1_pixel0_clk */
	0x23,		/* disp_cc_mdss_dptx1_pixel1_clk */
	0x25,		/* disp_cc_mdss_dptx1_usb_router_link_intf_clk */
	0x2F,		/* disp_cc_mdss_dptx2_aux_clk */
	0x2B,		/* disp_cc_mdss_dptx2_link_clk */
	0x56,		/* disp_cc_mdss_dptx2_link_dpin_clk */
	0x2D,		/* disp_cc_mdss_dptx2_link_intf_clk */
	0x29,		/* disp_cc_mdss_dptx2_pixel0_clk */
	0x2A,		/* disp_cc_mdss_dptx2_pixel1_clk */
	0x2C,		/* disp_cc_mdss_dptx2_usb_router_link_intf_clk */
	0x33,		/* disp_cc_mdss_dptx3_aux_clk */
	0x31,		/* disp_cc_mdss_dptx3_link_clk */
	0x57,		/* disp_cc_mdss_dptx3_link_dpin_clk */
	0x32,		/* disp_cc_mdss_dptx3_link_intf_clk */
	0x30,		/* disp_cc_mdss_dptx3_pixel0_clk */
	0x19,		/* disp_cc_mdss_esc0_clk */
	0x1A,		/* disp_cc_mdss_esc1_clk */
	0x39,		/* disp_cc_mdss_mdp1_clk */
	0x12,		/* disp_cc_mdss_mdp_clk */
	0x3A,		/* disp_cc_mdss_mdp_lut1_clk */
	0x13,		/* disp_cc_mdss_mdp_lut_clk */
	0x3D,		/* disp_cc_mdss_non_gdsc_ahb_clk */
	0xF,		/* disp_cc_mdss_pclk0_clk */
	0x10,		/* disp_cc_mdss_pclk1_clk */
	0x11,		/* disp_cc_mdss_pclk2_clk */
	0x3F,		/* disp_cc_mdss_rscc_ahb_clk */
	0x3E,		/* disp_cc_mdss_rscc_vsync_clk */
	0x3B,		/* disp_cc_mdss_vsync1_clk */
	0x14,		/* disp_cc_mdss_vsync_clk */
	0x36,		/* disp_cc_osc_clk */
	0x4D,		/* measure_only_disp_cc_sleep_clk */
	0x4B,		/* measure_only_disp_cc_xo_clk */
};

static struct clk_debug_mux disp_cc_debug_mux = {
	.priv = &debug_mux_priv,
	.debug_offset = 0x11000,
	.post_div_offset = 0xD000,
	.cbcr_offset = 0xD004,
	.src_sel_mask = 0x1FF,
	.src_sel_shift = 0,
	.post_div_mask = 0xF,
	.post_div_shift = 0,
	.post_div_val = 4,
	.mux_sels = disp_cc_debug_mux_sels,
	.num_mux_sels = ARRAY_SIZE(disp_cc_debug_mux_sels),
	.hw.init = &(const struct clk_init_data){
		.name = "disp_cc_debug_mux",
		.ops = &clk_debug_mux_ops,
		.parent_names = disp_cc_debug_mux_parent_names,
		.num_parents = ARRAY_SIZE(disp_cc_debug_mux_parent_names),
	},
};

static const char *const eva_cc_debug_mux_parent_names[] = {
	"eva_cc_mvs0_clk",
	"eva_cc_mvs0_freerun_clk",
	"eva_cc_mvs0_shift_clk",
	"eva_cc_mvs0c_clk",
	"eva_cc_mvs0c_freerun_clk",
	"eva_cc_mvs0c_shift_clk",
	"measure_only_eva_cc_ahb_clk",
	"measure_only_eva_cc_sleep_clk",
	"measure_only_eva_cc_xo_clk",
};

static int eva_cc_debug_mux_sels[] = {
	0x4,		/* eva_cc_mvs0_clk */
	0x5,		/* eva_cc_mvs0_freerun_clk */
	0xA,		/* eva_cc_mvs0_shift_clk */
	0x1,		/* eva_cc_mvs0c_clk */
	0x2,		/* eva_cc_mvs0c_freerun_clk */
	0xB,		/* eva_cc_mvs0c_shift_clk */
	0x8,		/* measure_only_eva_cc_ahb_clk */
	0xC,		/* measure_only_eva_cc_sleep_clk */
	0x9,		/* measure_only_eva_cc_xo_clk */
};

static struct clk_debug_mux eva_cc_debug_mux = {
	.priv = &debug_mux_priv,
	.debug_offset = 0x9A4C,
	.post_div_offset = 0x80A8,
	.cbcr_offset = 0x80AC,
	.src_sel_mask = 0x3F,
	.src_sel_shift = 0,
	.post_div_mask = 0xF,
	.post_div_shift = 0,
	.post_div_val = 3,
	.mux_sels = eva_cc_debug_mux_sels,
	.num_mux_sels = ARRAY_SIZE(eva_cc_debug_mux_sels),
	.hw.init = &(const struct clk_init_data){
		.name = "eva_cc_debug_mux",
		.ops = &clk_debug_mux_ops,
		.parent_names = eva_cc_debug_mux_parent_names,
		.num_parents = ARRAY_SIZE(eva_cc_debug_mux_parent_names),
	},
};

static const char *const gcc_debug_mux_parent_names[] = {
	"apss_cc_debug_mux",
	"cam_cc_debug_mux",
	"disp_cc_debug_mux",
	"eva_cc_debug_mux",
	"gcc_aggre_noc_pcie_3a_west_sf_axi_clk",
	"gcc_aggre_noc_pcie_3b_west_sf_axi_clk",
	"gcc_aggre_noc_pcie_4_west_sf_axi_clk",
	"gcc_aggre_noc_pcie_5_east_sf_axi_clk",
	"gcc_aggre_noc_pcie_6_west_sf_axi_clk",
	"gcc_aggre_ufs_phy_axi_clk",
	"gcc_aggre_usb2_prim_axi_clk",
	"gcc_aggre_usb3_mp_axi_clk",
	"gcc_aggre_usb3_prim_axi_clk",
	"gcc_aggre_usb3_sec_axi_clk",
	"gcc_aggre_usb3_tert_axi_clk",
	"gcc_aggre_usb4_0_axi_clk",
	"gcc_aggre_usb4_1_axi_clk",
	"gcc_aggre_usb4_2_axi_clk",
	"gcc_camera_hf_axi_clk",
	"gcc_camera_sf_axi_clk",
	"gcc_cfg_noc_pcie_anoc_ahb_clk",
	"gcc_cfg_noc_pcie_anoc_south_ahb_clk",
	"gcc_cfg_noc_usb2_prim_axi_clk",
	"gcc_cfg_noc_usb3_mp_axi_clk",
	"gcc_cfg_noc_usb3_prim_axi_clk",
	"gcc_cfg_noc_usb3_sec_axi_clk",
	"gcc_cfg_noc_usb3_tert_axi_clk",
	"gcc_cfg_noc_usb_anoc_ahb_clk",
	"gcc_cfg_noc_usb_anoc_south_ahb_clk",
	"gcc_disp_hf_axi_clk",
	"gcc_eva_axi0_clk",
	"gcc_eva_axi0c_clk",
	"gcc_gp1_clk",
	"gcc_gp2_clk",
	"gcc_gp3_clk",
	"gcc_gpu_gemnoc_gfx_clk",
	"gcc_gpu_gpll0_clk_src",
	"gcc_gpu_gpll0_div_clk_src",
	"gcc_pcie_0_aux_clk",
	"gcc_pcie_0_cfg_ahb_clk",
	"gcc_pcie_0_mstr_axi_clk",
	"gcc_pcie_0_phy_rchng_clk",
	"gcc_pcie_0_pipe_clk",
	"gcc_pcie_0_slv_axi_clk",
	"gcc_pcie_0_slv_q2a_axi_clk",
	"gcc_pcie_1_aux_clk",
	"gcc_pcie_1_cfg_ahb_clk",
	"gcc_pcie_1_mstr_axi_clk",
	"gcc_pcie_1_phy_rchng_clk",
	"gcc_pcie_1_pipe_clk",
	"gcc_pcie_1_slv_axi_clk",
	"gcc_pcie_1_slv_q2a_axi_clk",
	"gcc_pcie_2_aux_clk",
	"gcc_pcie_2_cfg_ahb_clk",
	"gcc_pcie_2_mstr_axi_clk",
	"gcc_pcie_2_phy_rchng_clk",
	"gcc_pcie_2_pipe_clk",
	"gcc_pcie_2_slv_axi_clk",
	"gcc_pcie_2_slv_q2a_axi_clk",
	"gcc_pcie_3a_aux_clk",
	"gcc_pcie_3a_cfg_ahb_clk",
	"gcc_pcie_3a_mstr_axi_clk",
	"gcc_pcie_3a_phy_rchng_clk",
	"gcc_pcie_3a_pipe_clk",
	"gcc_pcie_3a_slv_axi_clk",
	"gcc_pcie_3a_slv_q2a_axi_clk",
	"gcc_pcie_3b_aux_clk",
	"gcc_pcie_3b_cfg_ahb_clk",
	"gcc_pcie_3b_mstr_axi_clk",
	"gcc_pcie_3b_phy_rchng_clk",
	"gcc_pcie_3b_pipe_clk",
	"gcc_pcie_3b_pipe_div2_clk",
	"gcc_pcie_3b_slv_axi_clk",
	"gcc_pcie_3b_slv_q2a_axi_clk",
	"gcc_pcie_4_aux_clk",
	"gcc_pcie_4_cfg_ahb_clk",
	"gcc_pcie_4_mstr_axi_clk",
	"gcc_pcie_4_phy_rchng_clk",
	"gcc_pcie_4_pipe_clk",
	"gcc_pcie_4_pipe_div2_clk",
	"gcc_pcie_4_slv_axi_clk",
	"gcc_pcie_4_slv_q2a_axi_clk",
	"gcc_pcie_5_aux_clk",
	"gcc_pcie_5_cfg_ahb_clk",
	"gcc_pcie_5_mstr_axi_clk",
	"gcc_pcie_5_phy_rchng_clk",
	"gcc_pcie_5_pipe_clk",
	"gcc_pcie_5_pipe_div2_clk",
	"gcc_pcie_5_slv_axi_clk",
	"gcc_pcie_5_slv_q2a_axi_clk",
	"gcc_pcie_6_aux_clk",
	"gcc_pcie_6_cfg_ahb_clk",
	"gcc_pcie_6_mstr_axi_clk",
	"gcc_pcie_6_phy_rchng_clk",
	"gcc_pcie_6_pipe_clk",
	"gcc_pcie_6_pipe_div2_clk",
	"gcc_pcie_6_slv_axi_clk",
	"gcc_pcie_6_slv_q2a_axi_clk",
	"gcc_pcie_noc_pwrctl_clk",
	"gcc_pcie_noc_qosgen_extref_clk",
	"gcc_pcie_noc_sf_center_clk",
	"gcc_pcie_noc_slave_sf_east_clk",
	"gcc_pcie_noc_slave_sf_west_clk",
	"gcc_pcie_noc_tsctr_clk",
	"gcc_pcie_phy_3a_aux_clk",
	"gcc_pcie_phy_3b_aux_clk",
	"gcc_pcie_phy_4_aux_clk",
	"gcc_pcie_phy_5_aux_clk",
	"gcc_pcie_phy_6_aux_clk",
	"gcc_pdm2_clk",
	"gcc_pdm_ahb_clk",
	"gcc_pdm_xo4_clk",
	"gcc_qspi_cnoc_periph_ahb_clk",
	"gcc_qspi_core_clk",
	"gcc_qupv3_oob_core_2x_clk",
	"gcc_qupv3_oob_core_clk",
	"gcc_qupv3_oob_qspi_s0_clk",
	"gcc_qupv3_oob_qspi_s1_clk",
	"gcc_qupv3_oob_s0_clk",
	"gcc_qupv3_oob_s1_clk",
	"gcc_qupv3_oob_s_ahb_clk",
	"gcc_qupv3_oob_tcxo_clk",
	"gcc_qupv3_wrap0_core_2x_clk",
	"gcc_qupv3_wrap0_core_clk",
	"gcc_qupv3_wrap0_qspi_s2_clk",
	"gcc_qupv3_wrap0_qspi_s3_clk",
	"gcc_qupv3_wrap0_qspi_s6_clk",
	"gcc_qupv3_wrap0_s0_clk",
	"gcc_qupv3_wrap0_s1_clk",
	"gcc_qupv3_wrap0_s2_clk",
	"gcc_qupv3_wrap0_s3_clk",
	"gcc_qupv3_wrap0_s4_clk",
	"gcc_qupv3_wrap0_s5_clk",
	"gcc_qupv3_wrap0_s6_clk",
	"gcc_qupv3_wrap0_s7_clk",
	"gcc_qupv3_wrap1_core_2x_clk",
	"gcc_qupv3_wrap1_core_clk",
	"gcc_qupv3_wrap1_qspi_s2_clk",
	"gcc_qupv3_wrap1_qspi_s3_clk",
	"gcc_qupv3_wrap1_qspi_s6_clk",
	"gcc_qupv3_wrap1_s0_clk",
	"gcc_qupv3_wrap1_s1_clk",
	"gcc_qupv3_wrap1_s2_clk",
	"gcc_qupv3_wrap1_s3_clk",
	"gcc_qupv3_wrap1_s4_clk",
	"gcc_qupv3_wrap1_s5_clk",
	"gcc_qupv3_wrap1_s6_clk",
	"gcc_qupv3_wrap1_s7_clk",
	"gcc_qupv3_wrap2_core_2x_clk",
	"gcc_qupv3_wrap2_core_clk",
	"gcc_qupv3_wrap2_qspi_s2_clk",
	"gcc_qupv3_wrap2_qspi_s3_clk",
	"gcc_qupv3_wrap2_qspi_s6_clk",
	"gcc_qupv3_wrap2_s0_clk",
	"gcc_qupv3_wrap2_s1_clk",
	"gcc_qupv3_wrap2_s2_clk",
	"gcc_qupv3_wrap2_s3_clk",
	"gcc_qupv3_wrap2_s4_clk",
	"gcc_qupv3_wrap2_s5_clk",
	"gcc_qupv3_wrap2_s6_clk",
	"gcc_qupv3_wrap2_s7_clk",
	"gcc_qupv3_wrap_0_m_ahb_clk",
	"gcc_qupv3_wrap_0_s_ahb_clk",
	"gcc_qupv3_wrap_1_m_ahb_clk",
	"gcc_qupv3_wrap_1_s_ahb_clk",
	"gcc_qupv3_wrap_2_m_ahb_clk",
	"gcc_qupv3_wrap_2_s_ahb_clk",
	"gcc_sdcc2_ahb_clk",
	"gcc_sdcc2_apps_clk",
	"gcc_sdcc4_ahb_clk",
	"gcc_sdcc4_apps_clk",
	"gcc_ufs_phy_ahb_clk",
	"gcc_ufs_phy_axi_clk",
	"gcc_ufs_phy_ice_core_clk",
	"gcc_ufs_phy_phy_aux_clk",
	"gcc_ufs_phy_rx_symbol_0_clk",
	"gcc_ufs_phy_rx_symbol_1_clk",
	"gcc_ufs_phy_tx_symbol_0_clk",
	"gcc_ufs_phy_unipro_core_clk",
	"gcc_usb20_master_clk",
	"gcc_usb20_mock_utmi_clk",
	"gcc_usb20_sleep_clk",
	"gcc_usb30_mp_master_clk",
	"gcc_usb30_mp_mock_utmi_clk",
	"gcc_usb30_mp_sleep_clk",
	"gcc_usb30_prim_master_clk",
	"gcc_usb30_prim_mock_utmi_clk",
	"gcc_usb30_prim_sleep_clk",
	"gcc_usb30_sec_master_clk",
	"gcc_usb30_sec_mock_utmi_clk",
	"gcc_usb30_sec_sleep_clk",
	"gcc_usb30_tert_master_clk",
	"gcc_usb30_tert_mock_utmi_clk",
	"gcc_usb30_tert_sleep_clk",
	"gcc_usb3_mp_phy_aux_clk",
	"gcc_usb3_mp_phy_com_aux_clk",
	"gcc_usb3_mp_phy_pipe_0_clk",
	"gcc_usb3_mp_phy_pipe_1_clk",
	"gcc_usb3_prim_phy_aux_clk",
	"gcc_usb3_prim_phy_com_aux_clk",
	"gcc_usb3_prim_phy_pipe_clk",
	"gcc_usb3_sec_phy_aux_clk",
	"gcc_usb3_sec_phy_com_aux_clk",
	"gcc_usb3_sec_phy_pipe_clk",
	"gcc_usb3_tert_phy_aux_clk",
	"gcc_usb3_tert_phy_com_aux_clk",
	"gcc_usb3_tert_phy_pipe_clk",
	"gcc_usb4_0_cfg_ahb_clk",
	"gcc_usb4_0_dp0_clk",
	"gcc_usb4_0_dp1_clk",
	"gcc_usb4_0_master_clk",
	"gcc_usb4_0_phy_p2rr2p_pipe_clk",
	"gcc_usb4_0_phy_pcie_pipe_clk",
	"gcc_usb4_0_phy_rx0_clk",
	"gcc_usb4_0_phy_rx1_clk",
	"gcc_usb4_0_phy_usb_pipe_clk",
	"gcc_usb4_0_sb_if_clk",
	"gcc_usb4_0_sys_clk",
	"gcc_usb4_0_tmu_clk",
	"gcc_usb4_0_uc_hrr_clk",
	"gcc_usb4_1_cfg_ahb_clk",
	"gcc_usb4_1_dp0_clk",
	"gcc_usb4_1_dp1_clk",
	"gcc_usb4_1_master_clk",
	"gcc_usb4_1_phy_p2rr2p_pipe_clk",
	"gcc_usb4_1_phy_pcie_pipe_clk",
	"gcc_usb4_1_phy_rx0_clk",
	"gcc_usb4_1_phy_rx1_clk",
	"gcc_usb4_1_phy_usb_pipe_clk",
	"gcc_usb4_1_sb_if_clk",
	"gcc_usb4_1_sys_clk",
	"gcc_usb4_1_tmu_clk",
	"gcc_usb4_1_uc_hrr_clk",
	"gcc_usb4_2_cfg_ahb_clk",
	"gcc_usb4_2_dp0_clk",
	"gcc_usb4_2_dp1_clk",
	"gcc_usb4_2_master_clk",
	"gcc_usb4_2_phy_p2rr2p_pipe_clk",
	"gcc_usb4_2_phy_pcie_pipe_clk",
	"gcc_usb4_2_phy_rx0_clk",
	"gcc_usb4_2_phy_rx1_clk",
	"gcc_usb4_2_phy_usb_pipe_clk",
	"gcc_usb4_2_sb_if_clk",
	"gcc_usb4_2_sys_clk",
	"gcc_usb4_2_tmu_clk",
	"gcc_usb4_2_uc_hrr_clk",
	"gcc_video_axi0_clk",
	"gcc_video_axi0c_clk",
	"gcc_video_axi1_clk",
	"gpu_cc_debug_mux",
	"mc_cc0_debug_mux",
	"mc_cc1_debug_mux",
	"measure_only_cnoc_clk",
	"measure_only_snoc_clk",
	"video_cc_debug_mux",
};

static int gcc_debug_mux_sels[] = {
	0x252,		/* apss_cc_debug_mux */
	0xC7,		/* cam_cc_debug_mux */
	0xCB,		/* disp_cc_debug_mux */
	0xDB,		/* eva_cc_debug_mux */
	0x87,		/* gcc_aggre_noc_pcie_3a_west_sf_axi_clk */
	0x88,		/* gcc_aggre_noc_pcie_3b_west_sf_axi_clk */
	0x89,		/* gcc_aggre_noc_pcie_4_west_sf_axi_clk */
	0x8A,		/* gcc_aggre_noc_pcie_5_east_sf_axi_clk */
	0x8B,		/* gcc_aggre_noc_pcie_6_west_sf_axi_clk */
	0x85,		/* gcc_aggre_ufs_phy_axi_clk */
	0x8F,		/* gcc_aggre_usb2_prim_axi_clk */
	0x8E,		/* gcc_aggre_usb3_mp_axi_clk */
	0x84,		/* gcc_aggre_usb3_prim_axi_clk */
	0x90,		/* gcc_aggre_usb3_sec_axi_clk */
	0x91,		/* gcc_aggre_usb3_tert_axi_clk */
	0x92,		/* gcc_aggre_usb4_0_axi_clk */
	0x93,		/* gcc_aggre_usb4_1_axi_clk */
	0x94,		/* gcc_aggre_usb4_2_axi_clk */
	0xC2,		/* gcc_camera_hf_axi_clk */
	0xC4,		/* gcc_camera_sf_axi_clk */
	0x71,		/* gcc_cfg_noc_pcie_anoc_ahb_clk */
	0x68,		/* gcc_cfg_noc_pcie_anoc_south_ahb_clk */
	0x66,		/* gcc_cfg_noc_usb2_prim_axi_clk */
	0x67,		/* gcc_cfg_noc_usb3_mp_axi_clk */
	0x55,		/* gcc_cfg_noc_usb3_prim_axi_clk */
	0x6B,		/* gcc_cfg_noc_usb3_sec_axi_clk */
	0x6C,		/* gcc_cfg_noc_usb3_tert_axi_clk */
	0x69,		/* gcc_cfg_noc_usb_anoc_ahb_clk */
	0x6A,		/* gcc_cfg_noc_usb_anoc_south_ahb_clk */
	0xCA,		/* gcc_disp_hf_axi_clk */
	0xD8,		/* gcc_eva_axi0_clk */
	0xD9,		/* gcc_eva_axi0c_clk */
	0x267,		/* gcc_gp1_clk */
	0x268,		/* gcc_gp2_clk */
	0x269,		/* gcc_gp3_clk */
	0x2CB,		/* gcc_gpu_gemnoc_gfx_clk */
	0x2CD,		/* gcc_gpu_gpll0_clk_src */
	0x2CE,		/* gcc_gpu_gpll0_div_clk_src */
	0x16B,		/* gcc_pcie_0_aux_clk */
	0x16A,		/* gcc_pcie_0_cfg_ahb_clk */
	0x169,		/* gcc_pcie_0_mstr_axi_clk */
	0x16D,		/* gcc_pcie_0_phy_rchng_clk */
	0x16C,		/* gcc_pcie_0_pipe_clk */
	0x168,		/* gcc_pcie_0_slv_axi_clk */
	0x167,		/* gcc_pcie_0_slv_q2a_axi_clk */
	0x162,		/* gcc_pcie_1_aux_clk */
	0x161,		/* gcc_pcie_1_cfg_ahb_clk */
	0x160,		/* gcc_pcie_1_mstr_axi_clk */
	0x164,		/* gcc_pcie_1_phy_rchng_clk */
	0x163,		/* gcc_pcie_1_pipe_clk */
	0x15F,		/* gcc_pcie_1_slv_axi_clk */
	0x15E,		/* gcc_pcie_1_slv_q2a_axi_clk */
	0x174,		/* gcc_pcie_2_aux_clk */
	0x173,		/* gcc_pcie_2_cfg_ahb_clk */
	0x172,		/* gcc_pcie_2_mstr_axi_clk */
	0x176,		/* gcc_pcie_2_phy_rchng_clk */
	0x175,		/* gcc_pcie_2_pipe_clk */
	0x171,		/* gcc_pcie_2_slv_axi_clk */
	0x170,		/* gcc_pcie_2_slv_q2a_axi_clk */
	0x26F,		/* gcc_pcie_3a_aux_clk */
	0x26E,		/* gcc_pcie_3a_cfg_ahb_clk */
	0x26D,		/* gcc_pcie_3a_mstr_axi_clk */
	0x272,		/* gcc_pcie_3a_phy_rchng_clk */
	0x270,		/* gcc_pcie_3a_pipe_clk */
	0x26C,		/* gcc_pcie_3a_slv_axi_clk */
	0x26B,		/* gcc_pcie_3a_slv_q2a_axi_clk */
	0x27B,		/* gcc_pcie_3b_aux_clk */
	0x27A,		/* gcc_pcie_3b_cfg_ahb_clk */
	0x279,		/* gcc_pcie_3b_mstr_axi_clk */
	0x27E,		/* gcc_pcie_3b_phy_rchng_clk */
	0x27C,		/* gcc_pcie_3b_pipe_clk */
	0x27D,		/* gcc_pcie_3b_pipe_div2_clk */
	0x278,		/* gcc_pcie_3b_slv_axi_clk */
	0x277,		/* gcc_pcie_3b_slv_q2a_axi_clk */
	0x297,		/* gcc_pcie_4_aux_clk */
	0x296,		/* gcc_pcie_4_cfg_ahb_clk */
	0x295,		/* gcc_pcie_4_mstr_axi_clk */
	0x29A,		/* gcc_pcie_4_phy_rchng_clk */
	0x298,		/* gcc_pcie_4_pipe_clk */
	0x299,		/* gcc_pcie_4_pipe_div2_clk */
	0x294,		/* gcc_pcie_4_slv_axi_clk */
	0x293,		/* gcc_pcie_4_slv_q2a_axi_clk */
	0x289,		/* gcc_pcie_5_aux_clk */
	0x288,		/* gcc_pcie_5_cfg_ahb_clk */
	0x287,		/* gcc_pcie_5_mstr_axi_clk */
	0x28C,		/* gcc_pcie_5_phy_rchng_clk */
	0x28A,		/* gcc_pcie_5_pipe_clk */
	0x28B,		/* gcc_pcie_5_pipe_div2_clk */
	0x286,		/* gcc_pcie_5_slv_axi_clk */
	0x285,		/* gcc_pcie_5_slv_q2a_axi_clk */
	0x2A3,		/* gcc_pcie_6_aux_clk */
	0x2A2,		/* gcc_pcie_6_cfg_ahb_clk */
	0x2A1,		/* gcc_pcie_6_mstr_axi_clk */
	0x2A6,		/* gcc_pcie_6_phy_rchng_clk */
	0x2A4,		/* gcc_pcie_6_pipe_clk */
	0x2A5,		/* gcc_pcie_6_pipe_div2_clk */
	0x2A0,		/* gcc_pcie_6_slv_axi_clk */
	0x29F,		/* gcc_pcie_6_slv_q2a_axi_clk */
	0x43,		/* gcc_pcie_noc_pwrctl_clk */
	0x42,		/* gcc_pcie_noc_qosgen_extref_clk */
	0x44,		/* gcc_pcie_noc_sf_center_clk */
	0x45,		/* gcc_pcie_noc_slave_sf_east_clk */
	0x46,		/* gcc_pcie_noc_slave_sf_west_clk */
	0x41,		/* gcc_pcie_noc_tsctr_clk */
	0x275,		/* gcc_pcie_phy_3a_aux_clk */
	0x283,		/* gcc_pcie_phy_3b_aux_clk */
	0x29D,		/* gcc_pcie_phy_4_aux_clk */
	0x291,		/* gcc_pcie_phy_5_aux_clk */
	0x2A9,		/* gcc_pcie_phy_6_aux_clk */
	0x1D6,		/* gcc_pdm2_clk */
	0x1D4,		/* gcc_pdm_ahb_clk */
	0x1D5,		/* gcc_pdm_xo4_clk */
	0x2DB,		/* gcc_qspi_cnoc_periph_ahb_clk */
	0x2DC,		/* gcc_qspi_core_clk */
	0x1A1,		/* gcc_qupv3_oob_core_2x_clk */
	0x1A0,		/* gcc_qupv3_oob_core_clk */
	0x1A5,		/* gcc_qupv3_oob_qspi_s0_clk */
	0x1A6,		/* gcc_qupv3_oob_qspi_s1_clk */
	0x1A2,		/* gcc_qupv3_oob_s0_clk */
	0x1A3,		/* gcc_qupv3_oob_s1_clk */
	0x19F,		/* gcc_qupv3_oob_s_ahb_clk */
	0x1A4,		/* gcc_qupv3_oob_tcxo_clk */
	0x1C8,		/* gcc_qupv3_wrap0_core_2x_clk */
	0x1C7,		/* gcc_qupv3_wrap0_core_clk */
	0x1D2,		/* gcc_qupv3_wrap0_qspi_s2_clk */
	0x1D3,		/* gcc_qupv3_wrap0_qspi_s3_clk */
	0x1D1,		/* gcc_qupv3_wrap0_qspi_s6_clk */
	0x1C9,		/* gcc_qupv3_wrap0_s0_clk */
	0x1CA,		/* gcc_qupv3_wrap0_s1_clk */
	0x1CB,		/* gcc_qupv3_wrap0_s2_clk */
	0x1CC,		/* gcc_qupv3_wrap0_s3_clk */
	0x1CD,		/* gcc_qupv3_wrap0_s4_clk */
	0x1CE,		/* gcc_qupv3_wrap0_s5_clk */
	0x1CF,		/* gcc_qupv3_wrap0_s6_clk */
	0x1D0,		/* gcc_qupv3_wrap0_s7_clk */
	0x1AA,		/* gcc_qupv3_wrap1_core_2x_clk */
	0x1A9,		/* gcc_qupv3_wrap1_core_clk */
	0x1B4,		/* gcc_qupv3_wrap1_qspi_s2_clk */
	0x1B5,		/* gcc_qupv3_wrap1_qspi_s3_clk */
	0x1B3,		/* gcc_qupv3_wrap1_qspi_s6_clk */
	0x1AB,		/* gcc_qupv3_wrap1_s0_clk */
	0x1AC,		/* gcc_qupv3_wrap1_s1_clk */
	0x1AD,		/* gcc_qupv3_wrap1_s2_clk */
	0x1AE,		/* gcc_qupv3_wrap1_s3_clk */
	0x1AF,		/* gcc_qupv3_wrap1_s4_clk */
	0x1B0,		/* gcc_qupv3_wrap1_s5_clk */
	0x1B1,		/* gcc_qupv3_wrap1_s6_clk */
	0x1B2,		/* gcc_qupv3_wrap1_s7_clk */
	0x1B9,		/* gcc_qupv3_wrap2_core_2x_clk */
	0x1B8,		/* gcc_qupv3_wrap2_core_clk */
	0x1C3,		/* gcc_qupv3_wrap2_qspi_s2_clk */
	0x1C4,		/* gcc_qupv3_wrap2_qspi_s3_clk */
	0x1C2,		/* gcc_qupv3_wrap2_qspi_s6_clk */
	0x1BA,		/* gcc_qupv3_wrap2_s0_clk */
	0x1BB,		/* gcc_qupv3_wrap2_s1_clk */
	0x1BC,		/* gcc_qupv3_wrap2_s2_clk */
	0x1BD,		/* gcc_qupv3_wrap2_s3_clk */
	0x1BE,		/* gcc_qupv3_wrap2_s4_clk */
	0x1BF,		/* gcc_qupv3_wrap2_s5_clk */
	0x1C0,		/* gcc_qupv3_wrap2_s6_clk */
	0x1C1,		/* gcc_qupv3_wrap2_s7_clk */
	0x1C5,		/* gcc_qupv3_wrap_0_m_ahb_clk */
	0x1C6,		/* gcc_qupv3_wrap_0_s_ahb_clk */
	0x1A7,		/* gcc_qupv3_wrap_1_m_ahb_clk */
	0x1A8,		/* gcc_qupv3_wrap_1_s_ahb_clk */
	0x1B6,		/* gcc_qupv3_wrap_2_m_ahb_clk */
	0x1B7,		/* gcc_qupv3_wrap_2_s_ahb_clk */
	0x199,		/* gcc_sdcc2_ahb_clk */
	0x198,		/* gcc_sdcc2_apps_clk */
	0x19C,		/* gcc_sdcc4_ahb_clk */
	0x19B,		/* gcc_sdcc4_apps_clk */
	0x2AB,		/* gcc_ufs_phy_ahb_clk */
	0x2AA,		/* gcc_ufs_phy_axi_clk */
	0x2B2,		/* gcc_ufs_phy_ice_core_clk */
	0x2B3,		/* gcc_ufs_phy_phy_aux_clk */
	0x2AE,		/* gcc_ufs_phy_rx_symbol_0_clk */
	0x2B4,		/* gcc_ufs_phy_rx_symbol_1_clk */
	0x2AD,		/* gcc_ufs_phy_tx_symbol_0_clk */
	0x2B1,		/* gcc_ufs_phy_unipro_core_clk */
	0x116,		/* gcc_usb20_master_clk */
	0x118,		/* gcc_usb20_mock_utmi_clk */
	0x117,		/* gcc_usb20_sleep_clk */
	0x10B,		/* gcc_usb30_mp_master_clk */
	0x10D,		/* gcc_usb30_mp_mock_utmi_clk */
	0x10C,		/* gcc_usb30_mp_sleep_clk */
	0x179,		/* gcc_usb30_prim_master_clk */
	0x17C,		/* gcc_usb30_prim_mock_utmi_clk */
	0x17B,		/* gcc_usb30_prim_sleep_clk */
	0x182,		/* gcc_usb30_sec_master_clk */
	0x184,		/* gcc_usb30_sec_mock_utmi_clk */
	0x183,		/* gcc_usb30_sec_sleep_clk */
	0x18A,		/* gcc_usb30_tert_master_clk */
	0x18C,		/* gcc_usb30_tert_mock_utmi_clk */
	0x18B,		/* gcc_usb30_tert_sleep_clk */
	0x10E,		/* gcc_usb3_mp_phy_aux_clk */
	0x10F,		/* gcc_usb3_mp_phy_com_aux_clk */
	0x110,		/* gcc_usb3_mp_phy_pipe_0_clk */
	0x111,		/* gcc_usb3_mp_phy_pipe_1_clk */
	0x17D,		/* gcc_usb3_prim_phy_aux_clk */
	0x17E,		/* gcc_usb3_prim_phy_com_aux_clk */
	0x17F,		/* gcc_usb3_prim_phy_pipe_clk */
	0x185,		/* gcc_usb3_sec_phy_aux_clk */
	0x186,		/* gcc_usb3_sec_phy_com_aux_clk */
	0x187,		/* gcc_usb3_sec_phy_pipe_clk */
	0x18D,		/* gcc_usb3_tert_phy_aux_clk */
	0x18E,		/* gcc_usb3_tert_phy_com_aux_clk */
	0x18F,		/* gcc_usb3_tert_phy_pipe_clk */
	0x129,		/* gcc_usb4_0_cfg_ahb_clk */
	0x126,		/* gcc_usb4_0_dp0_clk */
	0x12F,		/* gcc_usb4_0_dp1_clk */
	0x121,		/* gcc_usb4_0_master_clk */
	0x12E,		/* gcc_usb4_0_phy_p2rr2p_pipe_clk */
	0x123,		/* gcc_usb4_0_phy_pcie_pipe_clk */
	0x12A,		/* gcc_usb4_0_phy_rx0_clk */
	0x12B,		/* gcc_usb4_0_phy_rx1_clk */
	0x128,		/* gcc_usb4_0_phy_usb_pipe_clk */
	0x122,		/* gcc_usb4_0_sb_if_clk */
	0x124,		/* gcc_usb4_0_sys_clk */
	0x127,		/* gcc_usb4_0_tmu_clk */
	0x125,		/* gcc_usb4_0_uc_hrr_clk */
	0x140,		/* gcc_usb4_1_cfg_ahb_clk */
	0x13D,		/* gcc_usb4_1_dp0_clk */
	0x146,		/* gcc_usb4_1_dp1_clk */
	0x138,		/* gcc_usb4_1_master_clk */
	0x145,		/* gcc_usb4_1_phy_p2rr2p_pipe_clk */
	0x13A,		/* gcc_usb4_1_phy_pcie_pipe_clk */
	0x141,		/* gcc_usb4_1_phy_rx0_clk */
	0x142,		/* gcc_usb4_1_phy_rx1_clk */
	0x13F,		/* gcc_usb4_1_phy_usb_pipe_clk */
	0x139,		/* gcc_usb4_1_sb_if_clk */
	0x13B,		/* gcc_usb4_1_sys_clk */
	0x13E,		/* gcc_usb4_1_tmu_clk */
	0x13C,		/* gcc_usb4_1_uc_hrr_clk */
	0x157,		/* gcc_usb4_2_cfg_ahb_clk */
	0x154,		/* gcc_usb4_2_dp0_clk */
	0x15D,		/* gcc_usb4_2_dp1_clk */
	0x14F,		/* gcc_usb4_2_master_clk */
	0x15C,		/* gcc_usb4_2_phy_p2rr2p_pipe_clk */
	0x151,		/* gcc_usb4_2_phy_pcie_pipe_clk */
	0x158,		/* gcc_usb4_2_phy_rx0_clk */
	0x159,		/* gcc_usb4_2_phy_rx1_clk */
	0x156,		/* gcc_usb4_2_phy_usb_pipe_clk */
	0x150,		/* gcc_usb4_2_sb_if_clk */
	0x152,		/* gcc_usb4_2_sys_clk */
	0x155,		/* gcc_usb4_2_tmu_clk */
	0x153,		/* gcc_usb4_2_uc_hrr_clk */
	0xD2,		/* gcc_video_axi0_clk */
	0xD3,		/* gcc_video_axi0c_clk */
	0xD4,		/* gcc_video_axi1_clk */
	0x2CA,		/* gpu_cc_debug_mux */
	0x210,		/* mc_cc0_debug_mux */
	0x217,		/* mc_cc1_debug_mux */
	0x4D,		/* measure_only_cnoc_clk */
	0xB,		/* measure_only_snoc_clk */
	0xD6,		/* video_cc_debug_mux */
};

static struct clk_debug_mux gcc_debug_mux = {
	.priv = &debug_mux_priv,
	.debug_offset = 0x87048,
	.post_div_offset = 0x87000,
	.cbcr_offset = 0x87004,
	.src_sel_mask = 0x3FF,
	.src_sel_shift = 0,
	.post_div_mask = 0xF,
	.post_div_shift = 0,
	.post_div_val = 2,
	.mux_sels = gcc_debug_mux_sels,
	.num_mux_sels = ARRAY_SIZE(gcc_debug_mux_sels),
	.hw.init = &(const struct clk_init_data){
		.name = "gcc_debug_mux",
		.ops = &clk_debug_mux_ops,
		.parent_names = gcc_debug_mux_parent_names,
		.num_parents = ARRAY_SIZE(gcc_debug_mux_parent_names),
	},
};

static const char *const gpu_cc_debug_mux_parent_names[] = {
	"gpu_cc_ahb_clk",
	"gpu_cc_cx_accu_shift_clk",
	"gpu_cc_cx_ff_clk",
	"gpu_cc_cx_gmu_clk",
	"gpu_cc_cxo_clk",
	"gpu_cc_demet_clk",
	"gpu_cc_dpm_clk",
	"gpu_cc_freq_measure_clk",
	"gpu_cc_gx_accu_shift_clk",
	"gpu_cc_gx_acd_ahb_ff_clk",
	"gpu_cc_gx_gmu_clk",
	"gpu_cc_gx_rcg_ahb_ff_clk",
	"gpu_cc_hub_aon_clk",
	"gpu_cc_hub_cx_int_clk",
	"gpu_cc_memnoc_gfx_clk",
	"gpu_cc_sleep_clk",
	"gx_clkctl_debug_mux",
	"measure_only_gpu_cc_cb_clk",
	"measure_only_gpu_cc_cxo_aon_clk",
	"measure_only_gpu_cc_gx_ahb_ff_clk",
	"measure_only_gpu_cc_rscc_hub_aon_clk",
	"measure_only_gpu_cc_rscc_xo_aon_clk",
};

static int gpu_cc_debug_mux_sels[] = {
	0x17,		/* gpu_cc_ahb_clk */
	0x24,		/* gpu_cc_cx_accu_shift_clk */
	0x20,		/* gpu_cc_cx_ff_clk */
	0x1D,		/* gpu_cc_cx_gmu_clk */
	0x1E,		/* gpu_cc_cxo_clk */
	0x10,		/* gpu_cc_demet_clk */
	0x25,		/* gpu_cc_dpm_clk */
	0xF,		/* gpu_cc_freq_measure_clk */
	0x15,		/* gpu_cc_gx_accu_shift_clk */
	0x13,		/* gpu_cc_gx_acd_ahb_ff_clk */
	0x11,		/* gpu_cc_gx_gmu_clk */
	0x14,		/* gpu_cc_gx_rcg_ahb_ff_clk */
	0x2C,		/* gpu_cc_hub_aon_clk */
	0x1F,		/* gpu_cc_hub_cx_int_clk */
	0x21,		/* gpu_cc_memnoc_gfx_clk */
	0x1B,		/* gpu_cc_sleep_clk */
	0xB,		/* gx_clkctl_debug_mux */
	0x2A,		/* measure_only_gpu_cc_cb_clk */
	0xE,		/* measure_only_gpu_cc_cxo_aon_clk */
	0x12,		/* measure_only_gpu_cc_gx_ahb_ff_clk */
	0x2B,		/* measure_only_gpu_cc_rscc_hub_aon_clk */
	0xD,		/* measure_only_gpu_cc_rscc_xo_aon_clk */
};

static struct clk_debug_mux gpu_cc_debug_mux = {
	.priv = &debug_mux_priv,
	.debug_offset = 0x9564,
	.post_div_offset = 0x9270,
	.cbcr_offset = 0x9274,
	.src_sel_mask = 0xFF,
	.src_sel_shift = 0,
	.post_div_mask = 0xF,
	.post_div_shift = 0,
	.post_div_val = 2,
	.mux_sels = gpu_cc_debug_mux_sels,
	.num_mux_sels = ARRAY_SIZE(gpu_cc_debug_mux_sels),
	.hw.init = &(const struct clk_init_data){
		.name = "gpu_cc_debug_mux",
		.ops = &clk_debug_mux_ops,
		.parent_names = gpu_cc_debug_mux_parent_names,
		.num_parents = ARRAY_SIZE(gpu_cc_debug_mux_parent_names),
	},
};

static const char *const gx_clkctl_debug_mux_parent_names[] = {
	"measure_only_gx_clkctl_acd_ahb_ff_clk",
	"measure_only_gx_clkctl_ahb_ff_clk",
	"measure_only_gx_clkctl_cxo_clk",
	"measure_only_gx_clkctl_gfx3d_clk",
	"measure_only_gx_clkctl_gfx3d_rdvm_clk",
	"measure_only_gx_clkctl_xo_clk",
};

static int gx_clkctl_debug_mux_sels[] = {
	0x11,		/* measure_only_gx_clkctl_acd_ahb_ff_clk */
	0x10,		/* measure_only_gx_clkctl_ahb_ff_clk */
	0x4,		/* measure_only_gx_clkctl_cxo_clk */
	0x3,		/* measure_only_gx_clkctl_gfx3d_clk */
	0x6,		/* measure_only_gx_clkctl_gfx3d_rdvm_clk */
	0x1,		/* measure_only_gx_clkctl_xo_clk */
};

static struct clk_debug_mux gx_clkctl_debug_mux = {
	.priv = &debug_mux_priv,
	.debug_offset = 0x4144,
	.post_div_offset = U32_MAX,
	.cbcr_offset = 0x4088,
	.src_sel_mask = 0xFF,
	.src_sel_shift = 0,
	.post_div_mask = 0x1,
	.post_div_shift = 0,
	.post_div_val = 1,
	.mux_sels = gx_clkctl_debug_mux_sels,
	.num_mux_sels = ARRAY_SIZE(gx_clkctl_debug_mux_sels),
	.hw.init = &(const struct clk_init_data){
		.name = "gx_clkctl_debug_mux",
		.ops = &clk_debug_mux_ops,
		.parent_names = gx_clkctl_debug_mux_parent_names,
		.num_parents = ARRAY_SIZE(gx_clkctl_debug_mux_parent_names),
	},
};

static const char *const video_cc_debug_mux_parent_names[] = {
	"measure_only_video_cc_ahb_clk",
	"measure_only_video_cc_sleep_clk",
	"measure_only_video_cc_xo_clk",
	"video_cc_mvs0_clk",
	"video_cc_mvs0_freerun_clk",
	"video_cc_mvs0_shift_clk",
	"video_cc_mvs0c_clk",
	"video_cc_mvs0c_freerun_clk",
	"video_cc_mvs0c_shift_clk",
	"video_cc_mvs1_clk",
	"video_cc_mvs1_freerun_clk",
	"video_cc_mvs1_shift_clk",
};

static int video_cc_debug_mux_sels[] = {
	0xC,		/* measure_only_video_cc_ahb_clk */
	0x11,		/* measure_only_video_cc_sleep_clk */
	0xD,		/* measure_only_video_cc_xo_clk */
	0x4,		/* video_cc_mvs0_clk */
	0x5,		/* video_cc_mvs0_freerun_clk */
	0xE,		/* video_cc_mvs0_shift_clk */
	0x1,		/* video_cc_mvs0c_clk */
	0x2,		/* video_cc_mvs0c_freerun_clk */
	0x10,		/* video_cc_mvs0c_shift_clk */
	0x8,		/* video_cc_mvs1_clk */
	0x9,		/* video_cc_mvs1_freerun_clk */
	0xF,		/* video_cc_mvs1_shift_clk */
};

static struct clk_debug_mux video_cc_debug_mux = {
	.priv = &debug_mux_priv,
	.debug_offset = 0x9A4C,
	.post_div_offset = 0x80E4,
	.cbcr_offset = 0x80E8,
	.src_sel_mask = 0x3F,
	.src_sel_shift = 0,
	.post_div_mask = 0xF,
	.post_div_shift = 0,
	.post_div_val = 3,
	.mux_sels = video_cc_debug_mux_sels,
	.num_mux_sels = ARRAY_SIZE(video_cc_debug_mux_sels),
	.hw.init = &(const struct clk_init_data){
		.name = "video_cc_debug_mux",
		.ops = &clk_debug_mux_ops,
		.parent_names = video_cc_debug_mux_parent_names,
		.num_parents = ARRAY_SIZE(video_cc_debug_mux_parent_names),
	},
};

static const char *const mc_cc0_debug_mux_parent_names[] = {
	"measure_only_mccc0_clk",
};

static const char *const mc_cc1_debug_mux_parent_names[] = {
	"measure_only_mccc1_clk",
};

static struct clk_debug_mux mc_cc0_debug_mux = {
	.period_offset = 0x50,
	.hw.init = &(struct clk_init_data){
		.name = "mc_cc0_debug_mux",
		.ops = &clk_debug_mux_ops,
		.parent_names = mc_cc0_debug_mux_parent_names,
		.num_parents = ARRAY_SIZE(mc_cc0_debug_mux_parent_names),
	},
};

static struct clk_debug_mux mc_cc1_debug_mux = {
	.period_offset = 0x50,
	.hw.init = &(struct clk_init_data){
		.name = "mc_cc1_debug_mux",
		.ops = &clk_debug_mux_ops,
		.parent_names = mc_cc1_debug_mux_parent_names,
		.num_parents = ARRAY_SIZE(mc_cc1_debug_mux_parent_names),
	},
};

static struct mux_regmap_names mux_list[] = {
	{ .mux = &mc_cc0_debug_mux, .regmap_name = "qcom,mccc0" },
	{ .mux = &mc_cc1_debug_mux, .regmap_name = "qcom,mccc1" },
	{ .mux = &video_cc_debug_mux, .regmap_name = "qcom,videocc" },
	{ .mux = &gx_clkctl_debug_mux, .regmap_name = "qcom,gxclkctl" },
	{ .mux = &gpu_cc_debug_mux, .regmap_name = "qcom,gpucc" },
	{ .mux = &eva_cc_debug_mux, .regmap_name = "qcom,evacc" },
	{ .mux = &disp_cc_debug_mux, .regmap_name = "qcom,dispcc" },
	{ .mux = &cam_cc_debug_mux, .regmap_name = "qcom,camcc" },
	{ .mux = &apss_cc_debug_mux, .regmap_name = "qcom,apsscc" },
	{ .mux = &gcc_debug_mux, .regmap_name = "qcom,gcc" },
};

static struct clk_dummy measure_only_cam_cc_gdsc_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_cam_cc_gdsc_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_cam_cc_sleep_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_cam_cc_sleep_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_cnoc_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_cnoc_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_disp_cc_sleep_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_disp_cc_sleep_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_disp_cc_xo_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_disp_cc_xo_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_eva_cc_ahb_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_eva_cc_ahb_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_eva_cc_sleep_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_eva_cc_sleep_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_eva_cc_xo_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_eva_cc_xo_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_gpu_cc_cb_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_gpu_cc_cb_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_gpu_cc_cxo_aon_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_gpu_cc_cxo_aon_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_gpu_cc_gx_ahb_ff_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_gpu_cc_gx_ahb_ff_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_gx_clkctl_acd_ahb_ff_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_gx_clkctl_acd_ahb_ff_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_gx_clkctl_ahb_ff_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_gx_clkctl_ahb_ff_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_gx_clkctl_cxo_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_gx_clkctl_cxo_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_gx_clkctl_gfx3d_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_gx_clkctl_gfx3d_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_gx_clkctl_gfx3d_rdvm_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_gx_clkctl_gfx3d_rdvm_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_gx_clkctl_xo_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_gx_clkctl_xo_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_gpu_cc_rscc_hub_aon_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_gpu_cc_rscc_hub_aon_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_gpu_cc_rscc_xo_aon_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_gpu_cc_rscc_xo_aon_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_mccc0_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_mccc0_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_mccc1_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_mccc1_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_ncc0_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_ncc0_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_ncc1_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_ncc1_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_ncc2_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_ncc2_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_snoc_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_snoc_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_video_cc_ahb_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_video_cc_ahb_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_video_cc_sleep_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_video_cc_sleep_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_dummy measure_only_video_cc_xo_clk = {
	.rrate = 1000,
	.hw.init = &(const struct clk_init_data){
		.name = "measure_only_video_cc_xo_clk",
		.ops = &clk_dummy_ops,
	},
};

static struct clk_hw *debugcc_glymur_hws[] = {
	&measure_only_cam_cc_gdsc_clk.hw,
	&measure_only_cam_cc_sleep_clk.hw,
	&measure_only_cnoc_clk.hw,
	&measure_only_disp_cc_sleep_clk.hw,
	&measure_only_disp_cc_xo_clk.hw,
	&measure_only_eva_cc_ahb_clk.hw,
	&measure_only_eva_cc_sleep_clk.hw,
	&measure_only_eva_cc_xo_clk.hw,
	&measure_only_gpu_cc_cb_clk.hw,
	&measure_only_gpu_cc_cxo_aon_clk.hw,
	&measure_only_gpu_cc_gx_ahb_ff_clk.hw,
	&measure_only_gpu_cc_rscc_hub_aon_clk.hw,
	&measure_only_gpu_cc_rscc_xo_aon_clk.hw,
	&measure_only_gx_clkctl_acd_ahb_ff_clk.hw,
	&measure_only_gx_clkctl_ahb_ff_clk.hw,
	&measure_only_gx_clkctl_cxo_clk.hw,
	&measure_only_gx_clkctl_gfx3d_clk.hw,
	&measure_only_gx_clkctl_gfx3d_rdvm_clk.hw,
	&measure_only_gx_clkctl_xo_clk.hw,
	&measure_only_mccc0_clk.hw,
	&measure_only_mccc1_clk.hw,
	&measure_only_ncc0_clk.hw,
	&measure_only_ncc1_clk.hw,
	&measure_only_ncc2_clk.hw,
	&measure_only_snoc_clk.hw,
	&measure_only_video_cc_ahb_clk.hw,
	&measure_only_video_cc_sleep_clk.hw,
	&measure_only_video_cc_xo_clk.hw,
};

static const struct of_device_id clk_debug_match_table[] = {
	{ .compatible = "qcom,glymur-debugcc" },
	{ }
};

static int clk_debug_glymur_probe(struct platform_device *pdev)
{
	struct clk *clk;
	int ret = 0, i;

	BUILD_BUG_ON(ARRAY_SIZE(apss_cc_debug_mux_parent_names) !=
		ARRAY_SIZE(apss_cc_debug_mux_sels));
	BUILD_BUG_ON(ARRAY_SIZE(cam_cc_debug_mux_parent_names) !=
		ARRAY_SIZE(cam_cc_debug_mux_sels));
	BUILD_BUG_ON(ARRAY_SIZE(disp_cc_debug_mux_parent_names) !=
		ARRAY_SIZE(disp_cc_debug_mux_sels));
	BUILD_BUG_ON(ARRAY_SIZE(eva_cc_debug_mux_parent_names) !=
		ARRAY_SIZE(eva_cc_debug_mux_sels));
	BUILD_BUG_ON(ARRAY_SIZE(gcc_debug_mux_parent_names) != ARRAY_SIZE(gcc_debug_mux_sels));
	BUILD_BUG_ON(ARRAY_SIZE(gpu_cc_debug_mux_parent_names) !=
		ARRAY_SIZE(gpu_cc_debug_mux_sels));
	BUILD_BUG_ON(ARRAY_SIZE(gx_clkctl_debug_mux_parent_names) !=
		ARRAY_SIZE(gx_clkctl_debug_mux_sels));
	BUILD_BUG_ON(ARRAY_SIZE(video_cc_debug_mux_parent_names) !=
		ARRAY_SIZE(video_cc_debug_mux_sels));

	clk = devm_clk_get(&pdev->dev, "xo_clk_src");
	if (IS_ERR(clk)) {
		if (PTR_ERR(clk) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Unable to get xo clock\n");
		return PTR_ERR(clk);
	}

	debug_mux_priv.cxo = clk;

	for (i = 0; i < ARRAY_SIZE(mux_list); i++) {
		if (IS_ERR_OR_NULL(mux_list[i].mux->regmap)) {
			ret = map_debug_bases(pdev, mux_list[i].regmap_name,
					      mux_list[i].mux);
			if (ret == -EBADR)
				continue;
			else if (ret)
				return ret;
		}
	}

	for (i = 0; i < ARRAY_SIZE(debugcc_glymur_hws); i++) {
		clk = devm_clk_register(&pdev->dev, debugcc_glymur_hws[i]);
		if (IS_ERR(clk)) {
			dev_err(&pdev->dev, "Unable to register %s, err:(%ld)\n",
				qcom_clk_hw_get_name(debugcc_glymur_hws[i]),
				PTR_ERR(clk));
			return PTR_ERR(clk);
		}
	}

	for (i = 0; i < ARRAY_SIZE(mux_list); i++) {
		ret = devm_clk_register_debug_mux(&pdev->dev, mux_list[i].mux);
		if (ret) {
			dev_err(&pdev->dev, "Unable to register mux clk %s, err:(%d)\n",
				qcom_clk_hw_get_name(&mux_list[i].mux->hw),
				ret);
			return ret;
		}
	}

	ret = clk_debug_measure_register(&gcc_debug_mux.hw);
	if (ret) {
		dev_err(&pdev->dev, "Could not register Measure clocks\n");
		return ret;
	}

	dev_info(&pdev->dev, "Registered debug measure clocks\n");

	return ret;
}

static struct platform_driver clk_debug_driver = {
	.probe = clk_debug_glymur_probe,
	.driver = {
		.name = "glymur-debugcc",
		.of_match_table = clk_debug_match_table,
	},
};

static int __init clk_debug_glymur_init(void)
{
	return platform_driver_register(&clk_debug_driver);
}
fs_initcall(clk_debug_glymur_init);

MODULE_DESCRIPTION("QTI DEBUG CC Glymur Driver");
MODULE_LICENSE("GPL");
