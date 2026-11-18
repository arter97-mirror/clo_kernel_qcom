// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kern_levels.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/printk.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "cap1296.h"

#define MAX_KEY_NUM  5
#define NUMBER_OF_SENSORS   6

uint8_t CAP_error;
uint8_t CAP_pressOccurred;
uint8_t CAP_releaseOccurred;
uint8_t CAP_state;
uint8_t CAP_lowPowerMode;
uint8_t CAP_keyMode;
int8_t CAP_sensorDeltas[NUMBER_OF_SENSORS];

uint8_t key_array[] = {
	KEY_F11,
	KEY_MUTE,
	KEY_VOLUMEDOWN,
	KEY_VOLUMEUP,
	KEY_POWER,
};

struct cap1296_device {
	struct device *dev;
	struct workqueue_struct *wq;
	struct work_struct work;
	struct i2c_client *client;
	struct input_dev *input;
	struct regulator *vddio;
	int irq;
	int irq_gpio;
	uint8_t *touch_data;
	uint8_t device_id;
	bool init_complete;
	struct work_struct resume_work;
};

static struct cap1296_device *global_cap;
static struct i2c_client *cap_client;

static DEFINE_MUTEX(cap1296_i2c_mutex);
static void cap1296_ts_worker(struct work_struct *work);

static int cap1296_i2c_read(struct cap1296_device *cap, uint8_t reg)
{
	struct i2c_client *client = to_i2c_client(cap->dev);
	struct i2c_msg msg[2];
	uint8_t val;
	int ret;

	if (!client->adapter) {
		dev_err(cap->dev, "!client->adapter\n");
		return -ENODEV;
	}

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = sizeof(reg);
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = &val;
	msg[1].len = sizeof(val);

	mutex_lock(&cap1296_i2c_mutex);
	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	mutex_unlock(&cap1296_i2c_mutex);

	if (ret < 0) {
		dev_err(cap->dev, "Fail to read reg 0x%x\n", reg);
		return ret;
	}

	return val;
}

static int cap1296_i2c_write(struct cap1296_device *cap,
				uint8_t reg, uint8_t val)
{
	struct i2c_client *client = to_i2c_client(cap->dev);
	struct i2c_msg msg[1];
	uint8_t data[2];
	int ret;

	data[0] = reg;
	data[1] = val;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = data;
	msg[0].len = ARRAY_SIZE(data);

	mutex_lock(&cap1296_i2c_mutex);
	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	mutex_unlock(&cap1296_i2c_mutex);
	if (ret < 0) {
		dev_err(cap->dev, "Fail to write reg 0x%x\n", reg);
		return ret;
	}

	return 0;
}

uint8_t CAP_getMainControl(struct cap1296_device *cap)
{
	uint8_t value = 0;

	value = cap1296_i2c_read(cap, 0x00);
	return value;
}

uint8_t CAP_getConfiguration2(struct cap1296_device *cap)
{
	uint8_t value = 0;

	value = cap1296_i2c_read(cap, 0x44);
	return value;
}

uint8_t CAP_getGeneralStatus(struct cap1296_device *cap)
{
	uint8_t value = 0;

	value = cap1296_i2c_read(cap, 0x02);
	return value;
}

uint8_t CAP_getSensorStatus(struct cap1296_device *cap)
{
	uint8_t value = 0;

	value = cap1296_i2c_read(cap, 0x03);

	return value;
}

void CAP_resetGeneralStatus(struct cap1296_device *cap)
{
	uint8_t value = 0;

	value = cap1296_i2c_read(cap, 0x00);
	if (value < 0)
		return;

	value &= 0xFE;  // Clear the INT bit (bit 0)
	cap1296_i2c_write(cap, 0x00, value);
}

void CAP_enterStandby(struct cap1296_device *cap)
{
	uint8_t value = 0;

	value = cap1296_i2c_read(cap, 0x00);
	if (value < 0)
		return;

	value |= 0x20;// Set the Standby bit

	cap1296_i2c_write(cap, 0x00, value);
}

void CAP_exitStandby(struct cap1296_device *cap)
{
	uint8_t value = 0;

	value = cap1296_i2c_read(cap, 0x00);
	if (value < 0)
		return;

	value &= 0xDF;

	cap1296_i2c_write(cap, 0x00, value);
}

void CAP_exitDeepSleep(struct cap1296_device *cap)
{
	uint8_t value = 0;

	value = cap1296_i2c_read(cap, 0x00);
	if (value < 0)
		return;

	value &= 0xEF;
	cap1296_i2c_write(cap, 0x00, value);
}

void CAP_enterDeepSleep(struct cap1296_device *cap)
{
	uint8_t value = 0;

	value = cap1296_i2c_read(cap, 0x00);
	if (value < 0)
		return;

	value |= 0x10;
	cap1296_i2c_write(cap, 0x00, value);
}

static ssize_t power_on_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cap1296_device *cap = dev_get_drvdata(dev);

	return snprintf(buf, 60, "%d\n", regulator_is_enabled(cap->vddio));
}

static ssize_t power_on_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct cap1296_device *cap = dev_get_drvdata(dev);
	unsigned int data = 0;
	int ret;

	ret = kstrtoint (buf, 16, &data);
	if (ret)
		return ret;

	if (data) {
		if (!regulator_is_enabled(cap->vddio)) {
			ret = regulator_enable(cap->vddio);
			if (ret) {
				dev_err(dev, "enable vddio failed\n");
				return ret;
			}
		}
	} else {
		if (regulator_is_enabled(cap->vddio)) {
			ret = regulator_disable(cap->vddio);
			if (ret) {
				dev_err(dev, "disable vddio failed\n");
				return ret;
			}
		}
	}

	return count;
}

static ssize_t chip_id_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cap1296_device *cap = dev_get_drvdata(dev);

	return snprintf(buf, 60, "0x%x\n", cap1296_i2c_read(cap, 0xFD));
}

static DEVICE_ATTR_RW(power_on);
static DEVICE_ATTR_RO(chip_id);

static struct attribute *cap1296_attributes[] = {
	&dev_attr_power_on.attr,
	&dev_attr_chip_id.attr,
	NULL
};

static struct attribute_group cap1296_attribute_group = {
	.attrs = cap1296_attributes
};

static int cap1296_ts_init(struct i2c_client *client,
				struct cap1296_device *cap)
{
	struct input_dev *input_device;
	int rc = 0;
	int i;

	//apply input device
	input_device = input_allocate_device();
	if (!input_device) {
		dev_err(cap->dev, "!input_device\n");
		rc = -ENOMEM;
	}

	//init input device
	cap->input = input_device;
	input_device->name = "touch_keys";
	input_device->id.bustype = BUS_I2C;
	input_device->dev.parent = &client->dev;
	input_set_drvdata(input_device, cap);

	input_device->evbit[0] = BIT_MASK(EV_KEY);//report key type
	for (i = 0; i < MAX_KEY_NUM; i++)
		set_bit(key_array[i], input_device->keybit);

	//create a workqueue
	cap->wq = create_singlethread_workqueue("kworkqueue_ts");
	if (!cap->wq) {
		dev_err(&client->dev, "Could not create workqueue\n");
		goto error_wq_create;
	}

	//clean workqueue
	flush_workqueue(cap->wq);
	//add a task to workqueue, to execute cap1296_ts_worker
	INIT_WORK(&cap->work, cap1296_ts_worker);
	rc = input_register_device(input_device);  //register input device
	if (rc)
		goto error_unreg_device;

	return 0;

error_unreg_device:
	destroy_workqueue(cap->wq);
error_wq_create:
	input_free_device(input_device);

	return rc;
}

static int cap1296_init_chip(struct cap1296_device *cap)
{
	uint8_t value = 0;
	uint8_t i = 0;
	int ret;

	for (i = 0; i < 5; i++) {
		value = cap1296_i2c_read(cap, 0xFD);
		if (value == CAP1296_ID) {
			dev_info(cap->dev, "Match chip ID 0x%x\n", value);
			break;
		}
		msleep(200);
	}
	if (value != CAP1296_ID) {
		dev_err(cap->dev, "Error: expect ID: 0x%x, Invalid chip ID 0x%x\n",
				CAP1296_ID, value);
		return -ENODEV;
	}

	/* Load the Default Configuration */
	for (i = 0; i < CAP_NUM_WRITABLE_REGISTERS; i++) {
		value = CAP_defaultConfiguration[i][1];
		ret = cap1296_i2c_write(cap, CAP_defaultConfiguration[i][0], value);
		if (ret < 0) {
			dev_err(cap->dev, "Error: cap load default configuration failed\n");
			return -1;
		}
	}
	cap->init_complete = true;

	return 0;
}

static irqreturn_t cap_ts_irq_handler(int irq, void *dev_id)
{
	struct cap1296_device *cap = dev_id;

	disable_irq_nosync(cap->irq);  //disable irq

	//Determine whether the waiting queue is suspended
	if (!work_pending(&cap->work))
		queue_work(cap->wq, &cap->work);

	return IRQ_HANDLED;
}

static void cap1296_ts_worker(struct work_struct *work)
{
	struct cap1296_device *cap = container_of(work,
				struct cap1296_device, work);
	struct input_dev *input_device;
	uint8_t latchedState;
	uint8_t currentState;

	input_device = cap->input;
	// Get latched and current state information
	latchedState = CAP_getSensorStatus(cap);
	CAP_resetGeneralStatus(cap);
	currentState = CAP_getSensorStatus(cap);

	// If the current state and the latched state are not
	// the same, there must have been a release. The XOR
	// of the two state variables will output the sensors
	// that were released.
	CAP_releaseOccurred = currentState & latchedState;

	if (CAP_releaseOccurred != 0) {
		switch (CAP_releaseOccurred) {
		case CAP_KEY_CHANNAL1:
			//cs1 press
			input_report_key(input_device, KEY_MUTE, 1);
			input_sync(input_device);
			CAP_keyMode = CAP_KEY_MUTE_FLAG_STATUS_REG;
			break;

		case CAP_KEY_CHANNAL2:
			//cs2 press
			input_report_key(input_device, KEY_VOLUMEUP, 1);
			input_sync(input_device);
			CAP_keyMode = CAP_KEY_UP_FLAG_STATUS_REG;
			break;

		case CAP_KEY_CHANNAL3:
			//cs3 press
			input_report_key(input_device, KEY_F11, 1);
			input_sync(input_device);
			CAP_keyMode = CAP_KEY_F11_FLAG_STATUS_REG;
			break;

		case CAP_KEY_CHANNAL4:
			//cs4 press
			input_report_key(input_device, KEY_VOLUMEDOWN, 1);
			input_sync(input_device);
			CAP_keyMode = CAP_KEY_DOWN_FLAG_STATUS_REG;
			break;

		case CAP_KEY_CHANNAL5:
			//cs5 press
			input_report_key(input_device, KEY_POWER, 1);
			input_sync(input_device);
			CAP_keyMode = CAP_KEY_POWER_FLAG_STATUS_REG;
			break;

		default:
			dev_err(cap->dev, "Unknown ts input type\n");
		break;
		}
	} else {
		switch (CAP_keyMode) {
		case CAP_KEY_MUTE_FLAG_STATUS_REG:
			//cs1 release
			input_report_key(input_device, KEY_MUTE, 0);
			input_sync(input_device);
			CAP_keyMode = 0;
			break;

		case CAP_KEY_UP_FLAG_STATUS_REG:
			//cs2 release
			input_report_key(input_device, KEY_VOLUMEUP, 0);
			input_sync(input_device);
			CAP_keyMode = 0;
			break;

		case CAP_KEY_F11_FLAG_STATUS_REG:
			//cs3 release
			input_report_key(input_device, KEY_F11, 0);
			input_sync(input_device);
			CAP_keyMode = 0;
			break;

		case CAP_KEY_DOWN_FLAG_STATUS_REG:
			//cs4 release
			input_report_key(input_device, KEY_VOLUMEDOWN, 0);
			input_sync(input_device);
			CAP_keyMode = 0;
			break;

		case CAP_KEY_POWER_FLAG_STATUS_REG:
			//cs5 release
			input_report_key(input_device, KEY_POWER, 0);
			input_sync(input_device);
			CAP_keyMode = 0;
			break;

		default:
			dev_err(cap->dev, "Unknown ts input type\n");
		break;
		}
	}
	dev_dbg(cap->dev, "CAP_releaseOccurred=%d currentState=%d\n",
			CAP_releaseOccurred, currentState);

	enable_irq(cap->irq);
}

static int cap1296_probe(struct i2c_client *client)
{
	struct cap1296_device *cap = NULL;
	struct device_node *np = client->dev.of_node;
	uint8_t state;
	int ret;

	dev_dbg(&client->dev, "cap1296 probe start\n");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "I2C functionality not supported\n");
		return -ENODEV;
	}

	cap = devm_kzalloc(&client->dev, sizeof(struct cap1296_device), GFP_KERNEL);
	if (!cap)
		return -ENOMEM;

	cap->client = client;
	i2c_set_clientdata(client, cap);
	cap->device_id = 0;
	global_cap = cap;
	cap->dev = &client->dev;
	dev_set_drvdata(cap->dev, cap);

	cap->irq_gpio = of_get_named_gpio(np, "irq-gpio", 0);
	if (cap->irq_gpio <= 0) {
		dev_err(&client->dev, "Fail to get irq gpio\n");
		return -1;
	}

	cap->vddio = regulator_get(cap->dev, "vddio");
	if (IS_ERR_OR_NULL(cap->vddio)) {
		dev_err(&client->dev, "Fail to get vddio regulator\n");
		ret = PTR_ERR(cap->vddio);
		goto gpio_err;
	} else {
		ret = regulator_enable(cap->vddio);
		if (ret) {
			dev_err(&client->dev, "enable vddio failed\n");
			goto gpio_err;
		}
	}
	msleep(200);
	ret = cap1296_init_chip(cap);
	if (ret < 0)
		goto ldo_err;

	cap_client = client;

	ret = cap1296_ts_init(client, cap);
	if (ret) {
		dev_err(&client->dev, "cap1296 init failed; %d\n", ret);
		goto ldo_err;
	}

	cap->irq = gpio_to_irq(cap->irq_gpio);
	if (cap->irq < 0) {
		dev_err(&client->dev, "gpio_to_irq failed, irq_gpio=%d ret=%d\n",
			cap->irq_gpio, cap->irq);
			ret = cap->irq;
		goto ldo_err;
	}

	ret = request_irq(cap->irq, cap_ts_irq_handler,
			IRQF_TRIGGER_LOW, client->name, cap);
	if (ret) {
		dev_err(cap->dev, "Cannot allocate ts INT! ERRNO:%d\n", ret);
		goto ldo_err;
	}

	CAP_exitDeepSleep(cap);
	state = CAP_getSensorStatus(cap);
	if (state < 0)
		dev_err(cap->dev, "get sensor status failed\n");

	if (state)
		CAP_resetGeneralStatus(cap);

	if (state & 0x80)// Enter standby when CS8 is pressed
		CAP_enterStandby(cap);
	else if (state & 0x02)// Exit standby when CS2 is pressed
		CAP_exitStandby(cap);
	else if (state & 0x20)// Enter Low Power mode when CS6 is pressed
		CAP_lowPowerMode = 1;
	else if (state & 0x08)// Exit Low Power mode when CS4 is pressed
		CAP_lowPowerMode = 0;

	ret = sysfs_create_group(&cap->dev->kobj,
			&cap1296_attribute_group);
	if (ret)
		dev_err(cap->dev, "Fail to creat sysfs\n");

	dev_dbg(&client->dev, "probe successfully\n");

	return 0;

ldo_err:
	regulator_disable(cap->vddio);
	cap->vddio = NULL;
gpio_err:

	return ret;
}

static void cap1296_remove(struct i2c_client *client)
{
	struct cap1296_device *cap = i2c_get_clientdata(client);

	cancel_work_sync(&cap->work);

	if (cap->irq > 0) {
		disable_irq(cap->irq);
		free_irq(cap->irq, cap);
	}

	if (cap->wq)
		destroy_workqueue(cap->wq);

	if (cap->input) {
		dev_info(cap->dev, "Unregister input device\n");
		input_unregister_device(cap->input);
		cap->input = NULL;
	}

	i2c_set_clientdata(client, NULL);
	dev_info(&client->dev, "driver unregistered\n");
}

static const struct of_device_id cap1296_id_match[] = {
	{ .compatible = "microchip,cap1296" },
	{ }
};
MODULE_DEVICE_TABLE(of, cap1296_id_match);

static struct i2c_driver cap1296_driver = {
	.driver = {
		.name = "cap1296",
		.of_match_table = of_match_ptr(cap1296_id_match),
	},
	.probe = cap1296_probe,
	.remove = cap1296_remove,
};

static int __init cap1296_init(void)
{
	return i2c_add_driver(&cap1296_driver);
}

static void __exit cap1296_exit(void)
{
	i2c_del_driver(&cap1296_driver);
}

module_init(cap1296_init);
module_exit(cap1296_exit);

MODULE_DESCRIPTION("cap1296 touch sensor");
MODULE_LICENSE("GPL");
