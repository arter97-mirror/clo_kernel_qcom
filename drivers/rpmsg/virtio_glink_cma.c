// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of_address.h>
#include <linux/rpmsg.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/workqueue.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/rpmsg/qcom_glink.h>
#include <linux/ipc_logging.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include "qcom_glink_cma.h"
#if IS_ENABLED(CONFIG_RPMSG_GLINK_DS_QB)
#include <linux/power_mode.h>
#endif

#define VIRTIO_GLINK_DEBUG_LOG(ctxt, fmt, ...)	\
	ipc_log_string(ctxt, "%s: %d" fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

#define VIRTIO_GLINK_BRIDGE_SUCCESS (0)
#define VIRTIO_GLINK_BRIDGE_ENOMEM (-1)
#define VIRTIO_GLINK_BRIDGE_ENODEV (-2)
#define VIRTIO_GLINK_BRIDGE_EINVAL (-3)

#define VIRTIO_GLINK_BRIDGE_NO_LABEL (0xff)

/* 0xC00A  */
#define VIRTIO_ID_GLINK_BRIDGE (49162)
#define SSR_PWRUP_TIMEOUT 5000

enum {
	CDSP0,
	CDSP1,
	CDSP2,
	CDSP3,
	ADSP0,
	ADSP1,
	ADSP2,
	DSP_MAX,
	DSP_ERR = 0xff
};

static const char * const to_dsp_str[DSP_MAX] = {
	[CDSP0] = "cdsp",
	[CDSP1] = "cdsp1",
	[CDSP2] = "cdsp2",
	[CDSP3] = "cdsp3",
	[ADSP0] = "adsp",
	[ADSP1] = "adsp1",
	[ADSP2] = "adsp2",
};

#define DSP_LABEL_TO_STR(dsp) (((dsp) >= DSP_MAX) ? "INVALID DSP" : to_dsp_str[(dsp)])

enum {
	MSG_SETUP,		/* inbound / outbound */
	MSG_SETUP_ACK,		/* outbound */
	MSG_SSR_AFTER_POWERUP,	/* outbound */
	MSG_SSR_SETUP,		/* inbound */
	MSG_SSR_SETUP_ACK,	/* outbound */
	MSG_INBUF_RECLAIM,	/* inbound */
	MSG_MAX,
	MSG_ERR = 0xff,
};

enum {
	IDLE_STATE,
	DS_DONE,
	QB_IN_PROGRESS,
};

struct virtio_glink_bridge_msg {
	__virtio32 type;
	__virtio32 label;
	__virtio32 address;
	__virtio32 size;
};

struct virtio_glink_bridge_msg_ack {
	__virtio32 type;
	__virtio32 label;
	__virtio32 status;
};

struct virtio_glink_bridge_dsp_info {
	int label;
	const char *label_str;
	struct device_node *np;
	struct virtio_glink_bridge *vgbridge;

	struct glink_cma_config config;
	struct glink_cma_dev *gdev;

	struct notifier_block nb;
	void *notifier_handle;
	struct mutex ssr_lock;
	struct work_struct ssr_work;
	struct list_head node;
	wait_queue_head_t wq;
	bool pending_vq;
	bool ssr;

};

struct virtio_glink_bridge {
	struct virtio_device *vdev;
	struct virtqueue *vq;
	struct list_head dsp_infos;
	struct work_struct rx_work;
	struct workqueue_struct *ds_qb_wq;
	spinlock_t ds_qb_lock;
	int ds_qb_status;
	int dsp_count;
	void *buf;
	void *ilc;
};

static int virtio_glink_bridge_dsp_str_to_label(const char * const label_str)
{
	int i;

	for (i = CDSP0; i < DSP_MAX; i++) {
		if (!strcmp(label_str, DSP_LABEL_TO_STR(i)))
			return i;
	}

	return -EINVAL;
}

static int virtio_glink_bridge_to_msg_ack_type(u32 msg_type)
{
	switch (msg_type) {
	case MSG_SETUP:
		return MSG_SETUP_ACK;
	case MSG_SSR_SETUP:
		return MSG_SSR_SETUP_ACK;
	default:
		return MSG_ERR;
	}
}

static int virtio_glink_bridge_msg_type_supported(u32 msg_type)
{
	switch (msg_type) {
	case MSG_SETUP:
	case MSG_SSR_SETUP:
	case MSG_INBUF_RECLAIM:
		return true;
	default:
		return false;
	}
}

static int virtio_glink_bridge_send_msg(struct virtio_glink_bridge *vgbridge,
					u32 msg_type, u32 label)
{
	struct virtio_device *vdev = vgbridge->vdev;
	struct virtio_glink_bridge_msg *msg;
	struct scatterlist sg;
	int rc;

	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "msg_type %u, label %u", msg_type, label);
	msg = vgbridge->buf;
	memset(msg, 0, sizeof(*msg));
	msg->type = cpu_to_virtio32(vdev, msg_type);
	msg->label = cpu_to_virtio32(vdev, label);
	sg_init_one(&sg, msg, sizeof(struct virtio_glink_bridge_msg));

	rc = virtqueue_add_inbuf(vgbridge->vq, &sg, 1, msg, GFP_KERNEL);
	if (rc) {
		dev_err(&vdev->dev, "fail to add input buffer rc %d\n", rc);
		return rc;
	}

	virtqueue_kick(vgbridge->vq);
	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "success");

	return 0;
}

static int virtio_glink_bridge_send_msg_ack(struct virtio_glink_bridge *vgbridge,
						u32 msg_type, u32 label, int status)
{
	struct virtio_device *vdev = vgbridge->vdev;
	struct virtio_glink_bridge_msg_ack *ack;
	struct scatterlist sg;
	int rc;

	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "msg_type %u, label %u, status %d", msg_type, label,
			       status);
	ack = vgbridge->buf;
	memset(ack, 0, sizeof(*ack));
	ack->type = cpu_to_virtio32(vdev, msg_type);
	ack->label = cpu_to_virtio32(vdev, label);
	ack->status = cpu_to_virtio32(vdev, status);
	sg_init_one(&sg, ack, sizeof(struct virtio_glink_bridge_msg));

	rc = virtqueue_add_inbuf(vgbridge->vq, &sg, 1, ack, GFP_KERNEL);
	if (rc) {
		dev_err(&vdev->dev, "fail to add input buffer rc %d\n", rc);
		return rc;
	}

	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "virtqueue_kick");
	virtqueue_kick(vgbridge->vq);

	return 0;
}

static void virtio_glink_bridge_ssr_after_powerup(struct work_struct *work)
{
	struct virtio_glink_bridge_dsp_info *dsp_info = container_of(work,
						struct virtio_glink_bridge_dsp_info, ssr_work);
	unsigned long flags;
	int err;

	if (!dsp_info->vgbridge) {
		pr_err("%s: vgbridge uninitialized\n", __func__);
		return;
	}

	spin_lock_irqsave(&dsp_info->vgbridge->ds_qb_lock, flags);
	dsp_info->ssr = true;

	if (!dsp_info->pending_vq) {
		if (virtio_glink_bridge_send_msg(dsp_info->vgbridge, MSG_SSR_AFTER_POWERUP,
					     dsp_info->label) != 0) {
			pr_err("%s: send AFTER_POWERUP failed\n", __func__);
			spin_unlock_irqrestore(&dsp_info->vgbridge->ds_qb_lock, flags);
			return;
		}

		err = wait_event_lock_irq_timeout(dsp_info->wq, !dsp_info->ssr,
						  dsp_info->vgbridge->ds_qb_lock,
						  msecs_to_jiffies(SSR_PWRUP_TIMEOUT));
		if (err <= 0) {
			dsp_info->pending_vq = true;
			pr_err("%s: Failed to recv resp for MSG_SSR_AFTER_POWERUP dsp: %s err:%d\n",
				 __func__, dsp_info->label_str, err);
		}
	} else {
		pr_err("%s: Skip MSG_SSR_AFTER_POWERUP for dsp:%s, prev pkt resp pending\n",
			__func__, dsp_info->label_str);
	}
	spin_unlock_irqrestore(&dsp_info->vgbridge->ds_qb_lock, flags);
}

static void virtio_glink_bridge_ssr_after_shutdown(struct virtio_glink_bridge_dsp_info *dsp_info)
{
	if (!dsp_info || !dsp_info->gdev)
		return;
	qcom_glink_cma_unregister(dsp_info->gdev);
	dsp_info->gdev = NULL;
}

static int virtio_glink_bridge_ssr_cb(struct notifier_block *nb,
					unsigned long state, void *data)
{
	struct virtio_glink_bridge_dsp_info *dsp_info;
	struct device *dev;

	dsp_info = container_of(nb, struct virtio_glink_bridge_dsp_info, nb);

	spin_lock(&dsp_info->vgbridge->ds_qb_lock);
	if (dsp_info->vgbridge->ds_qb_status == QB_IN_PROGRESS ||
			dsp_info->vgbridge->ds_qb_status == DS_DONE) {
		VIRTIO_GLINK_DEBUG_LOG(dsp_info->vgbridge->ilc,
				       "event %lu recvd for label %s state %d skip ssr",
				       state, dsp_info->label_str,
				       dsp_info->vgbridge->ds_qb_status);
		spin_unlock(&dsp_info->vgbridge->ds_qb_lock);
		return NOTIFY_DONE;
	}
	spin_unlock(&dsp_info->vgbridge->ds_qb_lock);

	mutex_lock(&dsp_info->ssr_lock);
	dev = &dsp_info->vgbridge->vdev->dev;

	VIRTIO_GLINK_DEBUG_LOG(dsp_info->vgbridge->ilc, "received cb state %ld for %s\n",
			       state, dsp_info->label_str);

	switch (state) {
	case QCOM_SSR_AFTER_POWERUP:
		queue_work(dsp_info->vgbridge->ds_qb_wq, &dsp_info->ssr_work);
		break;
	case QCOM_SSR_AFTER_SHUTDOWN:
		virtio_glink_bridge_ssr_after_shutdown(dsp_info);
		break;
	default:
		break;
	}
	mutex_unlock(&dsp_info->ssr_lock);

	VIRTIO_GLINK_DEBUG_LOG(dsp_info->vgbridge->ilc, "success");
	return NOTIFY_DONE;
}

static struct virtio_glink_bridge_dsp_info *virtio_glink_bridge_get_dsp_info(
							struct virtio_glink_bridge *vgbridge,
							u32 label)
{
	struct virtio_glink_bridge_dsp_info *dsp_info;

	list_for_each_entry(dsp_info, &vgbridge->dsp_infos, node) {
		if (dsp_info->label == label)
			return dsp_info;
	}

	return NULL;
}

static void virtio_glink_bridge_rx_work(struct work_struct *work)
{
	struct virtio_glink_bridge *vgbridge = container_of(work, struct virtio_glink_bridge,
								rx_work);
	struct virtio_glink_bridge_dsp_info *dsp_info;
	struct virtio_device *vdev = vgbridge->vdev;
	u32 msg_type, msg_ack_type = MSG_ERR;
	struct virtio_glink_bridge_msg *msg;
	struct device *dev = &vdev->dev;
	struct glink_cma_config *config;
	u32 label, address, size;
	unsigned long flags;
	unsigned int len;
	void *handle;
	int rc;

	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "Enter");

	msg = (struct virtio_glink_bridge_msg *)virtqueue_get_buf(vgbridge->vq, &len);
	if (!msg || len != sizeof(*msg)) {
		dev_err(dev, "fail to get virtqueue buffer, len %u\n", len);
		label = VIRTIO_GLINK_BRIDGE_NO_LABEL;
		rc = VIRTIO_GLINK_BRIDGE_EINVAL;
		goto out;
	}

	label = virtio32_to_cpu(vdev, msg->label);

	msg_type = virtio32_to_cpu(vdev, msg->type);
	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "Received label %u msg_type %u\n",
			       label, msg_type);

	if (!virtio_glink_bridge_msg_type_supported(msg_type)) {
		dev_err(dev, "unsupported msg type %u\n", msg_type);
		rc = VIRTIO_GLINK_BRIDGE_EINVAL;
		goto out;
	}

	dsp_info = virtio_glink_bridge_get_dsp_info(vgbridge, label);
	if (!dsp_info) {
		if (msg_type == MSG_INBUF_RECLAIM)
			return;
		dev_err(dev, "fail to find dsp_info %u\n", label);
		rc = VIRTIO_GLINK_BRIDGE_ENODEV;
		goto out;
	}

	if (msg_type == MSG_INBUF_RECLAIM) {
		spin_lock_irqsave(&vgbridge->ds_qb_lock, flags);
		if (!dsp_info->ssr) {
			spin_unlock_irqrestore(&vgbridge->ds_qb_lock, flags);
			return;
		}
		dsp_info->ssr = false;
		spin_unlock_irqrestore(&vgbridge->ds_qb_lock, flags);
		wake_up(&dsp_info->wq);
		VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "Received RECLAIM for label %u", label);

		spin_lock_irqsave(&vgbridge->ds_qb_lock, flags);
		if (dsp_info->pending_vq) {
			pr_err("%s: Recvd delayed RECLAIM for label %s\n", __func__,
			       dsp_info->label_str);
			dsp_info->pending_vq = false;
		}

		if (vgbridge->ds_qb_status == QB_IN_PROGRESS) {
			vgbridge->dsp_count--;
			if (!vgbridge->dsp_count)
				vgbridge->ds_qb_status = IDLE_STATE;
		}
		spin_unlock_irqrestore(&vgbridge->ds_qb_lock, flags);
		return;
	}

	msg_ack_type = virtio_glink_bridge_to_msg_ack_type(msg_type);
	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "Received msg_type %u and to be sent ack_type %u",
			       msg_type, msg_ack_type);

	mutex_lock(&dsp_info->ssr_lock);

	if (dsp_info->gdev) {
		dev_err(dev, "DSP already registered\n");
		rc = VIRTIO_GLINK_BRIDGE_EINVAL;
		goto unlock;
	}
	switch (msg_type) {
	case MSG_SETUP:
		dsp_info->vgbridge = vgbridge;

		address = virtio32_to_cpu(vdev, msg->address);
		size = virtio32_to_cpu(vdev, msg->size);

		VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc,
				       "msg_type: %u label: %u address: 0x%x size: 0x%x",
				       msg_type, label, address, size);

		config = &dsp_info->config;

		config->base = devm_memremap(dev, address, size, MEMREMAP_WC);
		if (IS_ERR(config->base)) {
			dev_err(dev, "memremap fail\n");
			config->base = NULL;
			rc = VIRTIO_GLINK_BRIDGE_ENOMEM;
			goto unlock;
		}
		config->size = size;

		VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "glink cma register");
		dsp_info->gdev = qcom_glink_cma_register(dev, dsp_info->np, config);
		if (IS_ERR(dsp_info->gdev)) {
			dev_err(dev, "fail to register with GLINK CMA core\n");
			dsp_info->gdev = NULL;
			rc = VIRTIO_GLINK_BRIDGE_EINVAL;
			goto unlock;
		}

		dsp_info->nb.notifier_call = virtio_glink_bridge_ssr_cb;

		VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "ssr register");
		handle = qcom_register_ssr_notifier(dsp_info->label_str, &dsp_info->nb);
		if (IS_ERR_OR_NULL(handle)) {
			dev_err(dev, "fail to register with SSR notifier for %s\n",
				dsp_info->label_str);
			rc = VIRTIO_GLINK_BRIDGE_EINVAL;
			goto unlock;
		}

		dsp_info->notifier_handle = handle;
		break;
	case MSG_SSR_SETUP:
		dsp_info->gdev = qcom_glink_cma_register(dev, dsp_info->np, &dsp_info->config);
		if (IS_ERR(dsp_info->gdev)) {
			dev_err(dev, "fail to register with GLINK CMA core\n");
			dsp_info->gdev = NULL;
			rc = VIRTIO_GLINK_BRIDGE_EINVAL;
			goto unlock;
		}
		VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "glink cma registered after SSR");
		break;
	}

	rc = VIRTIO_GLINK_BRIDGE_SUCCESS;
unlock:
	virtio_glink_bridge_send_msg_ack(vgbridge, msg_ack_type, label, rc);
	mutex_unlock(&dsp_info->ssr_lock);
	return;
out:
	virtio_glink_bridge_send_msg_ack(vgbridge, msg_ack_type, label, rc);
}

static void virtio_glink_bridge_isr(struct virtqueue *vq)
{
	struct virtio_glink_bridge *vgbridge = vq->vdev->priv;

	schedule_work(&vgbridge->rx_work);
}

static int virtio_glink_bridge_init_vqs(struct virtio_glink_bridge *vgbridge)
{
	vgbridge->vq = virtio_find_single_vq(vgbridge->vdev, virtio_glink_bridge_isr,
						"glink_bridge");

	return PTR_ERR_OR_ZERO(vgbridge->vq);
}

static int virtio_glink_bridge_of_parse(struct virtio_glink_bridge *vgbridge)
{
	struct virtio_glink_bridge_dsp_info *dsp_info;
	struct device *dev = &vgbridge->vdev->dev;
	struct device_node *parent_np, *child_np;
	int rc = 0;

	parent_np = dev->parent->of_node->child;

	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "Enter");
	for_each_child_of_node(parent_np, child_np) {
		if (of_find_property(child_np, "compatible", NULL))
			continue;

		dsp_info = devm_kzalloc(dev, sizeof(*dsp_info), GFP_KERNEL);
		if (!dsp_info) {
			rc = -ENOMEM;
			goto out;
		}

		rc = of_property_read_string(child_np, "label", &dsp_info->label_str);
		if (rc)
			goto out;

		dsp_info->label = virtio_glink_bridge_dsp_str_to_label(dsp_info->label_str);
		if (dsp_info->label < 0) {
			rc = -EINVAL;
			goto out;
		}

		mutex_init(&dsp_info->ssr_lock);
		dsp_info->np = child_np;

		dsp_info->pending_vq = false;
		init_waitqueue_head(&dsp_info->wq);
		INIT_WORK(&dsp_info->ssr_work, virtio_glink_bridge_ssr_after_powerup);

		list_add_tail(&dsp_info->node, &vgbridge->dsp_infos);
	}
out:
	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "Exit %d", rc);
	return rc;
}

static int virtio_glink_bridge_probe(struct virtio_device *vdev)
{
	struct virtio_glink_bridge *vgbridge;
	struct device *dev = &vdev->dev;
	int rc;

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
		return -ENODEV;

	vgbridge = devm_kzalloc(dev, sizeof(struct virtio_glink_bridge), GFP_KERNEL);
	if (!vgbridge)
		return -ENOMEM;

	vgbridge->buf = devm_kzalloc(dev, sizeof(struct virtio_glink_bridge_msg), GFP_KERNEL);
	if (!vgbridge->buf)
		return -ENOMEM;

	vdev->priv = vgbridge;
	vgbridge->vdev = vdev;
	INIT_LIST_HEAD(&vgbridge->dsp_infos);
	INIT_WORK(&vgbridge->rx_work, virtio_glink_bridge_rx_work);

	vgbridge->ilc = ipc_log_context_create(2, "virtio-glink", 0);

	rc = virtio_glink_bridge_of_parse(vgbridge);
	if (rc) {
		dev_err(dev, "fail to set up dsp_infos %d\n", rc);
		return rc;
	}

	rc = virtio_glink_bridge_init_vqs(vgbridge);
	if (rc) {
		dev_err(dev, "fail to initialize virtqueue %d\n", rc);
		return rc;
	}

	virtio_device_ready(vdev);

	rc = virtio_glink_bridge_send_msg(vgbridge, MSG_SETUP, VIRTIO_GLINK_BRIDGE_NO_LABEL);
	if (rc)
		goto err;

	spin_lock_init(&vgbridge->ds_qb_lock);
	vgbridge->ds_qb_status = IDLE_STATE;
	vgbridge->ds_qb_wq = create_singlethread_workqueue("virtio_glink_wq");
	if (!vgbridge->ds_qb_wq) {
		pr_err("%s: failed to create wq\n", __func__);
		rc = -ENOMEM;
		goto err;
	}

	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "success");
	return 0;
err:
	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);
	return rc;
}

static void virtio_glink_bridge_remove(struct virtio_device *vdev)
{
	struct virtio_glink_bridge *vgbridge = vdev->priv;
	struct virtio_glink_bridge_dsp_info *dsp_info;

	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "Enter");

	list_for_each_entry(dsp_info, &vgbridge->dsp_infos, node) {
		if (dsp_info->gdev) {
			qcom_glink_cma_unregister(dsp_info->gdev);
			dsp_info->gdev = NULL;
		}
	}

	cancel_work_sync(&vgbridge->rx_work);
	destroy_workqueue(vgbridge->ds_qb_wq);

	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);

	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "success");
}

#if IS_ENABLED(CONFIG_RPMSG_GLINK_DS_QB)
static int virtio_glink_suspend(struct device *dev)
{
	struct virtio_glink_bridge_dsp_info *dsp_info;
	struct virtio_device *vdev = dev_to_virtio(dev);
	struct virtio_glink_bridge *vgbridge;
	unsigned long flags;
	int rc = -1;

	vgbridge = (struct virtio_glink_bridge *)vdev->priv;
	vgbridge->dsp_count = 0;

	rc = qcom_get_power_mode();
	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "power mode = %d\n", rc);

	/* Cleanup resources only for Deepsleep. Donot tear down channel in STR case. */
	if (rc == PM_DEEPSLEEP && pm_suspend_target_state == PM_SUSPEND_MEM) {
		spin_lock_irqsave(&vgbridge->ds_qb_lock, flags);
		vgbridge->ds_qb_status = DS_DONE;
		spin_unlock_irqrestore(&vgbridge->ds_qb_lock, flags);

		VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "virtio glink DS");

		list_for_each_entry(dsp_info, &vgbridge->dsp_infos, node) {
			vgbridge->dsp_count++;
			if (dsp_info->gdev) {
				qcom_glink_cma_unregister(dsp_info->gdev);
				dsp_info->gdev = NULL;
			}
		}
	}
	return 0;
}

static int virtio_glink_resume(struct device *dev)
{
	struct virtio_device *vdev = dev_to_virtio(dev);
	struct virtio_glink_bridge *vgbridge = (struct virtio_glink_bridge *)vdev->priv;
	struct virtio_glink_bridge_dsp_info *dsp_info;
	unsigned long flags;

	VIRTIO_GLINK_DEBUG_LOG(vgbridge->ilc, "\n");
	spin_lock_irqsave(&vgbridge->ds_qb_lock, flags);
	if (vgbridge->ds_qb_status == DS_DONE) {
		list_for_each_entry(dsp_info, &vgbridge->dsp_infos, node)
			queue_work(vgbridge->ds_qb_wq, &dsp_info->ssr_work);
		vgbridge->ds_qb_status = QB_IN_PROGRESS;
	}
	spin_unlock_irqrestore(&vgbridge->ds_qb_lock, flags);

	return 0;
}

static const struct dev_pm_ops virtio_glink_pm_ops = {
	.suspend = virtio_glink_suspend,
	.resume = virtio_glink_resume,
};
#endif /* IS_ENABLED(CONFIG_RPMSG_GLINK_DS_QB) */

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_GLINK_BRIDGE, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
};

static struct virtio_driver virtio_glink_bridge_driver = {
	.feature_table			= features,
	.feature_table_size		= ARRAY_SIZE(features),
	.driver.name			= KBUILD_MODNAME,
	.driver.owner			= THIS_MODULE,
	.id_table			= id_table,
	.probe				= virtio_glink_bridge_probe,
	.remove				= virtio_glink_bridge_remove,
#if IS_ENABLED(CONFIG_RPMSG_GLINK_DS_QB)
	.driver.pm			= &virtio_glink_pm_ops,
#endif
};

module_virtio_driver(virtio_glink_bridge_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio glink cma driver");
MODULE_LICENSE("GPL");
