// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include <linux/regulator/consumer.h>

#define USB_PHY_UTMI_CTRL0		(0x3c)
#define SLEEPM				BIT(0)

#define USB_PHY_UTMI_CTRL5		(0x50)
#define POR				BIT(1)

#define USB_PHY_HS_PHY_CTRL_COMMON0	(0x54)
#define PHY_ENABLE			BIT(0)
#define SIDDQ_SEL			BIT(1)
#define SIDDQ				BIT(2)
#define FSEL				GENMASK(6, 4)
#define FSEL_38_4_MHZ_VAL		(0x6)

#define USB_PHY_HS_PHY_CTRL2		(0x64)
#define USB2_SUSPEND_N			BIT(2)
#define USB2_SUSPEND_N_SEL		BIT(3)

#define USB_PHY_CFG0			(0x94)
#define UTMI_PHY_CMN_CTRL_OVERRIDE_EN	BIT(1)

#define USB_PHY_CFG1			(0x154)
#define PLL_EN				BIT(0)

#define USB_PHY_FSEL_SEL		(0xb8)
#define FSEL_SEL			BIT(0)

#define USB_PHY_XCFGI_39_32		(0x16c)
#define HSTX_PE				GENMASK(3, 2)

#define USB_PHY_XCFGI_71_64		(0x17c)
#define HSTX_SWING			GENMASK(3, 0)

#define USB_PHY_XCFGI_31_24		(0x168)
#define HSTX_SLEW			GENMASK(2, 0)

#define USB_PHY_XCFGI_7_0		(0x15c)
#define PLL_LOCK_TIME			GENMASK(1, 0)

/* EUD CSR field */
#define EUD_EN2				BIT(0)

/* VIOCTL_EUD_DETECT register based EUD_DETECT field */
#define EUD_DETECT			BIT(0)

#define M31_EUSB_PHY_INIT_CFG(o, b, v)	\
{				\
	.off = o,		\
	.mask = b,		\
	.val = v,		\
}

struct m31_phy_tbl_entry {
	u32 off;
	u32 mask;
	u32 val;
};

struct m31_eusb2_priv_data {
	const struct m31_phy_tbl_entry	*setup_seq;
	unsigned int			setup_seq_nregs;
	const struct m31_phy_tbl_entry	*override_seq;
	unsigned int			override_seq_nregs;
	const struct m31_phy_tbl_entry	*reset_seq;
	unsigned int			reset_seq_nregs;
	unsigned int			fsel;
};

static const struct m31_phy_tbl_entry m31_eusb2_setup_tbl[] = {
	M31_EUSB_PHY_INIT_CFG(USB_PHY_CFG0, UTMI_PHY_CMN_CTRL_OVERRIDE_EN, 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_UTMI_CTRL5, POR, 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_HS_PHY_CTRL_COMMON0, PHY_ENABLE, 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_CFG1, PLL_EN, 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_FSEL_SEL, FSEL_SEL, 1),
};

static const struct m31_phy_tbl_entry m31_eusb_phy_override_tbl[] = {
	M31_EUSB_PHY_INIT_CFG(USB_PHY_XCFGI_39_32, HSTX_PE, 0),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_XCFGI_71_64, HSTX_SWING, 7),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_XCFGI_31_24, HSTX_SLEW, 0),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_XCFGI_7_0, PLL_LOCK_TIME, 0),
};

static const struct m31_phy_tbl_entry m31_eusb_phy_reset_tbl[] = {
	M31_EUSB_PHY_INIT_CFG(USB_PHY_HS_PHY_CTRL2, USB2_SUSPEND_N_SEL, 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_HS_PHY_CTRL2, USB2_SUSPEND_N, 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_UTMI_CTRL0, SLEEPM, 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_HS_PHY_CTRL_COMMON0, SIDDQ_SEL, 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_HS_PHY_CTRL_COMMON0, SIDDQ, 0),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_UTMI_CTRL5, POR, 0),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_HS_PHY_CTRL2, USB2_SUSPEND_N_SEL, 0),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_CFG0, UTMI_PHY_CMN_CTRL_OVERRIDE_EN, 0),
};

static const struct regulator_bulk_data m31_eusb_phy_vregs[] = {
	{ .supply = "vdd" },
	{ .supply = "vdda12" },
	{ .supply = "vdd_refgen" },
};

#define M31_EUSB_NUM_VREGS		ARRAY_SIZE(m31_eusb_phy_vregs)

struct m31eusb2_phy {
	struct phy			 *phy;
	void __iomem			 *base;
	void __iomem			 *eud_enable_reg;
	void __iomem			 *eud_detect_reg;
	const struct m31_eusb2_priv_data *data;
	enum phy_mode			 mode;

	struct regulator_bulk_data	 *vregs;
	struct clk			 *clk;
	struct reset_control		 *reset;
	struct clk			 *ref_clk_src;
	struct clk			 *ref_clk;

	struct phy			 *repeater;
	bool				 clocks_enabled;
	bool				 power_enabled;
	bool				 repeater_enabled;
};

static inline bool is_eud_debug_mode_active(struct m31eusb2_phy *phy)
{
	if (phy->eud_enable_reg &&
	    (readl_relaxed(phy->eud_enable_reg) & EUD_EN2))
		return true;

	return false;
}

static void m31eusb2_phy_update_eud_detect(struct m31eusb2_phy *phy, bool set)
{
	if (!phy->eud_detect_reg)
		return;

	/*
	 * eud_detect_reg is shared across ports (one EUD_DETECT bit per port),
	 * so read-modify-write to avoid clearing the other ports' bits.
	 */
	if (set)
		writel_relaxed(readl_relaxed(phy->eud_detect_reg) | EUD_DETECT,
			       phy->eud_detect_reg);
	else
		writel_relaxed(readl_relaxed(phy->eud_detect_reg) & ~EUD_DETECT,
			       phy->eud_detect_reg);
}

static int m31eusb2_phy_write_readback(void __iomem *base, u32 offset,
					const u32 mask, u32 val)
{
	u32 write_val;
	u32 tmp;

	tmp = readl_relaxed(base + offset);
	tmp &= ~mask;
	write_val = tmp | val;

	writel_relaxed(write_val, base + offset);

	tmp = readl_relaxed(base + offset);
	tmp &= mask;

	if (tmp != val) {
		pr_err("write: %x to offset: %x FAILED\n", val, offset);
		return -EINVAL;
	}

	return 0;
}

static int m31eusb2_phy_write_sequence(struct m31eusb2_phy *phy,
				       const struct m31_phy_tbl_entry *tbl,
				       int num)
{
	int i;
	int ret;

	for (i = 0 ; i < num; i++, tbl++) {
		dev_dbg(&phy->phy->dev, "Offset:%x BitMask:%x Value:%x",
			tbl->off, tbl->mask, tbl->val);

		ret = m31eusb2_phy_write_readback(phy->base,
						   tbl->off, tbl->mask,
						   tbl->val << __ffs(tbl->mask));
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int m31eusb2_phy_set_mode(struct phy *uphy, enum phy_mode mode, int submode)
{
	struct m31eusb2_phy *phy = phy_get_drvdata(uphy);

	phy->mode = mode;

	return phy_set_mode_ext(phy->repeater, mode, submode);
}

static int m31eusb2_phy_clocks(struct m31eusb2_phy *phy, bool on)
{
	int ret;

	if (phy->clocks_enabled == on)
		return 0;

	if (on) {
		ret = clk_prepare_enable(phy->ref_clk_src);
		if (ret)
			return ret;

		if (phy->ref_clk)
			ret = clk_prepare_enable(phy->ref_clk);
	} else {
		if (phy->ref_clk)
			clk_disable_unprepare(phy->ref_clk);

		clk_disable_unprepare(phy->ref_clk_src);
	}
	phy->clocks_enabled = on;

	return 0;
}

static int m31eusb2_phy_power(struct m31eusb2_phy *phy, bool on)
{
	int ret;

	if (phy->power_enabled == on)
		return 0;

	if (on) {
		ret = regulator_bulk_enable(M31_EUSB_NUM_VREGS, phy->vregs);
		if (ret)
			return ret;

		/*
		 * Set eud_detect_reg after powering on eUSB PHY rails to bring
		 * EUD out of reset
		 */
		m31eusb2_phy_update_eud_detect(phy, true);
	} else {
		/* Clear eud_detect_reg to put EUD in reset */
		m31eusb2_phy_update_eud_detect(phy, false);

		/* Ensure the register write is completed */
		mb();

		regulator_bulk_disable(M31_EUSB_NUM_VREGS, phy->vregs);
	}
	phy->power_enabled = on;

	return 0;
}

static int m31eusb2_phy_repeater_init(struct m31eusb2_phy *phy, bool init)
{
	int ret;

	if (phy->repeater_enabled == init)
		return 0;

	if (init) {
		ret = phy_init(phy->repeater);
		if (ret)
			return ret;
	} else {
		phy_exit(phy->repeater);
	}
	phy->repeater_enabled = init;

	return 0;
}

static int m31eusb2_phy_init(struct phy *uphy)
{
	struct m31eusb2_phy *phy = phy_get_drvdata(uphy);
	const struct m31_eusb2_priv_data *data = phy->data;
	int ret;

	ret = m31eusb2_phy_power(phy, true);
	if (ret) {
		dev_err(&uphy->dev, "failed to enable regulator, %d\n", ret);
		return ret;
	}

	ret = m31eusb2_phy_repeater_init(phy, true);
	if (ret) {
		dev_err(&uphy->dev, "repeater init failed. %d\n", ret);
		goto disable_vreg;
	}

	ret = m31eusb2_phy_clocks(phy, true);
	if (ret) {
		dev_err(&uphy->dev, "failed to enable phy clock, %d\n", ret);
		goto disable_repeater;
	}

	/* Dont reset the PHY if EUD is active */
	if (is_eud_debug_mode_active(phy))
		return 0;

	/* Perform phy reset */
	reset_control_assert(phy->reset);
	udelay(5);
	reset_control_deassert(phy->reset);

	m31eusb2_phy_write_sequence(phy, data->setup_seq, data->setup_seq_nregs);
	m31eusb2_phy_write_readback(phy->base,
				     USB_PHY_HS_PHY_CTRL_COMMON0, FSEL,
				     FIELD_PREP(FSEL, data->fsel));
	m31eusb2_phy_write_sequence(phy, data->override_seq, data->override_seq_nregs);
	m31eusb2_phy_write_sequence(phy, data->reset_seq, data->reset_seq_nregs);

	return 0;

disable_repeater:
	m31eusb2_phy_repeater_init(phy, false);
disable_vreg:
	m31eusb2_phy_power(phy, false);

	return 0;
}

static int m31eusb2_phy_exit(struct phy *uphy)
{
	struct m31eusb2_phy *phy = phy_get_drvdata(uphy);

	m31eusb2_phy_clocks(phy, false);
	m31eusb2_phy_repeater_init(phy, false);
	m31eusb2_phy_power(phy, false);

	return 0;
}

static const struct phy_ops m31eusb2_phy_gen_ops = {
	.init		= m31eusb2_phy_init,
	.exit		= m31eusb2_phy_exit,
	.set_mode	= m31eusb2_phy_set_mode,
	.owner		= THIS_MODULE,
};

static int m31eusb2_phy_runtime_suspend(struct device *dev)
{
	struct m31eusb2_phy *phy = dev_get_drvdata(dev);

	dev_dbg(dev, "Suspending M31 eUSB2 Phy\n");

	m31eusb2_phy_clocks(phy, false);

	return 0;
}

static int m31eusb2_phy_runtime_resume(struct device *dev)
{
	struct m31eusb2_phy *phy = dev_get_drvdata(dev);

	dev_dbg(dev, "Resuming M31 eUSB2 Phy\n");

	m31eusb2_phy_clocks(phy, true);

	return 0;
}

static const struct dev_pm_ops m31eusb2_phy_pm_ops = {
	SET_RUNTIME_PM_OPS(m31eusb2_phy_runtime_suspend,
			   m31eusb2_phy_runtime_resume, NULL)
};

static int m31eusb2_phy_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	const struct m31_eusb2_priv_data *data;
	struct device *dev = &pdev->dev;
	struct m31eusb2_phy *phy;
	int ret;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	data = device_get_match_data(dev);
	if (IS_ERR(data))
		return -EINVAL;
	phy->data = data;

	phy->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(phy->base))
		return PTR_ERR(phy->base);

	phy->eud_enable_reg = devm_platform_ioremap_resource_byname(pdev,
						"eud_enable_reg");
	if (IS_ERR(phy->eud_enable_reg)) {
		phy->eud_enable_reg = NULL;
		dev_info(dev, "missing eud_enable register address\n");
	}

	phy->eud_detect_reg = devm_platform_ioremap_resource_byname(pdev,
						"eud_detect_reg");
	if (IS_ERR(phy->eud_detect_reg)) {
		phy->eud_detect_reg = NULL;
		dev_info(dev, "missing eud_detect register address\n");
	}

	phy->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(phy->reset))
		return PTR_ERR(phy->reset);

	phy->ref_clk_src = devm_clk_get(dev, "ref_clk_src");
	if (IS_ERR(phy->ref_clk_src))
		return dev_err_probe(dev, PTR_ERR(phy->ref_clk_src),
				     "failed to get ref clk src\n");

	phy->ref_clk = devm_clk_get_optional(dev, "ref_clk");
	if (IS_ERR(phy->ref_clk))
		return dev_err_probe(dev, PTR_ERR(phy->ref_clk),
				     "failed to get ref clk\n");

	dev_set_drvdata(dev, phy);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	phy->phy = devm_phy_create(dev, NULL, &m31eusb2_phy_gen_ops);
	if (IS_ERR(phy->phy))
		return dev_err_probe(dev, PTR_ERR(phy->phy),
				     "failed to create phy\n");

	ret = devm_regulator_bulk_get_const(dev, M31_EUSB_NUM_VREGS,
					    m31_eusb_phy_vregs, &phy->vregs);
	if (ret)
		return dev_err_probe(dev, ret,
				"failed to get regulator supplies\n");

	phy_set_drvdata(phy->phy, phy);

	phy->repeater = devm_of_phy_get_by_index(dev, dev->of_node, 0);
	if (IS_ERR(phy->repeater))
		return dev_err_probe(dev, PTR_ERR(phy->repeater),
				     "failed to get repeater\n");

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (!IS_ERR(phy_provider))
		dev_info(dev, "Registered M31 USB phy\n");

	/*
	 * EUD may be enabled in boot loader and to keep EUD session alive across
	 * kernel boot, initialise HS PHY.
	 */
	if (is_eud_debug_mode_active(phy)) {
		m31eusb2_phy_power(phy, true);
		m31eusb2_phy_repeater_init(phy, true);
		m31eusb2_phy_clocks(phy, true);
	}

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct m31_eusb2_priv_data m31_eusb_v1_data = {
	.setup_seq = m31_eusb2_setup_tbl,
	.setup_seq_nregs = ARRAY_SIZE(m31_eusb2_setup_tbl),
	.override_seq = m31_eusb_phy_override_tbl,
	.override_seq_nregs = ARRAY_SIZE(m31_eusb_phy_override_tbl),
	.reset_seq = m31_eusb_phy_reset_tbl,
	.reset_seq_nregs = ARRAY_SIZE(m31_eusb_phy_reset_tbl),
	.fsel = FSEL_38_4_MHZ_VAL,
};

static const struct of_device_id m31eusb2_phy_id_table[] = {
	{ .compatible = "qcom,m31-eusb2-phy", .data = &m31_eusb_v1_data },
	{ },
};
MODULE_DEVICE_TABLE(of, m31eusb2_phy_id_table);

static struct platform_driver m31eusb2_phy_driver = {
	.probe = m31eusb2_phy_probe,
	.driver = {
		.name = "qcom-m31eusb2-phy",
		.pm = &m31eusb2_phy_pm_ops,
		.of_match_table = m31eusb2_phy_id_table,
	},
};

module_platform_driver(m31eusb2_phy_driver);

MODULE_DESCRIPTION("eUSB2 Qualcomm M31 HSPHY driver");
MODULE_LICENSE("GPL");
