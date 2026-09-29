// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018-19, Linaro Limited
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/pcs-xpcs-qcom.h>
#include <linux/pm_opp.h>
#include <linux/pm_domain.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/workqueue.h>

#include "stmmac.h"
#include "stmmac_platform.h"

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

/* SDCC_USR_CTL bits */
#define SDCC_USR_CTL_DDR_BYPASS			BIT(30)

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

#define SGMII_10M_RX_CLK_DVDR			0x31

#define ETHQOS_MAX_NOC_CLKS			3

/* GDSC Regulators MACROS */
#define EMAC_GDSC_NAME "gdsc_emac"
#define EMAC_VREG_RGMII_NAME "vreg_rgmii"
#define EMAC_VREG_EMAC_PHY_NAME "vreg_emac_phy"
#define EMAC_VREG_RGMII_IO_PADS_NAME "vreg_rgmii_io_pads"

enum domain_t {
	POWER_CORE = 0,
	POWER_CLK = 1,
	PERF_SERDES = 2,
	PERF_5G_SERDES = 3,
};

struct ethqos_emac_por {
	unsigned int offset;
	unsigned int value;
};

struct ethqos_noc_clk_cfg {
	const char *id;
	unsigned long rate;
};

struct ethqos_emac_driver_data {
	const struct ethqos_emac_por *por;
	struct dwxgmac_addrs dwxgmac_addrs;
	unsigned int num_por;
	bool rgmii_config_loopback_en;
	bool has_emac_ge_3;
	const char *link_clk_name;
	u32 has_flags;
	bool has_integrated_pcs;
	u32 dma_addr_width;
	unsigned int ptp_clk_rate;
	unsigned int axi_clk_rate;
	struct dwmac4_addrs dwmac4_addrs;
	bool needs_sgmii_loopback;
	bool has_hdma;
	bool has_macsec;
	const struct ethqos_noc_clk_cfg *noc_clk_cfg;
	unsigned int num_noc_clks;
	struct dev_pm_domain_attach_data pd_data;
};

struct qcom_ethqos {
	struct platform_device *pdev;
	void __iomem *rgmii_base;
	void __iomem *macsec_base;
	void __iomem *mac_base;
	int (*configure_func)(struct qcom_ethqos *ethqos);

	unsigned int link_clk_rate;
	struct clk *link_clk;
	struct phy *serdes_phy;
	unsigned int speed;
	int serdes_speed;
	phy_interface_t phy_mode;

	int gpio_phy_intr_redirect;
	int switch_reset_detect_irq;
	struct work_struct switch_irq_work;
	u32 phy_intr;

	struct regulator *gdsc_emac;
	struct regulator *reg_rgmii;
	struct regulator *reg_emac_phy;
	struct regulator *reg_rgmii_io_pads;
	const struct ethqos_emac_por *por;
	unsigned int num_por;
	bool rgmii_config_loopback_en;
	bool has_emac_ge_3;
	bool has_macsec;
	bool needs_sgmii_loopback;
	bool use_domains;
	struct dev_pm_domain_list *pd_list;
	struct clk_bulk_data noc_clks[ETHQOS_MAX_NOC_CLKS];
	int num_noc_clks;
};

static int phytype = BOARD_UNKNOWN;
static int boardtype = PHY_UNKNOWN;

#ifdef MODULE
static char *board;
module_param(board, charp, 0640);
MODULE_PARM_DESC(board, "board type of the device");

static char *enet;
module_param(enet, charp, 0640);
MODULE_PARM_DESC(enet, "enet value for the phy connection");
#endif

static int set_board_type(char *board_params)
{
	pr_info("qcom-ethqos: %s Board Param in command line: %s\n", __func__, board_params);
	if (!strcmp(board_params, "Air"))
		boardtype = AIR_BOARD;
	else if (!strcmp(board_params, "Star"))
		boardtype = STAR_BOARD;
	else
		return -EINVAL;
	return 0;
}

static int set_phy_type(char *enet_params)
{
	pr_info("qcom-ethqos: %s Enet Param in command line: %s\n", __func__, enet_params);
	if (!strcmp(enet_params, "1") || !strcmp(enet_params, "2"))
		phytype = PHY_1G;
	else if (!strcmp(enet_params, "3") || !strcmp(enet_params, "6"))
		phytype = PHY_25G;
	else if (!strcmp(enet_params, "4") || !strcmp(enet_params, "5"))
		phytype = SWITCH;
	else
		return -EINVAL;
	return 0;
}

#ifndef MODULE
__setup("dwmac_qcom_eth.board=", set_board_type);

__setup("dwmac_qcom_eth.enet=", set_phy_type);
#endif

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

	/* RGMII-ID expects 25 and 2.5 MHz for 100M and 10M (DLL bypass
	 * mode, no doubling), while other RGMII variants use 50 and 5 MHz.
	 */
	if (ethqos->phy_mode == PHY_INTERFACE_MODE_RGMII_ID &&
	    speed != SPEED_1000)
		ethqos->link_clk_rate /= 2;

	clk_set_rate(ethqos->link_clk, ethqos->link_clk_rate);
}

static int setup_gpio_input_common
	(struct device *dev, const char *name, int *gpio)
{
	int ret = 0;

	if (of_find_property(dev->of_node, name, NULL)) {
		*gpio = ret = of_get_named_gpio(dev->of_node, name, 0);
		if (ret >= 0) {
			ret = gpio_request(*gpio, name);
			if (ret) {
				dev_dbg(dev, "Can't get GPIO %s, ret = %d\n",
					name, *gpio);
				*gpio = -1;
				return ret;
			}

			ret = gpio_direction_input(*gpio);
			if (ret) {
				dev_dbg(dev, "failed GPIO %s direction ret=%d\n",
					name, ret);
				return ret;
			}
		} else {
			if (ret == -EPROBE_DEFER)
				dev_dbg(dev, "get EMAC_GPIO probe defer\n");
			else
				dev_dbg(dev, "can't get gpio %s ret %d\n", name,
					ret);
			return ret;
		}
	} else {
		dev_dbg(dev, "can't find gpio %s\n", name);
		ret = -EINVAL;
	}

	return ret;
}

void ethqos_disable_regulators(struct qcom_ethqos *ethqos)
{
	if (ethqos->reg_rgmii) {
		regulator_disable(ethqos->reg_rgmii);
		ethqos->reg_rgmii = NULL;
	}

	if (ethqos->reg_emac_phy) {
		regulator_disable(ethqos->reg_emac_phy);
		ethqos->reg_emac_phy = NULL;
	}

	if (ethqos->reg_rgmii_io_pads) {
		regulator_disable(ethqos->reg_rgmii_io_pads);
		ethqos->reg_rgmii_io_pads = NULL;
	}

	if (ethqos->gdsc_emac) {
		regulator_disable(ethqos->gdsc_emac);
		ethqos->gdsc_emac = NULL;
	}
}

int ethqos_init_regulators(struct qcom_ethqos *ethqos)
{
	struct device *dev = &ethqos->pdev->dev;
	int ret = 0;

	if (of_property_read_bool(ethqos->pdev->dev.of_node,
				  "gdsc_emac-supply")) {
		ethqos->gdsc_emac =
		devm_regulator_get(&ethqos->pdev->dev, EMAC_GDSC_NAME);
		if (IS_ERR(ethqos->gdsc_emac)) {
			dev_dbg(dev, "Can not get <%s>\n", EMAC_GDSC_NAME);
			return PTR_ERR(ethqos->gdsc_emac);
		}

		ret = regulator_enable(ethqos->gdsc_emac);
		if (ret) {
			dev_dbg(dev, "Can not enable <%s>\n", EMAC_GDSC_NAME);
			goto reg_error;
		}

		dev_info(dev, "Enabled <%s>\n", EMAC_GDSC_NAME);
	}

	if (of_property_read_bool(ethqos->pdev->dev.of_node,
				  "vreg_rgmii-supply")) {
		ethqos->reg_rgmii =
		devm_regulator_get(&ethqos->pdev->dev, EMAC_VREG_RGMII_NAME);
		if (IS_ERR(ethqos->reg_rgmii)) {
			dev_dbg(dev, "Can not get <%s>\n", EMAC_VREG_RGMII_NAME);
			return PTR_ERR(ethqos->reg_rgmii);
		}

		ret = regulator_enable(ethqos->reg_rgmii);
		if (ret) {
			dev_dbg(dev, "Can not enable <%s>\n",
				EMAC_VREG_RGMII_NAME);
			goto reg_error;
		}

		dev_info(dev, "Enabled <%s>\n", EMAC_VREG_RGMII_NAME);
	}

	if (of_property_read_bool(ethqos->pdev->dev.of_node,
				  "vreg_emac_phy-supply")) {
		ethqos->reg_emac_phy =
		devm_regulator_get(&ethqos->pdev->dev, EMAC_VREG_EMAC_PHY_NAME);
		if (IS_ERR(ethqos->reg_emac_phy)) {
			dev_dbg(dev, "Can not get <%s>\n",
				EMAC_VREG_EMAC_PHY_NAME);
			return PTR_ERR(ethqos->reg_emac_phy);
		}

		ret = regulator_enable(ethqos->reg_emac_phy);
		if (ret) {
			dev_dbg(dev, "Can not enable <%s>\n",
				EMAC_VREG_EMAC_PHY_NAME);
			goto reg_error;
		}

		dev_info(dev, "Enabled <%s>\n", EMAC_VREG_EMAC_PHY_NAME);
	}

	if (of_property_read_bool(ethqos->pdev->dev.of_node,
				  "vreg_rgmii_io_pads-supply")) {
		ethqos->reg_rgmii_io_pads = devm_regulator_get
		(&ethqos->pdev->dev, EMAC_VREG_RGMII_IO_PADS_NAME);
		if (IS_ERR(ethqos->reg_rgmii_io_pads)) {
			dev_dbg(dev, "Can not get <%s>\n",
				EMAC_VREG_RGMII_IO_PADS_NAME);
			return PTR_ERR(ethqos->reg_rgmii_io_pads);
		}

		ret = regulator_enable(ethqos->reg_rgmii_io_pads);
		if (ret) {
			dev_dbg(dev, "Can not enable <%s>\n",
				EMAC_VREG_RGMII_IO_PADS_NAME);
			goto reg_error;
		}

		dev_info(dev, "Enabled <%s>\n", EMAC_VREG_RGMII_IO_PADS_NAME);
	}

	return ret;

reg_error:
	dev_err(dev, "%s failed\n", __func__);
	ethqos_disable_regulators(ethqos);
	return ret;
}

void ethqos_free_gpios(struct qcom_ethqos *ethqos)
{
	if (gpio_is_valid(ethqos->gpio_phy_intr_redirect))
		gpio_free(ethqos->gpio_phy_intr_redirect);
	ethqos->gpio_phy_intr_redirect = -1;
}

int ethqos_init_pinctrl(struct device *dev)
{
	struct pinctrl *pinctrl;
	struct pinctrl_state *pinctrl_state;
	int i = 0;
	int num_names;
	const char *name;
	int ret = 0;

	pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(pinctrl)) {
		ret = PTR_ERR(pinctrl);
		dev_dbg(dev, "Failed to get pinctrl, err = %d\n", ret);
		return ret;
	}

	num_names = of_property_count_strings(dev->of_node, "pinctrl-names");
	if (num_names < 0) {
		dev_dbg(dev, "Cannot parse pinctrl-names: %d\n", num_names);
		return num_names;
	}

	for (i = 0; i < num_names; i++) {
		ret = of_property_read_string_index(dev->of_node,
						    "pinctrl-names",
						    i, &name);

		if (!strcmp(name, PINCTRL_STATE_DEFAULT))
			continue;

		pinctrl_state = pinctrl_lookup_state(pinctrl, name);
		if (IS_ERR_OR_NULL(pinctrl_state)) {
			ret = PTR_ERR(pinctrl_state);
			dev_dbg(dev, "lookup_state %s failed %d\n", name, ret);
			return ret;
		}

		dev_info(dev, "pinctrl_lookup_state %s succeeded\n", name);

		ret = pinctrl_select_state(pinctrl, pinctrl_state);
		if (ret) {
			dev_dbg(dev, "select_state %s failed %d\n", name, ret);
			return ret;
		}

		dev_info(dev, "pinctrl_select_state %s succeeded\n", name);
	}

	return ret;
}

int ethqos_init_gpio(struct qcom_ethqos *ethqos)
{
	struct device *dev = &ethqos->pdev->dev;
	int ret = 0;

	ethqos->gpio_phy_intr_redirect = -1;

	ret = ethqos_init_pinctrl(dev);
	if (ret) {
		dev_dbg(dev, "ethqos_init_pinctrl failed");
		return ret;
	}

	if (!of_property_present(dev->of_node, "qcom,phy-intr-redirect")) {
		dev_dbg(dev, "gpio <qcom,phy-intr-redirect> not present so skipping it\n");
		return ret; /* success, nothing to do */
	}

	ret = setup_gpio_input_common(&ethqos->pdev->dev, "qcom,phy-intr-redirect",
				      &ethqos->gpio_phy_intr_redirect);

	if (ret) {
		dev_dbg(dev, "Failed to setup <%s> gpio\n",
			"qcom,phy-intr-redirect");
		goto gpio_error;
	}

	return ret;

gpio_error:
	ethqos_free_gpios(ethqos);
	return ret;
}

static void
qcom_ethqos_set_sgmii_loopback(struct qcom_ethqos *ethqos, bool enable)
{
	if (!ethqos->needs_sgmii_loopback ||
	    ethqos->phy_mode != PHY_INTERFACE_MODE_2500BASEX)
		return;

	rgmii_updatel(ethqos,
		      SGMII_PHY_CNTRL1_SGMII_TX_TO_RX_LOOPBACK_EN,
		      enable ? SGMII_PHY_CNTRL1_SGMII_TX_TO_RX_LOOPBACK_EN : 0,
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
	.has_integrated_pcs = true,
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

static const struct ethqos_noc_clk_cfg shikra_noc_clks[] = {
	{ "axi",               120000000 },
	{ "axi-noc",           120000000 },
	{ "pcie-tile-axi-noc", 120000000 },
};

static const struct ethqos_emac_driver_data shikra_data = {
	.dma_addr_width = 36,
	.has_emac_ge_3 = true,
	.noc_clk_cfg = shikra_noc_clks,
	.num_noc_clks = ARRAY_SIZE(shikra_noc_clks),
	.rgmii_config_loopback_en = false,
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
	.axi_clk_rate = 380000000,
	.ptp_clk_rate = 250000000,
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
	.pd_data = {
		.pd_flags = PD_FLAG_NO_DEV_LINK,
		.pd_names = (const char*[]) {"power_core", "power_mdio", "perf_serdes",
					     "perf_5g_serdes"},
		.num_pd_names = 4,
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
	.axi_clk_rate = 380000000,
	.ptp_clk_rate = 250000000,
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
	.pd_data = {
		.pd_flags = PD_FLAG_NO_DEV_LINK,
		.pd_names = (const char*[]) {"power_core", "power_mdio", "perf_serdes",
					     "perf_5g_serdes"},
		.num_pd_names = 4,
	},
};

static int qcom_ethqos_is_genpd_on(struct device *dev)
{
	struct generic_pm_domain *genpd = pd_to_genpd(dev->pm_domain);

	return (genpd->status == GENPD_STATE_ON);
}

static int qcom_ethqos_domain_attach(struct qcom_ethqos *ethqos)
{
	const struct ethqos_emac_driver_data *data;
	struct device *dev = &ethqos->pdev->dev;

	data = of_device_get_match_data(dev);
	if (!data) {
		dev_err(dev, "%s driver data is NULL\n", __func__);
		return -EINVAL;
	}

	dev_dbg(dev, "%s num pd %d\n", __func__, data->pd_data.num_pd_names);
	return dev_pm_domain_attach_list(dev, &data->pd_data, &ethqos->pd_list);
}

static int qcom_ethqos_domain_on(struct qcom_ethqos *ethqos, enum domain_t dom)
{
	struct device *dev = ethqos->pd_list->pd_devs[dom];
	int ret = 0;

	if (!qcom_ethqos_is_genpd_on(dev)) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			dev_err(dev, "poweron(domain=%d) failed.(err=%d)\n", dom, ret);

		dev_dbg(dev, "Requesting power on for (domain=%d)", dom);
	} else {
		dev_dbg(dev, "Domain is already on (domain=%d)", dom);
	}

	return ret;
}

static void qcom_ethqos_domain_off(struct qcom_ethqos *ethqos, enum domain_t dom)
{
	struct device *dev = ethqos->pd_list->pd_devs[dom];
	int ret = 0;

	if (qcom_ethqos_is_genpd_on(dev)) {
		ret = pm_runtime_put_sync(dev);
		if (ret < 0)
			dev_err(dev, "poweroff(domain=%d) failed.(err=%d)\n", dom, ret);

		dev_dbg(dev, "Requesting power off for (domain=%d)", dom);
	} else {
		dev_dbg(dev, "Domain is already off (domain=%d)", dom);
	}
}

static int qcom_ethqos_serdes_set_level(struct qcom_ethqos *ethqos)
{
	struct device *dev = NULL;
	struct dev_pm_opp *opp;
	int ret = 0;

	if (ethqos->phy_mode == PHY_INTERFACE_MODE_USXGMII ||
	    ethqos->phy_mode == PHY_INTERFACE_MODE_10GBASER) {
		dev = ethqos->pd_list->pd_devs[PERF_SERDES];
	} else if (ethqos->phy_mode == PHY_INTERFACE_MODE_5GBASER) {
		dev = ethqos->pd_list->pd_devs[PERF_5G_SERDES];
	} else {
		dev = ethqos->pd_list->pd_devs[PERF_SERDES];
	}

	opp = dev_pm_opp_find_level_exact(dev, ethqos->speed);
	if (IS_ERR(opp))
		return -EINVAL;

	ret = dev_pm_opp_set_opp(dev, opp);
	if (ret)
		dev_err(dev, "Failed to set serdes level. (err=%d)\n", ret);

	if (!ret) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret >= 0)
			ret = pm_runtime_put_sync(dev);
	}

	dev_pm_opp_put(opp);

	return ret;
}

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

		if (ethqos->has_emac_ge_3)
			rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP,
				      RGMII_CONFIG2_RX_PROG_SWAP,
				      RGMII_IO_MACRO_CONFIG2);
		else
			rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP,
				      0, RGMII_IO_MACRO_CONFIG2);

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
		if (ethqos->has_emac_ge_3)
			rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP,
				      RGMII_CONFIG2_RX_PROG_SWAP,
				      RGMII_IO_MACRO_CONFIG2);
		else
			rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP,
				      0, RGMII_IO_MACRO_CONFIG2);
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

static void ethqos_rgmii_id_macro_init(struct qcom_ethqos *ethqos)
{
	rgmii_updatel(ethqos, RGMII_CONFIG2_TX_TO_RX_LOOPBACK_EN,
		      0, RGMII_IO_MACRO_CONFIG2);

	if (ethqos->speed == SPEED_1000)
		rgmii_updatel(ethqos, RGMII_CONFIG_DDR_MODE,
			      RGMII_CONFIG_DDR_MODE, RGMII_IO_MACRO_CONFIG);
	else
		rgmii_updatel(ethqos, RGMII_CONFIG_DDR_MODE,
			      0, RGMII_IO_MACRO_CONFIG);

	rgmii_updatel(ethqos, RGMII_CONFIG_BYPASS_TX_ID_EN,
		      RGMII_CONFIG_BYPASS_TX_ID_EN, RGMII_IO_MACRO_CONFIG);
	rgmii_updatel(ethqos, RGMII_CONFIG_POS_NEG_DATA_SEL,
		      0, RGMII_IO_MACRO_CONFIG);
	rgmii_updatel(ethqos, RGMII_CONFIG_PROG_SWAP,
		      0, RGMII_IO_MACRO_CONFIG);

	if (ethqos->has_emac_ge_3)
		rgmii_updatel(ethqos, RGMII_CONFIG2_DATA_DIVIDE_CLK_SEL,
			      0, RGMII_IO_MACRO_CONFIG2);
	else
		rgmii_updatel(ethqos, RGMII_CONFIG2_DATA_DIVIDE_CLK_SEL,
			      RGMII_CONFIG2_DATA_DIVIDE_CLK_SEL,
			      RGMII_IO_MACRO_CONFIG2);

	rgmii_updatel(ethqos, RGMII_CONFIG2_TX_CLK_PHASE_SHIFT_EN,
		      0, RGMII_IO_MACRO_CONFIG2);

	if (ethqos->speed == SPEED_1000)
		rgmii_updatel(ethqos, RGMII_CONFIG2_RSVD_CONFIG15,
			      0, RGMII_IO_MACRO_CONFIG2);
	else
		rgmii_updatel(ethqos, RGMII_CONFIG2_RSVD_CONFIG15,
			      RGMII_CONFIG2_RSVD_CONFIG15, RGMII_IO_MACRO_CONFIG2);

	if (!ethqos->rgmii_config_loopback_en)
		rgmii_updatel(ethqos, RGMII_CONFIG_LOOPBACK_EN,
			      0, RGMII_IO_MACRO_CONFIG);

	rgmii_updatel(ethqos, RGMII_CONFIG2_RX_PROG_SWAP,
		      RGMII_CONFIG2_RX_PROG_SWAP, RGMII_IO_MACRO_CONFIG2);
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

	if (ethqos->phy_mode == PHY_INTERFACE_MODE_RGMII_ID) {
		rgmii_updatel(ethqos, SDCC_DLL_CONFIG_PDN,
			      SDCC_DLL_CONFIG_PDN, SDCC_HC_REG_DLL_CONFIG);
		rgmii_updatel(ethqos, SDCC_USR_CTL_DDR_BYPASS,
			      SDCC_USR_CTL_DDR_BYPASS, SDCC_USR_CTL);
		ethqos_rgmii_id_macro_init(ethqos);
		return 0;
	}

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

static void ethqos_set_serdes_speed(struct qcom_ethqos *ethqos, int speed)
{
	if (ethqos->serdes_speed != speed) {
		phy_set_speed(ethqos->serdes_phy, speed);
		ethqos->serdes_speed = speed;
	}
}

static void ethqos_force_macsec_bypass(struct qcom_ethqos *ethqos)
{
	void __iomem *macsec_base = ethqos->macsec_base;
	u32 val;

	if (!macsec_base)
		return;

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
		ethqos_set_serdes_speed(ethqos, SPEED_2500);
		stmmac_pcs_ctrl_ane(priv, priv->ioaddr, 0, 0, 0);
		break;
	case SPEED_1000:
		val &= ~ETHQOS_MAC_CTRL_PORT_SEL;
		rgmii_updatel(ethqos, RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_CONFIG2_RGMII_CLK_SEL_CFG,
			      RGMII_IO_MACRO_CONFIG2);
		ethqos_set_serdes_speed(ethqos, SPEED_1000);
		if (priv->plat->disable_pcs_ane)
			stmmac_pcs_ctrl_ane(priv, priv->ioaddr, 0, 0, 0);
		else
			stmmac_pcs_ctrl_ane(priv, priv->ioaddr, 1, 0, 0);
		break;
	case SPEED_100:
		val |= ETHQOS_MAC_CTRL_PORT_SEL | ETHQOS_MAC_CTRL_SPEED_MODE;
		ethqos_set_serdes_speed(ethqos, SPEED_1000);
		if (priv->plat->disable_pcs_ane)
			stmmac_pcs_ctrl_ane(priv, priv->ioaddr, 0, 0, 0);
		else
			stmmac_pcs_ctrl_ane(priv, priv->ioaddr, 1, 0, 0);
		break;
	case SPEED_10:
		val |= ETHQOS_MAC_CTRL_PORT_SEL;
		val &= ~ETHQOS_MAC_CTRL_SPEED_MODE;
		rgmii_updatel(ethqos, RGMII_CONFIG_SGMII_CLK_DVDR,
			      FIELD_PREP(RGMII_CONFIG_SGMII_CLK_DVDR,
					 SGMII_10M_RX_CLK_DVDR),
			      RGMII_IO_MACRO_CONFIG);
		ethqos_set_serdes_speed(ethqos, SPEED_1000);
		if (priv->plat->disable_pcs_ane)
			stmmac_pcs_ctrl_ane(priv, priv->ioaddr, 0, 0, 0);
		else
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

	if (ethqos->use_domains)
		qcom_ethqos_serdes_set_level(ethqos);

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

	if (ethqos->has_macsec)
		ethqos_force_macsec_bypass(ethqos);

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

	if (ethqos->use_domains)
		qcom_ethqos_serdes_set_level(ethqos);

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

static int qcom_ethqos_serdes_up(struct net_device *ndev, void *priv)
{
	struct qcom_ethqos *ethqos = priv;
	int ret = 0;

	ret = qcom_ethqos_serdes_set_level(ethqos);

	return ret;
}

static void qcom_ethqos_serdes_down(struct net_device *ndev, void *priv)
{
	struct qcom_ethqos *ethqos = priv;
	int old_speed = ethqos->speed;

	ethqos->speed = 10;
	qcom_ethqos_serdes_set_level(ethqos);

	ethqos->speed = old_speed;
}

/* callback for stmmac runtime suspend/resume functions */
static int qcom_ethqos_domain_transition_d0d1(void *priv, bool high)
{
	struct qcom_ethqos *ethqos = priv;
	int ret = 0;

	if (high) {
		ret = qcom_ethqos_domain_on(ethqos, POWER_CLK);
		if (ret < 0) {
			dev_err(&ethqos->pdev->dev, "MDIO Transition from d1 to d0 failed\n");
			return ret;
		}
	} else {
		qcom_ethqos_domain_off(ethqos, POWER_CLK);
	}
	return ret;
}

static int qcom_ethqos_domain_transition_d0d3(void *priv, bool high)
{
	struct qcom_ethqos *ethqos = priv;
	int ret = 0;

	if (high) {
		ret = qcom_ethqos_domain_on(ethqos, POWER_CORE);
		if (ret < 0) {
			dev_err(&ethqos->pdev->dev, "CORE Transition from d3 to d0 failed\n");
			return ret;
		}

		ret = qcom_ethqos_domain_on(ethqos, POWER_CLK);
		if (ret < 0) {
			dev_err(&ethqos->pdev->dev, "MDIO Transition from d3 to d0 failed\n");
			qcom_ethqos_domain_off(ethqos, POWER_CORE);
			return ret;
		}

	} else {
		qcom_ethqos_domain_off(ethqos, POWER_CLK);
		qcom_ethqos_domain_off(ethqos, POWER_CORE);
	}

	return ret;
}

static void qcom_ethqos_exit(struct platform_device *pdev, void *prv)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct qcom_ethqos *ethqos = prv;

	/* pm_runtime_disable is called on driver remove.
	 * And not called on Suspend/resume.
	 */
	if (!pm_runtime_enabled(priv->device)) {
		pm_runtime_enable(priv->device);
		qcom_ethqos_domain_transition_d0d3(prv, false);
		pm_runtime_disable(priv->device);
		dev_pm_domain_detach_list(ethqos->pd_list);
		dev_dbg(&ethqos->pdev->dev, "Detaching all Power and Perf domains");
	} else {
		dev_dbg(&ethqos->pdev->dev, "Turning off Power and Perf domains");
	}
}

static int qcom_ethqos_init(struct platform_device *pdev, void *prv)
{
	struct qcom_ethqos *ethqos = prv;

	qcom_ethqos_serdes_set_level(ethqos);

	/* Enable functional clock to prevent DMA reset to timeout due
	 * to lacking PHY clock after the hardware block has been power
	 * cycled. The actual configuration will be adjusted once
	 * ethqos_fix_mac_speed() is invoked.
	 */
	ethqos_set_func_clk_en(ethqos);

	return 0;
}

static void qcom_ethqos_switch_irq_work(struct work_struct *work)
{
	struct qcom_ethqos *ethqos =
		container_of(work, struct qcom_ethqos, switch_irq_work);
	struct device *dev = &ethqos->pdev->dev;
	struct net_device *ndev = platform_get_drvdata(ethqos->pdev);
	struct stmmac_priv *priv;

	if (!ndev) {
		dev_err(dev, "ndev is NULL in switch_irq_work\n");
		return;
	}

	priv = netdev_priv(ndev);

	dev_dbg(dev, "Switch irq work: handling switch reset\n");
	stmmac_handle_switch_reset(priv);
}

static void qcom_ethqos_switch_irq_cleanup(void *data)
{
	struct qcom_ethqos *ethqos = data;

	cancel_work_sync(&ethqos->switch_irq_work);
}

static irqreturn_t qcom_ethqos_switch_reset_detect_irq_handler(int irq, void *data)
{
	struct qcom_ethqos *ethqos = data;

	schedule_work(&ethqos->switch_irq_work);
	return IRQ_HANDLED;
}

static void qcom_ethqos_setup_switch_reset_detect_irq(struct device *dev,
						      struct qcom_ethqos *ethqos)
{
	struct device_node *np;
	int irq, ret;

	ethqos->switch_reset_detect_irq = 0;

	if (!of_property_present(dev->of_node, "qcom,en-eth-switch-dbl")) {
		dev_dbg(dev, "en-eth-switch-dbl not configured\n");
		return;
	}

	np = of_find_compatible_node(NULL, NULL, "qcom,eth-switch-dbl");
	if (!np) {
		dev_warn(dev, "eth-switch-dbl node not found\n");
		return;
	}

	irq = of_irq_get(np, 0);
	of_node_put(np);

	if (irq <= 0) {
		dev_warn(dev, "Failed to get eth-switch-dbl IRQ: %d\n", irq);
		return;
	}

	INIT_WORK(&ethqos->switch_irq_work, qcom_ethqos_switch_irq_work);

	ret = devm_add_action_or_reset(dev,
				       qcom_ethqos_switch_irq_cleanup,
				       ethqos);
	if (ret) {
		dev_err(dev, "Failed to register IRQ cleanup: %d\n", ret);
		return;
	}

	ret = devm_request_irq(dev, irq,
			       qcom_ethqos_switch_reset_detect_irq_handler,
			       IRQF_SHARED, "eth-switch-dbl", ethqos);
	if (ret) {
		dev_err(dev, "Failed to request switch reset detect IRQ %d: %d\n",
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

static int qcom_ethqos_suspend(struct device *dev, void *prv)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct qcom_ethqos *ethqos = prv;

	if (priv->dev->irq > 0)
		disable_irq(priv->dev->irq);

	set_bit(STMMAC_DOWN, &priv->state);

	dev_dbg(&ethqos->pdev->dev, "Perf/power domain will turned off by SCMI framework.");

	return 0;
}

static int qcom_ethqos_resume(struct device *dev, void *prv)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct qcom_ethqos *ethqos = prv;
	int ret = 0;

	ret = qcom_ethqos_init(ethqos->pdev, prv);

	if (priv->dev->irq > 0)
		enable_irq(priv->dev->irq);

	dev_dbg(&ethqos->pdev->dev, "Perf/power domain will turned on by SCMI framework.");

	clear_bit(STMMAC_DOWN, &priv->state);
	return ret;
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

		if (ethqos->num_noc_clks) {
			ret = clk_bulk_prepare_enable(ethqos->num_noc_clks,
						      ethqos->noc_clks);
			if (ret) {
				dev_err(&ethqos->pdev->dev,
					"NOC clocks enable failed: %d\n", ret);
				clk_disable_unprepare(ethqos->link_clk);
				return ret;
			}
		}

		/* Enable functional clock to prevent DMA reset to timeout due
		 * to lacking PHY clock after the hardware block has been power
		 * cycled. The actual configuration will be adjusted once
		 * ethqos_fix_mac_speed() is invoked.
		 */
		ethqos_set_func_clk_en(ethqos);
	} else {
		if (ethqos->num_noc_clks)
			clk_bulk_disable_unprepare(ethqos->num_noc_clks,
						   ethqos->noc_clks);
		clk_disable_unprepare(ethqos->link_clk);
	}

	return ret;
}

static void ethqos_clks_disable(void *data)
{
	ethqos_clks_config(data, false);
}

static void ethqos_disable_regulators_action(void *data)
{
	ethqos_disable_regulators(data);
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

static struct phylink_pcs *ethqos_select_xpcs(struct stmmac_priv *priv,
					      phy_interface_t interface)
{
	return priv->hw->phylink_pcs;
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

static int qcom_ethqos_update_dt_string(struct device_node *node, const char *name,
					const char *value)
{
	struct property *prop;
	int ret = 0;

	prop = kzalloc(sizeof(*prop), GFP_KERNEL);
	if (!prop)
		return -ENOMEM;

	prop->name = kstrdup(name, GFP_KERNEL);
	if (!prop->name) {
		ret = -ENOMEM;
		goto err_name;
	}

	prop->value = kstrdup(value, GFP_KERNEL);
	if (!prop->value) {
		ret = -ENOMEM;
		goto err_value;
	}

	prop->length = strlen(value) + 1;

	if (of_update_property(node, prop)) {
		ret = -ENOMEM;
		goto err_update;
	}

	return ret;
err_update:
	kfree(prop->value);
err_value:
	kfree(prop->name);
err_name:
	kfree(prop);
	return ret;
}

static int qcom_ethqos_set_fixed_link(struct platform_device *pdev,
				      struct plat_stmmacenet_data *plat)
{
	struct device_node *fixed_link_node;
	struct device *dev = &pdev->dev;
	int ret = 0;

	fixed_link_node = of_get_child_by_name(dev->of_node, "fixed-link");
	if (!fixed_link_node)
		return 0;

	ret = qcom_ethqos_update_dt_string(fixed_link_node, "status", "okay");
	if (ret == 0) {
		dev_info(dev, "qcom-ethqos: %s Fixed-link forced to 'okay'\n", __func__);

		/*
		 * As we are using fixed-link there is no need of MDIO bus data.
		 * must use devm_kfree because it was allocated with devm_kzalloc.
		 */
		if (plat->mdio_bus_data) {
			devm_kfree(dev, plat->mdio_bus_data);
			plat->mdio_bus_data = NULL;
			dev_info(dev, "qcom-ethqos: %s mdio_bus_data freed\n", __func__);
		}
	} else {
		dev_err(dev, "qcom-ethqos: Failed to update fixed-link status\n");
	}

	of_node_put(fixed_link_node);
	return ret;
}

static int qcom_ethqos_check_mdio_and_fix_link(struct platform_device *pdev,
					       struct plat_stmmacenet_data *plat)
{
	/*
	 * Save the mdio subnode that stmmac DT parsing found.  We clear
	 * plat->mdio_node for the SWITCH/fixed-link paths (which don't need
	 * MDIO), but restore it for the normal PHY path so phylink can
	 * resolve phy-handle.
	 */
	struct device_node *dt_mdio = plat->mdio_node;
	struct device *dev = &pdev->dev;
	struct device_node *fixed_link_node;

	plat->board_type = boardtype;
	plat->phy_type = phytype;
	plat->mdio_node = NULL;

	if (phytype == SWITCH) {
		dev_info(dev, "Switch detected, Enabling fixed-link\n");
		return qcom_ethqos_set_fixed_link(pdev, plat);
	}

	fixed_link_node = of_get_child_by_name(dev->of_node, "fixed-link");
	if (fixed_link_node) {
		if (of_device_is_available(fixed_link_node)) {
			dev_info(dev, "Fixed link already enabled, not using MDIO\n");

			if (plat->mdio_bus_data) {
				devm_kfree(dev, plat->mdio_bus_data);
				plat->mdio_bus_data = NULL;
			}
			of_node_put(fixed_link_node);
			return 0;
		}

		of_node_put(fixed_link_node);
	}

	/*
	 * If we are here, we are in a PHY or UNKNOWN case without a fixed-link.
	 * Ensure mdio_bus_data is allocated for MDIO bus registration.
	 */
	if (!plat->mdio_bus_data) {
		plat->mdio_bus_data = devm_kzalloc(dev,
						   sizeof(*plat->mdio_bus_data),
						   GFP_KERNEL);
		if (!plat->mdio_bus_data)
			return -ENOMEM;

		plat->mdio_bus_data->needs_reset = true;
	}

	/* Restore DT-provided mdio node for phylink phy-handle resolution. */
	if (!plat->has_virtio_mdio && dt_mdio)
		plat->mdio_node = dt_mdio;

	return 0;
}

static int qcom_ethqos_hib_restore(struct device *dev)
{
	struct net_device *ndev = NULL;
	struct qcom_ethqos *ethqos;
	struct stmmac_priv *priv;
	int ret = 0;

	ethqos = get_stmmac_bsp_priv(dev);
	if (!ethqos)
		return -ENODEV;

	ndev = dev_get_drvdata(dev);

	if (!ndev)
		return -EINVAL;

	priv = netdev_priv(ndev);

	mutex_lock(&priv->lock);

	ret = ethqos_init_regulators(ethqos);
	if (ret) {
		dev_err(dev, "%s: Regulator init failed with ret = %d\n", __func__, ret);
		goto err_restore;
	}

	if (ethqos_init_gpio(ethqos))
		dev_warn(dev, "ethqos_init_gpio failed.\n");

	ret = pm_runtime_force_resume(dev);
	if (ret) {
		dev_err(dev, "%s: Clock Enablement Failed\n", __func__);
		ethqos_free_gpios(ethqos);
		ethqos_disable_regulators(ethqos);
		goto err_restore;
	}

	/* issue netdev up to device */

	if (!netif_running(ndev)) {
		rtnl_lock();
		ret = dev_open(ndev, NULL);
		rtnl_unlock();
		if (ret) {
			dev_err(dev, "%s: dev_open failed with ret = %d\n", __func__, ret);
			pm_runtime_force_suspend(dev);
			ethqos_free_gpios(ethqos);
			ethqos_disable_regulators(ethqos);
			goto err_restore;
		}
	}

	mutex_unlock(&priv->lock);
	return ret;
err_restore:
	mutex_unlock(&priv->lock);
	return ret;
}

static int qcom_ethqos_hib_freeze(struct device *dev)
{
	struct net_device *ndev = NULL;
	struct qcom_ethqos *ethqos;
	struct stmmac_priv *priv;
	int ret = 0;

	ethqos = get_stmmac_bsp_priv(dev);
	if (!ethqos)
		return -ENODEV;

	ndev = dev_get_drvdata(dev);

	if (!ndev)
		return -EINVAL;

	priv = netdev_priv(ndev);
	mutex_lock(&priv->lock);
	if (netif_running(ndev)) {
		rtnl_lock();
		dev_close(ndev);
		rtnl_unlock();
	}

	ret = pm_runtime_force_suspend(dev);
	if (ret) {
		dev_err(dev, "%s: Clock Disablement Failed\n", __func__);
		goto err_freeze;
	}

	ethqos_disable_regulators(ethqos);
	ethqos_free_gpios(ethqos);

	priv->speed = SPEED_UNKNOWN;
	mutex_unlock(&priv->lock);
	return 0;
err_freeze:
	mutex_unlock(&priv->lock);
	return ret;
}

static int qcom_ethqos_runtime_suspend(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct stmmac_priv *priv;

	if (!ndev)
		return 0;

	priv = netdev_priv(ndev);

	return stmmac_bus_clks_config(priv, false);
}

static int qcom_ethqos_runtime_resume(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct stmmac_priv *priv;

	if (!ndev)
		return 0;

	priv = netdev_priv(ndev);
	return stmmac_bus_clks_config(priv, true);
}

static const struct dev_pm_ops qcom_ethqos_dsqb_pm_ops = {
	.freeze = qcom_ethqos_hib_freeze,
	.restore = qcom_ethqos_hib_restore,
	.thaw = qcom_ethqos_hib_restore,
	.suspend = qcom_ethqos_hib_freeze,
	.resume = qcom_ethqos_hib_restore,
	.runtime_suspend = qcom_ethqos_runtime_suspend,
	.runtime_resume = qcom_ethqos_runtime_resume,
};

static int qcom_ethqos_init_noc_clks(struct qcom_ethqos *ethqos,
				     const struct ethqos_emac_driver_data *data)
{
	struct device *dev = &ethqos->pdev->dev;
	unsigned int i;
	int ret;

	for (i = 0; i < data->num_noc_clks; i++)
		ethqos->noc_clks[i].id = data->noc_clk_cfg[i].id;
	ethqos->num_noc_clks = data->num_noc_clks;

	ret = devm_clk_bulk_get(dev, ethqos->num_noc_clks, ethqos->noc_clks);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get NOC clocks\n");

	for (i = 0; i < data->num_noc_clks; i++) {
		ret = clk_set_rate(ethqos->noc_clks[i].clk,
				   data->noc_clk_cfg[i].rate);
		if (ret)
			dev_warn(dev, "Failed to set %s rate: %d\n",
				 data->noc_clk_cfg[i].id, ret);
	}

	return 0;
}

static int qcom_ethqos_lpm_sys_suspend(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct stmmac_priv *priv;
	int ret;

	if (!ndev)
		return -EINVAL;

	priv = netdev_priv(ndev);

	ret = stmmac_suspend(dev);
	if (ret)
		return ret;

	clk_disable_unprepare(priv->plat->clk_ptp_ref);

	if (pm_runtime_status_suspended(dev))
		return 0;

	return pm_runtime_force_suspend(dev);
}

static int qcom_ethqos_lpm_sys_resume(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct stmmac_priv *priv;
	int ret;

	if (!ndev)
		return -EINVAL;

	priv = netdev_priv(ndev);

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;

	ret = clk_prepare_enable(priv->plat->clk_ptp_ref);
	if (ret) {
		pm_runtime_force_suspend(dev);
		return ret;
	}

	return stmmac_resume(dev);
}

static const struct dev_pm_ops qcom_ethqos_lpm_pm_ops = {
	.suspend = qcom_ethqos_lpm_sys_suspend,
	.resume = qcom_ethqos_lpm_sys_resume,
	.runtime_suspend = qcom_ethqos_runtime_suspend,
	.runtime_resume = qcom_ethqos_runtime_resume,
};

static int qcom_ethqos_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	const struct ethqos_emac_driver_data *data;
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device *dev = &pdev->dev;
	struct qcom_ethqos *ethqos;
	int ret, i;

#ifdef MODULE
	if (enet)
		ret = set_phy_type(enet);

	if (board)
		ret = set_board_type(board);
#endif

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get platform resources\n");

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat)) {
		return dev_err_probe(dev, PTR_ERR(plat_dat),
				     "dt configuration failed\n");
	}

	plat_dat->disable_pcs_ane =
		of_property_read_bool(pdev->dev.of_node, "disable_pcs_ane");
	dev_info(dev, "disable_pcs_ane = %d\n", plat_dat->disable_pcs_ane);

	plat_dat->clks_config = ethqos_clks_config;

	ethqos = devm_kzalloc(dev, sizeof(*ethqos), GFP_KERNEL);
	if (!ethqos)
		return -ENOMEM;

	ret = of_get_phy_mode(np, &ethqos->phy_mode);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get phy mode\n");
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
	ethqos->speed = SPEED_1000;

	if (of_property_read_bool(np, "virtio-mdio"))
		plat_dat->has_virtio_mdio = true;

	qcom_ethqos_check_mdio_and_fix_link(pdev, plat_dat);

	ethqos->rgmii_base = devm_platform_ioremap_resource_byname(pdev, "rgmii");
	if (IS_ERR(ethqos->rgmii_base))
		return dev_err_probe(dev, PTR_ERR(ethqos->rgmii_base),
				     "Failed to map rgmii resource\n");

	if (of_device_is_compatible(np, "qcom,sa8797p-ethqos") ||
	    of_device_is_compatible(np, "qcom,sa8787p-ethqos")) {
		ret = qcom_ethqos_domain_attach(ethqos);
		if (ret < 0) {
			dev_err(dev, "Failed to attach domains.\n");
			return ret;
		}

		dev_info(dev, "domains loaded successfully.\n");
		ethqos->use_domains = true;
		plat_dat->clks_config = qcom_ethqos_domain_transition_d0d1;
		plat_dat->serdes_powerup = qcom_ethqos_serdes_up;
		plat_dat->serdes_powerdown = qcom_ethqos_serdes_down;
		plat_dat->exit = qcom_ethqos_exit;
		plat_dat->init = qcom_ethqos_init;
		plat_dat->suspend = qcom_ethqos_suspend;
		plat_dat->resume = qcom_ethqos_resume;

		qcom_ethqos_domain_transition_d0d3(ethqos, true);
		qcom_ethqos_serdes_set_level(ethqos);
	}

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
	ethqos->rgmii_config_loopback_en = data->rgmii_config_loopback_en;
	ethqos->has_emac_ge_3 = data->has_emac_ge_3;
	ethqos->needs_sgmii_loopback = data->needs_sgmii_loopback;

	if (data->num_noc_clks) {
		ret = qcom_ethqos_init_noc_clks(ethqos, data);
		if (ret)
			return ret;
	}

	if (!ethqos->use_domains) {
		if (of_device_is_compatible(np, "qcom,shikra-ethqos"))
			pdev->dev.driver->pm = &qcom_ethqos_lpm_pm_ops;
		else
			pdev->dev.driver->pm = &qcom_ethqos_dsqb_pm_ops;
		ret = ethqos_init_regulators(ethqos);

		if (ret)
			return dev_err_probe(dev, ret, "ethqos_init_regulators failed\n");

		ret = devm_add_action_or_reset(dev, ethqos_disable_regulators_action, ethqos);
		if (ret)
			return ret;

		if (ethqos_init_gpio(ethqos))
			dev_warn(dev, "ethqos_init_gpio failed.\n");

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
	}

	ethqos->serdes_phy = devm_phy_optional_get(dev, "serdes");
	if (IS_ERR(ethqos->serdes_phy))
		return dev_err_probe(dev, PTR_ERR(ethqos->serdes_phy),
				     "Failed to get serdes phy\n");

	ethqos->serdes_speed = SPEED_1000;
	ethqos_update_link_clk(ethqos, SPEED_1000);
	if (!ethqos->use_domains)
		ethqos_set_func_clk_en(ethqos);

	plat_dat->bsp_priv = ethqos;
	plat_dat->fix_mac_speed = ethqos_fix_mac_speed;
	plat_dat->dump_debug_regs = rgmii_dump;
	plat_dat->ptp_clk_freq_config = ethqos_ptp_clk_freq_config;
	plat_dat->clk_ref_rate = data->axi_clk_rate;
	if (ethqos->use_domains)
		plat_dat->clk_ptp_rate = data->ptp_clk_rate;
	else
		plat_dat->ptp_clk_freq_config = ethqos_ptp_clk_freq_config;
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
	if (of_property_present(dev->of_node, "qcom-xpcs-handle")) {
		plat_dat->select_pcs = ethqos_select_xpcs;
		plat_dat->pcs_init = ethqos_xpcs_init;
		plat_dat->pcs_exit = ethqos_xpcs_exit;
		plat_dat->safety_irq = ethqos_safety_feature;
		plat_dat->safety_pcs_stats = ethqos_xpcs_safety_stats;
	}
	if (of_property_read_bool(np, "snps,tso"))
		plat_dat->flags |= STMMAC_FLAG_TSO_EN;
	if (of_device_is_compatible(np, "qcom,qcs404-ethqos"))
		plat_dat->flags |= STMMAC_FLAG_RX_CLK_RUNS_IN_LPI;
	if (data->has_integrated_pcs)
		plat_dat->flags |= STMMAC_FLAG_HAS_INTEGRATED_PCS;
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

	ret =  devm_stmmac_pltfr_probe(pdev, plat_dat, &stmmac_res);
	if (ret)
		return ret;

	qcom_ethqos_setup_switch_reset_detect_irq(dev, ethqos);
	return ret;
}

static const struct of_device_id qcom_ethqos_match[] = {
	{ .compatible = "qcom,qcs404-ethqos", .data = &emac_v2_3_0_data},
	{ .compatible = "qcom,sa8775p-ethqos", .data = &emac_v4_0_0_data},
	{ .compatible = "qcom,sc8280xp-ethqos", .data = &emac_v3_0_0_data},
	{ .compatible = "qcom,shikra-ethqos", .data = &shikra_data},
	{ .compatible = "qcom,sm8150-ethqos", .data = &emac_v2_1_0_data},
	{ .compatible = "qcom,sa8797p-ethqos", .data = &emac_v6_6_0_data},
	{ .compatible = "qcom,sa8787p-ethqos", .data = &emac_v6_6_1_data},
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_ethqos_match);

static struct platform_driver qcom_ethqos_driver = {
	.probe  = qcom_ethqos_probe,
	.shutdown = stmmac_pltfr_remove,
	.driver = {
		.name           = "qcom-ethqos",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = qcom_ethqos_match,
	},
};
module_platform_driver(qcom_ethqos_driver);

MODULE_DESCRIPTION("Qualcomm ETHQOS driver");
MODULE_LICENSE("GPL v2");
