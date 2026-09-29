// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define AW2013_MAX_LEDS 4

#define AW21104_REG_MIN 0x00
#define AW21104_REG_CHIP_ID 0x00
#define AW21104_REG_ENANLE 0x01
#define AW21104_CHIP_ID 0x40

#define AW21104_REG_MAX 0x7f


struct aw21104_bys;

struct aw21104_bys_led {
	struct aw21104_bys *chip;
	struct led_classdev cdev;
	int num;
	unsigned int imax;
};

struct aw21104_bys {
	struct mutex m_lock;
	struct regulator_bulk_data regulators[2];
	struct aw21104_bys_led leds[AW2013_MAX_LEDS];
	struct i2c_client *client;
	struct regmap *regmap;
	int num_leds;
	bool enabled;
	int reg;
	int val;
};

static const struct regmap_config aw21104_bys_led_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW21104_REG_MAX,
};

static ssize_t bys_led_reg_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct aw21104_bys *chip;
	int rc;

	chip = (struct aw21104_bys *)i2c_get_clientdata(client);

	rc = scnprintf(buf, 10, "%x\n", chip->reg);
	dev_dbg(dev, "reg:%x\n", chip->reg);
	return rc;
}
static ssize_t bys_led_reg_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct aw21104_bys *chip;
	int rc;
	int reg;

	rc = kstrtoint(buf, 0, &reg);
	if (rc) {
		dev_err(dev, "Error in strtoint conversion.\n");
		return rc;
	}
	/* Validate register address*/
	if (reg < AW21104_REG_MIN || reg > AW21104_REG_MAX) {
		dev_err(dev, "Register address 0x%x out of range [0x%x-0x%x]\n",
				reg, AW21104_REG_MIN, AW21104_REG_MAX);
		return -EINVAL;
	}

	chip = (struct aw21104_bys *)i2c_get_clientdata(client);
	mutex_lock(&chip->m_lock);
	chip->reg = reg;
	mutex_unlock(&chip->m_lock);
	dev_dbg(dev, "reg:%x\n", chip->reg);

	return count;
}
static DEVICE_ATTR_RW(bys_led_reg);

static ssize_t bys_led_val_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct aw21104_bys *chip;
	int rc;

	chip = (struct aw21104_bys *)i2c_get_clientdata(client);

	rc = scnprintf(buf, 10, "%x\n", chip->val);
	dev_dbg(dev, "val:%x\n", chip->val);
	return rc;
}

static ssize_t bys_led_val_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct aw21104_bys *chip;
	int rc;
	int val;

	rc = kstrtoint(buf, 0, &val);
	if (rc) {
		dev_err(dev, "Error in strtoint conversion\n");
		return rc;
	}

	chip = (struct aw21104_bys *)i2c_get_clientdata(client);
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
static DEVICE_ATTR_RW(bys_led_val);

static struct attribute *aw21104_bys_led_attrs[] = {
	&dev_attr_bys_led_reg.attr,
	&dev_attr_bys_led_val.attr,
	NULL
};

static const struct attribute_group aw21104_bys_led_attr_group = {
	.attrs = aw21104_bys_led_attrs,
};

int aw21104_bys_led_probe(struct i2c_client *client)
{
	struct aw21104_bys *chip;
	int ret;
	unsigned int chipid;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	ret = devm_mutex_init(&client->dev, &chip->m_lock);
	if (ret)
		return ret;

	mutex_lock(&chip->m_lock);
	chip->client = client;
	i2c_set_clientdata(client, chip);

	chip->regmap = devm_regmap_init_i2c(client, &aw21104_bys_led_regmap_config);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(&client->dev, "Failed to allocate reg map: %d\n", ret);
		goto error;
	}

	ret = sysfs_create_group(&client->dev.kobj, &aw21104_bys_led_attr_group);
	if (ret) {
		dev_err(&client->dev, "failed to create sysfs group, err:%d\n", ret);
		goto error;
	}

	ret = regmap_read(chip->regmap, AW21104_REG_CHIP_ID, &chipid);
	if (ret) {
		dev_err(&client->dev, "Failed to read chip ID: %d\n", ret);
		ret = -EPROBE_DEFER;
		goto error2;
	}
	dev_dbg(&client->dev, "expected: 0x40 ChipId read is :%x\n", chipid);
	mutex_unlock(&chip->m_lock);
	return 0;
error2:
	sysfs_remove_group(&client->dev.kobj, &aw21104_bys_led_attr_group);

error:
	mutex_unlock(&chip->m_lock);
	return ret;
}

static void aw21104_bys_led_remove(struct i2c_client *client)
{
	if (client)
		sysfs_remove_group(&client->dev.kobj, &aw21104_bys_led_attr_group);
}

static const struct of_device_id aw21104_bys_led_match_table[] = {
	{ .compatible = "awinic,rgbw-bys-led", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, aw21104_bys_led_match_table);

static struct i2c_driver aw21104_bys_led_driver = {
	.driver = {
		.name = "rgbw-bys-led",
		.of_match_table = aw21104_bys_led_match_table,
	},
	.probe = aw21104_bys_led_probe,
	.remove = aw21104_bys_led_remove,
};

module_i2c_driver(aw21104_bys_led_driver);

MODULE_DESCRIPTION("AWINIC AW21104 RGBW BYS LED driver");
MODULE_LICENSE("GPL");
