// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/debugfs.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

#include "gdsc.h"
#include "gdsc-debug.h"

#define HW_CONTROL_MASK		BIT(1)

static int gdsc_genpd_hwctrl_get(void *data, u64 *val)
{
	struct gdsc_debug *gdsc_debug = data;
	struct gdsc *sc = gdsc_debug->sc;
	u32 regval;
	int ret;

	if (sc->rsupply) {
		ret = regulator_enable(sc->rsupply);
		if (ret)
			return ret;
	}

	regmap_read(sc->regmap, sc->gdscr, &regval);
	*val = !!(regval & HW_CONTROL_MASK);

	if (sc->rsupply)
		return regulator_disable(sc->rsupply);

	return 0;
}

static int gdsc_genpd_hwctrl_set(void *data, u64 val)
{
	struct gdsc_debug *gdsc_debug = data;
	struct gdsc *sc = gdsc_debug->sc;
	u32 mask = val ? HW_CONTROL_MASK : 0;
	int ret;

	if (sc->rsupply) {
		ret = regulator_enable(sc->rsupply);
		if (ret)
			return ret;
	}

	regmap_update_bits(sc->regmap, sc->gdscr, HW_CONTROL_MASK, mask);
	sc->hw_ctrl_mode = !!val;

	if (sc->rsupply)
		return regulator_disable(sc->rsupply);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(gdsc_genpd_debug_hwctrl_fops, gdsc_genpd_hwctrl_get,
					gdsc_genpd_hwctrl_set, "%llu\n");

static int gdsc_genpd_enable_get(void *data, u64 *val)
{
	struct gdsc_debug *gdsc_debug = data;

	if (!gdsc_debug->dev)
		*val = -ENODEV;
	else
		*val = pm_runtime_active(gdsc_debug->dev);

	return 0;
}

static void gdsc_debug_dev_release(struct device *dev)
{
	kfree(dev);
}

static int gdsc_genpd_enable_set(void *data, u64 val)
{
	struct gdsc_debug *gdsc_debug = data;
	struct gdsc *sc = gdsc_debug->sc;
	int ret;

	if (!gdsc_debug->dev) {
		/*
		 * Create debug consumer device and add it to power domain only
		 * when there is a first request to enable GDSC from debugfs
		 */
		gdsc_debug->dev = kzalloc(sizeof(*gdsc_debug->dev), GFP_KERNEL);
		if (!gdsc_debug->dev)
			return -ENOMEM;

		dev_set_name(gdsc_debug->dev, "%s:debug", sc->pd.name);
		/*
		 * A release callback must be registered for the device to prevent warnings
		 * about the missing release() function when device_unregister() is called
		 * in the debug unregister path.
		 */
		gdsc_debug->dev->release = gdsc_debug_dev_release;

		ret = device_register(gdsc_debug->dev);
		if (ret) {
			put_device(gdsc_debug->dev);
			kfree(gdsc_debug->dev);
			gdsc_debug->dev = NULL;
			return ret;
		}

		ret = pm_genpd_add_device(&sc->pd, gdsc_debug->dev);
		if (ret) {
			device_unregister(gdsc_debug->dev);
			kfree(gdsc_debug->dev);
			gdsc_debug->dev = NULL;
			return ret;
		}

		pm_runtime_enable(gdsc_debug->dev);
	}

	if (val)
		return pm_runtime_resume_and_get(gdsc_debug->dev);

	/* Return if GDSC is already suspended from debugfs */
	if (pm_runtime_suspended(gdsc_debug->dev))
		return 0;

	return pm_runtime_put_sync(gdsc_debug->dev);
}
DEFINE_DEBUGFS_ATTRIBUTE(gdsc_genpd_debug_enable_fops, gdsc_genpd_enable_get,
					gdsc_genpd_enable_set, "%lld\n");

static int print_hw_show(struct seq_file *m, void *unused)
{
	struct gdsc *sc = m->private;
	int ret;

	if (sc->rsupply) {
		ret = regulator_enable(sc->rsupply);
		if (ret)
			return ret;
	}

	gdsc_genpd_print_regs(m, sc);

	if (sc->rsupply)
		return regulator_disable(sc->rsupply);

	return 0;
}

static int print_hw_open(struct inode *inode, struct file *file)
{
	return single_open(file, print_hw_show, inode->i_private);
}

static const struct file_operations gdsc_print_hw_fops = {
	.open		= print_hw_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int gdsc_genpd_debug_create_one(struct gdsc_debug *gdsc_debug,
						struct dentry *pdentry)
{
	struct gdsc *sc;
	struct dentry *root;
	struct dentry *tmp;

	if (!gdsc_debug || !gdsc_debug->sc || !pdentry)
		return -EINVAL;

	sc = gdsc_debug->sc;

	root = debugfs_lookup(sc->pd.name, pdentry);
	if (IS_ERR_OR_NULL(root)) {
		pr_err("Unable to find %s debugfs directory\n", sc->pd.name);
		return -ENOENT;
	}

	tmp = debugfs_create_file("enable", 0644, root, gdsc_debug,
				&gdsc_genpd_debug_enable_fops);
	if (IS_ERR_OR_NULL(tmp)) {
		pr_err("Failed to create enable debugfs node for %s\n",
			sc->pd.name);
		return -ENOENT;
	}

	tmp = debugfs_create_file("gdsc_print_regs", 0444, root, sc,
			    &gdsc_print_hw_fops);
	if (IS_ERR_OR_NULL(tmp)) {
		pr_err("Failed to create print_regs debugfs node for %s\n",
			sc->pd.name);
		return -ENOENT;
	}

	if (sc->flags & (HW_CTRL | HW_CTRL_TRIGGER)) {
		tmp = debugfs_create_file("hwctrl", 0644, root, gdsc_debug,
				&gdsc_genpd_debug_hwctrl_fops);
		if (IS_ERR_OR_NULL(tmp)) {
			pr_err("Failed to create hwctrl debugfs node for %s\n",
				sc->pd.name);
			return -ENOENT;
		}
	}

	return 0;
}

int gdsc_genpd_debug_register(struct gdsc *sc)
{
	struct gdsc_debug *gdsc_debug;
	int ret = 0;

	gdsc_debug = kzalloc(sizeof(*gdsc_debug), GFP_KERNEL);
	if (!gdsc_debug)
		return -ENOMEM;

	gdsc_debug->sc = sc;

	mutex_lock(&gdsc_genpd_debug_lock);

	list_add(&gdsc_debug->list, &gdsc_genpd_debug_list);
	if (genpd_rootdir)
		ret = gdsc_genpd_debug_create_one(gdsc_debug, genpd_rootdir);

	mutex_unlock(&gdsc_genpd_debug_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(gdsc_genpd_debug_register);

void gdsc_genpd_debug_unregister(struct gdsc *sc)
{
	struct gdsc_debug *gdsc_debug, *tmp;

	mutex_lock(&gdsc_genpd_debug_lock);
	list_for_each_entry_safe(gdsc_debug, tmp, &gdsc_genpd_debug_list, list) {
		if (gdsc_debug->sc == sc) {
			if (gdsc_debug->dev) {
				pm_runtime_disable(gdsc_debug->dev);
				pm_genpd_remove_device(gdsc_debug->dev);
				device_unregister(gdsc_debug->dev);
				gdsc_debug->dev = NULL;
			}

			gdsc_debug->sc = NULL;
			list_del(&gdsc_debug->list);
			kfree(gdsc_debug);
		}
	}
	mutex_unlock(&gdsc_genpd_debug_lock);
}
EXPORT_SYMBOL_GPL(gdsc_genpd_debug_unregister);

/*
 * gdsc_genpd_debug_init()'s late_initcall_sync can execute before
 * the pm_genpd debugfs directory is created by genpd_debug_init()'s
 * late_initcall, resulting in a race where the debugfs lookup is
 * performed before the directory exists and consequently fails.
 * Use a scheduled delayed work to retry the lookup, rescheduling itself
 * with another 1 second delay if the directory still isn't there
 * rather than giving up.
 */
static void gdsc_debug_init_work_function(struct work_struct *work);
static DECLARE_DELAYED_WORK(gdsc_debug_init_work, gdsc_debug_init_work_function);

static void gdsc_debug_lookup_and_populate(void)
{
	struct gdsc_debug *gdsc_debug;

	mutex_lock(&gdsc_genpd_debug_lock);

	genpd_rootdir = debugfs_lookup("pm_genpd", NULL);
	if (!genpd_rootdir) {
		pr_err("Unable to find pm_genpd debugfs directory\n");
		mutex_unlock(&gdsc_genpd_debug_lock);
		schedule_delayed_work(&gdsc_debug_init_work, msecs_to_jiffies(1000));
		return;
	}

	list_for_each_entry(gdsc_debug, &gdsc_genpd_debug_list, list)
		gdsc_genpd_debug_create_one(gdsc_debug, genpd_rootdir);

	mutex_unlock(&gdsc_genpd_debug_lock);
}

static void gdsc_debug_init_work_function(struct work_struct *work)
{
	gdsc_debug_lookup_and_populate();
}

static int __init gdsc_genpd_debug_init(void)
{
	gdsc_debug_lookup_and_populate();

	return 0;
}
late_initcall_sync(gdsc_genpd_debug_init);
