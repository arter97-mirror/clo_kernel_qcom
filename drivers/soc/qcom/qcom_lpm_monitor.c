// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/soc/qcom/qmi.h>
#include <soc/qcom/qcom_lpm_monitor.h>

#include "qcom_lpm_monitor_service_v01.h"

static void qlpmm_ss_req_data_ops(struct qlpmm_ss_info *si,
				     struct lpm_monitor_report_to_apps_req_msg_v01 *req)
{
	int i;

	snprintf(si->ci.resource_name, QLPMM_MAX_NAME_LEN,
		req->data[0].resource_name.qmi_string);

	si->ci.lpm_resource_count = req->lpm_resource_count;
	si->ci.num_clk_enabled = req->data->num_clk_enabled;

	for (i = 0; i < req->data->num_clk_enabled; i++) {
		snprintf(si->ci.data[i].clock_name, QLPMM_MAX_NAME_LEN,
			req->data[0].data[i].clock_name.qmi_string);

		si->ci.data[i].count = req->data[0].data[i].count;
		si->ci.data[i].last_on_ts = req->data[0].data[i].last_on_timestamp;
		si->ci.data[i].last_off_ts = req->data[0].data[i].last_off_timestamp;
	}
}

static void qlpmm_subsystem_req_cb(struct qmi_handle *qmi,
				     struct sockaddr_qrtr *sq,
				     struct qmi_txn *txn,
				     const void *decoded)
{
	struct lpm_monitor_report_to_apps_resp_msg_v01 resp = { };
	struct lpm_monitor_report_to_apps_req_msg_v01 *req;

	resp.resp.result = QMI_RESULT_SUCCESS_V01;
	resp.resp.error = QMI_ERR_NONE_V01;
	qmi_send_response(qmi, sq, txn,
		QMI_LPM_MONITOR_REPORT_STATS_RESP_V01,
		LPM_MONITOR_REPORT_TO_APPS_RESP_MSG_V01_MAX_MSG_LEN,
		lpm_monitor_report_to_apps_resp_msg_v01_ei, &resp);

	req = (struct lpm_monitor_report_to_apps_req_msg_v01 *)decoded;

	struct qlpmm_ss_info *si;

	list_for_each_entry(si, &qlpmm_pd->subsystem_list, node) {
		if (si->client_connected == true)
			continue;
		if (req->magic_number == QMI_LPM_MONITOR_MAGIC_NUMBER_V01 &&
		   si->subsystem_id == req->subsystem_id) {
			si->client_sq = *sq;
			si->client_connected = true;
			qlpmm_pd->connected_client_num++;
		}
	}

	if (qlpmm_pd->connected_client_num) {
		list_for_each_entry(si, &qlpmm_pd->subsystem_list, node) {
			if (si->subsystem_id == req->subsystem_id &&
			   si->client_connected == true) {
				qlpmm_ss_req_data_ops(si, req);
				complete(&qlpmm_pd->comp);
			}
		}
	}
}

static void qlpmm_disconnect_bye_cb(struct qmi_handle *handle,
				     unsigned int node)
{
	struct qlpmm_ss_info *si;

	list_for_each_entry(si, &qlpmm_pd->subsystem_list, node) {
		if (si->client_connected && si->client_sq.sq_node == node) {
			si->client_connected = false;
			qlpmm_pd->connected_client_num--;
		}
	}
}

static void qlpmm_disconnect_del_client_cb(struct qmi_handle *qmi,
				     unsigned int node, unsigned int port)
{
	struct qlpmm_ss_info *si;

	list_for_each_entry(si, &qlpmm_pd->subsystem_list, node) {
		if (si->client_connected && si->client_sq.sq_node == node
			&& si->client_sq.sq_port == port) {
			si->client_connected = false;
			qlpmm_pd->connected_client_num--;
		}
	}
}

static struct qmi_ops server_ops = {
	.bye = qlpmm_disconnect_bye_cb,
	.del_client = qlpmm_disconnect_del_client_cb,
};

static const struct qmi_msg_handler handlers[] = {
	{
		.type = QMI_REQUEST,
		.msg_id = QMI_LPM_MONITOR_REPORT_STATS_REQ_V01,
		.ei = lpm_monitor_report_to_apps_req_msg_v01_ei,
		.decoded_size = sizeof(struct lpm_monitor_report_to_apps_req_msg_v01),
		.fn = qlpmm_subsystem_req_cb,
	},
	{ },
};

static int qlpmm_send_indication_to_ss(u32 subsystem_id)
{
	int ret;
	struct qlpmm_ss_info *si;

	list_for_each_entry(si, &qlpmm_pd->subsystem_list, node) {
		if (si->subsystem_id == subsystem_id) {
			if (si->client_connected) {
				ret = qmi_send_indication(&qlpmm_pd->handle,
					&si->client_sq,
					QMI_LPM_MONITOR_INITIATE_REPORT_IND_V01,
					LPM_MONITOR_HANDLING_IND_MSG_V01_MAX_MSG_LEN,
					lpm_monitor_handling_ind_msg_v01_ei, NULL);

				if (ret < 0) {
					pr_err("qlpmm send_indication Error to subsystem\n");
					return ret;
				}
			} else {
				pr_err("subsystem:%u disconnection\n", subsystem_id);
				return -EAGAIN;
			}
		}
	}

	return 0;
}

static void qlpmm_ss_data_ops(struct qlpmm_ss_clks_info *sci,
				     struct qlpmm_ss_info *si)
{
	int i;

	snprintf(sci->resource_name, QLPMM_MAX_NAME_LEN,
		si->ci.resource_name);

	sci->lpm_resource_count = si->ci.lpm_resource_count;
	sci->num_clk_enabled = si->ci.num_clk_enabled;

	for (i = 0; i < si->ci.num_clk_enabled; i++) {
		snprintf(sci->data[i].clock_name, QLPMM_MAX_NAME_LEN,
			si->ci.data[i].clock_name);
		sci->data[i].count = si->ci.data[i].count;
		sci->data[i].last_on_ts = si->ci.data[i].last_on_ts;
		sci->data[i].last_off_ts = si->ci.data[i].last_off_ts;
	}
}

int qlpmm_get_subsystem_lpm_data(u32 subsystem_id,
			       struct qlpmm_ss_clks_info *ss_clks_info)
{
	struct qlpmm_ss_info *si;
	int ret;

	if (!ss_clks_info || (subsystem_id >= SUBSYSTEM_ID_MAX_V01))
		return -ENODEV;

	list_for_each_entry(si, &qlpmm_pd->subsystem_list, node) {
		if (si->subsystem_id == subsystem_id) {
			if (si->client_connected) {
				if (si->rw_flag) {
					pr_err("get data frequently, please try again\n");
					return -EAGAIN;
				}
				mutex_lock(&qlpmm_pd->lock);
				si->rw_flag = true;

				ret = qlpmm_send_indication_to_ss(subsystem_id);
				if (ret) {
					si->rw_flag = false;
					mutex_unlock(&qlpmm_pd->lock);
					return -EAGAIN;
				}

				reinit_completion(&qlpmm_pd->comp);
				ret = wait_for_completion_timeout(&qlpmm_pd->comp, QLPMM_IND_TOUT);
				si->rw_flag = false;
				mutex_unlock(&qlpmm_pd->lock);
				if (!ret) {
					pr_err("timeout waiting for adsp ack, please try again\n");
					return -EAGAIN;
				}

				qlpmm_ss_data_ops(ss_clks_info, si);
			} else {
				pr_err("subsystem:%u disconnection\n", subsystem_id);
				return -EAGAIN;
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(qlpmm_get_subsystem_lpm_data);

static int qlpmm_get_adsp_clks_stats_show(struct seq_file *s,
			       void *unused)
{
	struct qlpmm_ss_clks_info *sci;
	int ret, i;

	sci = kzalloc(sizeof(*sci), GFP_KERNEL);
	if (!sci)
		return -ENOMEM;

	ret = qlpmm_get_subsystem_lpm_data(SUBSYSTEM_ADSP_V01, sci);
	if (ret) {
		pr_err("get adsp clks stats error, please try again\n");
		kfree(sci);
		return -EAGAIN;
	}

	seq_printf(s, "subsystem id:%u\nresource name:%s\n\n",
		SUBSYSTEM_ADSP_V01,
		sci->resource_name);
	seq_printf(s, "%-40s%-10s%-20s%-20s\n",
		"clock name",
		"count",
		"last on timestamp",
		"last off timestamp");

	for (i = 0; i < sci->num_clk_enabled; i++) {
		seq_printf(s, "%-40s%-10u%-20llu%-20llu\n",
			sci->data[i].clock_name,
			sci->data[i].count,
			sci->data[i].last_on_ts,
			sci->data[i].last_off_ts);
	}

	kfree(sci);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qlpmm_get_adsp_clks_stats);

static void qlpmm_cleanup(void)
{
	struct qlpmm_ss_info *si, *c_next;

	debugfs_remove_recursive(qlpmm_pd->root);

	list_for_each_entry_safe(si, c_next, &qlpmm_pd->subsystem_list, node) {
		si->client_connected = false;
		list_del(&si->node);
	}

	qmi_handle_release(&qlpmm_pd->handle);
}

static int qlpmm_ss_info_init(struct device *dev,
			       u32 subsystem_id)
{
	struct qlpmm_ss_info *si;

	si = devm_kzalloc(dev, sizeof(*si), GFP_KERNEL);
	if (!si) {
		qlpmm_cleanup();
		return -ENOMEM;
	}

	si->client_connected = false;
	si->rw_flag = false;
	si->subsystem_id = subsystem_id;

	list_add_tail(&si->node, &qlpmm_pd->subsystem_list);

	return 0;
}

static int qlpmm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	qlpmm_pd = devm_kzalloc(dev, sizeof(*qlpmm_pd), GFP_KERNEL);
	if (!qlpmm_pd)
		return -ENOMEM;

	qlpmm_pd->connected_client_num = 0;
	init_completion(&qlpmm_pd->comp);
	mutex_init(&qlpmm_pd->lock);
	INIT_LIST_HEAD(&qlpmm_pd->subsystem_list);
	ret = qlpmm_ss_info_init(dev, SUBSYSTEM_ADSP_V01);
	if (ret)
		return ret;

	ret = qmi_handle_init(&qlpmm_pd->handle,
		LPM_MONITOR_REPORT_TO_APPS_REQ_MSG_V01_MAX_MSG_LEN,
		&server_ops, handlers);
	if (ret)
		return ret;

	ret = qmi_add_server(&qlpmm_pd->handle,
		LPM_MONITOR_SERVICE_ID_V01,
		LPM_MONITOR_SERVICE_VERS_V01, 0);
	if (ret) {
		dev_err(dev, "qlpmm: error adding qmi server %d\n", ret);
		qmi_handle_release(&qlpmm_pd->handle);
		return ret;
	}

	qlpmm_pd->root = debugfs_create_dir("qcom_lpmm", NULL);

	debugfs_create_file("adsp_clks", 0400, qlpmm_pd->root,
		NULL, &qlpmm_get_adsp_clks_stats_fops);

	return ret;
}

static void qlpmm_remove(struct platform_device *pdev)
{
	qlpmm_cleanup();
}

static const struct of_device_id qlpmm_table[] = {
	{ .compatible = "qcom,lpmm" },
	{ }
};

static struct platform_driver qlpmm_driver = {
	.probe = qlpmm_probe,
	.remove = qlpmm_remove,
	.driver = {
		.name = "qcom_lpm_monitor",
		.of_match_table = qlpmm_table,
	},
};
module_platform_driver(qlpmm_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. (QTI) QLPMM driver");
