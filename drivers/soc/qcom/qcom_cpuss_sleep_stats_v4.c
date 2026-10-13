// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <soc/qcom/cpuss_telemetry.h>

#define CPUNAME_SZ 8
#define CLUSTERNAME_SZ 10
#define NAME_LENGTH 0x12
#define REGS_MAX 3

enum cpuss_sleep_stats {
	C2,
	C4,
	CL5,
	SS3,
	STATS_MAX,
};

#define QCOM_CORE_SLEEP_STATS_SHOW(name)	\
static int qcom_stats_core_##name##_show(struct seq_file *s, void *d)	\
{									\
	u32 *id = s->private;						\
	u32 val;							\
	val = get_telemetry_counter_value(*id);				\
	seq_printf(s, "%u\n", val);					\
									\
	return 0;							\
}									\
DEFINE_SHOW_ATTRIBUTE(qcom_stats_core_##name)				\

#define QCOM_SLEEP_STATS_SHOW(name)	\
static int qcom_stats_cluster_cpuss_##name##_show(struct seq_file *s, void *d)	\
{									\
	u32 *id = s->private;						\
	u64 val;							\
	val = get_telemetry_counter_value(*id);				\
	seq_printf(s, "%llu\n", val);					\
									\
	return 0;							\
}									\
DEFINE_SHOW_ATTRIBUTE(qcom_stats_cluster_cpuss_##name)			\

struct regs_config {
	void __iomem *base;
	u32 offset;
	char name[NAME_LENGTH];
	u32 residency_lo_hi_distance;
};

struct stats_config {
	struct regs_config count_regs[REGS_MAX];
	struct regs_config residency_regs[REGS_MAX];
};

struct stats_id_offset {
	int count_regs_offset[STATS_MAX];
	int residency_regs_offset[STATS_MAX];
};

struct core_stats_id {
	u32 c2_count_id;
	u32 c2_residency_id;
	u32 c4_count_id;
	u32 c4_residency_id;
};

struct cluster_stats_id {
	u32 cl5_count_id;
	u32 cl5_residency_id;
};

struct cpuss_stats_id {
	u32 ss3_count_id;
	u32 ss3_residency_id;
};

struct qcom_stats_prvdata {
	struct platform_device *pdev;
	int ncpu;
	int ncluster;
	void __iomem *base;
	struct stats_config *offset;
	struct stats_id_offset *id_offset;
	struct core_stats_id *core_id;
	struct cluster_stats_id *cluster_id;
	struct cpuss_stats_id *cpuss_id;
	struct dentry *rootdir;
	struct dentry **cpu_dir;
	struct dentry **cluster_dir;
	struct dentry *cpuss_dir;
};

QCOM_CORE_SLEEP_STATS_SHOW(count);
QCOM_CORE_SLEEP_STATS_SHOW(residency);
QCOM_SLEEP_STATS_SHOW(count);
QCOM_SLEEP_STATS_SHOW(residency);

static int qcom_stats_count_show(struct seq_file *s, void *d)
{
	struct regs_config *reg = s->private;
	u32 val;

	val = readl_relaxed(reg->base + reg->offset);
	seq_printf(s, "%u\n", val);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qcom_stats_count);

static int qcom_stats_residency_show(struct seq_file *s, void *d)
{
	struct regs_config *reg = s->private;
	u64 val;

	val = (u64)readl_relaxed(reg->base + reg->offset
				+ reg->residency_lo_hi_distance) << 32;
	val += readl_relaxed(reg->base + reg->offset);
	seq_printf(s, "%llu\n", val);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qcom_stats_residency);

static void qcom_create_count_file(struct qcom_stats_prvdata *pdata)
{
	int i;

	if (of_device_is_compatible(pdata->pdev->dev.of_node, "qcom,cpuss-sleep-stats-v4")) {
		struct stats_config *config = pdata->offset;
		char reg_name[3] = {0};

		for (i = 0; i < ARRAY_SIZE(config->count_regs); i++) {
			pdata->offset->count_regs[i].base = pdata->base;
			snprintf(reg_name, sizeof(reg_name), pdata->offset->count_regs[i].name);
			if (!strcmp(reg_name, "CL"))
				debugfs_create_file(pdata->offset->count_regs[i].name, 0400,
				pdata->cluster_dir[i], (void *)&pdata->offset->count_regs[i],
				&qcom_stats_count_fops);
			else if (!strcmp(reg_name, "SS"))
				debugfs_create_file(pdata->offset->count_regs[i].name, 0400,
				pdata->cpuss_dir, (void *)&pdata->offset->count_regs[i],
				&qcom_stats_count_fops);
		}
	}

	if (of_device_is_compatible(pdata->pdev->dev.of_node, "qcom,cpuss-sleep-stats-v6")) {
		for (i = 0; i < pdata->ncluster; i++) {
			pdata->cluster_id[i].cl5_count_id =
				pdata->id_offset->count_regs_offset[CL5] + i * 2;
			debugfs_create_file("CL5_Count", 0400, pdata->cluster_dir[i],
				(void *)&pdata->cluster_id[i].cl5_count_id,
				&qcom_stats_cluster_cpuss_count_fops);
		}

		pdata->cpuss_id->ss3_count_id =
				pdata->id_offset->count_regs_offset[SS3];
		debugfs_create_file("SS3_Count", 0400, pdata->cpuss_dir,
			(void *)&pdata->cpuss_id->ss3_count_id,
			&qcom_stats_cluster_cpuss_count_fops);

		for (i = 0; i < pdata->ncpu; i++) {
			pdata->core_id[i].c2_count_id =
				pdata->id_offset->count_regs_offset[C2] + i * 2;
			debugfs_create_file("C2_Count", 0400, pdata->cpu_dir[i],
					(void *)&pdata->core_id[i].c2_count_id,
					&qcom_stats_core_count_fops);

			pdata->core_id[i].c4_count_id =
				pdata->id_offset->count_regs_offset[C4] + i * 2;
			debugfs_create_file("C4_Count", 0400, pdata->cpu_dir[i],
					(void *)&pdata->core_id[i].c4_count_id,
					&qcom_stats_core_count_fops);
		}
	}
}

static void qcom_create_resindency_file(struct qcom_stats_prvdata *pdata)
{
	int i;

	if (of_device_is_compatible(pdata->pdev->dev.of_node, "qcom,cpuss-sleep-stats-v4")) {
		struct stats_config *config = pdata->offset;
		char reg_name[3] = {0};

		for (i = 0; i < ARRAY_SIZE(config->residency_regs); i++) {
			pdata->offset->residency_regs[i].base = pdata->base;
			snprintf(reg_name, sizeof(reg_name), pdata->offset->residency_regs[i].name);
			if (!strcmp(reg_name, "CL"))
				debugfs_create_file(pdata->offset->residency_regs[i].name, 0400,
				pdata->cluster_dir[i], (void *)&pdata->offset->residency_regs[i],
				&qcom_stats_residency_fops);
			else if (!strcmp(reg_name, "SS"))
				debugfs_create_file(pdata->offset->residency_regs[i].name, 0400,
				pdata->cpuss_dir, (void *)&pdata->offset->residency_regs[i],
				&qcom_stats_residency_fops);
		}
	}

	if (of_device_is_compatible(pdata->pdev->dev.of_node, "qcom,cpuss-sleep-stats-v6")) {
		for (i = 0; i < pdata->ncluster; i++) {
			pdata->cluster_id[i].cl5_residency_id =
				pdata->id_offset->residency_regs_offset[CL5] + i * 2;
			debugfs_create_file("CL5_Residency", 0400, pdata->cluster_dir[i],
				(void *)&pdata->cluster_id[i].cl5_residency_id,
				&qcom_stats_cluster_cpuss_residency_fops);
		}

		pdata->cpuss_id->ss3_residency_id =
				pdata->id_offset->residency_regs_offset[SS3];
		debugfs_create_file("SS3_Residency", 0400, pdata->cpuss_dir,
			(void *)&pdata->cpuss_id->ss3_residency_id,
			&qcom_stats_cluster_cpuss_residency_fops);

		for (i = 0; i < pdata->ncpu; i++) {
			pdata->core_id[i].c2_residency_id =
				pdata->id_offset->residency_regs_offset[C2] + i * 2;
			debugfs_create_file("C2_Residency", 0400, pdata->cpu_dir[i],
					(void *)&pdata->core_id[i].c2_residency_id,
					&qcom_stats_core_residency_fops);

			pdata->core_id[i].c4_residency_id =
				pdata->id_offset->residency_regs_offset[C4] + i * 2;
			debugfs_create_file("C4_Residency", 0400, pdata->cpu_dir[i],
					(void *)&pdata->core_id[i].c4_residency_id,
					&qcom_stats_core_residency_fops);
		}
	}
}

static void qcom_create_cpuss_dir(struct qcom_stats_prvdata *pdata)
{
	int i;
	char cpu_name[CPUNAME_SZ] = {0};
	char cluster_name[CLUSTERNAME_SZ] = {0};

	for (i = 0; i < pdata->ncpu; i++) {
		snprintf(cpu_name, sizeof(cpu_name), "pcpu%u", i);
		pdata->cpu_dir[i] = debugfs_create_dir(cpu_name, pdata->rootdir);
	}

	for (i = 0; i < pdata->ncluster; i++) {
		snprintf(cluster_name, sizeof(cluster_name), "cluster%u", i);
		pdata->cluster_dir[i] = debugfs_create_dir(cluster_name, pdata->rootdir);
	}

	pdata->cpuss_dir = debugfs_create_dir("cpuss", pdata->rootdir);
}

static const struct stats_id_offset qcom_cpuss_regs_offset_canoe = {
	.count_regs_offset = { 16, 32, 52, 60 },
	.residency_regs_offset = {17, 33, 53, 61},
};

static const struct stats_id_offset qcom_cpuss_regs_offset_glymur = {
	.count_regs_offset = { 36, 72, 114, 125 },
	.residency_regs_offset = { 37, 73, 115, 126 },
};

static const struct of_device_id qcom_cpuss_regs_offset_tlb[] = {
	{ .compatible = "qcom,cpuss-sleep-stats-canoe",
	  .data = &qcom_cpuss_regs_offset_canoe },
	{ .compatible = "qcom,cpuss-sleep-stats-glymur",
	  .data = &qcom_cpuss_regs_offset_glymur },
	{ },
};

static int qcom_cpuss_sleep_stats_v4_probe(struct platform_device *pdev)
{
	struct qcom_stats_prvdata *pdata;

	pdata = devm_kzalloc(&pdev->dev, sizeof(struct qcom_stats_prvdata),
			      GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->rootdir = debugfs_create_dir("qcom_cpuss_sleep_stats", NULL);
	if (IS_ERR(pdata->rootdir)) {
		dev_err(&pdev->dev, "Debugfs cpuss sleep stats directory creation failed\n");
		return -ENOENT;
	}

	pdata->pdev = pdev;

	if (of_device_is_compatible(pdev->dev.of_node, "qcom,cpuss-sleep-stats-v4")) {
		pdata->base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
		if (IS_ERR(pdata->base)) {
			debugfs_remove_recursive(pdata->rootdir);
			return PTR_ERR(pdata->base);
		}

		pdata->offset = (struct stats_config *)of_device_get_match_data(&pdev->dev);
		if (!pdata->offset) {
			debugfs_remove_recursive(pdata->rootdir);
			return -EINVAL;
		}
	}

	if (of_device_is_compatible(pdev->dev.of_node, "qcom,cpuss-sleep-stats-v6")) {
		const struct of_device_id *match_id;
		int ret = 0;

		pdata->ncpu = nr_cpu_ids;
		pdata->cpu_dir = devm_kzalloc(&pdev->dev, sizeof(struct dentry *) *
					pdata->ncpu, GFP_KERNEL);
		if (!pdata->cpu_dir)
			return -ENOMEM;

		ret = of_property_read_u32(pdev->dev.of_node, "num-clusters", &pdata->ncluster);
		if (ret < 0)
			return -ENODEV;

		pdata->cluster_dir = devm_kzalloc(&pdev->dev, sizeof(struct dentry *) *
					pdata->ncluster, GFP_KERNEL);
		if (!pdata->cluster_dir)
			return -ENOMEM;

		match_id = of_match_node(qcom_cpuss_regs_offset_tlb, pdev->dev.of_node);
		if (!match_id)
			return -ENODEV;

		pdata->id_offset = (struct stats_id_offset *)match_id->data;
		if (!pdata->id_offset)
			return -EINVAL;

		pdata->core_id = devm_kcalloc(&pdev->dev, pdata->ncpu,
						sizeof(*pdata->core_id), GFP_KERNEL);
		if (!pdata->core_id)
			return -ENOMEM;

		pdata->cluster_id = devm_kcalloc(&pdev->dev, pdata->ncluster,
						sizeof(*pdata->cluster_id), GFP_KERNEL);
		if (!pdata->cluster_id)
			return -ENOMEM;

		pdata->cpuss_id = devm_kcalloc(&pdev->dev, 1,
						sizeof(*pdata->cpuss_id), GFP_KERNEL);
		if (!pdata->cpuss_id)
			return -ENOMEM;

		qcom_create_cpuss_dir(pdata);
	}

	qcom_create_count_file(pdata);
	qcom_create_resindency_file(pdata);

	platform_set_drvdata(pdev, pdata->rootdir);

	return 0;
}

static void qcom_cpuss_sleep_stats_remove(struct platform_device *pdev)
{
	struct dentry *root = platform_get_drvdata(pdev);

	debugfs_remove_recursive(root);
}

struct stats_config qcom_cpuss_cntr_offsets = {
	.count_regs = {
		{
			.offset = 0x1220,
			.name = "CL0_CL5_Count",
		},
		{
			.offset = 0x1224,
			.name = "CL1_CL5_Count",
		},
		{
			.offset = 0x1018,
			.name = "SS3_Count",
		},
	},
	.residency_regs = {
		{
			.offset = 0x1320,
			.name = "CL0_CL5_Residency",
			.residency_lo_hi_distance = 0x10,
		},
		{
			.offset = 0x1324,
			.name = "CL1_CL5_Residency",
			.residency_lo_hi_distance = 0x10,
		},
		{
			.offset = 0x1118,
			.name = "SS3_Residency",
			.residency_lo_hi_distance = 0x4,
		},
	},
};

static const struct of_device_id qcom_cpuss_stats_v4_table[] = {
	{ .compatible = "qcom,cpuss-sleep-stats-v4", .data = &qcom_cpuss_cntr_offsets },
	{ .compatible = "qcom,cpuss-sleep-stats-v6" },
	{ },
};

static struct platform_driver qcom_cpuss_sleep_stats_v4 = {
	.probe = qcom_cpuss_sleep_stats_v4_probe,
	.remove = qcom_cpuss_sleep_stats_remove,
	.driver	= {
		.name = "qcom_cpuss_sleep_stats_v4",
		.of_match_table	= qcom_cpuss_stats_v4_table,
	},
};

module_platform_driver(qcom_cpuss_sleep_stats_v4);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. (QTI) CPUSS sleep stats v4 driver");
MODULE_LICENSE("GPL");
