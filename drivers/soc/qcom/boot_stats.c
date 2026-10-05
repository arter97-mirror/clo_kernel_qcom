// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2019, 2021 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

struct boot_stats {
	uint32_t bootloader_start;
	uint32_t bootloader_end;
	uint32_t bootloader_load_boot_start;
	uint32_t bootloader_load_boot_end;
	uint32_t bootloader_load_vendor_boot_start;
	uint32_t bootloader_load_vendor_boot_end;
	uint32_t bootloader_load_init_boot_start;
	uint32_t bootloader_load_init_boot_end;
} __packed;

static void __iomem *mpm_counter_base;
static uint32_t mpm_counter_freq;
static struct boot_stats __iomem *boot_stats;

static int mpm_parse_dt(void)
{
	struct device_node *np_imem, *np_mpm2;

	np_imem = of_find_compatible_node(NULL, NULL,
				"qcom,msm-imem-boot_stats");
	if (!np_imem) {
		pr_err("can't find qcom,msm-imem node\n");
		return -ENODEV;
	}

	boot_stats = of_iomap(np_imem, 0);
	if (!boot_stats) {
		pr_err("boot_stats: Can't map imem\n");
		goto err1;
	}

	np_mpm2 = of_find_compatible_node(NULL, NULL,
				"qcom,mpm2-sleep-counter");
	if (!np_mpm2) {
		pr_err("mpm_counter: can't find DT node\n");
		goto err_unmap_boot_stats;
	}

	if (of_property_read_u32(np_mpm2, "clock-frequency",
				 &mpm_counter_freq)) {
		pr_err("mpm_counter: can't read clock-frequency\n");
		goto err2;
	}

	if (!mpm_counter_freq) {
		pr_err("mpm_counter: clock-frequency must be non-zero\n");
		goto err2;
	}

	if (of_get_address(np_mpm2, 0, NULL, NULL)) {
		mpm_counter_base = of_iomap(np_mpm2, 0);
		if (!mpm_counter_base) {
			pr_err("mpm_counter: can't map counter base\n");
			goto err2;
		}
	} else {
		goto err2;
	}

	of_node_put(np_mpm2);
	of_node_put(np_imem);

	return 0;

err2:
	of_node_put(np_mpm2);

err_unmap_boot_stats:
	iounmap(boot_stats);
	boot_stats = NULL;

err1:
	of_node_put(np_imem);

	return -ENODEV;
}

static u64 ticks_to_ms(u32 ticks)
{
	return DIV_ROUND_UP_ULL((u64)ticks * MSEC_PER_SEC,
				mpm_counter_freq);
}

static void print_boot_stats(void)
{
	pr_info("KPI: Bootloader start count = %u\n",
		readl_relaxed(&boot_stats->bootloader_start));
	pr_info("KPI: Bootloader end count = %u\n",
		readl_relaxed(&boot_stats->bootloader_end));
	pr_info("KPI: Bootloader load kernel count = %u\n",
		readl_relaxed(&boot_stats->bootloader_load_boot_end) -
		readl_relaxed(&boot_stats->bootloader_load_boot_start));
	pr_info("KPI: Kernel MPM timestamp = %u\n",
		readl_relaxed(mpm_counter_base));
	pr_info("KPI: Kernel MPM Clock frequency = %u\n",
		mpm_counter_freq);
}

/*
 * Exposes all currently printed boot statistics through one read-only
 * procfs entry:
 *
 *     /proc/boot_stats
 *     where
 *     2BLL is ABL load time
 *     2BLE is ABL execution time
 *     SW is Kernel Entry time
 */
static int boot_stats_proc_show(struct seq_file *m, void *v)
{
	u32 bootloader_start;
	u32 bootloader_end;
	u32 bootloader_execution;

	bootloader_start =
		readl_relaxed(&boot_stats->bootloader_start);

	bootloader_end =
		readl_relaxed(&boot_stats->bootloader_end);

	bootloader_execution =
		bootloader_end - bootloader_start;

	seq_printf(m,
		  "2BLL: %llu ms\n"
		  "2BLE: %llu ms\n"
		  "SW: %llu ms\n",
		  (unsigned long long)
		  ticks_to_ms(bootloader_start),
		  (unsigned long long)
		  ticks_to_ms(bootloader_execution),
		  (unsigned long long)
		  ticks_to_ms(bootloader_end));

	return 0;
}

static struct proc_dir_entry *boot_stats_proc_entry;

static int __init boot_stats_init(void)
{
	int ret;

	ret = mpm_parse_dt();
	if (ret < 0)
		return ret;

	print_boot_stats();

	boot_stats_proc_entry = proc_create_single("boot_stats", 0444, NULL,
						    boot_stats_proc_show);
	if (!boot_stats_proc_entry) {
		pr_err("boot_stats: failed to create procfs entry\n");

		iounmap(mpm_counter_base);
		mpm_counter_base = NULL;

		iounmap(boot_stats);
		boot_stats = NULL;

		return -ENOMEM;
	}

	return 0;
}
module_init(boot_stats_init);

static void __exit boot_stats_exit(void)
{
	proc_remove(boot_stats_proc_entry);

	if (mpm_counter_base) {
		iounmap(mpm_counter_base);
		mpm_counter_base = NULL;
	}

	if (boot_stats) {
		iounmap(boot_stats);
		boot_stats = NULL;
	}
}
module_exit(boot_stats_exit);

MODULE_DESCRIPTION("MSM boot stats info driver");
MODULE_LICENSE("GPL");
