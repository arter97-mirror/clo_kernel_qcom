// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/util_macros.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/mutex.h>
#if IS_ENABLED(CONFIG_REGULATOR_DEBUG_CONTROL)
#include <linux/regulator/debug-regulator.h>
#endif

#define REN_SLG5B_RSC_CTRL 0x110D
/* VOUT selection registers (8-bit VSEL written to reg<7:0>) */
#define REN_SLG5B_LDO1_REG 0x2000 /*VOUT = 1200 mV + 10 mV * VSEL */
#define REN_SLG5B_LDO2_REG 0x2200 /*VOUT = 1200 mV + 10 mV * VSEL */
#define REN_SLG5B_LDO3_REG 0x2300 /*VOUT = 1200 mV + 10 mV * VSEL */
#define REN_SLG5B_LDO4_REG 0x2500 /*VOUT = 1200 mV + 10 mV * VSEL */
#define REN_SLG5B_LDO5_REG 0x2700 /*VOUT = 1200 mV + 10 mV * VSEL */
#define REN_SLG5B_LDO6_REG 0x2900 /*VOUT = 400 mV +   5 mV * VSEL */
#define REN_SLG5B_LDO7_REG 0x3100 /*VOUT = 400 mV +   5 mV * VSEL */
#define REN_SLG5B_LDO8_REG 0x3200 /*VOUT = 400 mV +   5 mV * VSEL */

#define RENSLG_NUM_LDOS 8

#define SLG5BV4987X_LDO1_EN_BIT (1 << 0)
#define SLG5BV4987X_LDO2_EN_BIT (1 << 1)
#define SLG5BV4987X_LDO3_EN_BIT (1 << 2)
#define SLG5BV4987X_LDO4_EN_BIT (1 << 3)
#define SLG5BV4987X_LDO5_EN_BIT (1 << 4)
#define SLG5BV4987X_LDO6_EN_BIT (1 << 5)
#define SLG5BV4987X_LDO7_EN_BIT (1 << 6)
#define SLG5BV4987X_LDO8_EN_BIT (1 << 7)

#define SLG5BV4987X_GPIO1_MUX_ARRAY_INPUT_SEL_16_REG      0x1710
#define SLG5BV4987X_GPIO2_MUX_ARRAY_INPUT_SEL_17_REG      0x1711
#define SLG5BV4987X_GPIO3_MUX_ARRAY_INPUT_SEL_18_REG      0x1712
#define SLG5BV4987X_GPIO4_MUX_ARRAY_INPUT_SEL_19_REG      0x1713

#define LEFT "Left"
#define RIGHT "Right"

struct renslg_ldo_info {
	u16 reg;        /* Base register for this LDO */
	int min_uv;     /* Minimum voltage in µV */
	int step_uv;    /* Voltage step size in µV */
};

/* renslg data */
struct renslg_data {
	struct regmap *regmap;
	struct device *dev;
	struct i2c_client *client;
	struct regulator_init_data *reg_init_data;
	struct regulator_desc regulator_desc;
	struct regulator_dev *regulator;
	struct mutex lock;
	int resource_ctrl;
	bool left;
	int ldo_mv[RENSLG_NUM_LDOS];
	int vsel[RENSLG_NUM_LDOS];
};

static char *pmic_ldo[2][RENSLG_NUM_LDOS] = {
	{ "L1G", "L2G", "L3G", "L4G", "L5G", "L6G", "L7G", "L8G"},
	{ "L1K", "L2K", "L3K", "L4K", "L5K", "L6K", "L7K", "L8K"},
};

static const struct renslg_ldo_info renslg_ldos[RENSLG_NUM_LDOS] = {
	{ REN_SLG5B_LDO1_REG, 1200000, 10000 },
	{ REN_SLG5B_LDO2_REG, 1200000, 10000 },
	{ REN_SLG5B_LDO3_REG, 1200000, 10000 },
	{ REN_SLG5B_LDO4_REG, 1200000, 10000 },
	{ REN_SLG5B_LDO5_REG, 1200000, 10000 },
	{ REN_SLG5B_LDO6_REG,  400000,  5000 },
	{ REN_SLG5B_LDO7_REG,  400000,  5000 },
	{ REN_SLG5B_LDO8_REG,  400000,  5000 },
};

static const struct regmap_config renslg_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register   = 0x3200,
	.cache_type     = REGCACHE_RBTREE,
	.use_single_read = true,
	.use_single_write = true,
};

static char *dt_names[RENSLG_NUM_LDOS] = {
	"ldo1-mv", "ldo2-mv", "ldo3-mv", "ldo4-mv", "ldo5-mv", "ldo6-mv", "ldo7-mv", "ldo8-mv"
};

void set_ldo(struct device *dev, int index)
{
	int ret;
	int vsel;
	struct i2c_client *client = to_i2c_client(dev);
	struct renslg_data *pdata = (struct renslg_data *)
						i2c_get_clientdata(client);

	vsel = ((pdata->ldo_mv[index] -
		(renslg_ldos[index].min_uv/1000)) /
			 (renslg_ldos[index].step_uv/1000));
	pdata->vsel[index] = vsel;
	dev_dbg(dev, "index:%d mv:%d writing vsel:%d",
			 index, pdata->ldo_mv[index], vsel);
	ret = regmap_write(pdata->regmap, renslg_ldos[index].reg, vsel);
	if (ret < 0)
		dev_err(dev, "failed to write REN_SLG5B_LDO1_REG vsel:%d, ret:%x\n",
			 vsel, ret);
}

static ssize_t renslg_type_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct renslg_data *chip;
	int rc;

	chip = (struct renslg_data *)i2c_get_clientdata(client);

	if (chip->left)
		rc = scnprintf(buf, 6, "%s\n", LEFT);
	else
		rc = scnprintf(buf, 7, "%s\n", RIGHT);

	return rc;
}

static ssize_t renslg_type_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int pmic, ldo, enable, ret = 0;
	struct regulator *ldo1;
	struct i2c_client *client = to_i2c_client(dev);
	struct renslg_data *pdata = (struct renslg_data *) i2c_get_clientdata(client);
	int ldos[2][RENSLG_NUM_LDOS] = {
	{ 2800000, 2800000, 2800000, 1800000, 1800000, 1100000, 1050000, 800000,},
	{ 2800000, 2800000, 2800000, 1800000, 1800000, 1100000, 1200000, 800000,},
	};

	if (sscanf(buf, "%x %x %x", &pmic, &ldo, &enable) != 3) {
		dev_err(dev,
		 "Invalid fmt,expected: PMIC#(0/1), LDO#(0-7), Enable(0/1)\n");
		return -EINVAL;
	}

	dev_dbg(dev, "pmic:%d, ldo:%d, enable:%d\n", pmic, ldo, enable);

	if (pmic < 0 || pmic >= 2) {
		dev_err(dev, "PMIC number out of range, %d\n", pmic);
		return -EINVAL;
	}

	if (pdata->left && pmic == 1) {
		dev_err(dev, "%s Incorrect PMIC number:%d, pl use 0 as pmic\n",
			(pdata->left ? LEFT : RIGHT), pmic);
		return -EINVAL;
	} else if (!pdata->left && pmic == 0) {
		dev_err(dev, "%s Incorrect PMIC number:%d, pl use 1 as pmic\n",
			(pdata->left ? LEFT : RIGHT), pmic);
		return -EINVAL;
	}

	if (ldo < 0 || ldo > RENSLG_NUM_LDOS) {
		dev_err(dev, "LDO number out of range, %d\n", ldo);
		return -EINVAL;
	}

	dev_dbg(dev, "ldo_name:%s\n", pmic_ldo[pmic][ldo]);
	/* matches DT supply */
	ldo1 = devm_regulator_get(dev, pmic_ldo[pmic][ldo]);
	if (IS_ERR(ldo1)) {
		dev_err(dev, "error getting regulator:%s\n",
				 pmic_ldo[pmic][ldo]);
		return PTR_ERR(ldo1);
	}

	dev_dbg(dev, "ldo:%s, voltage:%d\n",
				 pmic_ldo[pmic][ldo], ldos[pmic][ldo]);
	regulator_set_voltage(ldo1, ldos[pmic][ldo], ldos[pmic][ldo]);
	ret = regulator_enable(ldo1);
	if (ret)
		dev_err(dev, "Not able to enable regulator:%s, ret:%d\n",
					 pmic_ldo[pmic][ldo], ret);

	dev_dbg(dev, "after successfully enabling regulator:%s\n",
				 pmic_ldo[pmic][ldo]);
	regulator_disable(ldo1);

	return count;
}
static DEVICE_ATTR_RW(renslg_type);

static ssize_t renslg_disablereg_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return 0;
};

static void renslg_resource_set(struct renslg_data *pdata,
			unsigned int bit, bool enable)
{
	dev_dbg(pdata->dev, "%s enable:%d\n",
		(pdata->left ? LEFT : RIGHT), enable);
	if (enable)
		pdata->resource_ctrl |= BIT(bit);
	else
		pdata->resource_ctrl &= ~BIT(bit);
}

static int disable_reg(struct renslg_data *pdata, int id)
{
	int ret;

	mutex_lock(&pdata->lock);
	renslg_resource_set(pdata, id, false);
	ret = regmap_write(pdata->regmap, REN_SLG5B_RSC_CTRL,
				 pdata->resource_ctrl);
	mutex_unlock(&pdata->lock);

	return ret;
}

static ssize_t renslg_disablereg_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct renslg_data *pdata = (struct renslg_data *)
						i2c_get_clientdata(client);
	int rc, pmic = 0, ldo, i;

	if (kstrtoint(buf, 0, &ldo)) {
		dev_err(dev, "Invalid fmt,expected: LDO#(0-7, 8-ALL)\n");
		return -EINVAL;
	}

	if (ldo < 0 || ldo > RENSLG_NUM_LDOS) {
		dev_err(dev, "LDO number out of range, %d\n", ldo);
		return -EINVAL;
	}

	if (!pdata->left)
		pmic = 1;

	if (ldo == RENSLG_NUM_LDOS) {
		dev_dbg(dev, "Disbaling all LDO's for the pmic:%s\n",
			 (pdata->left ? LEFT : RIGHT));
		for (i = 0; i < RENSLG_NUM_LDOS; i++) {
			dev_dbg(dev, "Disabling ldo_name:%s\n",
					 pmic_ldo[pmic][i]);
			rc = disable_reg(pdata, i);
		}
	} else {
		dev_dbg(dev, "Disabling ldo_name:%s\n", pmic_ldo[pmic][ldo]);
		rc = disable_reg(pdata, ldo);
	}

	return count;
}
static DEVICE_ATTR_RW(renslg_disablereg);

static struct attribute *renslg_attrs[] = {
	&dev_attr_renslg_type.attr,
	&dev_attr_renslg_disablereg.attr,
	NULL
};

static const struct attribute_group renslg_attr_group = {
	.attrs = renslg_attrs,
};

static int renslg_ldo_enable(struct regulator_dev *rdev)
{
	struct renslg_data *pdata =
		(struct renslg_data *)rdev_get_drvdata(rdev);
	int id = rdev_get_id(rdev);
	int ret = 0;

	mutex_lock(&pdata->lock);
	renslg_resource_set(pdata, id, true);
	ret = regmap_write(pdata->regmap,
		 REN_SLG5B_RSC_CTRL, pdata->resource_ctrl);
	if (!pdata->left && id == 4) {
	/* Enable GPIO3 to enable LDO Switch for 6DOF NPU and Mono camera */
		ret = regmap_write(pdata->regmap,
			 SLG5BV4987X_GPIO3_MUX_ARRAY_INPUT_SEL_18_REG, 0x1);
		if (ret < 0)
			dev_err(pdata->dev,
				"failed to WR SLG5BV4987X_GPIO3 SLG5BR to 0x1 ret:%x\n", ret);
	}
	mutex_unlock(&pdata->lock);

	return ret;
}

/* Disable an LDO */
static int renslg_ldo_disable(struct regulator_dev *rdev)
{
	struct renslg_data *pdata =
		 (struct renslg_data *)rdev_get_drvdata(rdev);
	int id = rdev_get_id(rdev);
	int ret = 0;

	mutex_lock(&pdata->lock);
	renslg_resource_set(pdata, id, false);
	ret = regmap_write(pdata->regmap, REN_SLG5B_RSC_CTRL,
			 pdata->resource_ctrl);
	mutex_unlock(&pdata->lock);

	return ret;
}

static int renslg_ldo_is_enabled(struct regulator_dev *rdev)
{
	struct renslg_data *pdata =
		 (struct renslg_data *)rdev_get_drvdata(rdev);
	int id = rdev_get_id(rdev);

	dev_dbg(pdata->dev, "%s id:%d, !!(pdata->resource_ctrl & BIT(id)):%d\n",
	(pdata->left ? LEFT : RIGHT), id, !!(pdata->resource_ctrl & BIT(id)));

	return !!(pdata->resource_ctrl & BIT(id));
}

static int renslg_ldo_set_voltage(struct regulator_dev *rdev,
		int min_uV, int max_uV, unsigned int *selector)
{
	struct renslg_data *pdata =
		 (struct renslg_data *)rdev_get_drvdata(rdev);
	const struct renslg_ldo_info *ldo = &renslg_ldos[rdev_get_id(rdev)];
	int steps, ret;

	if (min_uV < ldo->min_uv)
		return -EINVAL;

	steps = (min_uV - ldo->min_uv) / ldo->step_uv;
	*selector = steps;

	mutex_lock(&pdata->lock);
	ret = regmap_write(pdata->regmap, ldo->reg, (u8)steps);
	mutex_unlock(&pdata->lock);

	return ret;
}

static int renslg_ldo_get_voltage(struct regulator_dev *rdev)
{
	struct renslg_data *pdata =
		 (struct renslg_data *)rdev_get_drvdata(rdev);
	const struct renslg_ldo_info *ldo = &renslg_ldos[rdev_get_id(rdev)];
	int val = 0, ret, id;

	id = rdev_get_id(rdev);
	ret = regmap_read(pdata->regmap, ldo->reg, &val);
	if (ret < 0) {
		dev_err(pdata->dev,
			 "failed to read data for %s, ldo#:%d  ldo->reg:%x, val:%x\n",
			(pdata->left ? LEFT : RIGHT), id, ldo->reg, val);
		return ret;
	}
	ret = ldo->min_uv + (val * ldo->step_uv);
	dev_dbg(pdata->dev, "ldo#:%d voltage:%d\n", id, ret);

	return ret;
}

static const struct regulator_ops renslg_ldo_ops = {
	.enable       = renslg_ldo_enable,
	.disable      = renslg_ldo_disable,
	.is_enabled   = renslg_ldo_is_enabled,
	.set_voltage  = renslg_ldo_set_voltage,
	.get_voltage  = renslg_ldo_get_voltage,
};

static int renslg_parse_dt(struct renslg_data *data)
{
	int ret = 0, i = 0;
	struct device *dev = data->dev;
	struct device_node *np = dev->of_node;

	data->left = of_property_read_bool(np, "left");
	dev_dbg(dev, "******************data->left:%d\n", data->left);

	for (i = 0; i < RENSLG_NUM_LDOS; i++) {
		ret = of_property_read_u32(np, dt_names[i], &data->ldo_mv[i]);
		if (ret) {
			dev_err(dev, "index:%d name:%s missing\n", i, dt_names[i]);
			return ret;
		}
	}
	return ret;
}

int renslg_regulator_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct renslg_data *pdata;
	struct regulator_config cfg = {};
	struct device_node *regs_np;
	struct device_node *reg_np;
	int i, ret = 0;

	pdata = devm_kzalloc(dev, sizeof(struct renslg_data), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->dev = dev;
	pdata->client = client;
	i2c_set_clientdata(client, pdata);

	pinctrl_pm_select_default_state(dev);

	ret = renslg_parse_dt(pdata);
	if (ret < 0) {
		dev_err(pdata->dev, "failed to parse DT config: %d\n", ret);
		return ret;
	}

	pdata->regmap = devm_regmap_init_i2c(client, &renslg_regmap_config);
	if (IS_ERR(pdata->regmap)) {
		ret = PTR_ERR(pdata->regmap);
		if (ret != -EPROBE_DEFER)
			dev_err(pdata->dev, "failed to initialize regmap: %d\n", ret);
		return ret;
	}

	devm_mutex_init(&client->dev, &pdata->lock);

	cfg.dev = &client->dev;
	cfg.driver_data = pdata;
	regs_np = of_get_child_by_name(dev->of_node, "regulators");
	if (!regs_np) {
		dev_err(&client->dev, "can't get regulators node\n");
		return -ENODEV;
	}

	for (i = 0; i < RENSLG_NUM_LDOS; i++) {
		struct regulator_desc *desc;
		struct regulator_dev  *rdev;

		desc = devm_kzalloc(&client->dev, sizeof(*desc), GFP_KERNEL);
		if (!desc) {
			ret = -ENOMEM;
			goto error;
		}

		desc->name = devm_kasprintf(&client->dev, GFP_KERNEL,
			"ldo%d", i + 1);

		reg_np = of_get_child_by_name(regs_np, desc->name);
		cfg.of_node = reg_np;

		if (pdata->left)
			desc->name = devm_kasprintf(&client->dev, GFP_KERNEL,
				"L%dG", i + 1);
		else
			desc->name = devm_kasprintf(&client->dev, GFP_KERNEL,
				"L%dK", i + 1);


		desc->id         = i;
		desc->type       = REGULATOR_VOLTAGE;
		desc->ops        = &renslg_ldo_ops;
		desc->owner      = THIS_MODULE;
		desc->min_uV     = renslg_ldos[i].min_uv;
		desc->uV_step    = renslg_ldos[i].step_uv;
		desc->n_voltages = 256;  /* 8-bit VSET register */
		desc->of_match = desc->name;

		rdev = devm_regulator_register(&client->dev, desc, &cfg);
		if (IS_ERR(rdev)) {
			ret = PTR_ERR(rdev);
			dev_err(&client->dev, "%s Failed to register LDO%d: %d\n",
				(pdata->left ? LEFT : RIGHT), i + 1, ret);
			goto error;
		}

		renslg_ldo_enable(rdev);
		set_ldo(dev, i);
		devm_regulator_debug_register(&client->dev, rdev);
	}

	ret = sysfs_create_group(&client->dev.kobj, &renslg_attr_group);
	if (ret) {
		dev_err(&client->dev,
			 "%s failed to create sysfs group, err:%d\n",
				(pdata->left ? LEFT : RIGHT), ret);
		return ret;
	}

error:
	of_node_put(reg_np);
	of_node_put(regs_np);

	return ret;
};

static const struct of_device_id renslg_of_match_table[] = {
	{ .compatible = "renesas,slg5b", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, renslg_of_match_table);

static struct i2c_driver renslg_regulator_driver = {
	.driver = {
		.name = "slg5b",
		.of_match_table = renslg_of_match_table,
	},
	.probe = renslg_regulator_probe,
};

module_i2c_driver(renslg_regulator_driver);

MODULE_DESCRIPTION("Renesas SLG5BV49293 regulator driver");
MODULE_LICENSE("GPL");
