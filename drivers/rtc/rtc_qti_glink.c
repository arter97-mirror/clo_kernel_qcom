// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qti_pmic_glink.h>
#include <linux/timekeeping.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define MSG_OWNER_RTC			32784
#define MSG_TYPE_REQ_RESP		1
#define RTC_GLINK_SET_ALARM_POLICY	0x62
#define RTC_GLINK_SET_ALARM_TIME	0x63
#define RTC_GLINK_GET_ALARM_POLICY	0x64
#define RTC_GLINK_GET_ALARM_TIME	0x65
#define RTC_GLINK_GET_REAL_TIME		0x68
#define RTC_GLINK_SET_REAL_TIME		0x69
#define RTC_GLINK_ALARM_EXPIRED		0x6A
#define RTC_GLINK_WAIT_TIME_MS		5000
#define RTC_GLINK_TAD_DC_TIMER_ID	1

struct rtc_glink_tad_real_time_data {
	u32 year : 16;        /* 1900 - 9999 */
	u32 month : 8;        /* 1 - 12 */
	u32 day : 8;          /* 1 - 31 */
	u32 hour : 8;         /* 0 - 23 */
	u32 minute : 8;       /* 0 - 59 */
	u32 sec : 8;          /* 0 - 59 */
	u32 valid : 8;        /* valid or not */
	u32 millisecond : 16; /* 1 - 1000 */
	u32 timezone : 16;    /* -1440 to 1440 or 2047 for unspecified */
	u32 daylight : 8;
	u32 reserved : 24;
};

/* 0x68: Get Real Time request */
struct rtc_glink_tad_generic_req {
	struct pmic_glink_hdr hdr;
};

/* 0x68: Get Real Time response */
struct rtc_glink_tad_grt_resp {
	struct pmic_glink_hdr hdr;
	__le32 return_status;
	__le32 real_time_data[4];
};

/* Response struct for 0x62, 0x63, 0x64, 0x65, 0x69 */
struct rtc_glink_tad_generic_resp {
	struct pmic_glink_hdr hdr;
	__le32 return_status;
};

/* 0x69: Set Real Time request */
struct rtc_glink_tad_srt_req {
	struct pmic_glink_hdr hdr;
	__le32 real_time_data[4];
};

/*
 * 0x62: Set Alarm Policy request
 * 0x63: Set Alarm Time request
 */
struct rtc_glink_tad_stv_req {
	struct pmic_glink_hdr hdr;
	__le32 timer_id;
	__le32 value;  /* policy_setting or timer_value */
};

/*
 * 0x64: Get Alarm Policy request
 * 0x65: Get Alarm Time request
 */
struct rtc_glink_tad_gtv_req {
	struct pmic_glink_hdr hdr;
	__le32 timer_id;
};

struct rtc_glink_dev {
	struct device *dev;
	struct pmic_glink_client *client;
	struct rtc_device *rtc;
	bool allow_set_time;
	struct rw_semaphore state_sem;
	atomic_t state;
	bool initialized;
	struct mutex transaction_lock;
	struct completion ack;
	int error;
	u32 resp_value;
	struct rtc_time resp_tm;
	bool alarm_pending;
	struct work_struct alarm_work;
};

#define GPS_EPOCH_OFFSET_SECS  315964800ULL  /* secs from 1970-01-01 to 1980-01-06 */

static void rtc_glink_rtc_time_to_tad_payload(struct rtc_time *tm,
					      __le32 data[4])
{
	time64_t gps_secs;
	struct rtc_time gps_tm;
	struct rtc_glink_tad_real_time_data t;

	/*
	 * The ADSP expects time relative to the GPS epoch (1980-01-06).
	 * Convert from Unix epoch (1970-01-01) by adding the fixed
	 * offset before packing the payload.
	 */
	gps_secs = rtc_tm_to_time64(tm) + GPS_EPOCH_OFFSET_SECS;
	rtc_time64_to_tm(gps_secs, &gps_tm);

	t = (struct rtc_glink_tad_real_time_data) {
		.year   = gps_tm.tm_year + 1900, .month = gps_tm.tm_mon  + 1,
		.day    = gps_tm.tm_mday, .hour = gps_tm.tm_hour,
		.minute = gps_tm.tm_min, .sec = gps_tm.tm_sec,
		.valid  = 1, .timezone = 2047,
	};

	data[0] = cpu_to_le32((u32)t.year | (u32)t.month << 16 | (u32)t.day << 24);
	data[1] = cpu_to_le32((u32)t.hour | (u32)t.minute << 8 |
			      (u32)t.sec << 16 | (u32)t.valid << 24);
	data[2] = cpu_to_le32((u32)t.millisecond | (u32)t.timezone << 16);
	data[3] = cpu_to_le32((u32)t.daylight);
}

static int rtc_glink_tad_payload_to_rtc_time(const __le32 data[4],
					     struct rtc_time *tm)
{
	time64_t gps_secs;
	struct rtc_glink_tad_real_time_data t;

	u32 w0 = le32_to_cpu(data[0]);
	u32 w1 = le32_to_cpu(data[1]);
	u32 w2 = le32_to_cpu(data[2]);
	u32 w3 = le32_to_cpu(data[3]);

	t.year = w0 & 0xffff;
	t.month = (w0 >> 16) & 0xff;
	t.day = (w0 >> 24) & 0xff;
	t.hour = w1 & 0xff;
	t.minute = (w1 >> 8) & 0xff;
	t.sec = (w1 >> 16) & 0xff;
	t.valid = (w1 >> 24) & 0xff;
	t.millisecond = w2 & 0xffff;
	t.timezone = (w2 >> 16) & 0xffff;
	t.daylight = w3 & 0xff;

	if (!t.valid)
		return -EINVAL;

	tm->tm_year = t.year - 1900;
	tm->tm_mon = t.month - 1;
	tm->tm_mday = t.day;
	tm->tm_hour = t.hour;
	tm->tm_min = t.minute;
	tm->tm_sec = t.sec;

	/*
	 * Remote processor always sends time relative to GPS epoch (1980-01-06).
	 * Subtract GPS offset to convert to Unix epoch (1970-01-01).
	 */
	gps_secs = rtc_tm_to_time64(tm);
	rtc_time64_to_tm(gps_secs - GPS_EPOCH_OFFSET_SECS, tm);

	return 0;
}

static int rtc_glink_send(struct rtc_glink_dev *rtc_glink, void *data, size_t len,
			  void *resp_out, size_t resp_size)
{
	int ret;
	long left;

	down_read(&rtc_glink->state_sem);
	if (!rtc_glink->initialized ||
	    atomic_read(&rtc_glink->state) != PMIC_GLINK_STATE_UP) {
		up_read(&rtc_glink->state_sem);
		return -ENOTCONN;
	}
	up_read(&rtc_glink->state_sem);

	mutex_lock(&rtc_glink->transaction_lock);

	reinit_completion(&rtc_glink->ack);
	rtc_glink->error = 0;

	ret = pmic_glink_write(rtc_glink->client, data, len);
	if (ret < 0)
		goto out_unlock;

	left = wait_for_completion_timeout(&rtc_glink->ack,
					   msecs_to_jiffies(RTC_GLINK_WAIT_TIME_MS));
	if (!left) {
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	ret = rtc_glink->error;

	if (ret == 0 && resp_out) {
		if (resp_size == sizeof(u32))
			*(u32 *)resp_out = rtc_glink->resp_value;
		else if (resp_size == sizeof(struct rtc_time))
			memcpy(resp_out, &rtc_glink->resp_tm, sizeof(struct rtc_time));
		else
			ret = -EINVAL;
	}

out_unlock:
	mutex_unlock(&rtc_glink->transaction_lock);
	return ret;
}

static int rtc_glink_alarm_set_req(struct rtc_glink_dev *rtc_glink,
				   u32 opcode, u32 value)
{
	struct rtc_glink_tad_stv_req msg = {
		.hdr.owner  = cpu_to_le32(MSG_OWNER_RTC),
		.hdr.type   = cpu_to_le32(MSG_TYPE_REQ_RESP),
		.hdr.opcode = cpu_to_le32(opcode),
		.timer_id   = cpu_to_le32(RTC_GLINK_TAD_DC_TIMER_ID),
		.value      = cpu_to_le32(value),
	};

	return rtc_glink_send(rtc_glink, &msg, sizeof(msg), NULL, 0);
}

static int rtc_glink_alarm_get_req(struct rtc_glink_dev *rtc_glink,
				   u32 opcode, u32 *value)
{
	struct rtc_glink_tad_gtv_req msg = {
		.hdr.owner  = cpu_to_le32(MSG_OWNER_RTC),
		.hdr.type   = cpu_to_le32(MSG_TYPE_REQ_RESP),
		.hdr.opcode = cpu_to_le32(opcode),
		.timer_id   = cpu_to_le32(RTC_GLINK_TAD_DC_TIMER_ID),
	};

	return rtc_glink_send(rtc_glink, &msg, sizeof(msg), value, sizeof(u32));
}

static int rtc_glink_get_real_time(struct rtc_glink_dev *rtc_glink,
				   struct rtc_time *tm)
{
	struct rtc_glink_tad_generic_req msg = {
		.hdr.owner  = cpu_to_le32(MSG_OWNER_RTC),
		.hdr.type   = cpu_to_le32(MSG_TYPE_REQ_RESP),
		.hdr.opcode = cpu_to_le32(RTC_GLINK_GET_REAL_TIME),
	};

	return rtc_glink_send(rtc_glink, &msg, sizeof(msg), tm, sizeof(*tm));
}

static int rtc_glink_set_real_time(struct rtc_glink_dev *rtc_glink,
				   struct rtc_time *tm)

{
	struct rtc_glink_tad_srt_req msg = {
		.hdr.owner  = cpu_to_le32(MSG_OWNER_RTC),
		.hdr.type   = cpu_to_le32(MSG_TYPE_REQ_RESP),
		.hdr.opcode = cpu_to_le32(RTC_GLINK_SET_REAL_TIME),
	};

	rtc_glink_rtc_time_to_tad_payload(tm, msg.real_time_data);

	return rtc_glink_send(rtc_glink, &msg, sizeof(msg), NULL, 0);
}

static int rtc_glink_read_time(struct device *dev, struct rtc_time *time)
{
	struct rtc_glink_dev *rtc_glink = dev_get_drvdata(dev);
	int ret;

	ret = rtc_glink_get_real_time(rtc_glink, time);
	if (ret) {
		dev_err(rtc_glink->dev,
			"Get real time (opcode=0x68) failed, ret=%d\n",
			ret);
	}

	return ret;
}

static int rtc_glink_set_time(struct device *dev, struct rtc_time *time)
{
	struct rtc_glink_dev *rtc_glink = dev_get_drvdata(dev);
	int ret;

	if (!rtc_glink->allow_set_time) {
		dev_info(rtc_glink->dev, "RTC time update disabled\n");
		return 0;
	}

	ret = rtc_valid_tm(time);
	if (ret)
		return ret;

	ret = rtc_glink_set_real_time(rtc_glink, time);
	if (ret)
		return ret;

	dev_dbg(rtc_glink->dev, "Set real time (opcode=0x69) succeeded\n");

	return 0;
}

static int rtc_glink_set_alarm_en(struct rtc_glink_dev *rtc_glink,
				  int enabled)
{
	struct rtc_glink_tad_stv_req msg = {
		.hdr.owner  = cpu_to_le32(MSG_OWNER_RTC),
		.hdr.type   = cpu_to_le32(MSG_TYPE_REQ_RESP),
		.hdr.opcode = cpu_to_le32(RTC_GLINK_SET_ALARM_POLICY),
		.timer_id   = cpu_to_le32(RTC_GLINK_TAD_DC_TIMER_ID),
		.value      = cpu_to_le32(enabled),
	};

	return rtc_glink_send(rtc_glink, &msg, sizeof(msg), NULL, 0);
}

static int _rtc_glink_update_alarm_state(struct rtc_glink_dev *rtc_glink,
					 bool enabled)
{
	int ret;

	if (!enabled) {
		ret = rtc_glink_alarm_set_req(rtc_glink, RTC_GLINK_SET_ALARM_TIME, 0);
		if (ret)
			return ret;
	}

	return rtc_glink_set_alarm_en(rtc_glink, enabled);
}

static int rtc_glink_set_alarm(struct device *dev,
			       struct rtc_wkalrm *alarm)
{
	struct rtc_glink_dev *rtc_glink = dev_get_drvdata(dev);
	time64_t alarm_time;
	time64_t secs_until_alarm;
	struct rtc_time now_tm;
	time64_t now_real;
	u32 alarm_secs;
	int ret;

	/*
	 * The alarm time is irrelevant for a disable request.
	 */
	if (!alarm->enabled)
		return _rtc_glink_update_alarm_state(rtc_glink, false);

	ret = rtc_glink_get_real_time(rtc_glink, &now_tm);
	if (ret)
		return ret;

	alarm_time = rtc_tm_to_time64(&alarm->time);
	now_real = rtc_tm_to_time64(&now_tm);
	secs_until_alarm = alarm_time - now_real;

	/*
	 * A zero or negative relative timeout must not be sent to
	 * the firmware, so clamp it to one second.
	 */
	if (secs_until_alarm <= 0)
		secs_until_alarm = 1;

	if (secs_until_alarm > U32_MAX)
		secs_until_alarm = U32_MAX;

	alarm_secs = (u32)secs_until_alarm;

	ret = rtc_glink_set_alarm_en(rtc_glink, 0);
	if (ret)
		return ret;

	ret = rtc_glink_alarm_set_req(rtc_glink,
				      RTC_GLINK_SET_ALARM_TIME,
				      alarm_secs);
	if (ret) {
		/* Leave alarm disabled to prevent stale wakeups on failure */
		return ret;
	}

	dev_dbg(rtc_glink->dev, "alarm set for (%u) seconds\n", alarm_secs);

	return _rtc_glink_update_alarm_state(rtc_glink, alarm->enabled);
}

static int rtc_glink_read_alarm(struct device *dev,
				struct rtc_wkalrm *alarm)
{
	struct rtc_glink_dev *rtc_glink = dev_get_drvdata(dev);
	u32 alarm_time_secs, alarm_en;
	struct rtc_time now_tm;
	time64_t now_secs, alarm_real;
	int ret;

	ret = rtc_glink_alarm_get_req(rtc_glink,
				      RTC_GLINK_GET_ALARM_TIME,
				      &alarm_time_secs);
	if (ret)
		return ret;

	if (alarm_time_secs == U32_MAX) {
		memset(alarm, 0, sizeof(*alarm));
		alarm->enabled = 0;
		return 0;
	}

	ret = rtc_glink_get_real_time(rtc_glink, &now_tm);
	if (ret)
		return ret;

	now_secs = rtc_tm_to_time64(&now_tm);

	ret = rtc_glink_alarm_get_req(rtc_glink,
				      RTC_GLINK_GET_ALARM_POLICY,
				      &alarm_en);
	if (ret)
		return ret;

	alarm_real = now_secs + (time64_t)alarm_time_secs;
	rtc_time64_to_tm(alarm_real, &alarm->time);
	alarm->enabled = !!alarm_en;

	return 0;
}

static void rtc_glink_alarm_work(struct work_struct *work)
{
	struct rtc_glink_dev *rtc_glink =
		container_of(work, struct rtc_glink_dev, alarm_work);

	down_write(&rtc_glink->state_sem);

	if (!rtc_glink->initialized ||
	    atomic_read(&rtc_glink->state) != PMIC_GLINK_STATE_UP) {
		up_write(&rtc_glink->state_sem);
		return;
	}

	if (rtc_glink->alarm_pending) {
		rtc_glink->alarm_pending = false;
		up_write(&rtc_glink->state_sem);
		rtc_update_irq(rtc_glink->rtc, 1, RTC_IRQF | RTC_AF);
		return;
	}

	up_write(&rtc_glink->state_sem);
}

static int validate_message(struct rtc_glink_dev *rtc_glink, void *data, size_t len)
{
	const struct pmic_glink_hdr *hdr = data;
	u32 opcode;

	if (!data)
		return -EINVAL;

	if (len < sizeof(*hdr))
		return -EPROTO;

	opcode = le32_to_cpu(hdr->opcode);

	switch (opcode) {
	case RTC_GLINK_ALARM_EXPIRED:
		return 0;

	case RTC_GLINK_GET_ALARM_TIME:
	case RTC_GLINK_GET_ALARM_POLICY:
	case RTC_GLINK_SET_ALARM_TIME:
	case RTC_GLINK_SET_ALARM_POLICY:
	case RTC_GLINK_SET_REAL_TIME:
		if (len < sizeof(struct rtc_glink_tad_generic_resp))
			return -EPROTO;
		return 0;

	case RTC_GLINK_GET_REAL_TIME:
		if (len < sizeof(struct rtc_glink_tad_grt_resp))
			return -EPROTO;
		return 0;

	default:
		dev_warn(rtc_glink->dev, "Invalid opcode 0x%x\n", opcode);
		return -EPROTO;
	}
}

static int rtc_glink_callback(void *priv, void *data, size_t len)
{
	struct rtc_glink_dev *rtc_glink = priv;
	const struct pmic_glink_hdr *hdr = data;
	u32 opcode;
	int ret;

	if (!rtc_glink || !data || len < sizeof(*hdr))
		return 0;

	opcode = le32_to_cpu(hdr->opcode);
	dev_dbg(rtc_glink->dev, "RX opcode=0x%x\n", opcode);

	/*
	 * Always latch alarm-expired events, even if probe is not complete yet.
	 * This prevents losing the event before initialized becomes true.
	 */
	if (opcode == RTC_GLINK_ALARM_EXPIRED) {
		pm_wakeup_dev_event(rtc_glink->dev, 50, true);

		down_write(&rtc_glink->state_sem);
		if (rtc_glink->initialized &&
		    atomic_read(&rtc_glink->state) == PMIC_GLINK_STATE_UP) {
			up_write(&rtc_glink->state_sem);
			rtc_update_irq(rtc_glink->rtc, 1, RTC_IRQF | RTC_AF);
		} else {
			rtc_glink->alarm_pending = true;
			up_write(&rtc_glink->state_sem);
		}
		return 0;
	}

	down_read(&rtc_glink->state_sem);
	if (atomic_read(&rtc_glink->state) != PMIC_GLINK_STATE_UP ||
	    !rtc_glink->initialized) {
		up_read(&rtc_glink->state_sem);
		return 0;
	}
	up_read(&rtc_glink->state_sem);

	ret = validate_message(rtc_glink, data, len);
	if (ret) {
		rtc_glink->error = ret;
		complete(&rtc_glink->ack);
		return 0;
	}

	switch (opcode) {
	case RTC_GLINK_GET_ALARM_TIME:
	case RTC_GLINK_GET_ALARM_POLICY: {
		const struct rtc_glink_tad_generic_resp *resp = data;

		rtc_glink->resp_value = le32_to_cpu(resp->return_status);
		rtc_glink->error = 0;
		complete(&rtc_glink->ack);
		break;
	}

	case RTC_GLINK_SET_ALARM_TIME:
	case RTC_GLINK_SET_ALARM_POLICY:
	case RTC_GLINK_SET_REAL_TIME: {
		const struct rtc_glink_tad_generic_resp *resp = data;
		u32 status;

		status = le32_to_cpu(resp->return_status);
		rtc_glink->error = status ? -EIO : 0;
		complete(&rtc_glink->ack);
		break;
	}

	case RTC_GLINK_GET_REAL_TIME: {
		const struct rtc_glink_tad_grt_resp *resp = data;
		u32 status;

		status = le32_to_cpu(resp->return_status);
		if (status) {
			rtc_glink->error = -EIO;
		} else {
			ret = rtc_glink_tad_payload_to_rtc_time(resp->real_time_data,
								&rtc_glink->resp_tm);
			rtc_glink->error = ret;
		}
		complete(&rtc_glink->ack);
		break;
	}

	default:
		dev_warn(rtc_glink->dev, "unhandled opcode 0x%x\n", opcode);
		break;
	}

	return 0;
}

static void rtc_glink_state_cb(void *priv, enum pmic_glink_state state)
{
	struct rtc_glink_dev *rtc_glink = priv;

	down_write(&rtc_glink->state_sem);

	if (!rtc_glink->initialized) {
		dev_warn(rtc_glink->dev,
			 "Driver not initialized, pmic_glink state %d\n",
			 state);
		up_write(&rtc_glink->state_sem);
		return;
	}

	atomic_set(&rtc_glink->state, state);

	if (state == PMIC_GLINK_STATE_DOWN) {
		dev_info(rtc_glink->dev, "pmic_glink state DOWN (remote SSR)\n");
		rtc_glink->error = -ENOTCONN;
		complete_all(&rtc_glink->ack);
		up_write(&rtc_glink->state_sem);

		cancel_work_sync(&rtc_glink->alarm_work);
		return;
	}

	if (state == PMIC_GLINK_STATE_UP) {
		dev_info(rtc_glink->dev, "pmic_glink state UP\n");

		rtc_glink->error = 0;

		if (rtc_glink->alarm_pending) {
			pm_wakeup_dev_event(rtc_glink->dev, 50, true);
			schedule_work(&rtc_glink->alarm_work);
		}
	}

	up_write(&rtc_glink->state_sem);
}

static int rtc_glink_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct rtc_glink_dev *rtc_glink = dev_get_drvdata(dev);

	return _rtc_glink_update_alarm_state(rtc_glink, enabled);
}

static const struct rtc_class_ops rtc_glink_rtc_ops = {
	.read_time        = rtc_glink_read_time,
	.set_time         = rtc_glink_set_time,
	.read_alarm       = rtc_glink_read_alarm,
	.set_alarm        = rtc_glink_set_alarm,
	.alarm_irq_enable = rtc_glink_alarm_irq_enable,
};

static int rtc_glink_probe(struct platform_device *pdev)
{
	struct pmic_glink_client_data client_data = {};
	struct device *dev = &pdev->dev;
	struct rtc_glink_dev *rtc_glink;
	int ret;

	rtc_glink = devm_kzalloc(dev, sizeof(*rtc_glink), GFP_KERNEL);
	if (!rtc_glink)
		return -ENOMEM;

	rtc_glink->allow_set_time = of_property_read_bool(pdev->dev.of_node,
							  "allow-set-time");

	rtc_glink->dev = dev;

	mutex_init(&rtc_glink->transaction_lock);
	init_rwsem(&rtc_glink->state_sem);
	init_completion(&rtc_glink->ack);
	INIT_WORK(&rtc_glink->alarm_work, rtc_glink_alarm_work);

	platform_set_drvdata(pdev, rtc_glink);

	rtc_glink->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc_glink->rtc))
		return PTR_ERR(rtc_glink->rtc);

	rtc_glink->rtc->ops = &rtc_glink_rtc_ops;
	rtc_glink->rtc->range_min = 0;
	rtc_glink->rtc->range_max = U32_MAX;

	client_data.id = MSG_OWNER_RTC;
	client_data.name = "rtc-qti-glink";
	client_data.msg_cb = rtc_glink_callback;
	client_data.priv = rtc_glink;
	client_data.state_cb = rtc_glink_state_cb;

	/*
	 * Register with pmic_glink. Callbacks can start happening
	 * immediately after this call.
	 */
	rtc_glink->client = pmic_glink_register_client(dev, &client_data);
	if (IS_ERR(rtc_glink->client)) {
		ret = PTR_ERR(rtc_glink->client);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to register pmic_glink client: %d\n", ret);
		return ret;
	}

	down_write(&rtc_glink->state_sem);
	atomic_set(&rtc_glink->state, PMIC_GLINK_STATE_UP);
	rtc_glink->initialized = true;
	up_write(&rtc_glink->state_sem);

	device_init_wakeup(dev, true);

	ret = devm_rtc_register_device(rtc_glink->rtc);
	if (ret) {
		dev_err(dev, "failed to register rtc device: %d\n", ret);
		goto err_unregister;
	}

	/* Replay any alarm that arrived during the probe sequence. */
	down_write(&rtc_glink->state_sem);
	if (rtc_glink->alarm_pending &&
		atomic_read(&rtc_glink->state) == PMIC_GLINK_STATE_UP) {
		pm_wakeup_dev_event(rtc_glink->dev, 50, true);
		schedule_work(&rtc_glink->alarm_work);
	}
	up_write(&rtc_glink->state_sem);

	return 0;

err_unregister:
	device_init_wakeup(dev, false);
	down_write(&rtc_glink->state_sem);
	rtc_glink->initialized = false;
	atomic_set(&rtc_glink->state, PMIC_GLINK_STATE_DOWN);
	up_write(&rtc_glink->state_sem);
	cancel_work_sync(&rtc_glink->alarm_work);
	complete(&rtc_glink->ack);
	pmic_glink_unregister_client(rtc_glink->client);
	return ret;
}

static void rtc_glink_remove(struct platform_device *pdev)
{
	struct rtc_glink_dev *rtc_glink = platform_get_drvdata(pdev);

	down_write(&rtc_glink->state_sem);
	atomic_set(&rtc_glink->state, PMIC_GLINK_STATE_DOWN);
	rtc_glink->initialized = false;
	up_write(&rtc_glink->state_sem);

	device_init_wakeup(rtc_glink->dev, false);

	cancel_work_sync(&rtc_glink->alarm_work);

	pmic_glink_unregister_client(rtc_glink->client);
}

static const struct of_device_id rtc_glink_of_match[] = {
	{ .compatible = "qcom,rtc-glink" },
	{}
};
MODULE_DEVICE_TABLE(of, rtc_glink_of_match);

static struct platform_driver rtc_glink_driver = {
	.driver = {
		.name           = "rtc_qti_glink",
		.of_match_table = rtc_glink_of_match,
	},
	.probe  = rtc_glink_probe,
	.remove = rtc_glink_remove,
};
module_platform_driver(rtc_glink_driver);

MODULE_DESCRIPTION("QTI Glink RTC driver");
MODULE_LICENSE("GPL");
