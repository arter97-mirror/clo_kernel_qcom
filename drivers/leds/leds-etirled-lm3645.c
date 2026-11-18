// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>

/* Register map (Table 7-7)
 * MAINREG register map (Table 6-5)
 */
#define LM3645_REG_CTRL_REG1            0x00
#define LM3645_REG_CTRL_REG2            0x01
#define LM3645_REG_D_MODE_REG           0x02
#define LM3645_REG_STR_CTRL_REG         0x03
#define LM3645_REG_STR_TIME_REG         0x04
#define LM3645_REG_D1_FLASH_REG         0x05
#define LM3645_REG_D2_FLASH_REG         0x06
#define LM3645_REG_D3_FLASH_REG         0x07
#define LM3645_REG_D4_FLASH_REG         0x08
#define LM3645_REG_D1_TORCH_REG         0x09
#define LM3645_REG_D2_TORCH_REG         0x0A
#define LM3645_REG_D3_TORCH_REG         0x0B
#define LM3645_REG_D4_TORCH_REG         0x0C
#define LM3645_REG_CUR_RAMP_REG         0x12
#define LM3645_REG_FLAG_RPT_REG         0x14
#define LM3645_REG_VOLT_FAULT_REG       0x15
#define LM3645_REG_THERM_FAULT_REG      0x16
#define LM3645_REG_DEV_INFO_REG         0x1B

/* CTRL_REG2 bits (Figure 6-16) */
#define LM3645_CTRL2_D1_EN              (1 << 0)
#define LM3645_CTRL2_D2_EN              (1 << 1)
#define LM3645_CTRL2_D3_EN              (1 << 2)
#define LM3645_CTRL2_D4_EN              (1 << 3)
#define LM3645_CTRL2_STR1_EN            (1 << 4)
#define LM3645_CTRL2_STR2_EN            (1 << 5)
#define LM3645_CTRL2_TOR_TX_EN          (1 << 6)
/* 0=Tx, 1=Torch */
#define LM3645_CTRL2_TOR_TX_MODE        (1 << 7)

/* D_MODE_REG (Figure 6-17): 2-bit per channel */
#define LM3645_DMODE_OFF                0x0
#define LM3645_DMODE_IR                 0x1
#define LM3645_DMODE_TORCH              0x2
#define LM3645_DMODE_FLASH              0x3

#define LM3645_DMODE_D1_SHIFT           0
#define LM3645_DMODE_D2_SHIFT           2
#define LM3645_DMODE_D3_SHIFT           4
#define LM3645_DMODE_D4_SHIFT           6

/* CUR_RAMP_REG (Figure 6-33): Torch_Ramp bits[2:0] */
#define LM3645_TORCH_RAMP_DISABLED      0x0

#define LM3645_DEV_INFO_REG_VAL         0x41

#define D1_ENABLED "D1_Enabled"
#define D1_DISABLED "D1_Disabled"

#define D1 "d1"
#define D2 "d2"
#define D3 "d3"
#define D4 "d4"

struct lm3645_etirled {
	struct mutex m_lock;
	struct i2c_client *client;
	struct regmap *regmap;
	int num_leds;
	int reg;
	int val;
	bool d1_toggle;
	bool d2_toggle;
	bool d3_toggle;
	bool d4_toggle;
};

static const struct regmap_config lm3645_etirled_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x1C,
};

static ssize_t etirled_reg_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lm3645_etirled *chip;
	int rc;

	chip = (struct lm3645_etirled *)i2c_get_clientdata(client);

	rc = scnprintf(buf, 10, "%x\n", chip->reg);
	dev_dbg(dev, "reg:%x\n", chip->reg);
	return rc;
}

static ssize_t etirled_reg_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lm3645_etirled *chip;
	int rc;
	int reg;

	rc = kstrtoint(buf, 0, &reg);
	if (rc) {
		dev_err(dev, "Error in strtoint conversion\n");
		return rc;
	}

	/* Validate register address */
	if (reg < LM3645_REG_CTRL_REG1 || reg > LM3645_REG_DEV_INFO_REG) {
		dev_err(dev, "Register address 0x%x out of range [0x%x-0x%x]\n",
			reg, LM3645_REG_CTRL_REG1, LM3645_REG_DEV_INFO_REG);
		return -EINVAL;
	}

	chip = (struct lm3645_etirled *)i2c_get_clientdata(client);
	mutex_lock(&chip->m_lock);
	chip->reg = reg;
	mutex_unlock(&chip->m_lock);
	dev_dbg(dev, "reg:%x\n", chip->reg);

	return count;
}
static DEVICE_ATTR_RW(etirled_reg);

static ssize_t etirled_val_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lm3645_etirled *chip;
	int rc;

	chip = (struct lm3645_etirled *)i2c_get_clientdata(client);

	rc = scnprintf(buf, 10, "%x\n", chip->val);
	dev_dbg(dev, "val:%x\n", chip->val);
	return rc;
}

static ssize_t etirled_val_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lm3645_etirled *chip;
	int rc;
	int val;

	rc = kstrtoint(buf, 0, &val);
	if (rc) {
		dev_err(dev, "Error in strtoint conversion\n");
		return rc;
	}

	chip = (struct lm3645_etirled *)i2c_get_clientdata(client);
	mutex_lock(&chip->m_lock);
	chip->val = val;
	dev_dbg(dev, "writing reg:%x with val:%x\n", chip->reg, chip->val);
	rc = regmap_write(chip->regmap, chip->reg, chip->val);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			chip->reg, chip->val);
	mutex_unlock(&chip->m_lock);

	return count;
}

static DEVICE_ATTR_RW(etirled_val);

static unsigned char lm3645_build_dmode(unsigned char d1_mode, unsigned char d2_mode,
			 unsigned char d3_mode, unsigned char d4_mode)
{
	return (unsigned char)(((d4_mode & 0x3) << LM3645_DMODE_D4_SHIFT) |
		((d3_mode & 0x3) << LM3645_DMODE_D3_SHIFT) |
		((d2_mode & 0x3) << LM3645_DMODE_D2_SHIFT) |
		((d1_mode & 0x3) << LM3645_DMODE_D1_SHIFT));
}

static int etirled_toggle_d1(struct device *dev, bool toggle)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lm3645_etirled *chip;
	int rc = 0;
	int val = 0;

	chip = (struct lm3645_etirled *)i2c_get_clientdata(client);

	/* Put all channels OFF in DMODE, then set D1 to TORCH (others OFF)*/
	dev_dbg(dev, "toggle:%d\n", toggle);
	rc = regmap_write(chip->regmap, LM3645_REG_D_MODE_REG,
					 lm3645_build_dmode(LM3645_DMODE_TORCH,
							LM3645_DMODE_OFF,
							LM3645_DMODE_OFF,
							LM3645_DMODE_OFF));
	if (rc)
		dev_err(dev, "failed to  write register:%x\n",
			LM3645_REG_D_MODE_REG);
	/* Optional: make torch ramp immediate for "toggle" perception
	 * CUR_RAMP_REG reset is 0x39; we only force Torch_Ramp bits[2:0] to 0 (disabled).
	 * (IR/Flash ramps remain at reset values.)
	 */

	regmap_read(chip->regmap, LM3645_REG_CUR_RAMP_REG, &val);
	regmap_write(chip->regmap, LM3645_REG_CUR_RAMP_REG, (val & 0xF8) |
					 LM3645_TORCH_RAMP_DISABLED);

	/* Program a conservative torch current for D1.
	 * Example code 0x20 (~45.6mA): 1.41mA*32 + 0.525mA
	 */
	regmap_write(chip->regmap, LM3645_REG_D1_TORCH_REG, 0x20);

	/* Enable or disable D1 via CTRL_REG2.D1_EN*/
	regmap_read(chip->regmap, LM3645_REG_CTRL_REG2, &val);

	if (toggle)
		val |= LM3645_CTRL2_D1_EN;
	else
		val &= (~LM3645_CTRL2_D1_EN);
	regmap_write(chip->regmap, LM3645_REG_CTRL_REG2, val);
	return rc;
}


static ssize_t etirled_toggle_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lm3645_etirled *chip;
	int rc;

	chip = (struct lm3645_etirled *)i2c_get_clientdata(client);
	if (chip->d1_toggle)
		rc = snprintf(buf, sizeof(D1_ENABLED), "%s\n", D1_ENABLED);
	else
		rc = snprintf(buf, sizeof(D1_DISABLED), "%s\n", D1_DISABLED);

	return rc;
}

static ssize_t etirled_toggle_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lm3645_etirled *chip;

	chip = (struct lm3645_etirled *)i2c_get_clientdata(client);
	mutex_lock(&chip->m_lock);
	chip = (struct lm3645_etirled *)i2c_get_clientdata(client);
	chip->d1_toggle = !chip->d1_toggle;
	etirled_toggle_d1(dev, chip->d1_toggle);
	mutex_unlock(&chip->m_lock);

	return count;
}

static DEVICE_ATTR_RW(etirled_toggle);

static struct attribute *lm3645_etirled_attrs[] = {
	&dev_attr_etirled_reg.attr,
	&dev_attr_etirled_val.attr,
	&dev_attr_etirled_toggle.attr,
	NULL
};

static const struct attribute_group lm3645_etirled_attr_group = {
	.attrs = lm3645_etirled_attrs,
};

int lm3645_etirled_led_probe(struct i2c_client *client)
{
	struct lm3645_etirled *chip;
	int ret;
	unsigned int chipid;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	ret = devm_mutex_init(&client->dev, &chip->m_lock);
	if (ret)
		goto error;

	mutex_lock(&chip->m_lock);
	chip->client = client;
	i2c_set_clientdata(client, chip);

	chip->regmap = devm_regmap_init_i2c(client, &lm3645_etirled_regmap_config);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(&client->dev, "Failed to allocate reg map: %d\n", ret);
		goto error2;
	}

	chip->d1_toggle = false;
	chip->d2_toggle = false;
	chip->d3_toggle = false;
	chip->d4_toggle = false;
	ret = sysfs_create_group(&client->dev.kobj, &lm3645_etirled_attr_group);
	if (ret) {
		dev_err(&client->dev, "failed to create sysfs group, err:%d\n", ret);
		goto error2;
	}

	ret = regmap_read(chip->regmap, LM3645_REG_DEV_INFO_REG, &chipid);
	if (ret) {
		dev_err(&client->dev, "Failed to read chip ID: %d\n", ret);
		goto error3;
	}
	dev_dbg(&client->dev, "expected: %x ChipId read is :%x\n",
			LM3645_DEV_INFO_REG_VAL, chipid);


	regmap_write(chip->regmap, LM3645_REG_D1_TORCH_REG, 0x20);
	mutex_unlock(&chip->m_lock);

	return 0;
error3:
	sysfs_remove_group(&client->dev.kobj, &lm3645_etirled_attr_group);

error2:
	mutex_unlock(&chip->m_lock);
error:
	return ret;
}

static void lm3645_etirled_led_remove(struct i2c_client *client)
{
	if (client) {
		struct lm3645_etirled *chip = (struct lm3645_etirled *)i2c_get_clientdata(client);

		mutex_lock(&chip->m_lock);
		sysfs_remove_group(&client->dev.kobj, &lm3645_etirled_attr_group);
		mutex_unlock(&chip->m_lock);
	}
}

static const struct of_device_id lm3645_etirled_led_match_table[] = {
	{ .compatible = "ti,et-irled", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, lm3645_etirled_led_match_table);

static struct i2c_driver lm3645_etirled_led_driver = {
	.driver = {
		.name = "et-irled",
		.of_match_table = lm3645_etirled_led_match_table,
	},
	.probe = lm3645_etirled_led_probe,
	.remove = lm3645_etirled_led_remove,
};

module_i2c_driver(lm3645_etirled_led_driver);

MODULE_DESCRIPTION("TI LM3645 IR LED driver");
MODULE_LICENSE("GPL");
