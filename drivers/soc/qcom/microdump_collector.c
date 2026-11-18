// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>

#if IS_ENABLED(CONFIG_QCOM_RAMDUMP)
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <soc/qcom/qcom_ramdump.h>
#endif

#define SMEM_SSR_REASON_MSS0	421
#define SMEM_SSR_DATA_MSS0	611
#define SMEM_MODEM	1

/*
 * This program collects the data from SMEM regions whenever the modem crashes
 * and stores it in /dev/ramdump_microdump_modem so as to expose it to
 * user space.
 */
#if IS_ENABLED(CONFIG_QCOM_RAMDUMP)

struct microdump_data {
	struct device *microdump_dev;
	void *microdump_modem_notify_handler;
	struct notifier_block microdump_modem_ssr_nb;
};

static int enable_microdump;
module_param(enable_microdump, int, 0644);

static int start_qcomdump;
module_param(start_qcomdump, int, 0644);

static struct microdump_data *drv;

static int microdump_crash_collection(void)
{
	int ret;
	size_t size_reason = 0, size_data = 0;
	void *crash_reason = NULL;
	void *crash_data = NULL;
	struct qcom_dump_segment segment[2];
	struct list_head head;

	INIT_LIST_HEAD(&head);
	memset(segment, 0, sizeof(segment));

	crash_data = qcom_smem_get(SMEM_MODEM
				, SMEM_SSR_DATA_MSS0, &size_data);

	if (IS_ERR_OR_NULL(crash_data)) {
		pr_err("%s: smem %d not available\n",
				__func__, SMEM_SSR_DATA_MSS0);
		goto out;
	}

	segment[0].va = crash_data;
	segment[0].size = size_data;
	list_add(&segment[0].node, &head);

	crash_reason = qcom_smem_get(QCOM_SMEM_HOST_ANY
				, SMEM_SSR_REASON_MSS0, &size_reason);

	if (!crash_reason) {
		pr_err("%s: smem %d is null\n",
				__func__, SMEM_SSR_REASON_MSS0);

		goto out;
	}

	if (IS_ERR(crash_reason)) {
		pr_err("%s: smem %d not available because of %ld\n",
				__func__, SMEM_SSR_REASON_MSS0, PTR_ERR(crash_reason));

		goto out;
	}

	segment[1].va = crash_reason;
	segment[1].size = size_reason;
	list_add(&segment[1].node, &head);

	if (!enable_microdump) {
		pr_err("%s: enable_microdump is false\n", __func__);
		goto out;
	}

	start_qcomdump = 1;
	ret = qcom_microdump(&head, drv->microdump_dev);
	if (ret)
		pr_err("%s: qcom_microdump() failed\n", __func__);

	start_qcomdump = 0;
out:
	return NOTIFY_OK;
}

static int microdump_modem_ssr_notifier_nb(struct notifier_block *nb,
		unsigned long code, void *data)
{
	struct qcom_ssr_notify_data *notify_data = data;

	if (code == QCOM_SSR_BEFORE_SHUTDOWN && notify_data->crashed)
		return microdump_crash_collection();
	else
		return NOTIFY_OK;
}

/* to detect crash when SSR is enabled */
static int microdump_modem_ssr_register_notifier(struct microdump_data *drv)
{
	int ret = 0;

	drv->microdump_modem_ssr_nb.notifier_call = microdump_modem_ssr_notifier_nb;

	drv->microdump_modem_notify_handler =
		qcom_register_ssr_notifier("mpss",
			&drv->microdump_modem_ssr_nb);

	if (IS_ERR(drv->microdump_modem_notify_handler)) {
		pr_err("Modem register notifier failed: %ld\n",
			PTR_ERR(drv->microdump_modem_notify_handler));
		ret = -EINVAL;
	}

	return ret;
}

static void microdump_modem_ssr_unregister_notifier(struct microdump_data *drv)
{
	qcom_unregister_ssr_notifier(drv->microdump_modem_notify_handler,
		&drv->microdump_modem_ssr_nb);

	drv->microdump_modem_notify_handler = NULL;
}

static void __exit microdump_exit(void)
{
	microdump_modem_ssr_unregister_notifier(drv);

	kfree(drv);
}


static int microdump_probe(struct platform_device *pdev)
{
	int ret = -ENOMEM;

	drv = kzalloc(sizeof(struct microdump_data), GFP_KERNEL);
	if (!drv)
		goto out;

	drv->microdump_dev = &pdev->dev;
	if (!drv->microdump_dev) {
		pr_err("%s: Unable to create a microdump_modem ramdump device\n"
			, __func__);
		ret = -ENODEV;
		goto out_kfree;
	}

	ret = microdump_modem_ssr_register_notifier(drv);
	if (ret) {
		pr_err("%s: microdump_modem_ssr_register_notifier failed\n", __func__);
		goto out_kfree;
	}

	return ret;

out_kfree:
	pr_err("%s: Failed to register microdump collector\n", __func__);
	kfree(drv);
	drv = NULL;
out:
	return ret;
}

static const struct of_device_id microdump_match_table[] = {
	{.compatible = "qcom,microdump-modem",},
	{}
};

static struct platform_driver microdump_driver = {
	.probe = microdump_probe,
	.driver = {
		.name = "msm_microdump_modem",
		.of_match_table = microdump_match_table,
	},
};

static int __init microdump_init(void)
{
	int ret;

	ret = platform_driver_register(&microdump_driver);

	if (ret)
		pr_err("%s: register failed %d\n", __func__, ret);

	return ret;
}

module_init(microdump_init)
module_exit(microdump_exit);
#endif

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Microdump Collector");
MODULE_LICENSE("GPL");
