// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/*
 * wear configuration driver.
 * Exposes /sys/class/wear_config/ for the HAL to access kernel subsystems.
 */

#define pr_fmt(fmt) "qcom-wear-config: " fmt

#include <linux/device/class.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define DRIVER_NAME "qcom-wear-config"
#define CLASS_NAME  "wear_config"

struct wear_config_priv {
	struct nvmem_cell *orientation_cell;
};

static struct wear_config_priv *wear_config_dev;
static DEFINE_MUTEX(wear_config_lock);

static ssize_t wrist_orientation_show(const struct class *cls,
				      const struct class_attribute *attr,
				      char *buf)
{
	struct wear_config_priv *priv;
	u8 *val;
	size_t len;
	int ret;

	mutex_lock(&wear_config_lock);
	priv = wear_config_dev;
	if (!priv) {
		mutex_unlock(&wear_config_lock);
		return -ENODEV;
	}

	val = nvmem_cell_read(priv->orientation_cell, &len);
	mutex_unlock(&wear_config_lock);

	if (IS_ERR(val)) {
		pr_err("%s: failed to read nvmem cell, err=%ld\n",
			 __func__, PTR_ERR(val));
		return PTR_ERR(val);
	}

	/* ensure buffer has at least 1 byte before accessing val[0] */
	if (len < 1) {
		pr_err("%s: nvmem cell returned empty buffer (len=%zu)\n",
		       __func__, len);
		kfree(val);
		return -ENODATA;
	}

	ret = sysfs_emit(buf, "%u\n", val[0]);
	kfree(val);

	return ret;
}

static ssize_t wrist_orientation_store(const struct class *cls,
				const struct class_attribute *attr,
				const char *buf, size_t count)
{
	struct wear_config_priv *priv;
	u8 val;
	int ret;

	ret = kstrtou8(buf, 10, &val);
	if (ret) {
		pr_err("%s: invalid value, ret=%d\n", __func__, ret);
		return ret;
	}


	mutex_lock(&wear_config_lock);
	priv = wear_config_dev;
	if (!priv) {
		mutex_unlock(&wear_config_lock);
		return -ENODEV;
	}

	ret = nvmem_cell_write(priv->orientation_cell, &val, sizeof(val));
	mutex_unlock(&wear_config_lock);

	if (ret < 0) {
		pr_err("%s: failed to write nvmem cell, ret=%d\n", __func__, ret);
		return ret;
	}

	if (ret != sizeof(val)) {
		pr_err("%s: partial write, expected=%zu written=%d\n",
			__func__, sizeof(val), ret);
		return -EIO;
	}

	pr_debug("%s: wrote orientation=%u\n", __func__, val);

	return count;
}
static CLASS_ATTR_RW(wrist_orientation);

static struct attribute *wear_config_attrs[] = {
	&class_attr_wrist_orientation.attr,
	NULL,
};
ATTRIBUTE_GROUPS(wear_config);


static struct class wear_config_class = {
	.name         = CLASS_NAME,
	.class_groups = wear_config_groups,
};

static int wear_config_probe(struct platform_device *pdev)
{
	struct wear_config_priv *priv;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->orientation_cell = devm_nvmem_cell_get(&pdev->dev, "logo-orientation");
	if (IS_ERR(priv->orientation_cell)) {
		int ret = PTR_ERR(priv->orientation_cell);

		if (ret == -EPROBE_DEFER)
			dev_dbg(&pdev->dev, "nvmem not ready, deferring probe\n");
		else
			dev_err(&pdev->dev, "failed to get nvmem cell: %d\n", ret);
		return ret;
	}

	mutex_lock(&wear_config_lock);
	wear_config_dev = priv;
	mutex_unlock(&wear_config_lock);

	platform_set_drvdata(pdev, priv);

	return 0;
}

static int wear_config_remove(struct platform_device *pdev)
{
	mutex_lock(&wear_config_lock);
	wear_config_dev = NULL;
	mutex_unlock(&wear_config_lock);

	return 0;
}

static const struct of_device_id wear_config_of_match[] = {
	{ .compatible = "qcom,wear-config" },
	{ },
};
MODULE_DEVICE_TABLE(of, wear_config_of_match);

static struct platform_driver wear_config_driver = {
	.driver = {
		.name           = DRIVER_NAME,
		.of_match_table = wear_config_of_match,
	},
	.probe  = wear_config_probe,
	.remove = wear_config_remove,
};

static int __init wear_config_init(void)
{
	int ret;

	ret = class_register(&wear_config_class);
	if (ret) {
		pr_err("%s: failed to register class, ret=%d\n", __func__, ret);
		return ret;
	}

	ret = platform_driver_register(&wear_config_driver);
	if (ret) {
		pr_err("%s: failed to register platform driver, ret=%d\n", __func__, ret);
		class_unregister(&wear_config_class);
		return ret;
	}

	return 0;
}

static void __exit wear_config_exit(void)
{
	platform_driver_unregister(&wear_config_driver);
	class_unregister(&wear_config_class);
}

module_init(wear_config_init);
module_exit(wear_config_exit);
MODULE_DESCRIPTION("Qualcomm wear configuration driver");
MODULE_LICENSE("GPL");
