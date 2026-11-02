// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/qti_pmic_glink.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define BATTERY_TELEMETRY_START_REQ	0x0050
#define BATTERY_TELEMETRY_STOP_REQ	0x0051
#define BATTERY_TELEMETRY_GET_DATA_REQ	0x0052

/*
 * Unique pmic_glink owner ID for the remote subsystem battery telemetry
 * service. Must match the owner assigned in the remote subsystem firmware.
 */
#define PMIC_GLINK_MSG_OWNER_BATT_TELEMETRY	32786
#define MSG_TYPE_REQ_RESP			1

#define TELEMETRY_SAMPLES_PER_CHUNK	1024
#define TELEMETRY_TOTAL_CHUNKS		32
#define TELEMETRY_TOTAL_SAMPLES		(TELEMETRY_SAMPLES_PER_CHUNK * \
					 TELEMETRY_TOTAL_CHUNKS)
#define TELEMETRY_ACK_TIMEOUT_MS	5000
#define TELEMETRY_CHUNK_TIMEOUT_MS	10000

enum telemetry_state {
	TELEMETRY_STATE_IDLE,
	TELEMETRY_STATE_STARTING,
	TELEMETRY_STATE_RUNNING,
	TELEMETRY_STATE_READY,
	TELEMETRY_STATE_STOPPING,
};

struct batt_telemetry_sample {
	s16 voltage;
	s16 curr;
} __packed;

struct telemetry_start_req_msg {
	struct pmic_glink_hdr hdr;
} __packed;

struct telemetry_stop_req_msg {
	struct pmic_glink_hdr hdr;
} __packed;

struct telemetry_get_data_req_msg {
	struct pmic_glink_hdr hdr;
	s32 data_chunk_id;
} __packed;

struct telemetry_start_resp_msg {
	struct pmic_glink_hdr hdr;
	s32 return_status;
} __packed;

struct telemetry_stop_resp_msg {
	struct pmic_glink_hdr hdr;
	s32 no_of_samples;
} __packed;

struct telemetry_get_data_resp_msg {
	struct pmic_glink_hdr hdr;
	s32 data_chunk_id;
	struct batt_telemetry_sample samples[TELEMETRY_SAMPLES_PER_CHUNK];
} __packed;

struct batt_telemetry_dev {
	struct batt_telemetry_sample *buffer;
	struct pmic_glink_client *client;
	struct work_struct work;
	struct completion ack_complete;
	struct completion chunk_complete;
	struct mutex lock;
	struct device *dev;
	size_t data_size;
	s32 total_samples;
	s32 chunks_received;
	enum telemetry_state state;
	bool channel_down;
	bool start_failed;
};

static int telemetry_send_start(struct batt_telemetry_dev *tdev)
{
	struct telemetry_start_req_msg msg = {};

	msg.hdr.owner = PMIC_GLINK_MSG_OWNER_BATT_TELEMETRY;
	msg.hdr.type = MSG_TYPE_REQ_RESP;
	msg.hdr.opcode = BATTERY_TELEMETRY_START_REQ;

	return pmic_glink_write(tdev->client, &msg, sizeof(msg));
}

static int telemetry_send_stop(struct batt_telemetry_dev *tdev)
{
	struct telemetry_stop_req_msg msg = {};

	msg.hdr.owner = PMIC_GLINK_MSG_OWNER_BATT_TELEMETRY;
	msg.hdr.type = MSG_TYPE_REQ_RESP;
	msg.hdr.opcode = BATTERY_TELEMETRY_STOP_REQ;

	return pmic_glink_write(tdev->client, &msg, sizeof(msg));
}

static int telemetry_send_get_chunk(struct batt_telemetry_dev *tdev,
				    s32 chunk_id)
{
	struct telemetry_get_data_req_msg msg = {};

	msg.hdr.owner = PMIC_GLINK_MSG_OWNER_BATT_TELEMETRY;
	msg.hdr.type = MSG_TYPE_REQ_RESP;
	msg.hdr.opcode = BATTERY_TELEMETRY_GET_DATA_REQ;
	msg.data_chunk_id = chunk_id;

	return pmic_glink_write(tdev->client, &msg, sizeof(msg));
}

static void telemetry_set_state(struct batt_telemetry_dev *tdev,
				enum telemetry_state old_state,
				enum telemetry_state new_state)
{
	guard(mutex)(&tdev->lock);
	if (tdev->state == old_state && !READ_ONCE(tdev->channel_down))
		tdev->state = new_state;
}

static int telemetry_fetch_all_chunks(struct batt_telemetry_dev *tdev)
{
	s32 total_samples;
	s32 num_chunks;
	s32 chunk_id;
	long timeout;
	int ret;

	{
		guard(mutex)(&tdev->lock);
		if (tdev->state != TELEMETRY_STATE_STOPPING ||
		    READ_ONCE(tdev->channel_down))
			return -ENODEV;

		tdev->chunks_received = 0;
		total_samples = tdev->total_samples;
	}

	num_chunks = DIV_ROUND_UP(total_samples, TELEMETRY_SAMPLES_PER_CHUNK);
	num_chunks = min_t(s32, num_chunks, TELEMETRY_TOTAL_CHUNKS);

	for (chunk_id = 0; chunk_id < num_chunks; chunk_id++) {
		reinit_completion(&tdev->chunk_complete);
		ret = telemetry_send_get_chunk(tdev, chunk_id);
		if (ret) {
			dev_err(tdev->dev, "Failed to send GET_DATA request: %d\n", ret);
			goto out_idle;
		}

		timeout = wait_for_completion_timeout(&tdev->chunk_complete,
						      msecs_to_jiffies(TELEMETRY_CHUNK_TIMEOUT_MS));
		if (!timeout) {
			dev_err(tdev->dev, "Chunk %d response timed out\n", chunk_id);
			ret = -ETIMEDOUT;
			goto out_idle;
		}

		if (READ_ONCE(tdev->channel_down))
			return -ENODEV;
	}

	{
		guard(mutex)(&tdev->lock);
		if (tdev->state != TELEMETRY_STATE_STOPPING ||
		    READ_ONCE(tdev->channel_down))
			return -ENODEV;

		tdev->data_size = total_samples * sizeof(*tdev->buffer);
		tdev->state = TELEMETRY_STATE_READY;
	}

	return 0;

out_idle:
	telemetry_set_state(tdev, TELEMETRY_STATE_STOPPING, TELEMETRY_STATE_IDLE);
	return ret;
}

static void telemetry_start(struct batt_telemetry_dev *tdev)
{
	long timeout;
	int ret;

	{
		guard(mutex)(&tdev->lock);
		if (tdev->state != TELEMETRY_STATE_STARTING ||
		    READ_ONCE(tdev->channel_down))
			return;

		tdev->start_failed = false;
		reinit_completion(&tdev->ack_complete);
	}

	ret = telemetry_send_start(tdev);
	if (ret) {
		dev_err(tdev->dev, "Failed to send START request: %d\n", ret);
		goto out_idle;
	}

	timeout = wait_for_completion_timeout(&tdev->ack_complete,
					      msecs_to_jiffies(TELEMETRY_ACK_TIMEOUT_MS));
	if (!timeout) {
		dev_err(tdev->dev, "START response timed out\n");
		goto out_idle;
	}

	{
		guard(mutex)(&tdev->lock);
		if (tdev->state != TELEMETRY_STATE_STARTING ||
		    READ_ONCE(tdev->channel_down))
			return;

		if (!tdev->start_failed) {
			tdev->state = TELEMETRY_STATE_RUNNING;
			return;
		}
	}

	dev_err(tdev->dev, "Telemetry START rejected by remote subsystem\n");
out_idle:
	telemetry_set_state(tdev, TELEMETRY_STATE_STARTING, TELEMETRY_STATE_IDLE);
}

static void telemetry_stop(struct batt_telemetry_dev *tdev)
{
	long timeout;
	int ret;

	{
		guard(mutex)(&tdev->lock);
		if (tdev->state != TELEMETRY_STATE_STOPPING ||
		    READ_ONCE(tdev->channel_down))
			return;

		reinit_completion(&tdev->ack_complete);
	}

	ret = telemetry_send_stop(tdev);
	if (ret) {
		dev_err(tdev->dev, "Failed to send STOP request: %d\n", ret);
		goto out_running;
	}

	timeout = wait_for_completion_timeout(&tdev->ack_complete,
					      msecs_to_jiffies(TELEMETRY_ACK_TIMEOUT_MS));
	if (!timeout) {
		dev_err(tdev->dev, "STOP response timed out\n");
		goto out_running;
	}

	if (READ_ONCE(tdev->channel_down))
		return;

	telemetry_fetch_all_chunks(tdev);
	return;

out_running:
	telemetry_set_state(tdev, TELEMETRY_STATE_STOPPING,
			    TELEMETRY_STATE_RUNNING);
}

static void batt_telemetry_work(struct work_struct *work)
{
	struct batt_telemetry_dev *tdev = container_of(work, struct batt_telemetry_dev,
						       work);
	enum telemetry_state state;

	{
		guard(mutex)(&tdev->lock);
		if (READ_ONCE(tdev->channel_down))
			return;

		state = tdev->state;
	}

	switch (state) {
	case TELEMETRY_STATE_STARTING:
		telemetry_start(tdev);
		break;
	case TELEMETRY_STATE_STOPPING:
		telemetry_stop(tdev);
		break;
	default:
		break;
	}
}

static int telemetry_handle_start_response(struct batt_telemetry_dev *tdev,
					   void *data, size_t len)
{
	struct telemetry_start_resp_msg *start_resp = data;

	if (len < sizeof(*start_resp))
		return -EINVAL;

	guard(mutex)(&tdev->lock);
	if (tdev->state != TELEMETRY_STATE_STARTING ||
	    READ_ONCE(tdev->channel_down))
		return -ENODEV;

	tdev->start_failed = start_resp->return_status;
	complete(&tdev->ack_complete);
	return 0;
}

static int telemetry_handle_stop_response(struct batt_telemetry_dev *tdev,
					  void *data, size_t len)
{
	struct telemetry_stop_resp_msg *stop_resp = data;

	if (len < sizeof(*stop_resp))
		return -EINVAL;

	guard(mutex)(&tdev->lock);
	if (tdev->state != TELEMETRY_STATE_STOPPING ||
	    READ_ONCE(tdev->channel_down))
		return -ENODEV;

	tdev->total_samples = clamp_t(s32, stop_resp->no_of_samples, 0,
				      TELEMETRY_TOTAL_SAMPLES);
	complete(&tdev->ack_complete);
	return 0;
}

static int telemetry_handle_chunk_response(struct batt_telemetry_dev *tdev,
					   void *data, size_t len)
{
	struct telemetry_get_data_resp_msg *chunk_resp = data;
	s32 chunk_id;

	if (len < sizeof(*chunk_resp))
		return -EINVAL;

	chunk_id = chunk_resp->data_chunk_id;
	if (chunk_id < 0 || chunk_id >= TELEMETRY_TOTAL_CHUNKS)
		return -EINVAL;

	guard(mutex)(&tdev->lock);
	if (tdev->state != TELEMETRY_STATE_STOPPING ||
	    READ_ONCE(tdev->channel_down))
		return -ENODEV;

	if (chunk_id != tdev->chunks_received)
		return -EINVAL;

	memcpy(&tdev->buffer[chunk_id * TELEMETRY_SAMPLES_PER_CHUNK],
	       chunk_resp->samples, sizeof(chunk_resp->samples));
	tdev->chunks_received++;
	complete(&tdev->chunk_complete);
	return 0;
}

/**
 * batt_telemetry_msg_callback() - GLink RX callback
 * @priv: battery telemetry device context
 * @data: message payload received from the remote subsystem
 * @len: length of @data in bytes
 *
 * Called by pmic_glink framework when the remote subsystem sends a message.
 *
 * Return: 0 on success, negative error values upon failure
 */
static int batt_telemetry_msg_callback(void *priv, void *data, size_t len)
{
	struct batt_telemetry_dev *tdev = priv;
	struct pmic_glink_hdr *hdr = data;

	if (len < sizeof(*hdr))
		return -EINVAL;

	switch (hdr->opcode) {
	case BATTERY_TELEMETRY_START_REQ:
		return telemetry_handle_start_response(tdev, data, len);
	case BATTERY_TELEMETRY_STOP_REQ:
		return telemetry_handle_stop_response(tdev, data, len);
	case BATTERY_TELEMETRY_GET_DATA_REQ:
		return telemetry_handle_chunk_response(tdev, data, len);
	default:
		dev_warn(tdev->dev, "Unknown opcode 0x%04x received\n", hdr->opcode);
		return -EINVAL;
	}
}

static void batt_telemetry_state_callback(void *priv, enum pmic_glink_state state)
{
	struct batt_telemetry_dev *tdev = priv;

	if (state == PMIC_GLINK_STATE_DOWN) {
		WRITE_ONCE(tdev->channel_down, true);
		complete_all(&tdev->ack_complete);
		complete_all(&tdev->chunk_complete);

		guard(mutex)(&tdev->lock);
		tdev->state = TELEMETRY_STATE_IDLE;
		tdev->start_failed = false;
		tdev->total_samples = 0;
		tdev->chunks_received = 0;
		tdev->data_size = 0;
		memset(tdev->buffer, 0,
		       sizeof(*tdev->buffer) * TELEMETRY_TOTAL_SAMPLES);
	} else if (state == PMIC_GLINK_STATE_UP)
		WRITE_ONCE(tdev->channel_down, false);
}

static ssize_t telemetry_control_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct batt_telemetry_dev *tdev = dev_get_drvdata(dev);

	if (sysfs_streq(buf, "start")) {
		{
			guard(mutex)(&tdev->lock);
			if (READ_ONCE(tdev->channel_down))
				return -ENODEV;
			if (tdev->state != TELEMETRY_STATE_IDLE &&
			    tdev->state != TELEMETRY_STATE_READY)
				return -EBUSY;

			tdev->total_samples = 0;
			tdev->chunks_received = 0;
			tdev->data_size = 0;
			tdev->state = TELEMETRY_STATE_STARTING;
			memset(tdev->buffer, 0,
			       sizeof(*tdev->buffer) * TELEMETRY_TOTAL_SAMPLES);
		}
	} else if (sysfs_streq(buf, "stop")) {
		{
			guard(mutex)(&tdev->lock);
			if (READ_ONCE(tdev->channel_down))
				return -ENODEV;
			if (tdev->state != TELEMETRY_STATE_RUNNING)
				return -EINVAL;

			tdev->data_size = 0;
			tdev->state = TELEMETRY_STATE_STOPPING;
		}
	} else {
		return -EINVAL;
	}

	schedule_work(&tdev->work);
	return count;
}
static DEVICE_ATTR_WO(telemetry_control);

static ssize_t telemetry_status_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct batt_telemetry_dev *tdev = dev_get_drvdata(dev);
	const char *status;

	guard(mutex)(&tdev->lock);
	switch (tdev->state) {
	case TELEMETRY_STATE_STARTING:
		status = "starting";
		break;
	case TELEMETRY_STATE_RUNNING:
		status = "running";
		break;
	case TELEMETRY_STATE_STOPPING:
		status = "stopping";
		break;
	case TELEMETRY_STATE_READY:
		status = "ready";
		break;
	default:
		status = "idle";
		break;
	}

	return sysfs_emit(buf, "%s\n", status);
}
static DEVICE_ATTR_RO(telemetry_status);

static ssize_t telemetry_data_read(struct file *filp, struct kobject *kobj,
				   struct bin_attribute *attr, char *buf, loff_t off,
				   size_t count)
{
	struct batt_telemetry_dev *tdev = dev_get_drvdata(kobj_to_dev(kobj));

	guard(mutex)(&tdev->lock);
	if (!tdev->data_size)
		return -ENODATA;

	return memory_read_from_buffer(buf, count, &off, tdev->buffer,
				       tdev->data_size);
}
static BIN_ATTR_RO(telemetry_data, 0);

static struct attribute *telemetry_attrs[] = {
	&dev_attr_telemetry_control.attr,
	&dev_attr_telemetry_status.attr,
	NULL,
};

static struct bin_attribute *telemetry_bin_attrs[] = {
	&bin_attr_telemetry_data,
	NULL,
};

static const struct attribute_group telemetry_group = {
	.name = "batt_telemetry",
	.attrs = telemetry_attrs,
	.bin_attrs = telemetry_bin_attrs,
};
__ATTRIBUTE_GROUPS(telemetry);

static void batt_telemetry_free_buffer(void *data)
{
	kvfree(data);
}

static int batt_telemetry_probe(struct platform_device *pdev)
{
	struct pmic_glink_client_data client_data = {};
	struct batt_telemetry_dev *tdev;
	struct device *hwmon_dev;
	int ret;

	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->buffer = kvzalloc(sizeof(*tdev->buffer) * TELEMETRY_TOTAL_SAMPLES,
				GFP_KERNEL);
	if (!tdev->buffer)
		return -ENOMEM;

	ret = devm_add_action_or_reset(&pdev->dev, batt_telemetry_free_buffer,
				       tdev->buffer);
	if (ret)
		return ret;

	tdev->dev = &pdev->dev;
	tdev->state = TELEMETRY_STATE_IDLE;
	mutex_init(&tdev->lock);
	init_completion(&tdev->ack_complete);
	init_completion(&tdev->chunk_complete);
	INIT_WORK(&tdev->work, batt_telemetry_work);
	platform_set_drvdata(pdev, tdev);

	client_data.id = PMIC_GLINK_MSG_OWNER_BATT_TELEMETRY;
	client_data.name = "batt_telemetry";
	client_data.msg_cb = batt_telemetry_msg_callback;
	client_data.state_cb = batt_telemetry_state_callback;
	client_data.priv = tdev;
	tdev->client = pmic_glink_register_client(&pdev->dev, &client_data);
	if (IS_ERR(tdev->client)) {
		ret = PTR_ERR(tdev->client);
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register pmic_glink client\n");
	}

	hwmon_dev = devm_hwmon_device_register_with_groups(&pdev->dev,
							   "batt_telemetry", tdev,
							    telemetry_groups);
	if (IS_ERR(hwmon_dev)) {
		ret = PTR_ERR(hwmon_dev);
		pmic_glink_unregister_client(tdev->client);
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register hwmon device\n");
	}

	return 0;
}

static void batt_telemetry_remove(struct platform_device *pdev)
{
	struct batt_telemetry_dev *tdev = platform_get_drvdata(pdev);

	cancel_work_sync(&tdev->work);
	pmic_glink_unregister_client(tdev->client);
}

static const struct of_device_id batt_telemetry_match_table[] = {
	{ .compatible = "qcom,batt-telemetry" },
	{}
};
MODULE_DEVICE_TABLE(of, batt_telemetry_match_table);

static struct platform_driver batt_telemetry_driver = {
	.driver = {
		.name = "qti_batt_telemetry",
		.of_match_table = batt_telemetry_match_table,
	},
	.probe = batt_telemetry_probe,
	.remove = batt_telemetry_remove,
};
module_platform_driver(batt_telemetry_driver);

MODULE_DESCRIPTION("QTI Battery Telemetry driver");
MODULE_LICENSE("GPL");
