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

/* Register map (Table 7-7) */
#define LP5814_REG_CHIP_EN         0x00
#define LP5814_REG_DEV_CONFIG0     0x01
#define LP5814_REG_DEV_CONFIG1     0x02
#define LP5814_REG_DEV_CONFIG2     0x03
#define LP5814_REG_DEV_CONFIG3     0x04
#define LP5814_REG_DEV_CONFIG4     0x05

#define LP5814_REG_ENGINE_CONFIG0  0x06
#define LP5814_REG_ENGINE_CONFIG1  0x07
#define LP5814_REG_ENGINE_CONFIG2  0x08
#define LP5814_REG_ENGINE_CONFIG3  0x09
#define LP5814_REG_ENGINE_CONFIG4  0x0A
#define LP5814_REG_ENGINE_CONFIG5  0x0B
#define LP5814_REG_ENGINE_CONFIG6  0x0C

#define LP5814_REG_SHUTDOWN_CMD    0x0D
#define LP5814_REG_RESET_CMD       0x0E
#define LP5814_REG_UPDATE_CMD      0x0F
#define LP5814_REG_START_CMD       0x10
#define LP5814_REG_STOP_CMD        0x11
#define LP5814_REG_PAUSE_CONTINUE  0x12
#define LP5814_REG_FLAG_CLR        0x13

#define LP5814_REG_OUT0_DC         0x14
#define LP5814_REG_OUT1_DC         0x15
#define LP5814_REG_OUT2_DC         0x16
#define LP5814_REG_OUT3_DC         0x17

#define LP5814_REG_OUT0_PWM        0x18
#define LP5814_REG_OUT1_PWM        0x19
#define LP5814_REG_OUT2_PWM        0x1A
#define LP5814_REG_OUT3_PWM        0x1B

#define LP5814_REG_FLAG            0x40

/* Command values (Tables 7-22 .. 7-26) */
#define LP5814_SHUTDOWN_CMD_VAL    0x33
#define LP5814_RESET_CMD_VAL       0xCC
#define LP5814_UPDATE_CMD_VAL      0x55
#define LP5814_START_CMD_VAL       0xFF
#define LP5814_STOP_CMD_VAL        0xAA

/* DEV_CONFIG0 bits */
#define LP5814_MAX_CURRENT_BIT     (1 << 0)  /* 0=25.5mA, 1=51mA */

/* DEV_CONFIG1 bits */
#define LP5814_OUT0_EN_BIT         (1 << 0)
#define LP5814_OUT1_EN_BIT         (1 << 1)
#define LP5814_OUT2_EN_BIT         (1 << 2)
#define LP5814_OUT3_EN_BIT         (1 << 3)

/* DEV_CONFIG3 bits */
#define LP5814_OUT0_AUTO_EN_BIT    (1 << 0)
#define LP5814_OUT1_AUTO_EN_BIT    (1 << 1)
#define LP5814_OUT2_AUTO_EN_BIT    (1 << 2)
#define LP5814_OUT3_AUTO_EN_BIT    (1 << 3)
#define LP5814_OUT0_EXP_EN_BIT     (1 << 4)
#define LP5814_OUT1_EXP_EN_BIT     (1 << 5)
#define LP5814_OUT2_EXP_EN_BIT     (1 << 6)
#define LP5814_OUT3_EXP_EN_BIT     (1 << 7)

/* FLAG bits */
#define LP5814_FLAG_POR_BIT        (1 << 0)
#define LP5814_FLAG_TSD_BIT        (1 << 1)
#define LP5814_FLAG_ENGINE_BUSY    (1 << 2)

/* FLAG_CLR bits */
#define LP5814_FLAGCLR_POR_CLR_BIT (1 << 0)  /* W1C */
#define LP5814_FLAGCLR_TSD_CLR_BIT (1 << 1)  /* W1C */

#define LP5814_REG_MIN 0x0
#define LP5814_REG_MAX 0x41

/* Output channel indices */
enum lp5814_out {
	LP5814_OUT0,
	LP5814_OUT1,
	LP5814_OUT2,
	LP5814_OUT3,
};

enum wrgb_color {
	RED,
	GREEN,
	BLUE,
	WHITE,
	OFF,
	UNKNOWN,
};
struct lp5814_user;

struct lp5814_user_led {
	struct lp5814_user *chip;
	struct led_classdev cdev;
	int num;
	unsigned int imax;
};

struct lp5814_user {
	struct mutex m_lock;
	struct lp5814_user_led leds[4];
	struct i2c_client *client;
	struct regmap *regmap;
	int num_leds;
	bool enabled;
	int reg;
	int val;
	enum wrgb_color color;
};

static const struct regmap_config lp5814_user_led_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = LP5814_REG_MAX,
};

static ssize_t user_led_reg_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lp5814_user *chip;
	int rc;

	chip = (struct lp5814_user *)i2c_get_clientdata(client);

	rc = scnprintf(buf, 10, "%x\n", chip->reg);
	dev_dbg(dev, "reg:%x\n", chip->reg);
	return rc;
}

static ssize_t user_led_reg_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lp5814_user *chip;
	int rc;
	int reg;

	rc = kstrtoint(buf, 0, &reg);
	if (rc) {
		dev_err(dev, "Error in strtoint conversion\n");
		return rc;
	}

	/* Validate register address */
	if (reg < LP5814_REG_MIN || reg > LP5814_REG_MAX) {
		dev_err(dev, "Register address 0x%x out of range [0x%x-0x%x]\n",
			reg, LP5814_REG_MIN, LP5814_REG_MAX);
		return -EINVAL;
	}

	chip = (struct lp5814_user *)i2c_get_clientdata(client);
	mutex_lock(&chip->m_lock);
	chip->reg = reg;
	mutex_unlock(&chip->m_lock);
	dev_dbg(dev, "reg:%x\n", chip->reg);

	return count;
}
static DEVICE_ATTR_RW(user_led_reg);

static ssize_t user_led_val_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lp5814_user *chip;
	int rc;

	chip = (struct lp5814_user *)i2c_get_clientdata(client);

	rc = scnprintf(buf, 10, "%x\n", chip->val);
	dev_dbg(dev, "val:%x\n", chip->val);
	return rc;
}

static ssize_t user_led_val_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lp5814_user *chip;
	int rc;
	int val;

	rc = kstrtoint(buf, 0, &val);
	if (rc) {
		dev_err(dev, "Error in strtoint conversion\n");
		return rc;
	}

	chip = (struct lp5814_user *)i2c_get_clientdata(client);
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

static DEVICE_ATTR_RW(user_led_val);

static int lp5814_user_led_turn_all_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lp5814_user *chip;
	int rc;
	int val = 0x0;

	chip = (struct lp5814_user *)i2c_get_clientdata(client);
	/* Disable all outputs */
	rc = regmap_write(chip->regmap, LP5814_REG_DEV_CONFIG1, val);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_DEV_CONFIG1, val);

	/* Manual mode (AUTO_EN=0), linear dimming (EXP_EN=0) */
	rc = regmap_write(chip->regmap, LP5814_REG_DEV_CONFIG3, val);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_DEV_CONFIG3, val);

	/* Clear DC and PWM */
	rc = regmap_write(chip->regmap, LP5814_REG_OUT0_DC, val);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_OUT0_DC, val);

	rc = regmap_write(chip->regmap, LP5814_REG_OUT1_DC, val);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_OUT1_DC, val);

	rc = regmap_write(chip->regmap, LP5814_REG_OUT2_DC, val);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_OUT2_DC, val);

	rc = regmap_write(chip->regmap, LP5814_REG_OUT3_DC, val);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_OUT3_DC, val);

	rc = regmap_write(chip->regmap, LP5814_REG_OUT0_PWM, val);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_OUT0_PWM, val);

	rc = regmap_write(chip->regmap, LP5814_REG_OUT1_PWM, val);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_OUT1_PWM, val);

	rc = regmap_write(chip->regmap, LP5814_REG_OUT2_PWM, val);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_OUT2_PWM, val);

	rc = regmap_write(chip->regmap, LP5814_REG_OUT3_PWM, val);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_OUT3_PWM, val);

	return rc;
}

static int lp5814_user_led_enable_wrgb_color(struct device *dev, enum wrgb_color color)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lp5814_user *chip;
	int rc = 0;
	int val1 = 0x0;
	int val2 = 0x0;
	int reg1 = 0x0;
	int reg2 = 0x0;

	lp5814_user_led_turn_all_off(dev);
	chip = (struct lp5814_user *)i2c_get_clientdata(client);
	rc = regmap_write(chip->regmap, LP5814_REG_DEV_CONFIG0, val1);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_DEV_CONFIG0, val1);
	rc = regmap_write(chip->regmap, LP5814_REG_DEV_CONFIG1, 0x0F);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:0xF\n",
			LP5814_REG_DEV_CONFIG1);
	rc = regmap_write(chip->regmap, LP5814_REG_DEV_CONFIG3, val1);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_DEV_CONFIG3, val1);
	rc = regmap_write(chip->regmap, LP5814_REG_UPDATE_CMD, LP5814_UPDATE_CMD_VAL);
	if (rc)
		dev_err(dev, "failed to  write register:%x with value:%x\n",
			LP5814_REG_UPDATE_CMD, LP5814_UPDATE_CMD_VAL);

	val1 = 0x64;
	val2 = 0x80;
	switch (color) {
	case RED:
		reg1 = LP5814_REG_OUT0_DC;
		reg2 = LP5814_REG_OUT0_PWM;
		break;
	case GREEN:
		reg1 = LP5814_REG_OUT1_DC;
		reg2 = LP5814_REG_OUT1_PWM;
		break;
	case BLUE:
		reg1 = LP5814_REG_OUT2_DC;
		reg2 = LP5814_REG_OUT2_PWM;
		break;
	case WHITE:
		reg1 = LP5814_REG_OUT3_DC;
		reg2 = LP5814_REG_OUT3_PWM;
		break;
	default:
		reg1 = 0xFF;
		reg2 = 0xFF;
		dev_err(dev, "Undefined color value is passed\n");
		break;
	}
	if (reg1 != 0xFF && reg2 != 0xFF) {
		rc = regmap_write(chip->regmap, reg1, val1);
		if (rc)
			dev_err(dev, "failed to  write register:%x with value:%x\n",
				reg1, val1);
		rc = regmap_write(chip->regmap, reg2, val2);
		if (rc)
			dev_err(dev, "failed to  write register:%x with value:%x\n",
				reg2, val2);
	}
	return rc;
}


static ssize_t user_led_color_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lp5814_user *chip;
	int rc;

	chip = (struct lp5814_user *)i2c_get_clientdata(client);

	switch (chip->color) {
	case RED:
		rc = snprintf(buf, 10, "%s\n", "RED");
		break;
	case BLUE:
		rc = snprintf(buf, 10, "%s\n", "BLUE");
		break;
	case GREEN:
		rc = snprintf(buf, 10, "%s\n", "GREEN");
		break;
	case WHITE:
		rc = snprintf(buf, 10, "%s\n", "WHITE");
		break;
	case OFF:
		rc = snprintf(buf, 10, "%s\n", "OFF");
		break;
	case UNKNOWN:
		rc = snprintf(buf, 10, "%s\n", "UNKNOWN");
		break;
	}
	dev_err(dev, "color:%s\n", buf);
	return rc;
}

static ssize_t user_led_color_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct lp5814_user *chip;

	chip = (struct lp5814_user *)i2c_get_clientdata(client);
	mutex_lock(&chip->m_lock);
	chip = (struct lp5814_user *)i2c_get_clientdata(client);
	if (!strncasecmp(buf, "red", 3)) {
		lp5814_user_led_enable_wrgb_color(dev, RED);
		chip->color = RED;
	} else if (!strncasecmp(buf, "green", 5)) {
		lp5814_user_led_enable_wrgb_color(dev, GREEN);
		chip->color = GREEN;
	} else if (!strncasecmp(buf, "blue", 4)) {
		lp5814_user_led_enable_wrgb_color(dev, BLUE);
		chip->color = BLUE;
	} else if (!strncasecmp(buf, "white", 5)) {
		lp5814_user_led_enable_wrgb_color(dev, WHITE);
		chip->color = WHITE;
	} else if (!strncasecmp(buf, "off", 3)) {
		lp5814_user_led_turn_all_off(dev);
		chip->color = OFF;
	} else {
		dev_dbg(dev, "color:%s doesn't match WRGB ones..\n", buf);
		chip->color = UNKNOWN;
	}

	mutex_unlock(&chip->m_lock);
	return count;
}

static DEVICE_ATTR_RW(user_led_color);

static struct attribute *lp5814_user_led_attrs[] = {
	&dev_attr_user_led_reg.attr,
	&dev_attr_user_led_val.attr,
	&dev_attr_user_led_color.attr,
	NULL
};

static const struct attribute_group lp5814_user_led_attr_group = {
	.attrs = lp5814_user_led_attrs,
};

int lp5814_user_led_probe(struct i2c_client *client)
{
	struct lp5814_user *chip;
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

	chip->regmap = devm_regmap_init_i2c(client, &lp5814_user_led_regmap_config);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(&client->dev, "Failed to allocate reg map: %d\n", ret);
		goto error;
	}

	ret = sysfs_create_group(&client->dev.kobj, &lp5814_user_led_attr_group);
	if (ret) {
		dev_err(&client->dev, "failed to create sysfs group, err:%d\n", ret);
		goto error;
	}

	ret = regmap_write(chip->regmap, LP5814_REG_RESET_CMD, LP5814_RESET_CMD_VAL);
	if (ret) {
		dev_err(&client->dev, "failed to  write register:%x with value:%x\n",
				LP5814_REG_RESET_CMD, LP5814_RESET_CMD_VAL);
		goto i2c_failed;
	}

	ret = regmap_read(chip->regmap, LP5814_REG_RESET_CMD, &chipid);
	if (ret) {
		dev_err(&client->dev, "Failed to read chip ID: %d\n", ret);
		goto i2c_failed;
	}
	dev_dbg(&client->dev, "expected: %x ChipId read is :%x\n",
			LP5814_RESET_CMD_VAL, chipid);

	ret = regmap_write(chip->regmap, LP5814_REG_CHIP_EN, 0x1);
	if (ret) {
		dev_err(&client->dev,
		"failed to  write register:LP5814_REG_CHIP_EN with value:0x1\n");
		goto i2c_failed;
	}
	mutex_unlock(&chip->m_lock);

	return 0;
i2c_failed:
	sysfs_remove_group(&client->dev.kobj, &lp5814_user_led_attr_group);
	ret = -EPROBE_DEFER;

error:
	mutex_unlock(&chip->m_lock);
	return ret;
}

static void lp5814_user_led_remove(struct i2c_client *client)
{
	if (client) {
		struct lp5814_user *chip = (struct lp5814_user *)i2c_get_clientdata(client);

		mutex_lock(&chip->m_lock);
		sysfs_remove_group(&client->dev.kobj, &lp5814_user_led_attr_group);
		lp5814_user_led_turn_all_off(&client->dev);
		mutex_unlock(&chip->m_lock);
	}
}

static const struct of_device_id lp5814_user_led_match_table[] = {
	{ .compatible = "ti,rgbw-user-led2", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, lp5814_user_led_match_table);

static struct i2c_driver lp5814_user_led_driver = {
	.driver = {
		.name = "rgbw-user-led2",
		.of_match_table = lp5814_user_led_match_table,
	},
	.probe = lp5814_user_led_probe,
	.remove = lp5814_user_led_remove,
};

module_i2c_driver(lp5814_user_led_driver);

MODULE_DESCRIPTION("TI LP5814 RGBW USER LED driver");
MODULE_LICENSE("GPL");
