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

#define TPS6286_VOUT1_REG 0x01
#define TPS6286_VOUT2_REG 0x02
#define TPS6286_CONTROL_REG 0x03
#define TPS6286_STATUS_REG 0x05
#define TPS6286_VOUT1_VAL 0x10
#define TPS6286_VOUT2_VAL 0x38

struct tps6286_user {
	struct mutex m_lock;
	struct i2c_client *client;
	struct regmap *regmap;
	int reg;
	int val;
	int readreg;
	bool left;
};

static const struct regmap_config tps6286_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TPS6286_STATUS_REG,
};

static ssize_t tps6286_regwrite_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct tps6286_user *chip;
	int rc;

	chip = (struct tps6286_user *)i2c_get_clientdata(client);
	if (chip->reg == -EINVAL) {
		rc = scnprintf(buf, 20, "Invalid Reg\n");
		return rc;
	}

	rc = scnprintf(buf, 20, "reg:%x val:%x\n", chip->reg, chip->val);
	dev_dbg(dev, "TPS6286 reg:%x, val:%x\n",
		chip->reg, chip->val);

	return rc;
}

static ssize_t tps6286_regwrite_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct tps6286_user *chip;
	int rc;
	int reg;
	int val;

	chip = (struct tps6286_user *)i2c_get_clientdata(client);
	if (sscanf(buf, "%x %x", &reg, &val) != 2) {
		dev_err(dev, "Invalid format\n");
		chip->reg = -EINVAL;
		return -EINVAL;
	}

	/* Validate register address */
	if (reg < TPS6286_VOUT1_REG || reg > TPS6286_STATUS_REG) {
		dev_err(dev, "TPS6286 Register address 0x%x out of range [0x%x-0x%x]\n",
			reg, TPS6286_VOUT1_REG, TPS6286_STATUS_REG);
		chip->reg = -EINVAL;
		return -EINVAL;
	}

	mutex_lock(&chip->m_lock);
	chip->reg = reg;
	chip->val = val;
	rc = regmap_write(chip->regmap, chip->reg, chip->val);
	if (rc)
		dev_err(dev, "TPS6286 failed to  write register:%x with value:%x\n",
			chip->reg, chip->val);

	mutex_unlock(&chip->m_lock);
	dev_dbg(dev, "TPS6286 writing register:%x with value:%x\n",
		chip->reg, chip->val);

	return count;
}
static DEVICE_ATTR_RW(tps6286_regwrite);

static ssize_t tps6286_regread_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct tps6286_user *chip;
	int rc;
	int val;

	chip = (struct tps6286_user *)i2c_get_clientdata(client);
	if (chip->readreg == -EINVAL) {
		rc = scnprintf(buf, 20, "Invalid Reg\n");
		return rc;
	}

	rc = regmap_read(chip->regmap, chip->readreg, &val);
	rc = scnprintf(buf, 20, "reg:%x val:%x\n", chip->readreg, val);
	dev_dbg(dev, "TPS6286 reg:%x val:%x\n",
		chip->readreg, chip->val);
	return rc;
}

static ssize_t tps6286_regread_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct tps6286_user *chip;
	int rc;
	int val;

	chip = (struct tps6286_user *)i2c_get_clientdata(client);
	rc = kstrtoint(buf, 0, &val);
	if (rc) {
		dev_err(dev, " TPS6286 Error in strtoint conversion\n");
		return rc;
	}

	/* Validate register address */
	if (val < TPS6286_VOUT1_REG || val > TPS6286_STATUS_REG) {
		dev_err(dev, "TPS6286 Register address 0x%x out of range [0x%x-0x%x]\n",
			val, TPS6286_VOUT1_REG, TPS6286_STATUS_REG);
		chip->readreg = -EINVAL;
		return -EINVAL;
	}

	mutex_lock(&chip->m_lock);
	chip->readreg = val;
	dev_dbg(dev, "TPS6286 reading reg:%x\n", chip->reg);
	mutex_unlock(&chip->m_lock);

	return count;
}

static DEVICE_ATTR_RW(tps6286_regread);

static struct attribute *tps6286_attrs[] = {
	&dev_attr_tps6286_regwrite.attr,
	&dev_attr_tps6286_regread.attr,
	NULL
};

static const struct attribute_group tps6286_attr_group = {
	.attrs = tps6286_attrs,
};

int tps6286_probe(struct i2c_client *client)
{
	struct tps6286_user *chip;
	int ret;
	unsigned int chipid;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	devm_mutex_init(&client->dev, &chip->m_lock);
	mutex_lock(&chip->m_lock);
	chip->client = client;
	i2c_set_clientdata(client, chip);

	chip->regmap = devm_regmap_init_i2c(client, &tps6286_regmap_config);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(&client->dev, "Failed to allocate reg map: %d\n",
					ret);
		goto error;
	}

	ret = sysfs_create_group(&client->dev.kobj, &tps6286_attr_group);
	if (ret) {
		dev_err(&client->dev, "failed to create sysfs group, err:%d\n",
				ret);
		goto error;
	}

	ret = regmap_read(chip->regmap, TPS6286_VOUT1_REG, &chipid);
	if (ret) {
		dev_err(&client->dev, "failed to read TPS6286_VOUT1_REG err:%d\n",
			ret);
		goto error2;
	}

	dev_dbg(&client->dev, "TPS6286_VOUT1_REG:%x, TPS6286_VOUT1_VAL:%x val:%x\n",
		TPS6286_VOUT1_REG, TPS6286_VOUT1_VAL, chipid);
	if (chipid != TPS6286_VOUT1_VAL) {
		dev_err(&client->dev, "TPS6286_VOUT1_VAL:%x doesn't match chipid:%x\n",
			TPS6286_VOUT1_VAL, chipid);
		goto error2;
	}

	ret = regmap_read(chip->regmap, TPS6286_VOUT2_REG, &chipid);
	if (ret) {
		dev_err(&client->dev, "failed to read TPS6286_VOUT2_REG err:%d\n", ret);
		goto error2;
	}

	if (chipid != TPS6286_VOUT2_VAL) {
		dev_err(&client->dev, "TPS6286_VOUT2_VAL:%x doesn't match chipid:%x\n",
			TPS6286_VOUT2_VAL, chipid);
		goto error2;
	}

	mutex_unlock(&chip->m_lock);

	return 0;
error2:
	sysfs_remove_group(&client->dev.kobj, &tps6286_attr_group);

error:
	mutex_unlock(&chip->m_lock);
	return ret;
}

static void tps6286_remove(struct i2c_client *client)
{
	if (client) {
		struct tps6286_user *chip = (struct tps6286_user *)i2c_get_clientdata(client);

		mutex_lock(&chip->m_lock);
		sysfs_remove_group(&client->dev.kobj, &tps6286_attr_group);
		mutex_unlock(&chip->m_lock);
	}
}

static const struct of_device_id tps6286_match_table[] = {
	{ .compatible = "ti,tps6286", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, tps6286_match_table);

static struct i2c_driver tps6286_driver = {
	.driver = {
		.name = "tps6286",
		.of_match_table = tps6286_match_table,
	},
	.probe = tps6286_probe,
	.remove = tps6286_remove,
};

module_i2c_driver(tps6286_driver);

MODULE_DESCRIPTION("TI TPS6286 Step-Down converter driver");
MODULE_LICENSE("GPL");
