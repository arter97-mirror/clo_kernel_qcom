// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#define pr_fmt(fmt)	"UCSI: %s: " fmt, __func__

#include <clocksource/arm_arch_timer.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/ipc_logging.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qti_pmic_glink.h>

#include "drivers/usb/typec/ucsi/ucsi.h"

/* PPM specific definitions */
#define MSG_OWNER_UC			32779
#define MSG_TYPE_REQ_RESP		1
#define UCSI_V1_BUF_SIZE		48
/*
 * For UCSI v2.0, MSG_IN and MSG_OUT size got increased from 16 to 256 bytes
 * thereby increasing the total buffer size to 528 bytes.
 */
#define UCSI_V2_BUF_SIZE		528
#define UCSI_BUF_SIZE			UCSI_V2_BUF_SIZE

#define UC_NOTIFY_RECEIVER_UCSI		0x0
#define UC_UCSI_READ_BUF_REQ		0x11
#define UC_UCSI_WRITE_BUF_REQ		0x12
#define UC_UCSI_USBC_NOTIFY_IND		0x13

/* Generic definitions */
#define CMD_PENDING			1
#define UCSI_LOG_BUF_SIZE		256
#define NUM_LOG_PAGES			10
#define UCSI_WAIT_TIME_MS		5000

#define ucsi_dbg(fmt, ...) \
	do { \
		ipc_log_string(ucsi_ipc_log, fmt, ##__VA_ARGS__); \
		pr_debug(fmt, ##__VA_ARGS__); \
	} while (0)

struct ucsi_read_buf_req_msg {
	struct pmic_glink_hdr	hdr;
};

struct ucsi_read_buf_resp_msg {
	struct pmic_glink_hdr	hdr;
	u8			buf[UCSI_BUF_SIZE];
	u32			ret_code;
};

struct ucsi_write_buf_req_msg {
	struct pmic_glink_hdr	hdr;
	u8			buf[UCSI_BUF_SIZE];
	u32			reserved;
};

struct ucsi_write_buf_resp_msg {
	struct pmic_glink_hdr	hdr;
	u32			ret_code;
};

struct ucsi_notify_ind_msg {
	struct pmic_glink_hdr	hdr;
	u32			notification;
	u32			receiver;
	u32			reserved;
};

struct ucsi_dev {
	struct device			*dev;
	struct ucsi			*ucsi;
	struct pmic_glink_client	*client;
	struct dentry			*debugfs_dir;
	struct completion		read_ack;
	struct completion		write_ack;
	struct completion		sync_write_ack;
	struct mutex			read_lock;
	struct mutex			write_lock;
	struct mutex			state_lock;
	struct ucsi_read_buf_resp_msg	rx_buf;
	struct ucsi_write_buf_req_msg	tx_buf;
	u16				ucsi_version;
	unsigned long			flags;
	atomic_t			rx_valid;
	struct work_struct		setup_work;
	struct work_struct		unregister_work;
	atomic_t			state;
	bool				remoteproc_timestamp_en;
};

struct remoteproc_ts {
	u32	hh;
	u32	mm;
	u32	ss;
	u32	dec;
};

static void *ucsi_ipc_log;

static char *offset_to_name(unsigned int offset)
{
	char *type;

	switch (offset) {
	case UCSI_VERSION:
		type = "VER:";
		break;
	case UCSI_CCI:
		type = "CCI:";
		break;
	case UCSI_CONTROL:
		type = "CONTROL:";
		break;
	case UCSI_MESSAGE_IN:
		type = "MSG_IN:";
		break;
	case UCSI_MESSAGE_OUT:
		type = "MSG_OUT:";
		break;
	default:
		type = "UNKNOWN:";
		break;
	}

	return type;
}

#define CLK_FREQ_KHZ		19200

static void get_remoteproc_timestamp(struct remoteproc_ts *ts)
{
	u64 us = (arch_timer_read_counter() * 1000 / CLK_FREQ_KHZ);
	u64 ss = div64_u64(us, 1000000);

	ts->dec = (u32)(us - (ss * 1000000llu));
	ts->hh = (u32)div64_u64(ss, 3600);
	ts->mm = (u32)(div64_u64(ss, 60) - (ts->hh * 60));
	ts->ss = (u32)(ss - (ts->hh * 3600 + ts->mm * 60));
}

static void ucsi_log(struct ucsi_dev *udev, const char *prefix,
			unsigned int offset, u8 *buf, size_t len)
{
	char str[UCSI_LOG_BUF_SIZE] = { 0 };
	u32 i, pos = 0;
	struct remoteproc_ts ts;

	for (i = 0; i < len && pos < sizeof(str) - 1; i++)
		pos += scnprintf(str + pos, sizeof(str) - pos, "%02x ", buf[i]);

	str[pos] = '\0';

	if (udev->remoteproc_timestamp_en) {
		get_remoteproc_timestamp(&ts);
		ucsi_dbg("%s %s %s (%02u:%02u:%02u.%06u)\n", prefix, offset_to_name(offset),
				str, ts.hh, ts.mm, ts.ss, ts.dec);
	} else {
		ucsi_dbg("%s %s %s\n", prefix, offset_to_name(offset), str);
	}
}

static size_t ucsi_get_max_len(struct ucsi_dev *udev, bool tx)
{
	size_t max_len = 0;

	if (!udev->ucsi_version || (udev->ucsi_version > UCSI_VERSION_1_2 &&
	    udev->ucsi_version <= UCSI_VERSION_2_1)) {
		/* If UCSI version is unknown, use the maximum length. */
		if (tx)
			max_len = sizeof(struct ucsi_write_buf_req_msg);
		else
			max_len = sizeof(struct ucsi_read_buf_resp_msg);
	} else if (udev->ucsi_version <= UCSI_VERSION_1_2) {
		/*
		 * This is basically following the size of struct
		 * ucsi_read_buf_resp_msg or ucsi_write_buf_req_msg
		 * but with appropriate UCSI message buffer size.
		 */
		max_len = sizeof(struct pmic_glink_hdr) + UCSI_V1_BUF_SIZE +
				sizeof(u32);
	}

	return max_len;
}

static int handle_ucsi_read_ack(struct ucsi_dev *udev, void *data, size_t len)
{
	size_t max_len;

	max_len = ucsi_get_max_len(udev, false);
	if (len > max_len) {
		pr_err("Incorrect received length %zu expected %zu\n", len,
			max_len);
		atomic_set(&udev->rx_valid, 0);
		return -EINVAL;
	}

	memset(&udev->rx_buf, 0, sizeof(udev->rx_buf));
	memcpy(&udev->rx_buf, data, len);
	if (udev->rx_buf.ret_code) {
		pr_err("ret_code: %u\n", udev->rx_buf.ret_code);
		return -EINVAL;
	}

	pr_debug("read ack\n");
	atomic_set(&udev->rx_valid, 1);
	complete(&udev->read_ack);

	return 0;
}

static int handle_ucsi_write_ack(struct ucsi_dev *udev, void *data, size_t len)
{
	struct ucsi_write_buf_resp_msg *msg_ptr;

	if (len != sizeof(*msg_ptr)) {
		pr_err("Incorrect received length %zu expected %zu\n", len,
			sizeof(*msg_ptr));
		return -EINVAL;
	}

	msg_ptr = data;
	if (msg_ptr->ret_code) {
		pr_err("ret_code: %u\n", msg_ptr->ret_code);
		return -EINVAL;
	}

	pr_debug("write ack\n");
	complete(&udev->write_ack);

	return 0;
}

static int handle_ucsi_notify(struct ucsi_dev *udev, void *data, size_t len)
{
	struct ucsi_notify_ind_msg *msg_ptr;
	struct ucsi_connector *con;
	u32 cci;
	u8 con_num;

	if (len != sizeof(*msg_ptr)) {
		pr_err("Incorrect received length %zu expected %zu\n", len,
			sizeof(*msg_ptr));
		return -EINVAL;
	}

	mutex_lock(&udev->state_lock);
	if (atomic_read(&udev->state) == PMIC_GLINK_STATE_DOWN) {
		pr_err("glink state is down\n");
		mutex_unlock(&udev->state_lock);
		return -ENOTCONN;
	}

	if (!udev->ucsi) {
		pr_err("ucsi is NULL\n");
		mutex_unlock(&udev->state_lock);
		return -ENODEV;
	}

	msg_ptr = data;
	cci = msg_ptr->notification;
	ucsi_log(udev, "notify:", UCSI_CCI, (u8 *)&cci, sizeof(cci));

	if (test_bit(CMD_PENDING, &udev->flags) &&
		cci & (UCSI_CCI_ACK_COMPLETE | UCSI_CCI_COMMAND_COMPLETE)) {
		pr_debug("received ack\n");
		complete(&udev->sync_write_ack);
	}

	con_num = UCSI_CCI_CONNECTOR(cci);
	pr_debug("con_num: %u num_connectors: %u\n", con_num,
		udev->ucsi->cap.num_connectors);

	if (con_num && con_num <= udev->ucsi->cap.num_connectors &&
		udev->ucsi->connector) {
		con = &udev->ucsi->connector[con_num - 1];
		if (con && con->ucsi)
			ucsi_connector_change(udev->ucsi, con_num);
	}
	mutex_unlock(&udev->state_lock);

	return 0;
}

static int ucsi_callback(void *priv, void *data, size_t len)
{
	struct pmic_glink_hdr *hdr = data;
	struct ucsi_dev *udev = priv;
	struct remoteproc_ts ts;

	if (udev->remoteproc_timestamp_en) {
		get_remoteproc_timestamp(&ts);
		ucsi_dbg("owner: %u type: %u opcode: %u len:%zu (%02u:%02u:%02u.%06u)\n",
			hdr->owner, hdr->type, hdr->opcode, len, ts.hh, ts.mm,
			ts.ss, ts.dec);
	} else {
		ucsi_dbg("owner: %u type: %u opcode: %u len:%zu\n", hdr->owner,
			hdr->type, hdr->opcode, len);
	}

	if (hdr->opcode == UC_UCSI_READ_BUF_REQ)
		handle_ucsi_read_ack(udev, data, len);
	else if (hdr->opcode == UC_UCSI_WRITE_BUF_REQ)
		handle_ucsi_write_ack(udev, data, len);
	else if (hdr->opcode == UC_UCSI_USBC_NOTIFY_IND)
		handle_ucsi_notify(udev, data, len);
	else
		pr_err("Unknown message opcode: %d\n", hdr->opcode);

	return 0;
}

static bool validate_ucsi_msg(struct ucsi_dev *udev, unsigned int offset,
				size_t len)
{
	size_t max_len = 0;

	if (udev->ucsi_version <= UCSI_VERSION_1_2)
		max_len = UCSI_V1_BUF_SIZE;
	else if (udev->ucsi_version <= UCSI_VERSION_2_1)
		max_len = UCSI_V2_BUF_SIZE;

	pr_debug("offset %u len %zu max_len: %zu\n", offset, len, max_len);

	if (offset + len > max_len) {
		pr_err("Incorrect length %zu or offset %u\n", len, offset);
		return false;
	}

	return true;
}

static int ucsi_qti_glink_write(struct ucsi_dev *udev, unsigned int offset,
			       const void *val, size_t val_len, bool sync)
{
	struct ucsi_write_buf_req_msg *ucsi_buf;
	size_t tx_len;
	int rc;

	if (!validate_ucsi_msg(udev, offset, val_len))
		return -EINVAL;

	ucsi_log(udev, sync ? "sync_write:" : "async_write:", offset,
			(u8 *)val, val_len);

	if (atomic_read(&udev->state) == PMIC_GLINK_STATE_DOWN)
		return 0;

	mutex_lock(&udev->write_lock);
	ucsi_buf = &udev->tx_buf;
	memset(ucsi_buf, 0, sizeof(*ucsi_buf));
	ucsi_buf->hdr.owner = MSG_OWNER_UC;
	ucsi_buf->hdr.type = MSG_TYPE_REQ_RESP;
	ucsi_buf->hdr.opcode = UC_UCSI_WRITE_BUF_REQ;
	memcpy(&ucsi_buf->buf[offset], val, val_len);
	tx_len = ucsi_get_max_len(udev, true);

	pr_debug("%s write\n", sync ? "sync" : "async");
	reinit_completion(&udev->write_ack);

	if (sync) {
		set_bit(CMD_PENDING, &udev->flags);
		reinit_completion(&udev->sync_write_ack);
	}

	rc = pmic_glink_write(udev->client, ucsi_buf, tx_len);
	if (rc < 0) {
		pr_err("Error in sending message rc=%d\n", rc);
		goto out;
	}

	rc = wait_for_completion_timeout(&udev->write_ack,
				msecs_to_jiffies(UCSI_WAIT_TIME_MS));
	if (!rc) {
		pr_err("timed out\n");
		rc = -ETIMEDOUT;
		goto out;
	} else {
		rc = 0;
	}

	if (sync) {
		rc = wait_for_completion_timeout(&udev->sync_write_ack,
					msecs_to_jiffies(UCSI_WAIT_TIME_MS));
		if (!rc) {
			pr_err("timed out for sync_write_ack\n");
			rc = -ETIMEDOUT;
			goto out;
		} else {
			rc = 0;
		}
	}

out:
	if (sync)
		clear_bit(CMD_PENDING, &udev->flags);

	mutex_unlock(&udev->write_lock);
	return rc;
}

static int ucsi_qti_async_control(struct ucsi *ucsi, u64 command)
{
	struct ucsi_dev *udev = ucsi_get_drvdata(ucsi);

	return ucsi_qti_glink_write(udev, UCSI_CONTROL, &command,
				sizeof(command), false);
}


static int ucsi_qti_sync_control(struct ucsi *ucsi, u64 command)
{
	struct ucsi_dev *udev = ucsi_get_drvdata(ucsi);

	return ucsi_qti_glink_write(udev, UCSI_CONTROL, &command,
				sizeof(command), true);
}

static int ucsi_qti_read(struct ucsi_dev *udev, unsigned int offset,
			       void *val, size_t val_len)
{
	struct ucsi_read_buf_req_msg ucsi_buf = { { 0 } };
	int rc;

	if (!validate_ucsi_msg(udev, offset, val_len))
		return -EINVAL;

	if (atomic_read(&udev->state) == PMIC_GLINK_STATE_DOWN)
		return 0;

	ucsi_buf.hdr.owner = MSG_OWNER_UC;
	ucsi_buf.hdr.type = MSG_TYPE_REQ_RESP;
	ucsi_buf.hdr.opcode = UC_UCSI_READ_BUF_REQ;

	mutex_lock(&udev->read_lock);

	pr_debug("read offset %s len %zu\n", offset_to_name(offset), val_len);
	reinit_completion(&udev->read_ack);
	rc = pmic_glink_write(udev->client, &ucsi_buf,
					sizeof(ucsi_buf));
	if (rc < 0) {
		pr_err("Error in sending message rc=%d\n", rc);
		goto out;
	}

	rc = wait_for_completion_timeout(&udev->read_ack,
				msecs_to_jiffies(UCSI_WAIT_TIME_MS));
	if (!rc) {
		pr_err("timed out\n");
		rc = -ETIMEDOUT;
		goto out;
	} else {
		rc = 0;
	}

	if (!atomic_read(&udev->rx_valid)) {
		rc = -ENODATA;
		goto out;
	}

	memcpy((u8 *)val, &udev->rx_buf.buf[offset], val_len);
	atomic_set(&udev->rx_valid, 0);
	ucsi_log(udev, "read:", offset, (u8 *)val, val_len);

out:
	mutex_unlock(&udev->read_lock);

	return rc;
}

static int ucsi_qti_read_version(struct ucsi *ucsi, u16 *version)
{
	struct ucsi_dev *udev = ucsi_get_drvdata(ucsi);

	return ucsi_qti_read(udev, UCSI_VERSION, version, sizeof(*version));
}

static int ucsi_qti_read_cci(struct ucsi *ucsi, u32 *cci)
{
	struct ucsi_dev *udev = ucsi_get_drvdata(ucsi);

	return ucsi_qti_read(udev, UCSI_CCI, cci, sizeof(*cci));
}

static int ucsi_qti_read_message_in(struct ucsi *ucsi, void *val,
					size_t val_len)
{
	struct ucsi_dev *udev = ucsi_get_drvdata(ucsi);

	return ucsi_qti_read(udev, UCSI_MESSAGE_IN, val, val_len);
}

static const struct ucsi_operations ucsi_qti_ops = {
	.read_version = ucsi_qti_read_version,
	.read_cci = ucsi_qti_read_cci,
	.poll_cci = ucsi_qti_read_cci,
	.read_message_in = ucsi_qti_read_message_in,
	.sync_control = ucsi_qti_sync_control,
	.async_control = ucsi_qti_async_control,
};

static int ucsi_setup(struct ucsi_dev *udev)
{
	int rc;

	mutex_lock(&udev->state_lock);
	if (udev->ucsi) {
		dev_err(udev->dev, "ucsi is not NULL\n");
		mutex_unlock(&udev->state_lock);
		return -EINVAL;
	}

	udev->ucsi = ucsi_create(udev->dev, &ucsi_qti_ops);
	if (IS_ERR(udev->ucsi)) {
		rc = PTR_ERR(udev->ucsi);
		dev_err(udev->dev, "ucsi_create failed rc=%d\n", rc);
		udev->ucsi = NULL;
		mutex_unlock(&udev->state_lock);
		return rc;
	}

	ucsi_set_drvdata(udev->ucsi, udev);

	rc = ucsi_register(udev->ucsi);
	if (rc) {
		dev_err(udev->dev, "ucsi_register failed rc=%d\n", rc);
		ucsi_destroy(udev->ucsi);
		udev->ucsi = NULL;
		mutex_unlock(&udev->state_lock);
		return rc;
	}

	mutex_unlock(&udev->state_lock);
	return 0;
}

static void ucsi_qti_setup_work(struct work_struct *work)
{
	struct ucsi_dev *udev = container_of(work, struct ucsi_dev,
			setup_work);

	ucsi_setup(udev);
}

static void ucsi_qti_unregister_work(struct work_struct *work)
{
	struct ucsi_dev *udev = container_of(work, struct ucsi_dev,
			unregister_work);
	struct ucsi *ucsi = NULL;

	/*
	 * NULL out udev->ucsi under state_lock first so that any concurrent
	 * handle_ucsi_notify() sees NULL and bails out safely, preventing a
	 * use-after-free on the ucsi object being torn down.
	 *
	 * ucsi_unregister() must be called outside state_lock because it
	 * calls cancel_work_sync() on connector works, which themselves
	 * acquire state_lock via handle_ucsi_notify() -- holding state_lock
	 * here would deadlock.
	 */
	mutex_lock(&udev->state_lock);
	if (udev->ucsi) {
		ucsi = udev->ucsi;
		udev->ucsi = NULL;
	}
	mutex_unlock(&udev->state_lock);

	if (!ucsi) {
		dev_dbg(udev->dev, "ucsi is NULL\n");
		return;
	}

	ucsi_unregister(ucsi);
	ucsi_destroy(ucsi);
}

static void ucsi_qti_state_cb(void *priv, enum pmic_glink_state state)
{
	struct ucsi_dev *udev = priv;

	dev_dbg(udev->dev, "state: %d\n", state);

	mutex_lock(&udev->state_lock);
	atomic_set(&udev->state, state);
	mutex_unlock(&udev->state_lock);

	switch (state) {
	case PMIC_GLINK_STATE_DOWN:
		cancel_work_sync(&udev->setup_work);
		schedule_work(&udev->unregister_work);
		break;
	case PMIC_GLINK_STATE_UP:
		cancel_work_sync(&udev->unregister_work);
		schedule_work(&udev->setup_work);
		break;
	default:
		break;
	}
}

static int ucsi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pmic_glink_client_data client_data;
	struct ucsi_dev *udev;
	u16 version = 0;
	int rc;

	udev = devm_kzalloc(dev, sizeof(*udev), GFP_KERNEL);
	if (!udev)
		return -ENOMEM;

	INIT_WORK(&udev->setup_work, ucsi_qti_setup_work);
	INIT_WORK(&udev->unregister_work, ucsi_qti_unregister_work);
	mutex_init(&udev->read_lock);
	mutex_init(&udev->write_lock);
	mutex_init(&udev->state_lock);
	init_completion(&udev->read_ack);
	init_completion(&udev->write_ack);
	init_completion(&udev->sync_write_ack);
	atomic_set(&udev->rx_valid, 0);
	atomic_set(&udev->state, PMIC_GLINK_STATE_UP);

	client_data.id = MSG_OWNER_UC;
	client_data.name = "ucsi";
	client_data.msg_cb = ucsi_callback;
	client_data.priv = udev;
	client_data.state_cb = ucsi_qti_state_cb;

	udev->client = pmic_glink_register_client(dev, &client_data);
	if (IS_ERR(udev->client)) {
		rc = PTR_ERR(udev->client);
		if (rc != -EPROBE_DEFER)
			dev_err(dev, "Error in registering with pmic_glink rc=%d\n",
				rc);
		return rc;
	}

	rc = ucsi_qti_read(udev, UCSI_VERSION, &version, sizeof(version));
	if (rc < 0)  {
		dev_err(dev, "Error reading version rc=%d\n", rc);
		pmic_glink_unregister_client(udev->client);
		return rc;
	}

	if (version == 0 || version > UCSI_VERSION_2_1) {
		dev_err(dev, "UCSI version %x not supported\n", version);
		pmic_glink_unregister_client(udev->client);
		return -ENODEV;
	}

	dev_info(dev, "UCSI version: %x\n", version);
	udev->ucsi_version = version;

	platform_set_drvdata(pdev, udev);
	udev->dev = dev;

	ucsi_ipc_log = ipc_log_context_create(NUM_LOG_PAGES, "ucsi", 0);
	if (!ucsi_ipc_log)
		dev_warn(dev, "Error in creating ipc_log_context\n");

	udev->debugfs_dir = debugfs_create_dir("ucsi_glink", NULL);
	if (IS_ERR(udev->debugfs_dir)) {
		dev_err(dev, "Failed to create ucsi_glink directory: %ld\n",
			PTR_ERR(udev->debugfs_dir));
		udev->debugfs_dir = NULL;
	} else {
		debugfs_create_bool("remoteproc_timestamp_en", 0600,
				udev->debugfs_dir,
				&udev->remoteproc_timestamp_en);
	}

	rc = ucsi_setup(udev);
	if (rc) {
		debugfs_remove_recursive(udev->debugfs_dir);
		ipc_log_context_destroy(ucsi_ipc_log);
		ucsi_ipc_log = NULL;
		pmic_glink_unregister_client(udev->client);
	}

	return rc;
}

static void ucsi_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ucsi_dev *udev = dev_get_drvdata(dev);
	struct ucsi *ucsi = NULL;
	int rc;

	debugfs_remove_recursive(udev->debugfs_dir);
	rc = pmic_glink_unregister_client(udev->client);
	if (rc < 0)
		dev_err(dev, "pmic_glink_unregister_client failed rc=%d\n",
			rc);

	cancel_work_sync(&udev->setup_work);
	cancel_work_sync(&udev->unregister_work);

	mutex_lock(&udev->state_lock);
	if (udev->ucsi) {
		ucsi = udev->ucsi;
		udev->ucsi = NULL;
	}
	mutex_unlock(&udev->state_lock);

	if (ucsi) {
		ucsi_unregister(ucsi);
		ucsi_destroy(ucsi);
	}

	ipc_log_context_destroy(ucsi_ipc_log);
	ucsi_ipc_log = NULL;
}

static const struct of_device_id ucsi_match_table[] = {
	{.compatible = "qcom,ucsi-glink"},
	{},
};

static struct platform_driver ucsi_driver = {
	.driver	= {
		.name = "ucsi_glink",
		.of_match_table = ucsi_match_table,
	},
	.probe	= ucsi_probe,
	.remove	= ucsi_remove,
};

module_platform_driver(ucsi_driver);

MODULE_DESCRIPTION("QTI UCSI Glink driver");
MODULE_LICENSE("GPL");
