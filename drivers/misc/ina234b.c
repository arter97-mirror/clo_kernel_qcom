// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define INA234_CONFIG_REG 0x00
#define INA234_SHUNT_VOLT_REG 0x01
#define INA234_BUS_VOLT_REG 0x02
#define INA234_POWER_REG 0x03
#define INA234_CURRENT_REG 0x04
#define INA234_CALIB_REG 0x05
#define INA234_MASK_EN_REG 0x06
#define INA234_ALERT_LIM_REG 0x07
#define INA234_MANUFACTURE_ID_REG 0x3E
#define INA234_DEVICE_ID_REG 0x3F
#define INA234_CONFIG_VAL 0x4127
#define INA234_RESET_CMD_VAL 0x00
#define INA234_MANUFACTURE_ID_VAL 0x5449
#define INA234_DEVICE_ID_VAL 0xA480
#define INA234_REG_MAX 0x3F

#define LEFT "Left"
#define RIGHT "Right"

struct ina234_user {
	struct mutex m_lock;
	struct i2c_client *client;
	struct regmap *regmap;
	int reg;
	int val;
	int readreg;
	bool left;
};

static const struct regmap_config ina234_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = INA234_REG_MAX,
};

static ssize_t ina234_regwrite_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ina234_user *chip;
	int rc;

	chip = (struct ina234_user *)i2c_get_clientdata(client);
	if (chip->reg == -EINVAL) {
		dev_err(dev, "%s INA284 set incorrect Reg value, pl set right value\n",
			(chip->left ? LEFT : RIGHT));
		rc = scnprintf(buf, 20, "Invalid Reg value\n");
		return rc;
	}
	rc = scnprintf(buf, 20, "reg:%x val:%x\n", chip->reg, chip->val);
	dev_dbg(dev, "%s INA284 reg:%x, val:%x\n",
			(chip->left ? LEFT : RIGHT), chip->reg, chip->val);

	return rc;
}

static ssize_t ina234_regwrite_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ina234_user *chip;
	int rc;
	unsigned int reg;
	unsigned int val;

	chip = (struct ina234_user *)i2c_get_clientdata(client);
	if (sscanf(buf, "%x %x", &reg, &val) != 2) {
		dev_err(dev, "Invalid format\n");
		return -EINVAL;
	}

	mutex_lock(&chip->m_lock);
	/* Validate register address */
	if (reg < INA234_CONFIG_REG || reg > INA234_REG_MAX) {
		dev_err(dev, "%s INA284 Register address 0x%x out of range [0x%x-0x%x]\n",
			(chip->left ? LEFT : RIGHT), reg, INA234_CONFIG_REG, INA234_REG_MAX);
		chip->reg = -EINVAL;
		mutex_unlock(&chip->m_lock);
		return -EINVAL;
	}

	chip->reg = reg;
	chip->val = val;
	dev_dbg(dev, "%s INA284 writing reg:%x, val:%x\n",
		(chip->left ? LEFT : RIGHT), chip->reg, chip->val);
	rc = regmap_write(chip->regmap, chip->reg, chip->val);
	if (rc)
		dev_err(dev, "%s INA284 failed to  write register:%x with value:%x\n",
			(chip->left ? LEFT : RIGHT), chip->reg, chip->val);

	mutex_unlock(&chip->m_lock);

	return count;
}
static DEVICE_ATTR_RW(ina234_regwrite);

static ssize_t ina234_regread_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ina234_user *chip;
	int rc;
	int val;

	chip = (struct ina234_user *)i2c_get_clientdata(client);
	mutex_lock(&chip->m_lock);
	if (chip->readreg == -EINVAL) {
		dev_err(dev, "%s INA284 set incorrect Reg value, pl set right value\n",
			(chip->left ? LEFT : RIGHT));
		rc = scnprintf(buf, 20, "Invalid Reg value\n");
		goto error;
	}

	rc = regmap_read(chip->regmap, chip->readreg, &val);
	if (rc) {
		dev_err(dev, "%s INA284 failed to  read register:%x\n",
			(chip->left ? LEFT : RIGHT), chip->readreg);
			goto error;
	}

	dev_dbg(dev, "%s INA284 reading reg:%x val is :%x\n",
			(chip->left ? LEFT : RIGHT), chip->readreg, val);
	rc = scnprintf(buf, 20, "reg:%x val:%x\n", chip->readreg, val);
error:
	mutex_unlock(&chip->m_lock);

	return rc;
}

static ssize_t ina234_regread_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ina234_user *chip;
	int rc;
	int val;

	chip = (struct ina234_user *)i2c_get_clientdata(client);
	rc = kstrtoint(buf, 0, &val);
	if (rc) {
		dev_err(dev, "%s INA284 Error in strtoint conversion\n",
				(chip->left ? LEFT : RIGHT));
		chip->readreg = -EINVAL;
		return rc;
	}

	mutex_lock(&chip->m_lock);
	/* Validate register address */
	if (val < INA234_CONFIG_REG || val > INA234_REG_MAX) {
		dev_err(dev, "%s INA284 Register address 0x%x out of range [0x%x-0x%x]\n",
			(chip->left ? LEFT : RIGHT), val, INA234_CONFIG_REG, INA234_REG_MAX);
		chip->readreg = -EINVAL;
		mutex_unlock(&chip->m_lock);
		return -EINVAL;
	}

	chip->readreg = val;
	mutex_unlock(&chip->m_lock);

	return count;
}

static DEVICE_ATTR_RW(ina234_regread);
static ssize_t ina234_type_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ina234_user *chip;
	int rc;

	chip = (struct ina234_user *)i2c_get_clientdata(client);

	if (chip->left)
		rc = snprintf(buf, 6, "%s\n", LEFT);
	else
		rc = snprintf(buf, 7, "%s\n", RIGHT);
	return rc;
}

static ssize_t ina234_type_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_RW(ina234_type);

static struct attribute *ina234_attrs[] = {
	&dev_attr_ina234_regread.attr,
	&dev_attr_ina234_regwrite.attr,
	&dev_attr_ina234_type.attr,
	NULL
};

static const struct attribute_group ina234_attr_group = {
	.attrs = ina234_attrs,
};

int ina234_probe(struct i2c_client *client)
{
	struct ina234_user *chip;
	int ret;
	unsigned int chipid;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	devm_mutex_init(&client->dev, &chip->m_lock);
	mutex_lock(&chip->m_lock);
	chip->client = client;
	i2c_set_clientdata(client, chip);

	chip->left = of_property_read_bool(client->dev.of_node, "left");
	dev_dbg(&client->dev, "chip->left:%d\n", chip->left);
	chip->regmap = devm_regmap_init_i2c(client, &ina234_regmap_config);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(&client->dev, "%s Failed to allocate reg map: %d\n",
				(chip->left ? LEFT : RIGHT), ret);
		goto error;
	}

	ret = sysfs_create_group(&client->dev.kobj, &ina234_attr_group);
	if (ret) {
		dev_err(&client->dev, "%s failed to create sysfs group, err:%d\n",
				(chip->left ? LEFT : RIGHT), ret);
		goto error;
	}

	ret = regmap_read(chip->regmap, INA234_MANUFACTURE_ID_REG, &chipid);
	if (ret) {
		dev_err(&client->dev, "failed to read %s INA234_MANUFACTURE_ID_REG err:%d\n",
				(chip->left ? LEFT : RIGHT), ret);
		goto error2;
	}

	if (chipid != INA234_MANUFACTURE_ID_VAL) {
		dev_err(&client->dev, "%s INA234_MANUFACTURE_ID_VAL:%x doesn't match chipid:%x\n",
				(chip->left ? LEFT : RIGHT), INA234_MANUFACTURE_ID_VAL, chipid);
		ret = -EINVAL;
		goto error2;
	}

	ret = regmap_read(chip->regmap, INA234_DEVICE_ID_REG, &chipid);
	if (ret) {
		dev_err(&client->dev, "%s failed to read INA234_DEVICE_ID_REG err:%d\n",
					(chip->left ? LEFT : RIGHT), ret);
		goto error2;
	}

	if (chipid != INA234_DEVICE_ID_VAL) {
		dev_err(&client->dev, "%s INA234_DEVICE_ID_VAL:%x doesn't match chipid:%d\n",
				(chip->left ? LEFT : RIGHT), INA234_DEVICE_ID_VAL, chipid);
		ret = -EINVAL;
		goto error2;
	}

	ret = regmap_read(chip->regmap, INA234_CONFIG_REG, &chipid);
	if (ret) {
		dev_err(&client->dev, "%s failed to read INA234_CONFIG_REG err:%d\n",
					(chip->left ? LEFT : RIGHT), ret);
		goto error2;
	}

	if (chipid != INA234_CONFIG_VAL) {
		dev_err(&client->dev, "%s INA234_CONFIG_VAL:%x doesn't match chipid:%d\n",
				(chip->left ? LEFT : RIGHT), INA234_CONFIG_VAL, chipid);
		ret = -EINVAL;
		goto error2;
	}

	mutex_unlock(&chip->m_lock);

	return 0;
error2:
	sysfs_remove_group(&client->dev.kobj, &ina234_attr_group);

error:
	mutex_unlock(&chip->m_lock);
	return ret;
}

static void ina234_remove(struct i2c_client *client)
{
	if (client) {
		struct ina234_user *chip = (struct ina234_user *)i2c_get_clientdata(client);

		mutex_lock(&chip->m_lock);
		sysfs_remove_group(&client->dev.kobj, &ina234_attr_group);
		mutex_unlock(&chip->m_lock);
	}
}

static const struct of_device_id ina234_match_table[] = {
	{ .compatible = "ti,ina234b", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, ina234_match_table);

static struct i2c_driver ina234_driver = {
	.driver = {
		.name = "ina234b",
		.of_match_table = ina234_match_table,
	},
	.probe = ina234_probe,
	.remove = ina234_remove,
};

module_i2c_driver(ina234_driver);

MODULE_DESCRIPTION("TI INA234 Current Monitor driver");
MODULE_LICENSE("GPL");
