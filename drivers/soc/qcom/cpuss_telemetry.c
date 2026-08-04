// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/qcom_scmi_vendor.h>
#include <linux/scmi_protocol.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <soc/qcom/cpuss_telemetry.h>

#define CPUSS_TELEMETRY_DIR_STRING "cpuss_telemetry"
#define SCMI_TELEMETRY_ALGO_STR  (0x454C454D45545259)

#define SOC_CENTRIC	(0)
#define CLUSTER_CENTRIC	(1)
#define CORE_CENTRIC	(2)

static struct scmi_protocol_handle *ph_telemetry;
static const struct qcom_scmi_vendor_ops *vendor_ops;
static struct telemetry_global_param_t *telemetry;
static const struct telemetry_counter_attributes_name_t *pname;
static struct telemetry_counter_attributes_value_t *pvalue;
static struct dentry *dir;
static struct dentry **cluster_dir;
static struct dentry **core_dir;
static u64 *fid;

/*
 * Unique counter ids are assigned to each telemetry counters,
 * where the id is relative to counter's position in the shared memory
 * for 'name' and 'value' arrays.
 * The first counter has an id of 0, the second 1, and so on.
 * Counters are located in a contigeous memory locations.
 */
const char *get_telemetry_counter_name(u32 counter_id)
{
	if (!pname)
		return NULL;

	return pname[counter_id].name;
}
EXPORT_SYMBOL_GPL(get_telemetry_counter_name);

int64_t get_telemetry_counter_value(u32 counter_id)
{
	int64_t stat;

	if (!telemetry || !pvalue)
		return -ENODEV;

	while (1) {
		int64_t start_value;

		start_value = telemetry->match_sequence;
		if (telemetry->match_sequence % 2 == 0) {
			stat = pvalue[counter_id].value;
			if (telemetry->match_sequence == start_value)
				break;
		}
	}

	return stat;
}
EXPORT_SYMBOL_GPL(get_telemetry_counter_value);

/*
 * Agent/HLOS performing a read on the shared memory must wait until FW/Platform
 * has completed updating counters. Read by agent must not happen on invalid or
 * stale values.
 */
static int generic_get(void *data, u64 *val)
{
	while (1) {
		int64_t start_value;

		start_value = telemetry->match_sequence;
		if (telemetry->match_sequence%2 == 0) {
			*val = pvalue[*(u32 *)data].value;
			if (telemetry->match_sequence == start_value)
				break;
		}
	}

	return 0;
}

static int set_req_update(void *data, u64 val)
{
	int scmi_status = 0;

	if (val < 1) {
		pr_err("scmi request update value is not valid, value must be > 0\n");
		return -EINVAL;
	}

	scmi_status = vendor_ops->set_param(ph_telemetry, &val, SCMI_TELEMETRY_ALGO_STR,
				SET_PARAM_REQUEST_UPDATE, sizeof(val));

	return scmi_status;
}

static int counter_info_file_show(struct seq_file *s, void *unused)
{
	u16 i;

	seq_puts(s, "'ent' stands for Entry counter\n");
	seq_puts(s, "'tnr' stands for Tenure counter\n");
	seq_puts(s, "_0 for soc centric counters\n");
	seq_puts(s, "_1 for cluster centric counters\n");
	seq_puts(s, "_2 for core centriC counter\n");
	seq_puts(s, "'a' in _1a, represents cluster_id\n");
	seq_puts(s, "'b' in _2b represent core_id\n");
	seq_puts(s, "e.g. name c2ent_2n implies c2 entry for core n\n");
	seq_printf(s, "\n\n%-15s %-15s\n", "Name", "ID");

	for (i = 0; i < telemetry->num_max_counters ; i++) {
		if (pvalue[i].value != -1)
			seq_printf(s, "%-15s %-15u\n", pname[i].name, i);
	}

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(telemetry_stat_request_update, NULL, set_req_update, "%llu\n");
DEFINE_SHOW_ATTRIBUTE(counter_info_file);
DEFINE_DEBUGFS_ATTRIBUTE(telemetry_stats_fops, generic_get, NULL, "%llu\n");

static void telemetry_memory_deallocation(void)
{
	if (telemetry) {
		iounmap(telemetry);
		telemetry = NULL;
	}

	if (pname) {
		iounmap((void *)pname);
		pname = NULL;
	}

	if (pvalue) {
		iounmap(pvalue);
		pvalue = NULL;
	}
}

static int cpuss_telemetry_create_fs_entries(struct scmi_device *sdev)
{
	struct dentry *ret;
	struct device *dev = &sdev->dev;
	char sub_dir_name[MAX_NAME_LENGTH];
	static struct dentry *soc_dir;
	char *delim[2] = {NULL};
	char *deliminated = NULL;
	char dup_name[MAX_NAME_LENGTH] = {'\0'};
	char *ptrname = NULL;
	u8 pos = 0;
	u8 hw_id = 0;
	u32 hwencd_val = 0;
	s32 temp = 0;
	u8 len = 0;

	fid = devm_kcalloc(dev, telemetry->num_max_counters, sizeof(u64), GFP_KERNEL);
	if (!fid)
		return -ENOMEM;

	dir = debugfs_create_dir(CPUSS_TELEMETRY_DIR_STRING, 0);
	if (!dir) {
		dev_err(dev, "Debugfs directory %s creation failed\n",
			CPUSS_TELEMETRY_DIR_STRING);
		return -ENOMEM;
	}

	cluster_dir = devm_kzalloc(dev, sizeof(struct dentry *) *
			telemetry->num_max_cluster, GFP_KERNEL);
	if (!cluster_dir)
		return -ENOMEM;

	for (u16 i = 0; i < telemetry->num_max_cluster ; i++) {
		snprintf(sub_dir_name, sizeof(sub_dir_name), "cluster_%d", i);
		cluster_dir[i] = debugfs_create_dir(sub_dir_name, dir);
		if (!cluster_dir[i]) {
			dev_err(dev, "Debugfs sub directory cluster %s creation failed\n",
					sub_dir_name);
			return -ENOMEM;
		}
	}

	core_dir = devm_kzalloc(dev, sizeof(struct dentry *) * telemetry->num_max_core, GFP_KERNEL);
	if (!core_dir)
		return -ENOMEM;

	for (u16 i = 0; i < telemetry->num_max_core; i++) {
		snprintf(sub_dir_name, sizeof(sub_dir_name), "core_%d", i);
		core_dir[i] = debugfs_create_dir(sub_dir_name, dir);

		if (!core_dir[i]) {
			dev_err(dev, "Debugfs sub directory cluster %s creation failed\n",
					sub_dir_name);
			return -ENOMEM;
		}
	}

	soc_dir = debugfs_create_dir("soc", dir);
	if (!soc_dir) {
		dev_err(dev, "Debugfs directory 'soc' creation failed\n");
		return -ENOMEM;
	}

	for (u32 i = 0; i < telemetry->num_max_counters; i++) {
		memset(dup_name, '\0', MAX_NAME_LENGTH);
		ptrname = &dup_name[0];

		strscpy(dup_name, pname[i].name, MAX_NAME_LENGTH);

		pos = 0;
		while ((deliminated = strsep(&ptrname, "_")) != NULL)
			delim[pos++] = deliminated;

		fid[i] = i;
		if (kstrtouint(delim[1], 0, &hwencd_val))
			return -ENOMEM;

		temp = hwencd_val;
		len = strlen(delim[1]);
		if (temp < 0)
			continue;

		while (temp >= 10)
			temp /= 10;

		hw_id = hwencd_val - temp * int_pow(10, (len - 1));

		switch (temp) {
		case SOC_CENTRIC:
			ret = debugfs_create_file(delim[0], 0440, soc_dir,
					(void *)&fid[i], &telemetry_stats_fops);
			break;
		case CLUSTER_CENTRIC:
			if (pvalue[i].value != -1) {
				ret = debugfs_create_file(delim[0], 0440,
						cluster_dir[hw_id], (void *)&fid[i],
						&telemetry_stats_fops);
			}
			break;
		case CORE_CENTRIC:
			if (pvalue[i].value != -1) {
				ret = debugfs_create_file(delim[0], 0440, core_dir[hw_id],
					(void *)&fid[i], &telemetry_stats_fops);
			}
			break;
		default:
			break;
		}

		if (!ret) {
			dev_err(dev, "Failed to create file %s\n", pname[i].name);
			return -ENOMEM;
		}
	}

	if (!debugfs_create_file("stat_update", 0640, dir, NULL,
			&telemetry_stat_request_update)) {
		dev_err(dev, "Failed to create file %s\n", "stat_update");
		return -ENOMEM;
	}

	if (!debugfs_create_file("counter_info", 0640, dir,
					NULL, &counter_info_file_fops)) {
		dev_err(dev, "Failed to create file %s\n", "counter_info");
		return -ENOMEM;
	}

	return 0;
}

static int scmi_cpuss_telemetry_probe(struct scmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	int scmi_tx_status;
	u64 sh_mem_base_address = 0;
	u32 gsize;
	u32 name_array_size;
	u32 value_array_size;

	vendor_ops = (const struct qcom_scmi_vendor_ops *)sdev->handle->devm_protocol_get(sdev,
			sdev->protocol_id, &ph_telemetry);
	if (IS_ERR(vendor_ops)) {
		dev_err(dev, "Cannot access protocol:0x%X - err:%ld\n", sdev->protocol_id,
				PTR_ERR(vendor_ops));
		return PTR_ERR(vendor_ops);
	}

	scmi_tx_status = vendor_ops->get_param(ph_telemetry, &sh_mem_base_address,
				SCMI_TELEMETRY_ALGO_STR, 0, 0, sizeof(sh_mem_base_address));
	if (scmi_tx_status) {
		pr_err("Not able to find shared memory location %s\n", __func__);
		return -ENODEV;
	}

	gsize = sizeof(struct telemetry_global_param_t);

	telemetry = (struct telemetry_global_param_t *)
			ioremap_cache(sh_mem_base_address, gsize);

	if (!telemetry) {
		dev_err(dev, "Failed to map memory for telemetry\n");
		return -ENOMEM;
	}

	name_array_size = sizeof(struct telemetry_counter_attributes_name_t) *
					telemetry->num_max_counters;
	pname = (const struct telemetry_counter_attributes_name_t *)
			ioremap_cache(telemetry->pname, name_array_size);
	if (!pname) {
		dev_err(dev, "Failed to map memory for telemetry counter names\n");
		goto mem_allocation_error_handler;
	}

	value_array_size = sizeof(struct telemetry_counter_attributes_value_t) *
					telemetry->num_max_counters;

	pvalue = (struct telemetry_counter_attributes_value_t *)
			ioremap_cache(telemetry->pvalue, value_array_size);
	if (!pvalue) {
		dev_err(dev, "Failed to map memory for telemetry counter values\n");
		goto mem_allocation_error_handler;
	}

	if (cpuss_telemetry_create_fs_entries(sdev)) {
		dev_err(dev, "Failed to create FS entries\n");
		debugfs_remove_recursive(dir);
		goto mem_allocation_error_handler;
	}

	return 0;

mem_allocation_error_handler:
	telemetry_memory_deallocation();
	return -ENOMEM;
}

static void scmi_cpuss_telemetry_remove(struct scmi_device *sdev)
{
	telemetry_memory_deallocation();
}

static const struct scmi_device_id scmi_id_table[] = {
	{ QCOM_SCMI_VENDOR_PROTOCOL, "cpuss_telemetry_vendor" },
	{},
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver scmi_cpuss_telemetry_driver = {
	.name = "scmi-cpuss-telemetry-driver",
	.probe = scmi_cpuss_telemetry_probe,
	.remove = scmi_cpuss_telemetry_remove,
	.id_table = scmi_id_table,
};

module_scmi_driver(scmi_cpuss_telemetry_driver);
MODULE_DESCRIPTION("QTI SCMI CPUSS Telemetry Driver");
MODULE_LICENSE("GPL");
