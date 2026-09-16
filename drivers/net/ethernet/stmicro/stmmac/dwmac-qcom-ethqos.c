// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018-19, Linaro Limited

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/pcs-xpcs-qcom.h>
#include <linux/of_irq.h>
#include <linux/irqdomain.h>

#include "stmmac.h"
#include "stmmac_platform.h"
#include <linux/iopoll.h>

#define DMA_BUS_MODE			0x00001000
#define DMA_BUS_MODE_SFT_RESET		(0x1 << 0)

#define RGMII_IO_MACRO_CONFIG		0x0
#define SDCC_HC_REG_DLL_CONFIG		0x4
#define SDCC_TEST_CTL			0x8
#define SDCC_HC_REG_DDR_CONFIG		0xC
#define SDCC_HC_REG_DLL_CONFIG2		0x10
#define SDC4_STATUS			0x14
#define SDCC_USR_CTL			0x18
#define RGMII_IO_MACRO_CONFIG2		0x1C
#define RGMII_IO_MACRO_DEBUG1		0x20
#define EMAC_SYSTEM_LOW_POWER_DEBUG	0x28
#define EMAC_WRAPPER_SGMII_PHY_CNTRL1	0xf4
#define RGMII_IO_MACRO_BYPASS 0x16C
#define EMAC_WRAPPER_SGMII_PHY_CNTRL0 0x170
#define EMAC_WRAPPER_USXGMII_MUX_SEL 0x1D0
#define RGMII_IO_MACRO_SCRATCH_2		0x44
#define EMAC_WRAPPER_SGMII_PHY_CNTRL1_V4 0x174
#define MACSEC_CTRL0_OFFSET			0x0

/* RGMII_IO_MACRO_CONFIG fields */
#define RGMII_CONFIG_FUNC_CLK_EN		BIT(30)
#define RGMII_CONFIG_POS_NEG_DATA_SEL		BIT(23)
#define RGMII_CONFIG_MAX_SPD_PRG_9		GENMASK(16, 8)
#define RGMII_CONFIG_MAX_SPD_PRG_2		GENMASK(7, 6)
#define RGMII_CONFIG_INTF_SEL			GENMASK(5, 4)
#define RGMII_CONFIG_BYPASS_TX_ID_EN		BIT(3)
#define RGMII_CONFIG_LOOPBACK_EN		BIT(2)
#define RGMII_CONFIG_PROG_SWAP			BIT(1)
#define RGMII_CONFIG_DDR_MODE			BIT(0)
#define RGMII_CONFIG_SGMII_CLK_DVDR		GENMASK(18, 10)

#define RGMII_CONFIG_MAX_SPD_PRG_9_V4		GENMASK(18, 10)
#define RGMII_CONFIG_MAX_SPD_PRG_2_V4		GENMASK(9, 6)

/*RGMII IO MACRO BYPASS fields*/
#define RGMII_BYPASS_EN		BIT(0)

/* SDCC_HC_REG_DLL_CONFIG fields */
#define SDCC_DLL_CONFIG_DLL_RST			BIT(30)
#define SDCC_DLL_CONFIG_PDN			BIT(29)
#define SDCC_DLL_CONFIG_MCLK_FREQ		GENMASK(26, 24)
#define SDCC_DLL_CONFIG_CDR_SELEXT		GENMASK(23, 20)
#define SDCC_DLL_CONFIG_CDR_EXT_EN		BIT(19)
#define SDCC_DLL_CONFIG_CK_OUT_EN		BIT(18)
#define SDCC_DLL_CONFIG_CDR_EN			BIT(17)
#define SDCC_DLL_CONFIG_DLL_EN			BIT(16)
#define SDCC_DLL_MCLK_GATING_EN			BIT(5)
#define SDCC_DLL_CDR_FINE_PHASE			GENMASK(3, 2)

/* SDCC_HC_REG_DDR_CONFIG fields */
#define SDCC_DDR_CONFIG_PRG_DLY_EN		BIT(31)
#define SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY	GENMASK(26, 21)
#define SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_CODE	GENMASK(29, 27)
#define SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_EN	BIT(30)
#define SDCC_DDR_CONFIG_TCXO_CYCLES_CNT		GENMASK(11, 9)
#define SDCC_DDR_CONFIG_PRG_RCLK_DLY		GENMASK(8, 0)

/* SDCC_HC_REG_DLL_CONFIG2 fields */
#define SDCC_DLL_CONFIG2_DLL_CLOCK_DIS		BIT(21)
#define SDCC_DLL_CONFIG2_MCLK_FREQ_CALC		GENMASK(17, 10)
#define SDCC_DLL_CONFIG2_DDR_TRAFFIC_INIT_SEL	GENMASK(3, 2)
#define SDCC_DLL_CONFIG2_DDR_TRAFFIC_INIT_SW	BIT(1)
#define SDCC_DLL_CONFIG2_DDR_CAL_EN		BIT(0)

/* SDC4_STATUS bits */
#define SDC4_STATUS_DLL_LOCK			BIT(7)

/* RGMII_IO_MACRO_CONFIG2 fields */
#define RGMII_CONFIG2_RSVD_CONFIG15		GENMASK(31, 17)
#define RGMII_CONFIG2_MODE_EN_VIA_GMII        BIT(21)
#define RGMII_CONFIG2_MAX_SPD_PRG_3		GENMASK(20, 17)
#define RGMII_CONFIG2_RGMII_CLK_SEL_CFG		BIT(16)
#define RGMII_CONFIG2_TX_TO_RX_LOOPBACK_EN	BIT(13)
#define RGMII_CONFIG2_CLK_DIVIDE_SEL		BIT(12)
#define RGMII_CONFIG2_RX_PROG_SWAP		BIT(7)
#define RGMII_CONFIG2_DATA_DIVIDE_CLK_SEL	BIT(6)
#define RGMII_CONFIG2_TX_CLK_PHASE_SHIFT_EN	BIT(5)

/* MAC_CTRL_REG bits */
#define ETHQOS_MAC_CTRL_SPEED_MODE		BIT(14)
#define ETHQOS_MAC_CTRL_PORT_SEL		BIT(15)

/* EMAC_WRAPPER_SGMII_PHY_CNTRL0 bits */
#define SGMII_PHY_CNTRL0_2P5G_1G_CLK_SEL		GENMASK(6, 5)

/* EMAC_WRAPPER_SGMII_PHY_CNTRL1 bits */
#define SGMII_PHY_CNTRL1_RGMII_SGMII_CLK_MUX_SEL BIT(0)
#define SGMII_PHY_CNTRL1_USXGMII_GMII_MASTER_CLK_MUX_SEL BIT(4)
#define SGMII_PHY_CNTRL1_SGMII_TX_TO_RX_LOOPBACK_EN	BIT(3)

/* EMAC_WRAPPER_USXGMII_MUX_SEL bits */
#define USXGMII_CLK_BLK_GMII_CLK_BLK_SEL BIT(1)
#define USXGMII_CLK_BLK_CLK_EN BIT(0)

/* RGMII_IO_MACRO_SCRATCH_2 bits */
#define RGMII_SCRATCH2_MAX_SPD_PRG_4		GENMASK(5, 2)
#define RGMII_SCRATCH2_MAX_SPD_PRG_5		GENMASK(9, 6)
#define RGMII_SCRATCH2_MAX_SPD_PRG_6		GENMASK(13, 10)

/* MACSEC WRAPPER bits */
#define MACSEC_BIT_DATA_BYPASS		BIT(2)
#define MACSEC_PWR_ON_RESET		BIT(9)

#define SGMII_10M_RX_CLK_DVDR			0x31

struct ethqos_emac_por {
	unsigned int offset;
	unsigned int value;
};

struct ethqos_emac_driver_data {
	const struct ethqos_emac_por *por;
	struct dwxgmac_addrs dwxgmac_addrs;
	unsigned int num_por;
	bool rgmii_config_loopback_en;
	bool has_emac_ge_3;
	const char *link_clk_name;
	u32 has_flags;
	u32 dma_addr_width;
	unsigned int axi_clk_rate;
	struct dwmac4_addrs dwmac4_addrs;
	bool needs_sgmii_loopback;
	bool has_hdma;
	bool has_macsec;
	bool has_io_macro_ge_4;
};

struct qcom_ethqos {
	struct platform_device *pdev;
	void __iomem *macsec_base;
	void __iomem *rgmii_base;
	void __iomem *mac_base;
	int (*configure_func)(struct qcom_ethqos *ethqos);

	unsigned int link_clk_rate;
	struct clk *link_clk;
	struct phy *serdes_phy;
	unsigned int speed;
	int serdes_speed;
	phy_interface_t phy_mode;
	int switch_reset_detect_irq;

	const struct ethqos_emac_por *por;
	unsigned int num_por;
	bool has_io_macro_ge_4;
	bool rgmii_config_loopback_en;
	bool has_emac_ge_3;
	bool has_macsec;
	bool needs_sgmii_loopback;
	bool needs_rx_prog_swap;
};

static int rgmii_readl(struct qcom_ethqos *ethqos, unsigned int offset)
{
	return readl(ethqos->rgmii_base + offset);
}

static void rgmii_writel(struct qcom_ethqos *ethqos,
			 int value, unsigned int offset)
{
	writel(value, ethqos->rgmii_base + offset);
}

static void rgmii_updatel(struct qcom_ethqos *ethqos,
			  int mask, int val, unsigned int offset)
{
	unsigned int temp;

	temp = rgmii_readl(ethqos, offset);
	temp = (temp & ~(mask)) | val;
	rgmii_writel(ethqos, temp, offset);
}

static void rgmii_dump(void *priv)
{
	struct qcom_ethqos *ethqos = priv;
	struct device *dev = &ethqos->pdev->dev;

	dev_dbg(dev, "Rgmii register dump\n");
	dev_dbg(dev, "RGMII_IO_MACRO_CONFIG: %x\n",
		rgmii_readl(ethqos, RGMII_IO_MACRO_CONFIG));
	dev_dbg(dev, "SDCC_HC_REG_DLL_CONFIG: %x\n",
		rgmii_readl(ethqos, SDCC_HC_REG_DLL_CONFIG));
	dev_dbg(dev, "SDCC_HC_REG_DDR_CONFIG: %x\n",
		rgmii_readl(ethqos, SDCC_HC_REG_DDR_CONFIG));
	dev_dbg(dev, "SDCC_HC_REG_DLL_CONFIG2: %x\n",
		rgmii_readl(ethqos, SDCC_HC_REG_DLL_CONFIG2));
	dev_dbg(dev, "SDC4_STATUS: %x\n",
		rgmii_readl(ethqos, SDC4_STATUS));
	dev_dbg(dev, "SDCC_USR_CTL: %x\n",
		rgmii_readl(ethqos, SDCC_USR_CTL));
	dev_dbg(dev, "RGMII_IO_MACRO_CONFIG2: %x\n",
		rgmii_readl(ethqos, RGMII_IO_MACRO_CONFIG2));
	dev_dbg(dev, "RGMII_IO_MACRO_DEBUG1: %x\n",
		rgmii_readl(ethqos, RGMII_IO_MACRO_DEBUG1));
	dev_dbg(dev, "EMAC_SYSTEM_LOW_POWER_DEBUG: %x\n",
		rgmii_readl(ethqos, EMAC_SYSTEM_LOW_POWER_DEBUG));
}

/* Clock rates */
#define RGMII_1000_NOM_CLK_FREQ			(250 * 1000 * 1000UL)
#define RGMII_ID_MODE_100_LOW_SVS_CLK_FREQ	 (50 * 1000 * 1000UL)
#define RGMII_ID_MODE_10_LOW_SVS_CLK_FREQ	  (5 * 1000 * 1000UL)

static void
ethqos_update_link_clk(struct qcom_ethqos *ethqos, unsigned int speed)
{
	if (!phy_interface_mode_is_rgmii(ethqos->phy_mode))
		return;

	switch (speed) {
	case SPEED_1000:
		ethqos->link_clk_rate =  RGMII_1000_NOM_CLK_FREQ;
		break;

	case SPEED_100:
		ethqos->link_clk_rate =  RGMII_ID_MODE_100_LOW_SVS_CLK_FREQ;
		break;

	case SPEED_10:
		ethqos->link_clk_rate =  RGMII_ID_MODE_10_LOW_SVS_CLK_FREQ;
		break;
	}

	clk_set_rate(ethqos->link_clk, ethqos->link_clk_rate);
}

static void
qcom_ethqos_set_sgmii_loopback(struct qcom_ethqos *ethqos, bool enable)
{
	if (ethqos->needs_sgmii_loopback &&
	    (ethqos->phy_mode == PHY_INTERFACE_MODE_2500BASEX ||
	     ethqos->phy_mode == PHY_INTERFACE_MODE_USXGMII))
		rgmii_updatel(ethqos,
			      SGMII_PHY_CNTRL1_SGMII_TX_TO_RX_LOOPBACK_EN,
			      enable ? SGMII_PHY_CNTRL1_SGMII_TX_TO_RX_LOOPBACK_EN : 0,
			      ethqos->has_io_macro_ge_4 ?
			      EMAC_WRAPPER_SGMII_PHY_CNTRL1_V4 :
			      EMAC_WRAPPER_SGMII_PHY_CNTRL1);
}

static void ethqos_set_func_clk_en(struct qcom_ethqos *ethqos)
{
	qcom_ethqos_set_sgmii_loopback(ethqos, true);
	rgmii_updatel(ethqos, RGMII_CONFIG_FUNC_CLK_EN,
		      RGMII_CONFIG_FUNC_CLK_EN, RGMII_IO_MACRO_CONFIG);
}

static const struct ethqos_emac_por emac_v2_3_0_por[] = {
	{ .offset = RGMII_IO_MACRO_CONFIG,	.value = 0x00C01343 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG,	.value = 0x2004642C },
	{ .offset = SDCC_HC_REG_DDR_CONFIG,	.value = 0x00000000 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG2,	.value = 0x00200000 },
	{ .offset = SDCC_USR_CTL,		.value = 0x00010800 },
	{ .offset = RGMII_IO_MACRO_CONFIG2,	.value = 0x00002060 },
};

static const struct ethqos_emac_driver_data emac_v2_3_0_data = {
	.por = emac_v2_3_0_por,
	.num_por = ARRAY_SIZE(emac_v2_3_0_por),
	.rgmii_config_loopback_en = true,
	.has_emac_ge_3 = false,
};

static const struct ethqos_emac_por emac_v2_1_0_por[] = {
	{ .offset = RGMII_IO_MACRO_CONFIG,	.value = 0x40C01343 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG,	.value = 0x2004642C },
	{ .offset = SDCC_HC_REG_DDR_CONFIG,	.value = 0x00000000 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG2,	.value = 0x00200000 },
	{ .offset = SDCC_USR_CTL,		.value = 0x00010800 },
	{ .offset = RGMII_IO_MACRO_CONFIG2,	.value = 0x00002060 },
};

static const struct ethqos_emac_driver_data emac_v2_1_0_data = {
	.por = emac_v2_1_0_por,
	.num_por = ARRAY_SIZE(emac_v2_1_0_por),
	.rgmii_config_loopback_en = false,
	.has_emac_ge_3 = false,
};

static const struct ethqos_emac_por emac_v2_3_1_por[] = {
	{ .offset = RGMII_IO_MACRO_CONFIG,	.value = 0x00C01343 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG,	.value = 0x2004642C },
	{ .offset = SDCC_HC_REG_DDR_CONFIG,	.value = 0x00000000 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG2,	.value = 0x00200000 },
	{ .offset = SDCC_USR_CTL,		.value = 0x00010800 },
	{ .offset = RGMII_IO_MACRO_CONFIG2,	.value = 0x00002060 },
};

static const struct ethqos_emac_driver_data emac_v2_3_1_data = {
	.por = emac_v2_3_1_por,
	.num_por = ARRAY_SIZE(emac_v2_3_1_por),
	.rgmii_config_loopback_en = true,
	.has_emac_ge_3 = false,
};

static const struct ethqos_emac_por emac_v3_0_0_por[] = {
	{ .offset = RGMII_IO_MACRO_CONFIG,	.value = 0x40c01343 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG,	.value = 0x2004642c },
	{ .offset = SDCC_HC_REG_DDR_CONFIG,	.value = 0x80040800 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG2,	.value = 0x00200000 },
	{ .offset = SDCC_USR_CTL,		.value = 0x00010800 },
	{ .offset = RGMII_IO_MACRO_CONFIG2,	.value = 0x00002060 },
};

static const struct ethqos_emac_driver_data emac_v3_0_0_data = {
	.por = emac_v3_0_0_por,
	.num_por = ARRAY_SIZE(emac_v3_0_0_por),
	.rgmii_config_loopback_en = false,
	.has_emac_ge_3 = true,
	.dwmac4_addrs = {
		.dma_chan = 0x00008100,
		.dma_chan_offset = 0x1000,
		.mtl_chan = 0x00008000,
		.mtl_chan_offset = 0x1000,
		.mtl_ets_ctrl = 0x00008010,
		.mtl_ets_ctrl_offset = 0x1000,
		.mtl_txq_weight = 0x00008018,
		.mtl_txq_weight_offset = 0x1000,
		.mtl_send_slp_cred = 0x0000801c,
		.mtl_send_slp_cred_offset = 0x1000,
		.mtl_high_cred = 0x00008020,
		.mtl_high_cred_offset = 0x1000,
		.mtl_low_cred = 0x00008024,
		.mtl_low_cred_offset = 0x1000,
	},
};

static const struct ethqos_emac_por emac_v4_0_0_por[] = {
	{ .offset = RGMII_IO_MACRO_CONFIG,	.value = 0x40c01343 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG,	.value = 0x2004642c },
	{ .offset = SDCC_HC_REG_DDR_CONFIG,	.value = 0x80040800 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG2,	.value = 0x00200000 },
	{ .offset = SDCC_USR_CTL,		.value = 0x00010800 },
	{ .offset = RGMII_IO_MACRO_CONFIG2,	.value = 0x00002060 },
};

static const struct ethqos_emac_por emac_v6_6_0_por[] = {
	{ .offset = RGMII_IO_MACRO_CONFIG,	.value = 0xC04D03 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG,	.value = 0x2004642C },
	{ .offset = SDCC_HC_REG_DDR_CONFIG,	.value = 0x80040800 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG2,	.value = 0x00200000 },
	{ .offset = SDCC_USR_CTL,		.value = 0x00010800 },
	{ .offset = RGMII_IO_MACRO_CONFIG2,	.value = 0x222060},
	{ .offset = RGMII_IO_MACRO_SCRATCH_2, .value = 0x4c },
};

static const struct ethqos_emac_por emac_v6_6_1_por[] = {
	{ .offset = RGMII_IO_MACRO_CONFIG,	.value = 0xC04D03 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG,	.value = 0x2004642C },
	{ .offset = SDCC_HC_REG_DDR_CONFIG,	.value = 0x80040800 },
	{ .offset = SDCC_HC_REG_DLL_CONFIG2,	.value = 0x00200000 },
	{ .offset = SDCC_USR_CTL,		.value = 0x00010800 },
	{ .offset = RGMII_IO_MACRO_CONFIG2,	.value = 0x222060},
	{ .offset = RGMII_IO_MACRO_SCRATCH_2, .value = 0x4c },
};

static const struct ethqos_emac_driver_data emac_v4_0_0_data = {
	.por = emac_v4_0_0_por,
	.num_por = ARRAY_SIZE(emac_v4_0_0_por),
	.rgmii_config_loopback_en = false,
	.has_emac_ge_3 = true,
	.link_clk_name = "phyaux",
	.has_flags = (STMMAC_FLAG_HAS_INTEGRATED_PCS | STMMAC_FLAG_SPH_DISABLE),
	.needs_sgmii_loopback = true,
	.dma_addr_width = 36,
	.dwmac4_addrs = {
		.dma_chan = 0x00008100,
		.dma_chan_offset = 0x1000,
		.mtl_chan = 0x00008000,
		.mtl_chan_offset = 0x1000,
		.mtl_ets_ctrl = 0x00008010,
		.mtl_ets_ctrl_offset = 0x1000,
		.mtl_txq_weight = 0x00008018,
		.mtl_txq_weight_offset = 0x1000,
		.mtl_send_slp_cred = 0x0000801c,
		.mtl_send_slp_cred_offset = 0x1000,
		.mtl_high_cred = 0x00008020,
		.mtl_high_cred_offset = 0x1000,
		.mtl_low_cred = 0x00008024,
		.mtl_low_cred_offset = 0x1000,
	},
};

static const struct ethqos_emac_driver_data emac_v6_6_0_data = {
	.por = emac_v6_6_0_por,
	.num_por = ARRAY_SIZE(emac_v6_6_0_por),
	.rgmii_config_loopback_en = false,
	.dma_addr_width = 40,
	.link_clk_name = "phyaux",
	.has_flags = STMMAC_FLAG_USE_THREADED_NAPI,
	.has_hdma = true,
	.needs_sgmii_loopback = true,
	.has_io_macro_ge_4 = true,
	.axi_clk_rate = 380000000,
	.dwxgmac_addrs = {
		.dma_even_chan_base  = 0x00008500,
		.dma_odd_chan_base = 0x00008580,
		.dma_chan_offset = 0x00001000,
		.mtl_chan_base = 0x00008000,
		.mtl_chan_offset =  0x00001000,
		.timestamp_base = 0x00007000,
		.pps_base = 0x00007080,
		.pps_offset = 0x10,
	},
};

/* emac_v6_6_1 is added because of the addition of new MACSEC
 * block and the flags associated with it.
 */
static const struct ethqos_emac_driver_data emac_v6_6_1_data = {
	.por = emac_v6_6_1_por,
	.num_por = ARRAY_SIZE(emac_v6_6_1_por),
	.rgmii_config_loopback_en = false,
	.dma_addr_width = 40,
	.link_clk_name = "phyaux",
	.has_flags = STMMAC_FLAG_USE_THREADED_NAPI,
	.has_hdma = true,
	.has_macsec = true,
	.needs_sgmii_loopback = true,
	.has_io_macro_ge_4 = true,
	.axi_clk_rate = 380000000,
	.dwxgmac_addrs = {
		.dma_even_chan_base  = 0x00008500,
		.dma_odd_chan_base = 0x00008580,
		.dma_chan_offset = 0x00001000,
		.mtl_chan_base = 0x00008000,
		.mtl_chan_offset =  0x00001000,
		.timestamp_base = 0x00007000,
		.pps_base = 0x00007080,
		.pps_offset = 0x10,
	},
};

static int ethqos_dll_configure(struct qcom_ethqos *ethqos)
{
	struct device *dev = &ethqos->pdev->dev;
	unsigned int val;
	int retry = 1000;

	/* Set CDR_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_CDR_EN,
		      SDCC_DLL_CONFIG_CDR_EN, SDCC_HC_REG_DLL_CONFIG);

	/* Set CDR_EXT_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_CDR_EXT_EN,
		      SDCC_DLL_CONFIG_CDR_EXT_EN, SDCC_HC_REG_DLL_CONFIG);

	/* Clear CK_OUT_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_CK_OUT_EN,
		      0, SDCC_HC_REG_DLL_CONFIG);

	/* Set DLL_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_DLL_EN,
		      SDCC_DLL_CONFIG_DLL_EN, SDCC_HC_REG_DLL_CONFIG);

	if (!ethqos->has_emac_ge_3) {
		rgmii_updatel(ethqos, SDCC_DLL_MCLK_GATING_EN,
			      0, SDCC_HC_REG_DLL_CONFIG);

		rgmii_updatel(ethqos, SDCC_DLL_CDR_FINE_PHASE,
			      0, SDCC_HC_REG_DLL_CONFIG);
	}

	/* Wait for CK_OUT_EN clear */
	do {
		val = rgmii_readl(ethqos, SDCC_HC_REG_DLL_CONFIG);
		val &= SDCC_DLL_CONFIG_CK_OUT_EN;
		if (!val)
			break;
		mdelay(1);
		retry--;
	} while (retry > 0);
	if (!retry)
		dev_err(dev, "Clear CK_OUT_EN timedout\n");

	/* Set CK_OUT_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_CK_OUT_EN,
		      SDCC_DLL_CONFIG_CK_OUT_EN, SDCC_HC_REG_DLL_CONFIG);

	/* Wait for CK_OUT_EN set */
	retry = 1000;
	do {
		val = rgmii_readl(ethqos, SDCC_HC_REG_DLL_CONFIG);
		val &= SDCC_DLL_CONFIG_CK_OUT_EN;
		if (val)
			break;
		mdelay(1);
		retry--;
	} while (retry > 0);
	if (!retry)
		dev_err(dev, "Set CK_OUT_EN timedout\n");

	/* Set DDR_CAL_EN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG2_DDR_CAL_EN,
		      SDCC_DLL_CONFIG2_DDR_CAL_EN, SDCC_HC_REG_DLL_CONFIG2);

	if (!ethqos->has_emac_ge_3) {
		rgmii_updatel(ethqos, SDCC_DLL_CONFIG2_DLL_CLOCK_DIS,
			      0, SDCC_HC_REG_DLL_CONFIG2);

		rgmii_updatel(ethqos, SDCC_DLL_CONFIG2_MCLK_FREQ_CALC,
			      0x1A << 10, SDCC_HC_REG_DLL_CONFIG2);

		rgmii_updatel(ethqos, SDCC_DLL_CONFIG2_DDR_TRAFFIC_INIT_SEL,
			      BIT(2), SDCC_HC_REG_DLL_CONFIG2);

		rgmii_updatel(ethqos, SDCC_DLL_CONFIG2_DDR_TRAFFIC_INIT_SW,
			      SDCC_DLL_CONFIG2_DDR_TRAFFIC_INIT_SW,
			      SDCC_HC_REG_DLL_CONFIG2);
	}

	return 0;
}

static int ethqos_rgmii_macro_init(struct qcom_ethqos *ethqos)
{
	struct device *dev = &ethqos->pdev->dev;
	int rx_prog_swap = 0;
	int phase_shift;
	int loopback;

	/* Determine if the PHY adds a 2 ns TX delay or the MAC handles it */
	if (ethqos->phy_mode == PHY_INTERFACE_MODE_RGMII_ID ||
	    ethqos->phy_mode == PHY_INTERFACE_MODE_RGMII_TXID)
		phase_shift = 0;
	else
		phase_shift = RGMII_CONFIG2_TX_CLK_PHASE_SHIFT_EN;

	/* Disable loopback mode */
	rgmii_updatel(ethqos, RGMII_CONFIG2_TX_TO_RX_LOOPBACK_EN,
		      0, RGMII_IO_MACRO_CONFIG2);

	/* Determine if this platform wants loopback enabled after programming */
	if (ethqos->rgmii_config_loopback_en)
		loopback = RGMII_CONFIG_LOOPBACK_EN;
	else
		loopback = 0;

	if (ethqos->needs_rx_prog_swap)
		rx_prog_swap = RGMII_CONFIG2_RX_PROG_SWAP;

	/* Select RGMII, write 0 to interface select */
	rgmii_updatel(ethqos, RGMII_CONFIG_INTF_SEL,
		      0, RGMII_IO_MACRO_CONFIG);

	switch (ethqos->speed) {
	case SPEED_1000:
		rgmii_updatel(ethqos, RGMII_CONFIG_DDR_MODE,
			      RGMII_CONFIG_DDR_MODE, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_BYPASS_TX_ID_EN,
			      0, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_POS_NEG_DATA_SEL,
			      RGMII_CONFIG_POS_NEG_DATA_SEL,
			      RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_PROG_SWAP,
			      RGMII_CONFIG_PROG_SWAP, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_DATA_DIVIDE_CLK_SEL,
			      0, RGMII_IO_MACRO_CONFIG2);

		rgmii_updatel(ethqos, RGMII_CONFIG2_TX_CLK_PHASE_SHIFT_EN,
			      phase_shift, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG2_RSVD_CONFIG15,
			      0, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP,
			      RGMII_CONFIG2_RX_PROG_SWAP,
			      RGMII_IO_MACRO_CONFIG2);

		/* PRG_RCLK_DLY = TCXO period * TCXO_CYCLES_CNT / 2 * RX delay ns,
		 * in practice this becomes PRG_RCLK_DLY = 52 * 4 / 2 * RX delay ns
		 */
		if (ethqos->has_emac_ge_3) {
			/* 0.9 ns */
			rgmii_updatel(ethqos, SDCC_DDR_CONFIG_PRG_RCLK_DLY,
				      115, SDCC_HC_REG_DDR_CONFIG);
		} else {
			/* 1.8 ns */
			rgmii_updatel(ethqos, SDCC_DDR_CONFIG_PRG_RCLK_DLY,
				      57, SDCC_HC_REG_DDR_CONFIG);
		}
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_PRG_DLY_EN,
			      SDCC_DDR_CONFIG_PRG_DLY_EN,
			      SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_LOOPBACK_EN,
			      loopback, RGMII_IO_MACRO_CONFIG);
		break;

	case SPEED_100:
		rgmii_updatel(ethqos, RGMII_CONFIG_DDR_MODE,
			      RGMII_CONFIG_DDR_MODE, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_BYPASS_TX_ID_EN,
			      RGMII_CONFIG_BYPASS_TX_ID_EN,
			      RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_POS_NEG_DATA_SEL,
			      0, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_PROG_SWAP,
			      0, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_DATA_DIVIDE_CLK_SEL,
			      0, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG2_TX_CLK_PHASE_SHIFT_EN,
			      phase_shift, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG_MAX_SPD_PRG_2,
			      BIT(6), RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_RSVD_CONFIG15,
			      0, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP, rx_prog_swap,
			      RGMII_IO_MACRO_CONFIG2);

		/* Write 0x5 to PRG_RCLK_DLY_CODE */
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_CODE,
			      (BIT(29) | BIT(27)), SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY,
			      SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY,
			      SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_EN,
			      SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_EN,
			      SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_LOOPBACK_EN,
			      loopback, RGMII_IO_MACRO_CONFIG);
		break;

	case SPEED_10:
		rgmii_updatel(ethqos, RGMII_CONFIG_DDR_MODE,
			      RGMII_CONFIG_DDR_MODE, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_BYPASS_TX_ID_EN,
			      RGMII_CONFIG_BYPASS_TX_ID_EN,
			      RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_POS_NEG_DATA_SEL,
			      0, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_PROG_SWAP,
			      0, RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_DATA_DIVIDE_CLK_SEL,
			      0, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG2_TX_CLK_PHASE_SHIFT_EN,
			      phase_shift, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG_MAX_SPD_PRG_9,
			      BIT(12) | GENMASK(9, 8),
			      RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_RSVD_CONFIG15,
			      0, RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP, rx_prog_swap,
			      RGMII_IO_MACRO_CONFIG2);

		/* Write 0x5 to PRG_RCLK_DLY_CODE */
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_CODE,
			      (BIT(29) | BIT(27)), SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY,
			      SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY,
			      SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_EN,
			      SDCC_DDR_CONFIG_EXT_PRG_RCLK_DLY_EN,
			      SDCC_HC_REG_DDR_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG_LOOPBACK_EN,
			      loopback, RGMII_IO_MACRO_CONFIG);
		break;
	default:
		dev_err(dev, "Invalid speed %d\n", ethqos->speed);
		return -EINVAL;
	}

	return 0;
}

static int ethqos_configure_rgmii(struct qcom_ethqos *ethqos)
{
	struct device *dev = &ethqos->pdev->dev;
	volatile unsigned int dll_lock;
	unsigned int i, retry = 1000;

	/* Reset to POR values and enable clk */
	for (i = 0; i < ethqos->num_por; i++)
		rgmii_writel(ethqos, ethqos->por[i].value,
			     ethqos->por[i].offset);
	ethqos_set_func_clk_en(ethqos);

	/* Initialize the DLL first */

	/* Set DLL_RST */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_DLL_RST,
		      SDCC_DLL_CONFIG_DLL_RST, SDCC_HC_REG_DLL_CONFIG);

	/* Set PDN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_PDN,
		      SDCC_DLL_CONFIG_PDN, SDCC_HC_REG_DLL_CONFIG);

	if (ethqos->has_emac_ge_3) {
		if (ethqos->speed == SPEED_1000) {
			rgmii_writel(ethqos, 0x1800000, SDCC_TEST_CTL);
			rgmii_writel(ethqos, 0x2C010800, SDCC_USR_CTL);
			rgmii_writel(ethqos, 0xA001, SDCC_HC_REG_DLL_CONFIG2);
		} else {
			rgmii_writel(ethqos, 0x40010800, SDCC_USR_CTL);
			rgmii_writel(ethqos, 0xA001, SDCC_HC_REG_DLL_CONFIG2);
		}
	}

	/* Clear DLL_RST */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_DLL_RST, 0,
		      SDCC_HC_REG_DLL_CONFIG);

	/* Clear PDN */
	rgmii_updatel(ethqos, SDCC_DLL_CONFIG_PDN, 0,
		      SDCC_HC_REG_DLL_CONFIG);

	if (ethqos->speed != SPEED_100 && ethqos->speed != SPEED_10) {
		/* Set DLL_EN */
		rgmii_updatel(ethqos, SDCC_DLL_CONFIG_DLL_EN,
			      SDCC_DLL_CONFIG_DLL_EN, SDCC_HC_REG_DLL_CONFIG);

		/* Set CK_OUT_EN */
		rgmii_updatel(ethqos, SDCC_DLL_CONFIG_CK_OUT_EN,
			      SDCC_DLL_CONFIG_CK_OUT_EN,
			      SDCC_HC_REG_DLL_CONFIG);

		/* Set USR_CTL bit 26 with mask of 3 bits */
		if (!ethqos->has_emac_ge_3)
			rgmii_updatel(ethqos, GENMASK(26, 24), BIT(26),
				      SDCC_USR_CTL);

		/* wait for DLL LOCK */
		do {
			mdelay(1);
			dll_lock = rgmii_readl(ethqos, SDC4_STATUS);
			if (dll_lock & SDC4_STATUS_DLL_LOCK)
				break;
			retry--;
		} while (retry > 0);
		if (!retry)
			dev_err(dev, "Timeout while waiting for DLL lock\n");
	}

	if (ethqos->speed == SPEED_1000)
		ethqos_dll_configure(ethqos);

	ethqos_rgmii_macro_init(ethqos);

	return 0;
}

static void ethqos_force_macsec_bypass(struct qcom_ethqos *ethqos)
{
	void __iomem *macsec_base = ethqos->macsec_base;
	u32 val;

	if (!macsec_base)
		return;

	val = readl_relaxed(macsec_base + MACSEC_CTRL0_OFFSET);
	val |= MACSEC_PWR_ON_RESET;
	writel_relaxed(val, macsec_base + MACSEC_CTRL0_OFFSET);

	val = readl_relaxed(macsec_base + MACSEC_CTRL0_OFFSET);
	val |= MACSEC_BIT_DATA_BYPASS;
	writel_relaxed(val, macsec_base + MACSEC_CTRL0_OFFSET);
}

/* On interface toggle MAC registers gets reset.
 * Configure MAC block for SGMII on ethernet phy link up
 */
static int ethqos_configure_sgmii(struct qcom_ethqos *ethqos)
{
	struct net_device *dev = platform_get_drvdata(ethqos->pdev);
	struct stmmac_priv *priv = netdev_priv(dev);
	int val;

	val = readl(ethqos->mac_base + MAC_CTRL_REG);

	switch (ethqos->speed) {
	case SPEED_2500:
		val &= ~ETHQOS_MAC_CTRL_PORT_SEL;
		rgmii_updatel(ethqos, RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_IO_MACRO_CONFIG2);
		if (ethqos->serdes_speed != SPEED_2500)
			phy_set_speed(ethqos->serdes_phy, SPEED_2500);
		ethqos->serdes_speed = SPEED_2500;
		stmmac_pcs_ctrl_ane(priv, priv->ioaddr, 0, 0, 0);
		break;
	case SPEED_1000:
		val &= ~ETHQOS_MAC_CTRL_PORT_SEL;
		rgmii_updatel(ethqos, RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_IO_MACRO_CONFIG2);
		if (ethqos->serdes_speed != SPEED_1000)
			phy_set_speed(ethqos->serdes_phy, SPEED_1000);
		ethqos->serdes_speed = SPEED_1000;
		stmmac_pcs_ctrl_ane(priv, priv->ioaddr, 1, 0, 0);
		break;
	case SPEED_100:
		val |= ETHQOS_MAC_CTRL_PORT_SEL | ETHQOS_MAC_CTRL_SPEED_MODE;
		if (ethqos->serdes_speed != SPEED_1000)
			phy_set_speed(ethqos->serdes_phy, SPEED_1000);
		ethqos->serdes_speed = SPEED_1000;
		stmmac_pcs_ctrl_ane(priv, priv->ioaddr, 1, 0, 0);
		break;
	case SPEED_10:
		val |= ETHQOS_MAC_CTRL_PORT_SEL;
		val &= ~ETHQOS_MAC_CTRL_SPEED_MODE;
		rgmii_updatel(ethqos, RGMII_CONFIG_SGMII_CLK_DVDR,
			      FIELD_PREP(RGMII_CONFIG_SGMII_CLK_DVDR,
					 SGMII_10M_RX_CLK_DVDR),
			      RGMII_IO_MACRO_CONFIG);
		if (ethqos->serdes_speed != SPEED_1000)
			phy_set_speed(ethqos->serdes_phy, ethqos->speed);
		ethqos->serdes_speed = SPEED_1000;
		stmmac_pcs_ctrl_ane(priv, priv->ioaddr, 1, 0, 0);
		break;
	}

	writel(val, ethqos->mac_base + MAC_CTRL_REG);

	return val;
}

static void qcom_ethqos_speed_mode_2500(struct net_device *ndev, void *data)
{
	struct stmmac_priv *priv = netdev_priv(ndev);

	priv->plat->max_speed = 2500;
	priv->plat->phy_interface = PHY_INTERFACE_MODE_2500BASEX;
}

static int  ethqos_configure_5gbaser(struct qcom_ethqos *ethqos)
{
	ethqos_set_func_clk_en(ethqos);

	rgmii_updatel(ethqos, RGMII_BYPASS_EN, RGMII_BYPASS_EN, RGMII_IO_MACRO_BYPASS);
	rgmii_updatel(ethqos, SGMII_PHY_CNTRL0_2P5G_1G_CLK_SEL, BIT(5),
		      EMAC_WRAPPER_SGMII_PHY_CNTRL0);
	rgmii_updatel(ethqos, SGMII_PHY_CNTRL1_RGMII_SGMII_CLK_MUX_SEL, 0,
		      EMAC_WRAPPER_SGMII_PHY_CNTRL1_V4);
	rgmii_updatel(ethqos, SGMII_PHY_CNTRL1_USXGMII_GMII_MASTER_CLK_MUX_SEL,
		      SGMII_PHY_CNTRL1_USXGMII_GMII_MASTER_CLK_MUX_SEL,
		      EMAC_WRAPPER_SGMII_PHY_CNTRL1_V4);
	rgmii_updatel(ethqos, SGMII_PHY_CNTRL1_SGMII_TX_TO_RX_LOOPBACK_EN, 0,
		      EMAC_WRAPPER_SGMII_PHY_CNTRL1_V4);
	rgmii_updatel(ethqos, RGMII_CONFIG2_MODE_EN_VIA_GMII, 0, RGMII_IO_MACRO_CONFIG2);
	rgmii_updatel(ethqos, USXGMII_CLK_BLK_GMII_CLK_BLK_SEL, USXGMII_CLK_BLK_GMII_CLK_BLK_SEL,
		      EMAC_WRAPPER_USXGMII_MUX_SEL);
	rgmii_updatel(ethqos, USXGMII_CLK_BLK_CLK_EN, 0, EMAC_WRAPPER_USXGMII_MUX_SEL);

	rgmii_updatel(ethqos, RGMII_CONFIG_MAX_SPD_PRG_2_V4, (BIT(6) | BIT(9)),
		      RGMII_IO_MACRO_CONFIG);
	rgmii_updatel(ethqos, RGMII_CONFIG_MAX_SPD_PRG_9_V4, (BIT(10) |  BIT(14) | BIT(15)),
		      RGMII_IO_MACRO_CONFIG);
	rgmii_updatel(ethqos, RGMII_CONFIG2_MAX_SPD_PRG_3, (BIT(17) | BIT(20)),
		      RGMII_IO_MACRO_CONFIG2);
	rgmii_updatel(ethqos, RGMII_SCRATCH2_MAX_SPD_PRG_4, BIT(2),
		      RGMII_IO_MACRO_SCRATCH_2);
	rgmii_updatel(ethqos, RGMII_SCRATCH2_MAX_SPD_PRG_5, (BIT(6) | BIT(7)),
		      RGMII_IO_MACRO_SCRATCH_2);
	rgmii_updatel(ethqos, RGMII_SCRATCH2_MAX_SPD_PRG_6, 0,
		      RGMII_IO_MACRO_SCRATCH_2);
	rgmii_updatel(ethqos, RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
		      RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
		      RGMII_IO_MACRO_CONFIG2);

	return 0;
}

static int ethqos_configure_usxgmii(struct qcom_ethqos *ethqos)
{
	unsigned int i;

	/* Reset to POR values */
	for (i = 0; i < ethqos->num_por; i++)
		rgmii_writel(ethqos, ethqos->por[i].value,
			     ethqos->por[i].offset);

	ethqos_set_func_clk_en(ethqos);

	rgmii_updatel(ethqos, RGMII_BYPASS_EN, RGMII_BYPASS_EN, RGMII_IO_MACRO_BYPASS);
	rgmii_updatel(ethqos, RGMII_CONFIG2_MODE_EN_VIA_GMII, 0, RGMII_IO_MACRO_CONFIG2);
	rgmii_updatel(ethqos, SGMII_PHY_CNTRL0_2P5G_1G_CLK_SEL, BIT(5),
		      EMAC_WRAPPER_SGMII_PHY_CNTRL0);
	rgmii_updatel(ethqos, SGMII_PHY_CNTRL1_RGMII_SGMII_CLK_MUX_SEL, 0,
		      EMAC_WRAPPER_SGMII_PHY_CNTRL1_V4);
	rgmii_updatel(ethqos, SGMII_PHY_CNTRL1_USXGMII_GMII_MASTER_CLK_MUX_SEL,
		      SGMII_PHY_CNTRL1_USXGMII_GMII_MASTER_CLK_MUX_SEL,
		      EMAC_WRAPPER_SGMII_PHY_CNTRL1_V4);
	rgmii_updatel(ethqos, SGMII_PHY_CNTRL1_SGMII_TX_TO_RX_LOOPBACK_EN, 0,
		      EMAC_WRAPPER_SGMII_PHY_CNTRL1_V4);
	rgmii_updatel(ethqos, USXGMII_CLK_BLK_GMII_CLK_BLK_SEL, 0, EMAC_WRAPPER_USXGMII_MUX_SEL);
	rgmii_updatel(ethqos, USXGMII_CLK_BLK_CLK_EN, 0, EMAC_WRAPPER_USXGMII_MUX_SEL);

	switch (ethqos->speed) {
	case SPEED_10000:
		rgmii_updatel(ethqos, USXGMII_CLK_BLK_GMII_CLK_BLK_SEL,
			      USXGMII_CLK_BLK_GMII_CLK_BLK_SEL,
			      EMAC_WRAPPER_USXGMII_MUX_SEL);
		break;

	case SPEED_5000:
		rgmii_updatel(ethqos, SGMII_PHY_CNTRL0_2P5G_1G_CLK_SEL, 0,
			      EMAC_WRAPPER_SGMII_PHY_CNTRL0);
		rgmii_updatel(ethqos, RGMII_CONFIG_MAX_SPD_PRG_2_V4, (BIT(6) | BIT(7)),
			      RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_MAX_SPD_PRG_3, (BIT(17) | BIT(18)),
			      RGMII_IO_MACRO_CONFIG2);
		break;

	case SPEED_2500:
		rgmii_updatel(ethqos, SGMII_PHY_CNTRL0_2P5G_1G_CLK_SEL, 0,
			      EMAC_WRAPPER_SGMII_PHY_CNTRL0);
		rgmii_updatel(ethqos, RGMII_CONFIG_SGMII_CLK_DVDR, (BIT(10) | BIT(11)),
			      RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_SCRATCH2_MAX_SPD_PRG_4, (BIT(2) | BIT(3)),
			      RGMII_IO_MACRO_SCRATCH_2);
		rgmii_updatel(ethqos, RGMII_SCRATCH2_MAX_SPD_PRG_5, 0,
			      RGMII_IO_MACRO_SCRATCH_2);
		break;

	case SPEED_1000:
		rgmii_updatel(ethqos, RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_IO_MACRO_CONFIG2);
		break;

	case SPEED_100:
		rgmii_updatel(ethqos, RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_CONFIG_MAX_SPD_PRG_2_V4, BIT(9),
			      RGMII_IO_MACRO_CONFIG);
		rgmii_updatel(ethqos, RGMII_CONFIG2_MAX_SPD_PRG_3, BIT(20),
			      RGMII_IO_MACRO_CONFIG2);
		rgmii_updatel(ethqos, RGMII_SCRATCH2_MAX_SPD_PRG_6, BIT(10),
			      RGMII_IO_MACRO_SCRATCH_2);
		break;

	case SPEED_10:
		rgmii_updatel(ethqos, RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_IO_MACRO_CONFIG2);
		break;

	default:
		dev_err(&ethqos->pdev->dev,
			"Invalid speed %d\n", ethqos->speed);
		return -EINVAL;
	}

	if (ethqos->has_macsec)
		ethqos_force_macsec_bypass(ethqos);

	return 0;
}

static int ethqos_configure(struct qcom_ethqos *ethqos)
{
	return ethqos->configure_func(ethqos);
}

/* QCOM GMAC4 DMA soft reset requires an internal clock reference (SGMII
 * TX-to-RX loopback) when the external PHY clock is unavailable (e.g. after
 * a safety error brings the link down). Without this loopback, the DMA
 * reset bit never auto-clears and the reset times out.
 */
static int qcom_ethqos_dma_reset(void *priv, void __iomem *ioaddr)
{
	struct plat_stmmacenet_data *plat = priv;
	struct qcom_ethqos *ethqos = plat->bsp_priv;
	u32 value;

	ethqos_set_func_clk_en(ethqos);

	value = readl(ioaddr + DMA_BUS_MODE);
	value |= DMA_BUS_MODE_SFT_RESET;
	writel(value, ioaddr + DMA_BUS_MODE);

	return readl_poll_timeout(ioaddr + DMA_BUS_MODE, value,
				  !(value & DMA_BUS_MODE_SFT_RESET),
				  10000, 1000000);
}

static void ethqos_safety_feature(struct stmmac_priv *priv, bool en)
{
	if (priv->sfty_irq > 0) {
		if (en)
			enable_irq(priv->sfty_irq);
		else
			disable_irq(priv->sfty_irq);
	}
}

static void ethqos_fix_mac_speed(void *priv_n, unsigned int speed, unsigned int mode)
{
	struct qcom_ethqos *ethqos = priv_n;
	struct net_device *dev = platform_get_drvdata(ethqos->pdev);
	struct stmmac_priv *priv = netdev_priv(dev);

	qcom_ethqos_set_sgmii_loopback(ethqos, false);
	ethqos->speed = speed;
	ethqos_update_link_clk(ethqos, speed);
	ethqos_configure(ethqos);
	if (priv->hw->phylink_pcs)
		qcom_xpcs_link_up(priv->hw->phylink_pcs, mode,
				  priv->plat->phy_interface, speed,
				  DUPLEX_FULL);
}

static int qcom_ethqos_map_switch_reset_detect_irq(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct device_node *irq_np;
	int irq;

	irq_np = of_parse_phandle(np, "switch-reset-detect-source", 0);
	if (!irq_np) {
		dev_dbg(dev, "switch-reset-detect-source node not found\n");
		return 0;
	}

	if (!of_property_present(irq_np, "interrupts") &&
	    !of_property_present(irq_np, "interrupts-extended")) {
		dev_dbg(dev, "No interrupts in switch reset detect node\n");
		of_node_put(irq_np);
		return 0;
	}

	irq = irq_of_parse_and_map(irq_np, 0);
	of_node_put(irq_np);

	if (!irq) {
		dev_dbg(dev, "Map switch reset detect IRQ failed\n");
		return 0;
	}

	return irq;
}

static void qcom_ethqos_unmap_switch_reset_detect_irq(void *data)
{
	irq_dispose_mapping((unsigned int)(unsigned long)data);
}

static irqreturn_t qcom_ethqos_switch_reset_detect_irq_handler(int irq, void *data)
{
	struct qcom_ethqos *ethqos = data;
	struct net_device *ndev = platform_get_drvdata(ethqos->pdev);
	struct stmmac_priv *priv;

	if (!ndev)
		return IRQ_NONE;

	priv = netdev_priv(ndev);
	stmmac_handle_switch_reset(priv);

	return IRQ_HANDLED;
}

static void qcom_ethqos_setup_switch_reset_detect_irq(struct device *dev,
						      struct qcom_ethqos *ethqos)
{
	int irq, ret;

	ethqos->switch_reset_detect_irq = 0;

	irq = qcom_ethqos_map_switch_reset_detect_irq(dev);
	if (irq <= 0)
		return;

	ret = devm_add_action_or_reset(dev,
				       qcom_ethqos_unmap_switch_reset_detect_irq,
				       (void *)(unsigned long)irq);
	if (ret) {
		dev_warn(dev, "Failed to register IRQ cleanup: %d\n", ret);
		/* The action will be called on failure; irq mapping already cleaned up */
		return;
	}

	ret = devm_request_irq(dev, irq,
			       qcom_ethqos_switch_reset_detect_irq_handler,
			       IRQF_TRIGGER_FALLING | IRQF_SHARED,
			       "qcom-ethqos-detect", ethqos);
	if (ret) {
		dev_warn(dev, "Failed to request switch reset detect IRQ %d: %d\n",
			 irq, ret);
		return;
	}

	ethqos->switch_reset_detect_irq = irq;

	dev_info(dev, "Registered switch reset detect IRQ %d\n",
		 ethqos->switch_reset_detect_irq);
}

static int qcom_ethqos_serdes_powerup(struct net_device *ndev, void *priv)
{
	struct qcom_ethqos *ethqos = priv;
	int ret;

	ret = phy_init(ethqos->serdes_phy);
	if (ret)
		return ret;

	ret = phy_power_on(ethqos->serdes_phy);
	if (ret)
		return ret;

	return phy_set_speed(ethqos->serdes_phy, ethqos->speed);
}

static void qcom_ethqos_serdes_powerdown(struct net_device *ndev, void *priv)
{
	struct qcom_ethqos *ethqos = priv;

	phy_power_off(ethqos->serdes_phy);
	phy_exit(ethqos->serdes_phy);
}

static int ethqos_clks_config(void *priv, bool enabled)
{
	struct qcom_ethqos *ethqos = priv;
	int ret = 0;

	if (enabled) {
		ret = clk_prepare_enable(ethqos->link_clk);
		if (ret) {
			dev_err(&ethqos->pdev->dev, "link_clk enable failed\n");
			return ret;
		}

		/* Enable functional clock to prevent DMA reset to timeout due
		 * to lacking PHY clock after the hardware block has been power
		 * cycled. The actual configuration will be adjusted once
		 * ethqos_fix_mac_speed() is invoked.
		 */
		ethqos_set_func_clk_en(ethqos);
	} else {
		clk_disable_unprepare(ethqos->link_clk);
	}

	return ret;
}

static void ethqos_clks_disable(void *data)
{
	ethqos_clks_config(data, false);
}

static void ethqos_ptp_clk_freq_config(struct stmmac_priv *priv)
{
	struct plat_stmmacenet_data *plat_dat = priv->plat;
	int err;

	if (!plat_dat->clk_ptp_ref)
		return;

	/* Max the PTP ref clock out to get the best resolution possible */
	err = clk_set_rate(plat_dat->clk_ptp_ref, ULONG_MAX);
	if (err)
		netdev_err(priv->dev, "Failed to max out clk_ptp_ref: %d\n", err);
	plat_dat->clk_ptp_rate = clk_get_rate(plat_dat->clk_ptp_ref);

	netdev_dbg(priv->dev, "PTP rate %d\n", plat_dat->clk_ptp_rate);
}

static void qcom_ethqos_get_queue_and_tc_from_vdma(struct stmmac_priv *priv,
						   u32 vdma_ch,
						   unsigned long *queue_mask,
						   u32 *tc)
{
	u32 tx_queues_cnt = priv->plat->tx_queues_to_use;
	int i;

	*queue_mask = 0;

	if (vdma_ch >= MTL_MAX_TX_QUEUES) {
		netdev_err(priv->dev, "VDMA channel %u out of range\n", vdma_ch);
		return;
	}

	/* Look up the TC this VDMA channel is mapped to */
	*tc = priv->plat->dma_cfg->tx_vdma_map[vdma_ch];

	/* Find all PDMA channels mapped to the same TC as the VDMA channel.
	 * A TC can map to multiple PDMA channels (1:many). Since PDMA channels
	 * and TX queues have a 1:1 correspondence, each matching PDMA channel
	 * index is set as a bit in queue_mask.
	 */
	for (i = 0; i < tx_queues_cnt; i++) {
		if (priv->plat->dma_cfg->tx_pdma_map[i] == *tc)
			*queue_mask |= BIT(i);
	}

	if (!*queue_mask)
		netdev_warn(priv->dev, "No PDMA channel found for VDMA %u (TC %u)\n",
			    vdma_ch, *tc);
}

static void ethqos_report_uevents(struct stmmac_priv *priv, enum stmmac_uevent_type event)
{
	char phy_mode[32];
	char event_type[32];
	char *envp[3];
	int i = 0;

	switch (event) {
	case FUSA_ERROR:
		snprintf(event_type, sizeof(event_type), "SAFETY_EVENT=FUSA_ERROR");
		break;
	case MAC_DOWN:
		snprintf(event_type, sizeof(event_type), "SAFETY_EVENT=MAC_DOWN");
		break;
	case MAC_UP:
		snprintf(event_type, sizeof(event_type), "SAFETY_EVENT=MAC_UP");
		break;
	default:
		dev_warn(priv->device, "Unknown UMD event %d\n", event);
		return;
	}

	envp[i++] = event_type;

	if (event != FUSA_ERROR) {
		snprintf(phy_mode, sizeof(phy_mode), "PHY_MODE=%s",
			 phy_modes(priv->plat->phy_interface));
		envp[i++] = phy_mode;
	}

	envp[i] = NULL;

	kobject_uevent_env(&priv->device->kobj, KOBJ_CHANGE, envp);
}

static int qcom_ethqos_hdma_cfg(struct platform_device *pdev, struct plat_stmmacenet_data *plat)
{
	struct device_node *np = pdev->dev.of_node;
	u32 map[STMMAC_CH_MAX];
	int count, i;

	plat->dma_cfg->orrq = 15;
	plat->dma_cfg->owrq = 15;
	plat->dma_cfg->txdcsz = 4;
	plat->dma_cfg->tdps = 1;
	plat->dma_cfg->rxdcsz = 4;
	plat->dma_cfg->rdps = 1;

	count = of_property_count_u32_elems(np, "qcom,tx-pdma-map");
	if (count > 0 && count <= STMMAC_CH_MAX &&
	    !of_property_read_u32_array(np, "qcom,tx-pdma-map", map, count)) {
		plat->dma_cfg->tx_pdma_custom_map = true;
		for (i = 0; i < count; i++)
			plat->dma_cfg->tx_pdma_map[i] = map[i];
	} else {
		dev_err(&pdev->dev, "Tx PDMA map not defined falling back to default config\n");
		return -EINVAL;
	}

	count = of_property_count_u32_elems(np, "qcom,rx-pdma-map");
	if (count > 0 && count <= STMMAC_CH_MAX &&
	    !of_property_read_u32_array(np, "qcom,rx-pdma-map", map, count)) {
		plat->dma_cfg->rx_pdma_custom_map = true;
		for (i = 0; i < count; i++)
			plat->dma_cfg->rx_pdma_map[i] = map[i];
	} else {
		dev_err(&pdev->dev, "Rx PDMA map not defined falling back to default config\n");
		return -EINVAL;
	}

	count = of_property_count_u32_elems(np, "qcom,tx-vdma-map");
	if (count > 0 && count <= STMMAC_CH_MAX &&
	    !of_property_read_u32_array(np, "qcom,tx-vdma-map", map, count)) {
		plat->dma_cfg->tx_vdma_custom_map = true;
		for (i = 0; i < count; i++)
			plat->dma_cfg->tx_vdma_map[i] = map[i];
	} else {
		dev_err(&pdev->dev, "Tx VDMA map not defined falling back to default config\n");
		return -EINVAL;
	}

	count = of_property_count_u32_elems(np, "qcom,rx-vdma-map");
	if (count > 0 && count <= STMMAC_CH_MAX &&
	    !of_property_read_u32_array(np, "qcom,rx-vdma-map", map, count)) {
		plat->dma_cfg->rx_vdma_custom_map = true;
		for (i = 0; i < count; i++)
			plat->dma_cfg->rx_vdma_map[i] = map[i];
	} else {
		dev_err(&pdev->dev, "Rx VDMA map not defined falling back to default config\n");
		return -EINVAL;
	}

	return 0;
}

static int ethqos_xpcs_init(struct stmmac_priv *priv)
{
	struct device_node *xpcs_node;

	xpcs_node = of_parse_phandle(priv->device->of_node, "qcom-xpcs-handle", 0);

	priv->hw->phylink_pcs = qcom_xpcs_create(xpcs_node, priv->plat->phy_interface);
	if (IS_ERR_OR_NULL(priv->hw->phylink_pcs))
		return -ENODEV;

	return 0;
}

static void ethqos_xpcs_exit(struct stmmac_priv *priv)
{
	qcom_xpcs_destroy(priv->hw->phylink_pcs);
}

static void ethqos_xpcs_safety_stats(struct stmmac_priv *priv, unsigned long *ptr)
{
	if (priv->sfty_irq > 0)
		qcom_xpcs_get_err_stats(priv->hw->phylink_pcs, ptr);
}

static int qcom_ethqos_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node, *root;
	const struct ethqos_emac_driver_data *data;
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device *dev = &pdev->dev;
	struct qcom_ethqos *ethqos;
	int ret, i;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get platform resources\n");

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat)) {
		return dev_err_probe(dev, PTR_ERR(plat_dat),
				     "dt configuration failed\n");
	}

	plat_dat->clks_config = ethqos_clks_config;

	ethqos = devm_kzalloc(dev, sizeof(*ethqos), GFP_KERNEL);
	if (!ethqos)
		return -ENOMEM;

	ret = of_get_phy_mode(np, &ethqos->phy_mode);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get phy mode\n");

	root = of_find_node_by_path("/");
	if (root && of_device_is_compatible(root, "qcom,sa8540p-ride"))
		ethqos->needs_rx_prog_swap = true;
	else
		ethqos->needs_rx_prog_swap =
			of_property_read_bool(np, "qcom,rx-prog-swap");
	of_node_put(root);

	switch (ethqos->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		ethqos->configure_func = ethqos_configure_rgmii;
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		plat_dat->speed_mode_2500 = qcom_ethqos_speed_mode_2500;
		fallthrough;
	case PHY_INTERFACE_MODE_SGMII:
		ethqos->configure_func = ethqos_configure_sgmii;
		break;
	case PHY_INTERFACE_MODE_5GBASER:
		ethqos->configure_func = ethqos_configure_5gbaser;
		break;
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_10GBASER:
		ethqos->configure_func = ethqos_configure_usxgmii;
		break;
	default:
		dev_err(dev, "Unsupported phy mode %s\n",
			phy_modes(ethqos->phy_mode));
		return -EINVAL;
	}

	ethqos->pdev = pdev;
	ethqos->rgmii_base = devm_platform_ioremap_resource_byname(pdev, "rgmii");
	if (IS_ERR(ethqos->rgmii_base))
		return dev_err_probe(dev, PTR_ERR(ethqos->rgmii_base),
				     "Failed to map rgmii resource\n");

	ethqos->mac_base = stmmac_res.addr;

	data = of_device_get_match_data(dev);
	if (!data) {
		dev_err(dev, "No match data found\n");
		return -ENODEV;
	}

	if (data->has_macsec) {
		ethqos->macsec_base = devm_platform_ioremap_resource_byname(pdev, "macsec");
		if (IS_ERR(ethqos->macsec_base)) {
			return dev_err_probe(dev, PTR_ERR(ethqos->macsec_base),
					"Failed to map macsec resource\n");
		}
	}

	ethqos->por = data->por;
	ethqos->num_por = data->num_por;
	ethqos->has_macsec = data->has_macsec;
	ethqos->has_io_macro_ge_4 = data->has_io_macro_ge_4;
	ethqos->rgmii_config_loopback_en = data->rgmii_config_loopback_en;
	ethqos->has_emac_ge_3 = data->has_emac_ge_3;
	ethqos->needs_sgmii_loopback = data->needs_sgmii_loopback;

	ethqos->link_clk = devm_clk_get(dev, data->link_clk_name ?: "rgmii");
	if (IS_ERR(ethqos->link_clk))
		return dev_err_probe(dev, PTR_ERR(ethqos->link_clk),
				     "Failed to get link_clk\n");

	ret = ethqos_clks_config(ethqos, true);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, ethqos_clks_disable, ethqos);
	if (ret)
		return ret;

	ethqos->serdes_phy = devm_phy_optional_get(dev, "serdes");
	if (IS_ERR(ethqos->serdes_phy))
		return dev_err_probe(dev, PTR_ERR(ethqos->serdes_phy),
				     "Failed to get serdes phy\n");

	ethqos->speed = SPEED_1000;
	ethqos->serdes_speed = SPEED_1000;
	ethqos_update_link_clk(ethqos, SPEED_1000);
	ethqos_set_func_clk_en(ethqos);

	if (stmmac_res.sfty_irq > 0) {
		plat_dat->report_uevents = ethqos_report_uevents;
		plat_dat->flags |= STMMAC_FLAG_HAS_ERROR_UEVENT;
	}
	plat_dat->bsp_priv = ethqos;
	plat_dat->fix_mac_speed = ethqos_fix_mac_speed;
	plat_dat->dump_debug_regs = rgmii_dump;
	plat_dat->ptp_clk_freq_config = ethqos_ptp_clk_freq_config;
	plat_dat->clk_ref_rate = data->axi_clk_rate;
	plat_dat->has_gmac4 = 1;
	if (ethqos->has_emac_ge_3)
		plat_dat->dwmac4_addrs = &data->dwmac4_addrs;
	plat_dat->pmt = 1;
	if (plat_dat->has_xgmac) {
		plat_dat->has_gmac4 = 0;
		plat_dat->dwxgmac_addrs = &data->dwxgmac_addrs;
		plat_dat->has_hdma = data->has_hdma;
		plat_dat->insert_ts_pktid = true;
		if (plat_dat->has_hdma) {
			ret = qcom_ethqos_hdma_cfg(pdev, plat_dat);
			if (ret)
				return ret;
			plat_dat->get_queue_and_tc_from_vdma =
				qcom_ethqos_get_queue_and_tc_from_vdma;
		}
	}
	if (plat_dat->has_gmac4)
		plat_dat->fix_soc_reset = qcom_ethqos_dma_reset;
	if (of_property_present(dev->of_node, "qcom-xpcs-handle")) {
		plat_dat->pcs_init = ethqos_xpcs_init;
		plat_dat->pcs_exit = ethqos_xpcs_exit;
		plat_dat->safety_irq = ethqos_safety_feature;
		plat_dat->safety_pcs_stats = ethqos_xpcs_safety_stats;
	}
	if (of_property_read_bool(np, "snps,tso"))
		plat_dat->flags |= STMMAC_FLAG_TSO_EN;
	if (of_device_is_compatible(np, "qcom,qcs404-ethqos"))
		plat_dat->flags |= STMMAC_FLAG_RX_CLK_RUNS_IN_LPI;
	if (data->has_flags)
		plat_dat->flags |= data->has_flags;
	if (data->dma_addr_width)
		plat_dat->host_dma_width = data->dma_addr_width;

	if (stmmac_res.tx_rx_irq[0] > 0 ||
	    (stmmac_res.rx_irq[0] > 0 && stmmac_res.tx_irq[0] > 0))
		plat_dat->flags |= STMMAC_FLAG_MULTI_IRQ_EN;

	if (ethqos->serdes_phy) {
		plat_dat->serdes_powerup = qcom_ethqos_serdes_powerup;
		plat_dat->serdes_powerdown  = qcom_ethqos_serdes_powerdown;
	}

	/* Enable TSO on queue0 and enable TBS on rest of the queues */
	for (i = 1; i < plat_dat->tx_queues_to_use; i++)
		plat_dat->tx_queues_cfg[i].tbs_en = 1;

	ret = devm_stmmac_pltfr_probe(pdev, plat_dat, &stmmac_res);

	/* Register switch reset detect IRQ */
	if (!ret)
		qcom_ethqos_setup_switch_reset_detect_irq(dev, ethqos);

	return ret;
}

static const struct of_device_id qcom_ethqos_match[] = {
	{ .compatible = "qcom,qcs404-ethqos", .data = &emac_v2_3_0_data},
	{ .compatible = "qcom,qcs615-ethqos", .data = &emac_v2_3_1_data},
	{ .compatible = "qcom,qcs8300-ethqos", .data = &emac_v4_0_0_data},
	{ .compatible = "qcom,sa8775p-ethqos", .data = &emac_v4_0_0_data},
	{ .compatible = "qcom,sc8280xp-ethqos", .data = &emac_v3_0_0_data},
	{ .compatible = "qcom,sm8150-ethqos", .data = &emac_v2_1_0_data},
	{ .compatible = "qcom,sa8620p-ethqos", .data = &emac_v4_0_0_data},
	{ .compatible = "qcom,sa8797p-ethqos", .data = &emac_v6_6_0_data},
	{ .compatible = "qcom,sa8787p-ethqos", .data = &emac_v6_6_1_data},
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_ethqos_match);

static struct platform_driver qcom_ethqos_driver = {
	.probe  = qcom_ethqos_probe,
	.driver = {
		.name           = "qcom-ethqos",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = qcom_ethqos_match,
	},
};
module_platform_driver(qcom_ethqos_driver);

MODULE_DESCRIPTION("Qualcomm ETHQOS driver");
MODULE_LICENSE("GPL v2");
