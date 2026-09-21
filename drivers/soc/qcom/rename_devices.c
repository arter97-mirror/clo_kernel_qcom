// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/blkdev.h>

#define PATH_SIZE	64
#define BLKDEV_MAJOR	254
#define PART_BITS	4

static int index_to_minor(int index)
{
	return index << PART_BITS;
}

int find_major_for_blk_dev(void)
{
	struct file *bdev_file;
	struct block_device *bdev;
	dev_t devt;
	int major_num;

	for (major_num = BLKDEV_MAJOR; major_num > 0; major_num--) {
		devt = MKDEV(major_num, 0);
		bdev_file = bdev_file_open_by_dev(devt, BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);

		if (IS_ERR(bdev_file))
			continue;

		bdev = file_bdev(bdev_file);

		if (!bdev) {
			bdev_fput(bdev_file);
			continue;
		}

		/* TO DO: Replace strncmp with regex */
		if (!strncmp(dev_name(&bdev->bd_device), "vda", 3)) {
			bdev_fput(bdev_file);
			return major_num;
		}

		bdev_fput(bdev_file);
	}
	return -ENODEV;
}

static void rename_blk_device_name(struct device_node *np)
{
	struct file *bdev_file;
	struct block_device *bdev;
	dev_t devt;
	struct device_node *node = np;
	char *modified_name;
	int major;
	int index = 0;

	major = find_major_for_blk_dev();
	if (major == -ENODEV) {
		pr_err("rename-devices: major not found!\n");
		return;
	}

	pr_info("rename-devices: major found = %d\n", major);

	while (!of_property_read_string_index(node, "rename-dev", index,
						(const char **)&modified_name)) {
		devt = MKDEV(major, index_to_minor(index));

		bdev_file = bdev_file_open_by_dev(devt, BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
		if (IS_ERR(bdev_file)) {
			pr_err("rename-devices: renaming to %s failed in bdev_file_open_by_dev\n",
										modified_name);
			return;
		}

		bdev = file_bdev(bdev_file);
		if (!bdev) {
			pr_err("rename-devices: renaming to %s failed in file_bdev\n",
										 modified_name);
			bdev_fput(bdev_file);
			return;
		}

		pr_info("rename-devices: device %s getting renamed to %s\n",
							dev_name(&bdev->bd_device), modified_name);
		device_rename(&bdev->bd_device, modified_name);
		bdev_fput(bdev_file);
		index++;
	}
}

static int __init rename_devices_init(void)
{
	struct device_node *node = NULL, *child = NULL;
	const char *device_type;

	node = of_find_compatible_node(NULL, NULL, "qcom,rename-devices");
	if (!node) {
		pr_err("rename-devices: qcom,rename-devices node is missing\n");
		goto out;
	}

	for_each_child_of_node(node, child) {
		if (!of_property_read_string(child, "device-type", &device_type)) {
			if (strncmp(device_type, "block", 5) == 0)
				rename_blk_device_name(child);
			else
				pr_err("rename-devices: unsupported device\n");
		} else
			pr_err("rename-devices: device-type is missing\n");
	}

out:
	of_node_put(node);
	return  0;
}

static void __exit rename_devices_exit(void)
{
	pr_err("rename-devices: exit\n");
}
#if IS_MODULE(CONFIG_RENAME_DEVICES)
module_init(rename_devices_init);
#else
late_initcall(rename_devices_init);
#endif
module_exit(rename_devices_exit);

MODULE_DESCRIPTION("Rename devices");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: virtio_blk");
MODULE_SOFTDEP("pre: virtio_mmio");
