// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/devm-helpers.h>
#include <linux/extcon-provider.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/suspend.h>

#define USB_ID_DEBOUNCE_MS	5	/* ms */
#define CHG1_INT_LATCHED_CLR	0x2714
#define VBUS_LATCH_BIT		BIT(0)

/* CHG1_POLARITY register: selects polarity of the PAD input to debouncer */
#define CHG1_POLARITY_REG	0x2747
#define INV_DEB_IN_BIT		BIT(7)

struct qcom_usb_extcon_info {
	struct extcon_dev *edev;
	int id_irq;
	int vbus_irq;
	struct delayed_work wq_detcable;
	unsigned long debounce_jiffies;
	struct regmap *regmap;
	bool vbus_polarity_inverted; /* true when INV_DEB_IN (bit7 of 0x2747) = 1 */
};

static const unsigned int qcom_usb_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

static void qcom_usb_extcon_detect_cable(struct work_struct *work)
{
	bool state = false;
	int ret;
	union extcon_property_value val;
	struct qcom_usb_extcon_info *info = container_of(to_delayed_work(work),
						    struct qcom_usb_extcon_info,
						    wq_detcable);

	if (info->id_irq > 0) {
		/* check ID and update cable state */
		ret = irq_get_irqchip_state(info->id_irq,
				IRQCHIP_STATE_LINE_LEVEL, &state);
		if (ret)
			return;

		if (!state) {
			val.intval = true;
			extcon_set_property(info->edev, EXTCON_USB_HOST,
						EXTCON_PROP_USB_SS, val);
		}
		extcon_set_state_sync(info->edev, EXTCON_USB_HOST, !state);
	}

	if (info->vbus_irq > 0) {
		/* check VBUS and update cable state */
		ret = irq_get_irqchip_state(info->vbus_irq,
				IRQCHIP_STATE_LINE_LEVEL, &state);
		if (ret)
			return;

		/*
		 * If CHG1_POLARITY.INV_DEB_IN (0x2747 bit7) is set the PAD input
		 * is inverted before it reaches the debouncer, so the IRQ line
		 * level seen by the kernel is the logical complement of the true
		 * VBUS presence.  XOR with the flag to normalise the state.
		 */
		state ^= info->vbus_polarity_inverted;

		if (state) {
			val.intval = true;
			extcon_set_property(info->edev, EXTCON_USB,
						EXTCON_PROP_USB_SS, val);
		}
		extcon_set_state_sync(info->edev, EXTCON_USB, state);
	}
}

static irqreturn_t qcom_usb_irq_handler(int irq, void *dev_id)
{
	struct qcom_usb_extcon_info *info = dev_id;

	queue_delayed_work(system_power_efficient_wq, &info->wq_detcable,
			   info->debounce_jiffies);

	return IRQ_HANDLED;
}

static int qcom_usb_extcon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_usb_extcon_info *info;
	unsigned int polarity_val;
	int ret;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	/*
	 * Obtain the parent regmap to read the CHG1_POLARITY register once
	 * during probe.  The register must not be polled at runtime because
	 * the HW spec states it must not change while EN_CTL.EN_DEB = 1.
	 */
	info->regmap = dev_get_regmap(dev->parent, NULL);
	if (!info->regmap) {
		dev_err(dev, "failed to get parent regmap\n");
		return -ENODEV;
	}

	/*
	 * Read CHG1_POLARITY (0x2747) bit 7 (INV_DEB_IN).
	 *   0x00 -> PAD connected to debouncer without inversion (normal)
	 *   0x80 -> inverted PAD connected to debouncer (polarity inverted)
	 */
	ret = regmap_read(info->regmap, CHG1_POLARITY_REG, &polarity_val);
	if (ret) {
		dev_err(dev, "failed to read CHG1_POLARITY reg: %d\n", ret);
		return ret;
	}
	info->vbus_polarity_inverted = !!(polarity_val & INV_DEB_IN_BIT);
	dev_dbg(dev, "CHG1_POLARITY=0x%02x, vbus_polarity_inverted=%d\n",
		polarity_val, info->vbus_polarity_inverted);

	info->edev = devm_extcon_dev_allocate(dev, qcom_usb_extcon_cable);
	if (IS_ERR(info->edev)) {
		dev_err(dev, "failed to allocate extcon device\n");
		return -ENOMEM;
	}

	ret = devm_extcon_dev_register(dev, info->edev);
	if (ret < 0) {
		dev_err(dev, "failed to register extcon device\n");
		return ret;
	}

	ret = extcon_set_property_capability(info->edev,
			EXTCON_USB, EXTCON_PROP_USB_SS);
	ret |= extcon_set_property_capability(info->edev,
			EXTCON_USB_HOST, EXTCON_PROP_USB_SS);
	if (ret) {
		dev_err(dev, "failed to register extcon props rc=%d\n",
						ret);
		return ret;
	}

	info->debounce_jiffies = msecs_to_jiffies(USB_ID_DEBOUNCE_MS);

	ret = devm_delayed_work_autocancel(dev, &info->wq_detcable,
					   qcom_usb_extcon_detect_cable);
	if (ret)
		return ret;

	info->id_irq = platform_get_irq_byname(pdev, "usb_id");
	if (info->id_irq > 0) {
		ret = devm_request_threaded_irq(dev, info->id_irq, NULL,
					qcom_usb_irq_handler,
					IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					pdev->name, info);
		if (ret < 0) {
			dev_err(dev, "failed to request handler for ID IRQ\n");
			return ret;
		}
	}

	info->vbus_irq = platform_get_irq_byname(pdev, "usb_vbus");
	if (info->vbus_irq > 0) {
		ret = devm_request_threaded_irq(dev, info->vbus_irq, NULL,
					qcom_usb_irq_handler,
					IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					pdev->name, info);
		if (ret < 0) {
			dev_err(dev, "failed to request handler for VBUS IRQ\n");
			return ret;
		}
	}

	if (info->id_irq < 0 && info->vbus_irq < 0) {
		dev_err(dev, "ID and VBUS IRQ not found\n");
		return -EINVAL;
	}

	platform_set_drvdata(pdev, info);
	device_init_wakeup(dev, 1);

	/* Perform initial detection */
	qcom_usb_extcon_detect_cable(&info->wq_detcable.work);

	return 0;
}

#ifdef CONFIG_PM_SLEEP

static int qcom_usb_extcon_restore(struct device *dev)
{
	struct qcom_usb_extcon_info *info = dev_get_drvdata(dev);
	struct platform_device *pdev = to_platform_device(dev);
	int ret = 0;

	if (info->id_irq > 0) {
		devm_free_irq(dev, info->id_irq, info);
		ret = devm_request_threaded_irq(dev, info->id_irq, NULL,
					qcom_usb_irq_handler,
					IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					pdev->name, info);
		if (ret < 0) {
			dev_err(dev, "failed to re-register ID IRQ %d, ret=%d\n",
				info->id_irq, ret);
			return ret;
		}
	}

	if (info->vbus_irq > 0) {
		devm_free_irq(dev, info->vbus_irq, info);
		ret = devm_request_threaded_irq(dev, info->vbus_irq, NULL,
					qcom_usb_irq_handler,
					IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					pdev->name, info);
		if (ret < 0) {
			dev_err(dev, "failed to re-register VBUS IRQ %d, ret=%d\n",
				info->vbus_irq, ret);
			return ret;
		}
	}

	qcom_usb_extcon_detect_cable(&info->wq_detcable.work);

	/* Write BIT(0) to CLR to rearm VBUS interrupt after queue */
	ret = regmap_write(info->regmap, CHG1_INT_LATCHED_CLR, VBUS_LATCH_BIT);

	return 0;
}

static int qcom_usb_extcon_suspend(struct device *dev)
{
	struct qcom_usb_extcon_info *info = dev_get_drvdata(dev);
	int ret = 0;

	if (device_may_wakeup(dev)) {
		if (info->id_irq > 0)
			ret = enable_irq_wake(info->id_irq);
		if (info->vbus_irq > 0)
			ret = enable_irq_wake(info->vbus_irq);
	}

	return ret;
}

static int qcom_usb_extcon_resume(struct device *dev)
{
	struct qcom_usb_extcon_info *info = dev_get_drvdata(dev);
	int ret = 0, vbus_ret = 0, id_ret = 0;
	bool id_state = false, vbus_state = false;
	bool needs_detection = false;

	if (pm_suspend_target_state == PM_SUSPEND_MEM)
		return qcom_usb_extcon_restore(dev);

	if (device_may_wakeup(dev)) {
		if (info->id_irq > 0)
			ret = disable_irq_wake(info->id_irq);
		if (info->vbus_irq > 0)
			ret = disable_irq_wake(info->vbus_irq);
	}

	if (info->vbus_irq > 0) {
		vbus_ret = irq_get_irqchip_state(info->vbus_irq,
				IRQCHIP_STATE_LINE_LEVEL, &vbus_state);
		if (vbus_ret == 0) {
			/* normalise against hardware polarity inversion */
			vbus_state ^= info->vbus_polarity_inverted;
			if (extcon_get_state(info->edev, EXTCON_USB) != vbus_state)
				needs_detection = true;
		} else {
			dev_warn(dev, "failed to get VBUS IRQ state: %d\n", vbus_ret);
		}
	}

	if (info->id_irq > 0) {
		id_ret = irq_get_irqchip_state(info->id_irq,
				IRQCHIP_STATE_LINE_LEVEL, &id_state);
		if (id_ret == 0 && extcon_get_state(info->edev, EXTCON_USB_HOST) != !id_state)
			needs_detection = true;
		else if (id_ret != 0)
			dev_warn(dev, "failed to get ID IRQ state: %d\n", id_ret);
	}

	if (needs_detection) {
		cancel_delayed_work_sync(&info->wq_detcable);
		qcom_usb_extcon_detect_cable(&info->wq_detcable.work);
	}

	return ret;
}
#endif

static const struct dev_pm_ops qcom_usb_extcon_pm_ops = {
	.suspend = qcom_usb_extcon_suspend,
	.resume = qcom_usb_extcon_resume,
	.restore = qcom_usb_extcon_restore,
};

static const struct of_device_id qcom_usb_extcon_dt_match[] = {
	{ .compatible = "qcom,pm8941-misc", },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_usb_extcon_dt_match);

static struct platform_driver qcom_usb_extcon_driver = {
	.probe		= qcom_usb_extcon_probe,
	.driver		= {
		.name	= "extcon-pm8941-misc",
		.pm	= &qcom_usb_extcon_pm_ops,
		.of_match_table = qcom_usb_extcon_dt_match,
	},
};
module_platform_driver(qcom_usb_extcon_driver);

MODULE_DESCRIPTION("QCOM USB ID extcon driver");
MODULE_LICENSE("GPL");
