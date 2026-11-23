// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/panic_notifier.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <linux/firmware/qcom/qcom_scm.h>

static void __iomem *msm_ps_hold;
static bool panic_in_progress;

static int msm_poweroff_panic(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	panic_in_progress = true;
	return NOTIFY_OK;
}

static struct notifier_block msm_poweroff_panic_nb = {
	.notifier_call = msm_poweroff_panic,
};

static int do_msm_poweroff(struct sys_off_data *data)
{
	/*
	 * emergency_restart() (SysRq 'b') skips reboot_notifier_list, so
	 * qcom_dload_reboot() never clears download mode — firmware retains
	 * FULLDUMP and enters dump mode instead of rebooting.  Set NODUMP here
	 * for non-panic, non-EDL restarts; preserve the EDL mode selected by
	 * qcom_dload_reboot().  qcom_scm_disable_sdi() and
	 * qcom_scm_set_download_mode() use the atomic SCM path (no
	 * qcom_scm_lock), safe after smp_send_stop().
	 */
	if (!panic_in_progress) {
		qcom_scm_disable_sdi();
		if (!data->cmd || strcmp(data->cmd, "edl"))
			qcom_scm_set_download_mode(QCOM_DOWNLOAD_NODUMP);
	}

	qcom_scm_deassert_ps_hold();
	writel(0, msm_ps_hold);
	mdelay(10000);

	return NOTIFY_DONE;
}

static int msm_restart_probe(struct platform_device *pdev)
{
	msm_ps_hold = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(msm_ps_hold))
		return PTR_ERR(msm_ps_hold);

	atomic_notifier_chain_register(&panic_notifier_list,
				       &msm_poweroff_panic_nb);

	devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_RESTART,
				      200, do_msm_poweroff, NULL);

	devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF,
				      SYS_OFF_PRIO_DEFAULT, do_msm_poweroff,
				      NULL);

	return 0;
}

static void msm_restart_remove(struct platform_device *pdev)
{
	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &msm_poweroff_panic_nb);
}

static const struct of_device_id of_msm_restart_match[] = {
	{ .compatible = "qcom,pshold", },
	{},
};
MODULE_DEVICE_TABLE(of, of_msm_restart_match);

static struct platform_driver msm_restart_driver = {
	.probe = msm_restart_probe,
	.remove = msm_restart_remove,
	.driver = {
		.name = "msm-restart",
		.of_match_table = of_match_ptr(of_msm_restart_match),
	},
};

static int __init msm_restart_init(void)
{
	return platform_driver_register(&msm_restart_driver);
}
module_init(msm_restart_init);

static __exit void msm_restart_exit(void)
{
	platform_driver_unregister(&msm_restart_driver);
}
module_exit(msm_restart_exit);

MODULE_DESCRIPTION("MSM Poweroff Driver");
MODULE_LICENSE("GPL");
