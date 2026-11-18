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

#define LEFT "Left"
#define RIGHT "Right"

#define SLG4DC_REG1 0x22
#define SLG4DC_REG2 0x53
#define SLG4DC_REG3 0x55
#define SLG4DC_REG4 0x57

struct slg4dc_user {
	struct mutex m_lock;
	struct i2c_client *client;
	struct regmap *regmap;
	int reg;
	int val;
	int readreg;
	bool left;
};

static const struct regmap_config slg4dc_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = SLG4DC_REG4,
};

static ssize_t slg4dc_regwrite_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct slg4dc_user *chip;
	int rc;

	chip = (struct slg4dc_user *)i2c_get_clientdata(client);

	if (chip->reg == -EINVAL) {
		rc = scnprintf(buf, 20, "Invalid Reg\n");
		return rc;
	}

	rc = scnprintf(buf, 20, "reg:%x val:%x\n", chip->reg, chip->val);
	dev_dbg(dev, "%s SLG4DC reg:%x, val:%x\n",
		(chip->left ? LEFT : RIGHT), chip->reg, chip->val);

	return rc;
}

static ssize_t slg4dc_regwrite_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct slg4dc_user *chip;
	int rc;
	int reg;
	int val;

	chip = (struct slg4dc_user *)i2c_get_clientdata(client);
	if (sscanf(buf, "%x %x", &reg, &val) != 2) {
		dev_err(dev, "Invalid format\n");
		chip->reg = -EINVAL;
		return -EINVAL;
	}

	/* Validate register address */
	if ((reg == SLG4DC_REG1) || (reg == SLG4DC_REG2) ||
		(reg == SLG4DC_REG3) || (reg == SLG4DC_REG4)) {
		mutex_lock(&chip->m_lock);
		chip->reg = reg;
		chip->val = val;
		rc = regmap_write(chip->regmap, chip->reg, chip->val);
		if (rc)
			dev_err(dev, "%s SLG4DC failed to  write register:%x with value:%x\n",
				(chip->left ? LEFT : RIGHT), chip->reg, chip->val);
		mutex_unlock(&chip->m_lock);
		dev_dbg(dev, "%s SLG4DC writing reg:%x with val:%x\n",
			(chip->left ? LEFT : RIGHT), chip->reg, chip->val);
	} else {
		chip->reg = -EINVAL;
		dev_err(dev, "%s SLG4DC Register address 0x%x out of range [0x%x-0x%x]\n",
			(chip->left ? LEFT : RIGHT), reg, SLG4DC_REG1, SLG4DC_REG4);
		return -EINVAL;
	}

	return count;
}
static DEVICE_ATTR_RW(slg4dc_regwrite);

static ssize_t slg4dc_regread_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct slg4dc_user *chip;
	int rc;
	int val;

	chip = (struct slg4dc_user *)i2c_get_clientdata(client);
	if (chip->readreg == -EINVAL) {
		rc = scnprintf(buf, 20, "Invalid Reg\n");
		return rc;
	}

	rc = regmap_read(chip->regmap, chip->readreg, &val);
	if (rc)
		dev_err(dev, "%s SLG4DC failed to read register:%x\n",
			(chip->left ? LEFT : RIGHT), chip->reg);
	else {
		scnprintf(buf, 20, "reg:%x val:%x\n", chip->readreg, val);
		dev_dbg(dev, "%s SLG4DC readreg:%x val:%x\n",
			(chip->left ? LEFT : RIGHT), chip->readreg, val);
	}

	return rc;
}

static ssize_t slg4dc_regread_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct slg4dc_user *chip;
	int rc;
	int val;

	chip = (struct slg4dc_user *)i2c_get_clientdata(client);
	rc = kstrtoint(buf, 0, &val);
	if (rc) {
		dev_err(dev, "%s SLG4DC Error in strtoint conversion\n",
					(chip->left ? LEFT : RIGHT));
		chip->readreg = -EINVAL;
		return rc;
	}

	/* Validate register address */
	if ((val == SLG4DC_REG1) || (val == SLG4DC_REG2) ||
			(val == SLG4DC_REG3) || (val == SLG4DC_REG4)) {
		mutex_lock(&chip->m_lock);
		chip->readreg = val;
		dev_dbg(dev, "%s SLG4DC reading reg:%x\n",
			(chip->left ? LEFT : RIGHT), chip->readreg);
		mutex_unlock(&chip->m_lock);
	} else {
		chip->readreg = -EINVAL;
		dev_err(dev, "%s SLG4DC Register address 0x%x out of range [0x%x-0x%x]\n",
			(chip->left ? LEFT : RIGHT), val, SLG4DC_REG1, SLG4DC_REG4);
		return -EINVAL;
	}

	return count;
}

static DEVICE_ATTR_RW(slg4dc_regread);
static ssize_t slg4dc_type_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct slg4dc_user *chip;
	int rc;

	chip = (struct slg4dc_user *)i2c_get_clientdata(client);

	if (chip->left)
		rc = snprintf(buf, 5, "%s\n", LEFT);
	else
		rc = snprintf(buf, 6, "%s\n", RIGHT);

	return rc;
}

static ssize_t slg4dc_type_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_RW(slg4dc_type);

static struct attribute *slg4dc_attrs[] = {
	&dev_attr_slg4dc_regread.attr,
	&dev_attr_slg4dc_regwrite.attr,
	&dev_attr_slg4dc_type.attr,
	NULL
};

static const struct attribute_group slg4dc_attr_group = {
	.attrs = slg4dc_attrs,
};

int slg4dc_probe(struct i2c_client *client)
{
	struct slg4dc_user *chip;
	int ret;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	devm_mutex_init(&client->dev, &chip->m_lock);
	mutex_lock(&chip->m_lock);
	chip->client = client;
	i2c_set_clientdata(client, chip);

	chip->left = of_property_read_bool(client->dev.of_node, "left");
	dev_dbg(&client->dev, "chip->left:%d\n", chip->left);
	chip->regmap = devm_regmap_init_i2c(client, &slg4dc_regmap_config);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(&client->dev, "%s Failed to allocate reg map: %d\n",
				(chip->left ? LEFT : RIGHT), ret);
		goto error;
	}

	ret = sysfs_create_group(&client->dev.kobj, &slg4dc_attr_group);
	if (ret) {
		dev_err(&client->dev, "%s failed to create sysfs group, err:%d\n",
				(chip->left ? LEFT : RIGHT), ret);
		goto error;
	}

	mutex_unlock(&chip->m_lock);

	return 0;

error:
	mutex_unlock(&chip->m_lock);
	return ret;
}

static void slg4dc_remove(struct i2c_client *client)
{
	if (client) {
		struct slg4dc_user *chip = (struct slg4dc_user *)i2c_get_clientdata(client);

		mutex_lock(&chip->m_lock);
		sysfs_remove_group(&client->dev.kobj, &slg4dc_attr_group);
		mutex_unlock(&chip->m_lock);
	}
}

static const struct of_device_id slg4dc_match_table[] = {
	{ .compatible = "renesas,slg4dc", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, slg4dc_match_table);

static struct i2c_driver slg4dc_driver = {
	.driver = {
		.name = "slg4dc",
		.of_match_table = slg4dc_match_table,
	},
	.probe = slg4dc_probe,
	.remove = slg4dc_remove,
};

module_i2c_driver(slg4dc_driver);

MODULE_DESCRIPTION("Renesas SLG4DC driver");
MODULE_LICENSE("GPL");
