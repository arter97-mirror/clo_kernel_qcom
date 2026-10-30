// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/device.h>
#include <linux/extcon-provider.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_platform.h>
#include <linux/usb/dwc3-msm.h>


struct ktm5000b0t {
	struct device *dev;
	struct extcon_dev *edev;
	struct platform_device *usb_pdev;

	u32 irq_gpio;
	u32 power_3v3;
	u32 reset;
	int id_irq;

	unsigned long debounce_jiffies;
	struct delayed_work wq_detcable;
};

static const unsigned int usb_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

#define USB_GPIO_DEBOUNCE_MS	20	/* ms */
#define USB_DWC3_MODE_RETRY_COUNT	100
#define USB_DWC3_MODE_RETRY_DELAY_MS	20

/* Perform initial detection */
static void extcon_initial_detect(struct ktm5000b0t *pdata)
{
	int id;

	/* check ID and update cable init state */
	id = pdata->irq_gpio ?
		gpio_get_value_cansleep(pdata->irq_gpio) : 1;

	if (id) {
		extcon_set_state_sync(pdata->edev, EXTCON_USB_HOST, false);
		extcon_set_state_sync(pdata->edev, EXTCON_USB, true);
	} else {
		extcon_set_state_sync(pdata->edev, EXTCON_USB_HOST, true);
		extcon_set_state_sync(pdata->edev, EXTCON_USB, false);
	}

}

static int extcon_set_dwc3_msm_mode(struct ktm5000b0t *pdata,
		bool mode)
{
	int rc;
	int retry_count = USB_DWC3_MODE_RETRY_COUNT;

	if (!pdata || !pdata->usb_pdev) {
		pr_err("invalid args or usb pdev not found\n");
		return -EINVAL;
	}

	while (retry_count) {
		rc = dwc3_msm_set_ss_mode(&pdata->usb_pdev->dev, mode);
		if (rc != -EAGAIN)
			break;
		dev_err(pdata->dev, "USB busy, retry\n");
		/* wait for hw recommended delay for usb */
		msleep(USB_DWC3_MODE_RETRY_DELAY_MS);
		retry_count--;
	}

	if (rc)
		dev_err(pdata->dev, "Error switch USB mode: %d\n", rc);

	return rc;
}

static void ktm5000_detect_cable(struct work_struct *work)
{
	int id;
	struct ktm5000b0t *pdata = container_of(to_delayed_work(work),
						    struct ktm5000b0t,
						    wq_detcable);

	/* check ID and update cable state */
	id = pdata->irq_gpio ?
		gpio_get_value_cansleep(pdata->irq_gpio) : 1;

	dev_dbg(pdata->dev, "Cable detect work triggered: id=%d\n", id);

	if (id) {
		extcon_set_state_sync(pdata->edev, EXTCON_USB_HOST, false);
		extcon_set_state_sync(pdata->edev, EXTCON_USB, true);
		extcon_set_dwc3_msm_mode(pdata, 1);
	} else {
		extcon_set_state_sync(pdata->edev, EXTCON_USB_HOST, true);
		extcon_set_state_sync(pdata->edev, EXTCON_USB, false);
		extcon_set_dwc3_msm_mode(pdata, 0);
	}

}

static irqreturn_t usb_irq_handler(int irq, void *dev_id)
{
	struct ktm5000b0t *pdata = dev_id;

	queue_delayed_work(system_power_efficient_wq, &pdata->wq_detcable,
			   pdata->debounce_jiffies);

	return IRQ_HANDLED;
}

static int ktm5000_parse_dt(struct ktm5000b0t *pdata)
{
	int ret = 0;
	struct device_node *np = pdata->dev->of_node;

	pdata->irq_gpio =
		of_get_named_gpio(np, "kinetic,irq-gpio", 0);
	if (gpio_is_valid(pdata->irq_gpio)) {
		pr_debug("kinetic-gpio-irq=%d\n", pdata->irq_gpio);
	} else {
		pdata->irq_gpio = 0;
		dev_err(pdata->dev, "kinetic,irq-gpio gpio not specified\n");
		ret = -EINVAL;
	}

	pdata->power_3v3 =
		of_get_named_gpio(np, "kinetic,power-3v3-en", 0);
	if (gpio_is_valid(pdata->power_3v3)) {
		pr_debug("kinetic-gpio-3v3=%d\n", pdata->power_3v3);
	} else {
		dev_err(pdata->dev, "kinetic,power-3v3-en gpio not specified\n");
		ret = -EINVAL;
	}

	pdata->reset =
		of_get_named_gpio(np, "kinetic,reset-gpio", 0);
	if (gpio_is_valid(pdata->reset)) {
		pr_debug("kinetic-reset-gpio=%d\n", pdata->reset);
	} else {
		dev_err(pdata->dev, "kinetic,reset-gpio gpio not specified\n");
		ret = -EINVAL;
	}

	return ret;
}

static int ktm5000_gpio_configure(struct ktm5000b0t *pdata, bool on)
{
	int ret = 0;

	if (on) {
		ret = gpio_request(pdata->power_3v3, "kinetic-gpio-3v3");
		if (ret) {
			dev_err(pdata->dev, "ktm5000 request kinetic-gpio-3v3 fail\n");
			goto error;
		}

		ret = gpio_direction_output(pdata->power_3v3, 1);
		if (ret) {
			dev_err(pdata->dev, "set kinetic-gpio-3v3 direction fail\n");
			goto power_err;
		}

		ret = gpio_request(pdata->reset, "kinetic-reset-gpio");
		if (ret < 0) {
			dev_err(pdata->dev, "gpio request kinetic-reset-gpio %d fail\n",
				pdata->reset);
			goto power_err;
		}

		ret = gpio_direction_output(pdata->reset, 0);
		if (ret < 0) {
			dev_err(pdata->dev, "set kinetic-reset-gpio %d fail\n", pdata->reset);
			goto reset_err;
		}

		usleep_range(2000, 5000);
		gpio_direction_output(pdata->reset, 1);
		usleep_range(5000, 8000);
	} else {
		if (gpio_is_valid(pdata->power_3v3))
			gpio_free(pdata->power_3v3);
		if (gpio_is_valid(pdata->reset))
			gpio_free(pdata->reset);
	}

	return ret;

reset_err:
	gpio_free(pdata->reset);
power_err:
	gpio_free(pdata->power_3v3);
error:
	return ret;
}

static ssize_t gpio3v3_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int ret = 0;
	int32_t command = 0;
	struct ktm5000b0t *pdata = dev_get_drvdata(dev);

	ret = kstrtoint(buf, 10, &command);
	if (command == 0) {
		ret = gpio_direction_output(pdata->power_3v3, 0);
		if (ret)
			pr_err("Failed to direction output kinetic_power_3v3=0 err\n");
	} else if (command == 1) {
		ret = gpio_direction_output(pdata->power_3v3, 1);
		if (ret)
			pr_err("Failed to direction output kinetic_power_3v3=1 err\n");
	}

	return len;
}

static ssize_t gpio3v3_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ktm5000b0t *pdata = dev_get_drvdata(dev);

	return scnprintf(buf, SZ_4K, "%d\n", gpio_get_value(pdata->power_3v3));
}

static DEVICE_ATTR_RW(gpio3v3);

static ssize_t reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int ret = 0;
	int32_t command = 0;
	struct ktm5000b0t *pdata = dev_get_drvdata(dev);

	ret = kstrtoint(buf, 10, &command);
	if (command == 0) {
		ret = gpio_direction_output(pdata->reset, 0);
		if (ret)
			pr_err("Failed to direction output kinetic_reset=0 err\n");
	} else if (command == 1) {
		ret = gpio_direction_output(pdata->reset, 1);
		if (ret)
			pr_err("Failed to direction output kinetic_reset=1 err\n");
	}

	return len;
}
static DEVICE_ATTR_WO(reset);

static int ktm5000_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct ktm5000b0t *pdata;
	struct device_node *usb_node;

	pdata = devm_kzalloc(&pdev->dev, sizeof(struct ktm5000b0t), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->dev = &pdev->dev;

	pdata->edev = devm_extcon_dev_allocate(pdata->dev, usb_extcon_cable);
	if (IS_ERR(pdata->edev)) {
		dev_err(pdata->dev, "failed to allocate extcon device\n");
		return -ENOMEM;
	}

	ret = devm_extcon_dev_register(pdata->dev, pdata->edev);
	if (ret < 0) {
		dev_err(pdata->dev, "failed to register extcon device\n");
		return ret;
	}

	ret = ktm5000_parse_dt(pdata);
	if (ret) {
		dev_err(pdata->dev, "failed to parse device tree\n");
		goto err_dt_parse;
	}

	usb_node = of_parse_phandle(pdata->dev->of_node, "usb-controller", 0);
	if (!usb_node) {
		dev_err(pdata->dev, "unable to get usb node\n");
		ret = -EINVAL;
		goto err_dt_parse;
	}

	pdata->usb_pdev = of_find_device_by_node(usb_node);
	of_node_put(usb_node);
	if (!pdata->usb_pdev) {
		dev_err(pdata->dev, "unable to get usb pdev\n");
		ret = -EPROBE_DEFER;
		goto err_dt_parse;
	}

	ret = ktm5000_gpio_configure(pdata, true);
	if (ret) {
		dev_err(pdata->dev, "failed to configure GPIOs\n");
		goto error;
	}

	dev_dbg(pdata->dev, "gpio3v3 = %d, gpioreset=%d\n", pdata->power_3v3,
		pdata->reset);

	device_create_file(pdata->dev, &dev_attr_gpio3v3);
	device_create_file(pdata->dev, &dev_attr_reset);

	pdata->debounce_jiffies = msecs_to_jiffies(USB_GPIO_DEBOUNCE_MS);

	INIT_DELAYED_WORK(&pdata->wq_detcable, ktm5000_detect_cable);

	if (pdata->irq_gpio) {
		pdata->id_irq = gpio_to_irq(pdata->irq_gpio);
		if (pdata->id_irq < 0) {
			dev_err(pdata->dev, "failed to get ID IRQ\n");
			ret = pdata->id_irq;
			goto error;
		}

		ret = devm_request_threaded_irq(pdata->dev, pdata->id_irq, NULL,
						usb_irq_handler,
						IRQF_TRIGGER_RISING |
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						pdev->name, pdata);
		if (ret < 0) {
			dev_err(pdata->dev, "failed to request handler for ID IRQ\n");
			goto error;
		}
	}

	platform_set_drvdata(pdev, pdata);
	device_set_wakeup_capable(&pdev->dev, true);
	extcon_initial_detect(pdata);

	dev_dbg(pdata->dev, "probe successfully!\n");

	return ret;

error:
	ktm5000_gpio_configure(pdata, false);
err_dt_parse:
	if (pdata->usb_pdev)
		platform_device_put(pdata->usb_pdev);
	return ret;
}

static void ktm5000_remove(struct platform_device *pdev)
{
	struct ktm5000b0t *pdata = platform_get_drvdata(pdev);

	if (pdata->usb_pdev)
		platform_device_put(pdata->usb_pdev);
	gpio_free(pdata->power_3v3);
	gpio_free(pdata->reset);

}

static const struct of_device_id ktm5000_match_table[] = {
	{ .compatible = "kinetic,ktm5000b0t", },
	{ }
};
MODULE_DEVICE_TABLE(of, ktm5000_match_table);

static struct platform_driver ktm5000_driver = {
	.probe		= ktm5000_probe,
	.remove		= ktm5000_remove,
	.driver		= {
		.name		= "kinetic,ktm5000b0t",
		.of_match_table	= ktm5000_match_table,
	},
};

module_platform_driver(ktm5000_driver);
MODULE_LICENSE("GPL");
