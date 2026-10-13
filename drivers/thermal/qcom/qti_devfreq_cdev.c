// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/devfreq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#define DEVFREQ_CDEV_NAME "gpu"
#define DEVFREQ_CDEV "qcom-devfreq-cdev"

struct devfreq_cdev_device {
	struct device_node *gpu_np;
	struct device *dev;
	struct device *gpu_dev;
	unsigned long *freq_table;
	int cur_state;
	int max_state;
	int retry_cnt;
	struct dev_pm_qos_request qos_max_freq_req;
	struct thermal_cooling_device *cdev;
};

static int devfreq_cdev_set_state(struct thermal_cooling_device *cdev,
					unsigned long state)
{
	struct devfreq_cdev_device *cdev_data = cdev->devdata;
	int ret = 0;
	unsigned long freq;

	if (state > cdev_data->max_state)
		return -EINVAL;
	if (state == cdev_data->cur_state)
		return 0;
	freq = cdev_data->freq_table[state];
	dev_dbg(cdev_data->dev, "cdev:%s Limit:%lu\n", cdev->type, freq);
	ret = dev_pm_qos_update_request(&cdev_data->qos_max_freq_req, freq);
	if (ret < 0) {
		dev_err(cdev_data->dev,
			"Error placing qos request:%lu. cdev:%s err:%d\n",
				freq, cdev->type, ret);
		return ret;
	}
	cdev_data->cur_state = state;

	return 0;
}

static int devfreq_cdev_get_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct devfreq_cdev_device *cdev_data = cdev->devdata;

	*state = cdev_data->cur_state;
	return 0;
}

static int devfreq_cdev_get_max_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct devfreq_cdev_device *cdev_data = cdev->devdata;

	*state = cdev_data->max_state;
	return 0;
}

static struct thermal_cooling_device_ops devfreq_cdev_ops = {
	.set_cur_state = devfreq_cdev_set_state,
	.get_cur_state = devfreq_cdev_get_state,
	.get_max_state = devfreq_cdev_get_max_state,
};

static int devfreq_cdev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned long freq = ULONG_MAX;
	unsigned long *freq_table;
	struct dev_pm_opp *opp;
	int ret = 0, freq_ct, i;
	struct devfreq_cdev_device *devfreq_cdev;
	struct platform_device *gpu_pdev;
	struct device_link *link;

	devfreq_cdev = devm_kzalloc(dev, sizeof(*devfreq_cdev), GFP_KERNEL);
	if (!devfreq_cdev)
		return -ENOMEM;

	devfreq_cdev->dev = dev;
	devfreq_cdev->gpu_np = of_parse_phandle(pdev->dev.of_node,
				"qcom,devfreq", 0);

	if (!devfreq_cdev->gpu_np)
		return PTR_ERR(devfreq_cdev->gpu_np);

	gpu_pdev = of_find_device_by_node(devfreq_cdev->gpu_np);
	if (!gpu_pdev) {
		dev_dbg(devfreq_cdev->dev, "Cannot find device node %s\n",
			devfreq_cdev->gpu_np->name);
		of_node_put(devfreq_cdev->gpu_np);
		return -ENODEV;
	}

	link = device_link_add(devfreq_cdev->dev, &gpu_pdev->dev,
		DL_FLAG_AUTOPROBE_CONSUMER);

	put_device(&gpu_pdev->dev);
	of_node_put(devfreq_cdev->gpu_np);

	if (!link) {
		dev_err(devfreq_cdev->dev, "add gpu device_link fail\n");
		return -ENODEV;
	}
	if (link->status == DL_STATE_DORMANT) {
		dev_dbg(devfreq_cdev->dev, "kgsl not probed yet:%d\n", ret);
		device_link_del(link);
		return -EPROBE_DEFER;
	}

	device_link_del(link);

	devfreq_cdev->gpu_dev = &gpu_pdev->dev;
	freq_ct = dev_pm_opp_get_opp_count(devfreq_cdev->gpu_dev);

	if (freq_ct <= 0) {
		dev_err(devfreq_cdev->dev, "kgsl freq table count is %d\n", freq_ct);
		return -EINVAL;
	}

	freq_table = devm_kcalloc(devfreq_cdev->dev, freq_ct,
					sizeof(*freq_table), GFP_KERNEL);
	if (!freq_table)
		return -ENOMEM;

	for (i = 0, freq = ULONG_MAX; i < freq_ct; i++, freq--) {

		opp = dev_pm_opp_find_freq_floor(devfreq_cdev->gpu_dev, &freq);
		if (IS_ERR(opp)) {
			ret = PTR_ERR(opp);
			return ret;
		}
		dev_pm_opp_put(opp);

		freq_table[i] = DIV_ROUND_UP(freq, 1000); //hz to khz
		dev_dbg(devfreq_cdev->dev, "%d. freq table:%lu\n", i,
						freq_table[i]);
	}
	devfreq_cdev->max_state = freq_ct-1;
	devfreq_cdev->freq_table = freq_table;
	ret = dev_pm_qos_add_request(devfreq_cdev->gpu_dev,
					&devfreq_cdev->qos_max_freq_req,
					DEV_PM_QOS_MAX_FREQUENCY,
					PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
	if (ret < 0)
		goto qos_exit;
	devfreq_cdev->cdev = thermal_cooling_device_register(DEVFREQ_CDEV_NAME,
					devfreq_cdev, &devfreq_cdev_ops);
	if (IS_ERR(devfreq_cdev->cdev)) {
		ret = PTR_ERR(devfreq_cdev->cdev);
		dev_err(devfreq_cdev->dev,
			"Cdev register failed for gpu, ret:%d\n", ret);
		devfreq_cdev->cdev = NULL;
		goto qos_exit;
	}

	platform_set_drvdata(pdev, devfreq_cdev);

	return 0;

qos_exit:
	dev_pm_qos_remove_request(&devfreq_cdev->qos_max_freq_req);

	return ret;
}

static void devfreq_cdev_remove(struct platform_device *pdev)
{
	struct devfreq_cdev_device *devfreq_cdev = platform_get_drvdata(pdev);

	if (devfreq_cdev->cdev) {
		thermal_cooling_device_unregister(devfreq_cdev->cdev);
		dev_pm_qos_remove_request(&devfreq_cdev->qos_max_freq_req);
	}
}

static const struct of_device_id devfreq_cdev_match[] = {
	{.compatible = "qcom,devfreq-cdev"},
	{}
};

static struct platform_driver devfreq_cdev_driver = {
	.probe          = devfreq_cdev_probe,
	.remove         = devfreq_cdev_remove,
	.driver         = {
		.name   = DEVFREQ_CDEV,
		.of_match_table = devfreq_cdev_match,
	},
};

static int __init devfreq_cdev_init(void)
{
	return platform_driver_register(&devfreq_cdev_driver);
}
module_init(devfreq_cdev_init);

static void __exit devfreq_cdev_exit(void)
{
	platform_driver_unregister(&devfreq_cdev_driver);
}
module_exit(devfreq_cdev_exit);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. devfreq cooling device driver");
MODULE_LICENSE("GPL");
