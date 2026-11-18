// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/debugfs.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/soc/qcom/qcom_aoss.h>
#include <linux/string.h>
#include <linux/types.h>

#define DEFAULT_MON_DUR 100
#define DEFAULT_START_DELAY 0
#define DEFAULT_CXPC_THRESH 1000
#define DEFAULT_DIV_THRESH 1000
#define LPM_MON_DRV_STR_SIZE 12
#define LPM_MON_BCM_STR_SIZE 8
#define LPM_MON_RESPONSE_MBOX_SIZE 4 /** 128 bits, or 4 u32s */

#define QMP_MSG_LEN 0x64

#define read_word(base, itr) ({					\
		u32 v;						\
		v = le32_to_cpu(readl_relaxed(base + itr));	\
		itr += sizeof(u32);				\
		/** Barrier to ensure sequential read */	\
		smp_rmb();					\
		v;						\
		})

#define QMX_APPEND_ARG(curr_arg, prev_arg, buffer, buffer_len, append_buf, arg_str) ({	\
		if (curr_arg != prev_arg) {		\
			scnprintf(append_buf, sizeof(append_buf),	\
				", " #arg_str ": %d",		\
				curr_arg);					\
			if ((buffer_len + strlen(append_buf) + 1) < QMP_MSG_LEN) { \
				scnprintf(buffer + buffer_len, \
				sizeof(buffer) - buffer_len, "%s", append_buf); \
					buffer_len = strlen(buffer);	\
				prev_arg = curr_arg;	\
			}	\
		}		\
		})

struct vx_data {
	u32 poll_cnt;
	u32 *cx_active_vt_cnt;
	u32 *ddr_active_vt_cnt;
	u32 **bcm_nd_active_vt_cnt;
	u32 *sleep_cnt;
	u32 adsp_island_cnt;
	u32 *drv_str_lens;
};

/** Structure containing all of the persistent data */
struct vx_platform_data {
	/** Base addresses for accessing shared memory for logging sessions */
	void __iomem *base;
	void __iomem *cxpc_base;
	/** Directories within debug directory for public access */
	struct dentry *vx_dir;
	struct dentry *param_dir;
	/** Number of ARC DRVS, BCM DRVs, and BCM nodes, for array sizing */
	size_t n_arc_drv;
	size_t n_bcm_drv;
	size_t n_bcm_nds;
	/** BCM node arrays can have elements of size 2 or 4 bytes */
	u32 bcm_arr_size;
	/** Names of the ARC DRVS and the BCM Nodes */
	char **arc_drvs;
	char **bcm_nds;
	u8 *arc_drv_name_lens;
	/** Lock and QMP messaging */
	struct mutex lock;
	struct qmp *qmp;
	/** Tunables that can affect the logging of the run */
	u32 mon_dur;
	u32 start_delay;
	u32 cxpc_thresh;
	u32 div_thresh;
	bool crash_enable;
	/** Track with what was previously sent to keep the QMP message small */
	u32 prev_mon_dur;
	u32 prev_start_delay;
	u32 prev_cxpc_thresh;
	u32 prev_div_thresh;
	bool prev_crash_enable;
	/** Track whether there's an active logging session */
	bool active_session;
	u32 lpm_mon_mbox[LPM_MON_RESPONSE_MBOX_SIZE];
};

static struct vx_platform_data *g_pd;

static int read_vx_data(struct vx_platform_data *pd, struct vx_data *data, void __iomem *base)
{
	int i, j, itr = 0;
	bool active_read = false;
	u32 nd_data, curr_data, str_len;

	/**
	 * The QMP response mailbox needs to be skipped as an offset from the base.
	 * Start reading data after the mailbox at the number of polls.
	 */
	for (i = 0; i < LPM_MON_RESPONSE_MBOX_SIZE; i++)
		itr += sizeof(u32);

	data->poll_cnt = read_word(base, itr);
	if (!data->poll_cnt)
		return -ENOENT;

	for (i = 0; i < pd->n_arc_drv; i++) {
		curr_data = read_word(base, itr);
		data->cx_active_vt_cnt[i] = curr_data;
		data->drv_str_lens[i] = curr_data;
	}

	for (i = 0; i < pd->n_arc_drv; i++) {
		curr_data = read_word(base, itr);
		data->ddr_active_vt_cnt[i] = curr_data;
		data->drv_str_lens[i] = MAX(data->drv_str_lens[i], curr_data);
	}

	for (i = 0; i < pd->n_bcm_drv; i++) {
		for (j = 0; j < pd->n_bcm_nds; j++) {
			/**
			 * It is possible that there are too many BCM nodes to fit the data
			 * into the allocated memory as 32 bit ints. In this instance, the
			 * data is then stored as 16 bit ints. However, writing to and
			 * reading from this memory can only occur in 32 bit chunks. Track
			 * accordingly if this is the lower or upper 32 bits.
			 */
			if (pd->bcm_arr_size == 2) {
				if (active_read) {
					nd_data >>= 16;
					active_read = false;
				} else {
					nd_data = read_word(base, itr);
					active_read = true;
				}
				curr_data = (nd_data & 0xFFFF);
				data->bcm_nd_active_vt_cnt[i][j] = curr_data;
				data->drv_str_lens[i] = MAX(data->drv_str_lens[i], curr_data);
			} else {
				curr_data = read_word(base, itr);
				data->bcm_nd_active_vt_cnt[i][j] = curr_data;
				data->drv_str_lens[i] = MAX(data->drv_str_lens[i], curr_data);
			}
		}
	}

	for (i = 0; i < pd->n_bcm_drv; i++) {
		curr_data = read_word(base, itr);
		data->sleep_cnt[i] = curr_data;
		data->drv_str_lens[i] = MAX(data->drv_str_lens[i], curr_data);
	}

	data->adsp_island_cnt = read_word(base, itr);

	/**
	 * When setting up the largest string lengths, compare the length of the
	 * integers as though they're decimal strings and compare them to the name
	 * lengths. This will make all columns have cleanly-formatted widths,
	 * increasing readability.
	 */
	for (i = 0; i < pd->n_arc_drv; i++) {
		str_len = 0;
		curr_data = data->drv_str_lens[i];

		if (!curr_data) {
			str_len = 1;
		} else {
			while (curr_data) {
				str_len++;
				curr_data /= 10;
			}
		}

		data->drv_str_lens[i] = MAX(str_len, pd->arc_drv_name_lens[i]);
	}

	return 0;
}

static void show_vx_data(struct vx_platform_data *pd, struct vx_data *data,
			 struct seq_file *seq)
{
	int i, j;

	seq_printf(seq, "Session Counts: %u\n", data->poll_cnt);
	seq_printf(seq, "ADSP In Island Count: %u\n", data->adsp_island_cnt);
	seq_printf(seq, "Duration (ms): %u\n", pd->mon_dur);

	seq_puts(seq, "Counts         |");
	for (i = 0; i < pd->n_arc_drv; i++)
		seq_printf(seq, " %*s|", data->drv_str_lens[i], pd->arc_drvs[i]);
	seq_puts(seq, "\n");

	seq_puts(seq, "CX Counts      |");
	for (i = 0; i < pd->n_arc_drv; i++)
		seq_printf(seq, " %*u|", data->drv_str_lens[i],
					data->cx_active_vt_cnt[i]);
	seq_puts(seq, "\n");

	seq_puts(seq, "DDR Counts     |");
	for (i = 0; i < pd->n_arc_drv; i++)
		seq_printf(seq, " %*u|", data->drv_str_lens[i],
					data->ddr_active_vt_cnt[i]);
	seq_puts(seq, "\n");

	for (i = 0; i < pd->n_bcm_nds; i++) {
		seq_printf(seq, "%s Counts%*s|", pd->bcm_nds[i],
					(u32)(LPM_MON_BCM_STR_SIZE - strlen(pd->bcm_nds[i])), "");
		for (j = 0; j < pd->n_arc_drv; j++) {
			if (j < pd->n_bcm_drv)
				seq_printf(seq, " %*u|", data->drv_str_lens[j],
							data->bcm_nd_active_vt_cnt[j][i]);
			else
				seq_printf(seq, " %*u|", data->drv_str_lens[j], 0);
		}
		seq_puts(seq, "\n");
	}

	seq_puts(seq, "Sleep Counts   |");
	for (i = 0; i < pd->n_arc_drv; i++) {
		if (i < pd->n_bcm_drv)
			seq_printf(seq, " %*u|", data->drv_str_lens[i], data->sleep_cnt[i]);
		else
			seq_printf(seq, " %*u|", data->drv_str_lens[i], 0);
	}
	seq_puts(seq, "\n\n");
}

/**
 * When requested to print out the data, it first must be read. The data from
 * AOP is stored in MSGRAM, and should be read on-demand, not whenever a
 * request is made to stop the profiling. There are two types of sessions:
 * monitor (overall) and CX (since the end of the last CXPC session). These use
 * the same layout, though, so we reuse the reading and writing functions to
 * parse and present the data. The same allocated memory can also be used, to
 * prevent doubling the function's memory allocation for temporary variables.
 */
static int vx_show(struct seq_file *seq, void *data)
{
	struct vx_platform_data *pd = seq->private;
	int i, ret = 0;
	struct vx_data *lpm_session;

	mutex_lock(&pd->lock);

	lpm_session = kmalloc(sizeof(struct vx_data), GFP_KERNEL);
	if (!lpm_session) {
		ret = -ENOMEM;
		goto failed_create_lpm_session;
	}

	/** Allocate the space required for the session to be tracked */
	lpm_session->cx_active_vt_cnt =
		kcalloc(pd->n_arc_drv, sizeof(u32), GFP_KERNEL);
	if (!lpm_session->cx_active_vt_cnt) {
		ret = -ENOMEM;
		goto failed_create_cx_arr;
	}

	lpm_session->ddr_active_vt_cnt =
		kcalloc(pd->n_arc_drv, sizeof(u32), GFP_KERNEL);
	if (!lpm_session->ddr_active_vt_cnt) {
		ret = -ENOMEM;
		goto failed_create_ddr_arr;
	}

	lpm_session->bcm_nd_active_vt_cnt =
		kcalloc(pd->n_bcm_drv, sizeof(u32 *), GFP_KERNEL);
	if (!lpm_session->bcm_nd_active_vt_cnt) {
		ret = -ENOMEM;
		goto failed_create_nd_arr;
	}

	for (i = 0; i < pd->n_bcm_drv; i++) {
		lpm_session->bcm_nd_active_vt_cnt[i] =
			kcalloc(pd->n_bcm_nds, sizeof(u32), GFP_KERNEL);
		if (!lpm_session->bcm_nd_active_vt_cnt[i]) {
			ret = -ENOMEM;
			goto failed_create_nd_cnt;
		}
	}

	lpm_session->sleep_cnt = kcalloc(pd->n_bcm_drv, sizeof(u32), GFP_KERNEL);
	if (!lpm_session->sleep_cnt) {
		ret = -ENOMEM;
		goto failed_create_sleep_cnt;
	}

	lpm_session->drv_str_lens = kcalloc(pd->n_arc_drv, sizeof(u32), GFP_KERNEL);
	if (!lpm_session->drv_str_lens) {
		ret = -ENOMEM;
		goto failed_create_drv_str_lens;
	}

	ret = read_vx_data(pd, lpm_session, pd->base);
	if (ret) {
		pr_err("Failure reading Monitor data: %d\n", ret);
		goto exit_start;
	}

	seq_puts(seq, "Monitor Session\n");
	show_vx_data(pd, lpm_session, seq);

	ret = read_vx_data(pd, lpm_session, pd->cxpc_base);
	if (ret) {
		pr_err("Failure reading CXPC data: %d\n", ret);
		goto exit_start;
	}

	seq_puts(seq, "CXPC Session\n");
	show_vx_data(pd, lpm_session, seq);

exit_start:
	kfree(lpm_session->drv_str_lens);
failed_create_drv_str_lens:
	kfree(lpm_session->sleep_cnt);
failed_create_sleep_cnt:
	i = pd->n_bcm_drv;
failed_create_nd_cnt:
	/**
	 * Need to subtract one to either start at the last element or the last
	 * element successfully allocated
	 */
	i--;
	for (; i >= 0; i--)
		kfree(lpm_session->bcm_nd_active_vt_cnt[i]);
	kfree(lpm_session->bcm_nd_active_vt_cnt);
failed_create_nd_arr:
	kfree(lpm_session->ddr_active_vt_cnt);
failed_create_ddr_arr:
	kfree(lpm_session->cx_active_vt_cnt);
failed_create_cx_arr:
	kfree(lpm_session);
failed_create_lpm_session:
	mutex_unlock(&pd->lock);

	return ret;
}

static int open_vx(struct inode *inode, struct file *file)
{
	return single_open(file, vx_show, inode->i_private);
}

static const struct file_operations sys_pm_vx_fops = {
	.open = open_vx,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/**
 * Due to limitations in message size, prioritize sending certain configuration
 * variables. Skip any variables that haven't changed from the previously sent
 * message. If there are more changes than fit, then don't send those until the
 * next message, when there is more space. Users can also directly send QMP
 * messages as needed.
 */
static int vx_start_polling(struct vx_platform_data *pd)
{
	int ret = 0;
	int buflen = 0;
	char buf[QMP_MSG_LEN] = {};
	char append_args[QMP_MSG_LEN] = {};

	scnprintf(buf, sizeof(buf), "{class: lpm_mon, cmd: start");

	buflen = strlen(buf);

	QMX_APPEND_ARG(pd->mon_dur, pd->prev_mon_dur, buf, buflen,
		append_args, mon_dur_ms);
	QMX_APPEND_ARG(pd->start_delay, pd->prev_start_delay, buf, buflen,
		append_args, start_delay_ms);
	QMX_APPEND_ARG(pd->cxpc_thresh, pd->prev_cxpc_thresh, buf, buflen,
		append_args, crash_dur_ms);
	QMX_APPEND_ARG(pd->div_thresh, pd->prev_div_thresh, buf, buflen,
		append_args, diverg_thr);
	QMX_APPEND_ARG(pd->crash_enable, pd->prev_crash_enable, buf, buflen,
		append_args, crash_en);

	scnprintf(buf + buflen, sizeof(buf) - buflen, "}");

	ret = qmp_send(pd->qmp, buf, sizeof(buf));
	if (ret)
		pr_err("Error sending QMP LPM start mon message: %d\n", ret);

	return ret;
}

static int vx_stop_polling(struct vx_platform_data *pd)
{
	int ret = 0;
	char buf[QMP_MSG_LEN] = {};

	/**
	 * In certain scenarios, users will want to crash only after exiting
	 * from sleep, for the full sleep study. Thus, send the additional
	 * arguments that could matter as part of stop, as well.
	 */
	scnprintf(buf, sizeof(buf),
		"{class: lpm_mon, cmd: stop, crash_en: %d, diverg_thr: %u}",
		  pd->crash_enable,
		  pd->div_thresh);

	ret = qmp_send(pd->qmp, buf, sizeof(buf));
	if (ret)
		pr_err("Error sending QMP LPM stop mon message: %d\n", ret);

	return ret;
}

static int active_session_get(void *data, u64 *val)
{
	struct vx_platform_data *pd = data;

	mutex_lock(&pd->lock);
	*val = (u64)pd->active_session;
	mutex_unlock(&pd->lock);

	return 0;
}

static int active_session_set(void *data, u64 val)
{
	struct vx_platform_data *pd = data;
	int ret = 0;

	mutex_lock(&pd->lock);
	if ((bool)val != pd->active_session) {
		pd->active_session = (bool)val;
		if (pd->active_session)
			ret = vx_start_polling(pd);
		else
			ret = vx_stop_polling(pd);
	}
	mutex_unlock(&pd->lock);

	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(active_session_fops,
			active_session_get,
			active_session_set,
			"%lld\n");

static void vx_create_debug_nodes(struct dentry *root, struct vx_platform_data *pd)
{
	debugfs_create_file("sys_pm_violators_v2", 0400, root,
				 pd, &sys_pm_vx_fops);
	debugfs_create_file("active_session", 0600, root,
				 pd, &active_session_fops);

	pd->param_dir = debugfs_create_dir("parameters", root);

	debugfs_create_u32("mon_dur_ms", 0600, pd->param_dir, &(pd->mon_dur));
	debugfs_create_u32("start_delay_ms", 0600, pd->param_dir, &(pd->start_delay));
	debugfs_create_u32("cxpc_thresh_ms", 0600, pd->param_dir, &(pd->cxpc_thresh));
	debugfs_create_u32("div_thresh_ms", 0600, pd->param_dir, &(pd->div_thresh));
	debugfs_create_bool("crash_enable", 0600, pd->param_dir, &(pd->crash_enable));
}

static void get_aop_resp(struct vx_platform_data *pd)
{
	int i, itr = 0;

	for (i = 0; i < LPM_MON_RESPONSE_MBOX_SIZE; i++)
		pd->lpm_mon_mbox[i] = read_word(pd->base, itr);
}

/**
 * When setting up the device, there are a series of arrays for tracking the
 * ARC and BCM nodes (and their respective names) that must be set up, all of
 * which are queried from AOP. To that end, a QMP mailbox must be set up, then
 * followed by the setup of these tracking variables, then setting the config
 * variables appropriately.
 */
static int vx_probe(struct platform_device *pdev)
{
	struct vx_platform_data *pd;
	int i, ret;
	char buf[QMP_MSG_LEN] = {};

	g_pd = pd = devm_kzalloc(&pdev->dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;

	pd->base = of_iomap(pdev->dev.of_node, 0);
	if (IS_ERR_OR_NULL(pd->base)) {
		ret = PTR_ERR(pd->base);
		goto failed_exit;
	}

	pd->cxpc_base = of_iomap(pdev->dev.of_node, 1);
	if (IS_ERR_OR_NULL(pd->cxpc_base)) {
		ret = PTR_ERR(pd->cxpc_base);
		goto failed_exit;
	}

	pd->qmp = qmp_get(&pdev->dev);
	if (IS_ERR(pd->qmp)) {
		ret = PTR_ERR(pd->qmp);
		goto failed_exit;
	}

	mutex_init(&pd->lock);

	scnprintf(buf, sizeof(buf), "{class: lpm_mon, cmd: get_num_arc_drvs}");
	ret = qmp_send(pd->qmp, buf, sizeof(buf));
	if (ret)
		goto failed_qmp;

	get_aop_resp(pd);
	pd->n_arc_drv = pd->lpm_mon_mbox[0];

	scnprintf(buf, sizeof(buf), "{class: lpm_mon, cmd: get_num_bcm_drvs}");
	ret = qmp_send(pd->qmp, buf, sizeof(buf));
	if (ret)
		goto failed_qmp;

	get_aop_resp(pd);
	pd->n_bcm_drv = pd->lpm_mon_mbox[0];

	scnprintf(buf, sizeof(buf), "{class: lpm_mon, cmd: get_bcm_vt_cnt_size}");
	ret = qmp_send(pd->qmp, buf, sizeof(buf));
	if (ret)
		goto failed_qmp;

	get_aop_resp(pd);
	pd->bcm_arr_size = pd->lpm_mon_mbox[0];

	scnprintf(buf, sizeof(buf), "{class: lpm_mon, cmd: get_num_bcm_vcds}");
	ret = qmp_send(pd->qmp, buf, sizeof(buf));
	if (ret)
		goto failed_qmp;

	get_aop_resp(pd);
	pd->n_bcm_nds = pd->lpm_mon_mbox[0];

	pd->arc_drvs = devm_kcalloc(&pdev->dev, pd->n_arc_drv, sizeof(char *), GFP_KERNEL);
	if (!pd->arc_drvs)
		goto failed_alloc;

	pd->arc_drv_name_lens = devm_kcalloc(&pdev->dev, pd->n_arc_drv, sizeof(u8), GFP_KERNEL);
	if (!pd->arc_drv_name_lens)
		goto failed_alloc;

	for (i = 0; i < pd->n_arc_drv; i++) {
		scnprintf(buf, sizeof(buf), "{class: lpm_mon, cmd: get_drv_name, drv: %d}", i);
		ret = qmp_send(pd->qmp, buf, sizeof(buf));
		if (ret)
			goto failed_qmp;

		get_aop_resp(pd);
		pd->arc_drv_name_lens[i] = strnlen((const char *)(pd->lpm_mon_mbox),
				LPM_MON_DRV_STR_SIZE);
		pd->arc_drvs[i] = devm_kcalloc(&pdev->dev, pd->arc_drv_name_lens[i] + 1,
				sizeof(char), GFP_KERNEL);
		if (!pd->arc_drvs[i])
			goto failed_alloc;

		strscpy(pd->arc_drvs[i], (const char *)(pd->lpm_mon_mbox),
				pd->arc_drv_name_lens[i] + 1);
	}

	pd->bcm_nds = devm_kcalloc(&pdev->dev, pd->n_bcm_nds, sizeof(char *), GFP_KERNEL);
	if (!pd->bcm_nds)
		goto failed_alloc;

	for (i = 0; i < pd->n_bcm_nds; i++) {
		scnprintf(buf, sizeof(buf), "{class: lpm_mon, cmd: get_vcd_name, vcd: %d}", i);
		ret = qmp_send(pd->qmp, buf, sizeof(buf));
		if (ret)
			goto failed_qmp;

		get_aop_resp(pd);
		pd->bcm_nds[i] = devm_kcalloc(&pdev->dev, LPM_MON_BCM_STR_SIZE + 1,
				sizeof(char), GFP_KERNEL);
		if (!pd->bcm_nds[i])
			goto failed_alloc;

		strscpy(pd->bcm_nds[i], (const char *)(pd->lpm_mon_mbox),
				LPM_MON_BCM_STR_SIZE + 1);
	}

	pd->vx_dir = debugfs_create_dir("sys_pm_vx", NULL);
	if (!pd->vx_dir) {
		ret = -EINVAL;
		goto failed_create_dir;
	}

	vx_create_debug_nodes(pd->vx_dir, pd);

	pd->mon_dur = DEFAULT_MON_DUR;
	pd->start_delay = DEFAULT_START_DELAY;
	pd->cxpc_thresh = DEFAULT_CXPC_THRESH;
	pd->div_thresh = DEFAULT_DIV_THRESH;
	pd->crash_enable = false;
	pd->prev_mon_dur = DEFAULT_MON_DUR;
	pd->prev_start_delay = DEFAULT_START_DELAY;
	pd->prev_cxpc_thresh = DEFAULT_CXPC_THRESH;
	pd->prev_div_thresh = DEFAULT_DIV_THRESH;
	pd->prev_crash_enable = false;

	platform_set_drvdata(pdev, pd);

	return 0;

failed_create_dir:
	debugfs_remove_recursive(pd->vx_dir);
	goto failed_exit;
failed_qmp:
	pr_err("SYS_PM_VX: QMX message failed: %s\n", buf);
	goto failed_exit;
failed_alloc:
	pr_err("SYS_PM_VX: Failed to allocate memory for array\n");
	ret = -ENOMEM;
failed_exit:
	iounmap(pd->base);
	iounmap(pd->cxpc_base);
	qmp_put(pd->qmp);
	return ret;
}

static void vx_remove(struct platform_device *pdev)
{
	struct vx_platform_data *pd = platform_get_drvdata(pdev);

	debugfs_remove_recursive(pd->vx_dir);
	iounmap(pd->base);
	iounmap(pd->cxpc_base);
	qmp_put(pd->qmp);
}


static const struct of_device_id vx_table[] = {
	{ .compatible = "qcom,sys-pm-violators-v2" },
	{ }
};

static struct platform_driver vx_driver = {
	.probe = vx_probe,
	.remove = vx_remove,
	.driver = {
		.name = "sys-pm-violators-v2",
		.of_match_table = vx_table,
	},
};
module_platform_driver(vx_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. (QTI) System PM Violators V2 driver");
MODULE_ALIAS("platform:sys_pm_vx_v2");
