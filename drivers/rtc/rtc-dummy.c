// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/of.h>
#include <linux/compat.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/rtc.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/kdev_t.h>

#include "rtc-core.h"

#define MIN_RTC_THRESHOLD_VALUE 1000 /* min threshold that indicates RTC value retention */
#define NUM_8_BIT_RTC_REGS              0x4

#define RTC_DEV_MAX 16 /* 16 RTCs should be enough for everyone... */
/**
 * struct pm8xxx_rtc_regs - describe RTC registers per PMIC versions
 * @ctrl:		address of control register
 * @write:		base address of write registers
 * @read:		base address of read registers
 * @alarm_ctrl:		address of alarm control register
 * @alarm_ctrl2:	address of alarm control2 register
 * @alarm_rw:		base address of alarm read-write registers
 * @alarm_en:		alarm enable mask
 */
struct pm8xxx_rtc_regs {
	unsigned int ctrl;
	unsigned int write;
	unsigned int read;
	unsigned int alarm_ctrl;
	unsigned int alarm_ctrl2;
	unsigned int alarm_rw;
	unsigned int alarm_en;
};

/*
 * @rtc:                rtc device for this driver.
 * @regmap:             regmap used to access RTC registers
 * @regs:               rtc registers description.
 */
struct rtc_dummy_data {
	struct rtc_device *rtc;
	struct regmap *regmap;
	const struct pm8xxx_rtc_regs *regs;
	time64_t offset;
	struct timer_list alarm;
	bool alarm_en;
};

static int rtc_dummy_dev_open(struct inode *inode, struct file *file)
{
	struct rtc_device *rtc = container_of(inode->i_cdev,
					struct rtc_device, char_dev);

	dev_dbg(rtc->dev.parent, ">>%s:%d\n", __func__, __LINE__);

	if (test_and_set_bit_lock(RTC_DEV_BUSY, &rtc->flags)) {
		dev_err(rtc->dev.parent, "%s:%d ret: EBUSY\n", __func__, __LINE__);
		return -EBUSY;
	}

	file->private_data = rtc;

	spin_lock_irq(&rtc->irq_lock);
	rtc->irq_data = 0;
	spin_unlock_irq(&rtc->irq_lock);

	dev_dbg(rtc->dev.parent, "<<%s:%d\n", __func__, __LINE__);
	return 0;
}

static ssize_t
rtc_dummy_dev_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct rtc_device *rtc = file->private_data;

	unsigned long data;
	ssize_t ret = -1;

	dev_dbg(rtc->dev.parent, ">>%s:%d\n",
				__func__, __LINE__);
	spin_lock_irq(&rtc->irq_lock);
	data = rtc->irq_data;
	rtc->irq_data = 0;
	spin_unlock_irq(&rtc->irq_lock);

	if (data != 0)
		ret = 0;

	if (ret == 0) {
		if (sizeof(int) != sizeof(long) &&
		    count == sizeof(unsigned int))
			ret = put_user(data, (unsigned int __user *)buf) ?:
				sizeof(unsigned int);
		else
			ret = put_user(data, (unsigned long __user *)buf) ?:
				sizeof(unsigned long);
	}
	dev_dbg(rtc->dev.parent, "<<%s:%d ret: %zd\n",
				__func__, __LINE__, ret);
	return ret;
}

static __poll_t rtc_dummy_dev_poll(struct file *file, poll_table *wait)
{
	struct rtc_device *rtc = file->private_data;
	unsigned long data;

	/* Call poll_wait(); */

	data = rtc->irq_data;

	return (data != 0) ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static long rtc_dummy_dev_ioctl(struct file *file,
			  unsigned int cmd, unsigned long arg)
{
	int err = 0;
	struct rtc_device *rtc = file->private_data;
	const struct rtc_class_ops *ops = rtc->ops;
	struct rtc_time tm;
	void __user *uarg = (void __user *)arg;

	dev_dbg(rtc->dev.parent, ">>%s:%d cmd: %d\n",
				__func__, __LINE__, cmd);

	/* check that the calling task has appropriate permissions
	 * for certain ioctls.
	 * ioctl commands RTC_EPOCH_SET, RTC_SET_TIME, RTC_PARAM_SET
	 * ioctl commands RTC_IRQP_SET, RTC_PIE_ON
	 * Refer dev.c for handling these in case needed.
	 */
	switch (cmd) {
	/*
	 * RTC_ALM_READ: rtc_read_alarm()
	 * RTC_ALM_SET: rtc_set_alarm()
	 */
	case RTC_RD_TIME:
		err = rtc_read_time(rtc, &tm);
		if (err < 0) {
			dev_err(rtc->dev.parent, "%s:%d err: %d\n",
						__func__, __LINE__, err);
			goto done;
		}

		if (copy_to_user(uarg, &tm, sizeof(tm)))
			err = -EFAULT;
		break;
	/*
	 * If needed, refer the functions
	 * RTC_SET_TIME: rtc_set_time()
	 * RTC_PIE_ON: rtc_irq_set_state()
	 * RTC_PIE_OFF: rtc_irq_set_state()
	 * RTC_AIE_ON: rtc_alarm_irq_enable()
	 * RTC_AIE_OFF: rtc_alarm_irq_enable()
	 * RTC_UIE_ON: rtc_update_irq_enable()
	 * RTC_UIE_OFF: rtc_update_irq_enable()
	 * RTC_IRQP_SET: rtc_irq_set_freq()
	 * RTC_WKALM_SET: rtc_set_alarm()
	 * RTC_WKALM_RD: rtc_read_alarm()
	 * RTC_PARAM_GET: rtc_read_offset()
	 * RTC_PARAM_SET: rtc_set_offset()
	 */
	default:
		/* Finally try the driver's ioctl interface */
		dev_dbg(rtc->dev.parent, "%s:%d cmd: %d\n", __func__, __LINE__, cmd);
		if (ops->ioctl) {
			err = ops->ioctl(rtc->dev.parent, cmd, arg);
			if (err == -ENOIOCTLCMD)
				err = -ENOTTY;
		} else
			err = -ENOTTY;
		break;
	}

done:
	dev_dbg(rtc->dev.parent, "<<%s:%d ret: %d\n", __func__, __LINE__, err);
	return err;
}

#ifdef CONFIG_COMPAT
#define RTC_IRQP_SET32		_IOW('p', 0x0c, __u32)
#define RTC_IRQP_READ32		_IOR('p', 0x0b, __u32)
#define RTC_EPOCH_SET32		_IOW('p', 0x0e, __u32)

static long rtc_dummy_dev_compat_ioctl(struct file *file,
				 unsigned int cmd, unsigned long arg)
{
	struct rtc_device *rtc = file->private_data;
	void __user *uarg = compat_ptr(arg);

	dev_dbg(rtc->dev.parent, ">>%s:%d cmd: %d\n",
				__func__, __LINE__, cmd);

	switch (cmd) {
	case RTC_IRQP_READ32:
		return put_user(rtc->irq_freq, (__u32 __user *)uarg);

	case RTC_IRQP_SET32:
		/* arg is a plain integer, not pointer */
		return rtc_dummy_dev_ioctl(file, RTC_IRQP_SET, arg);

	case RTC_EPOCH_SET32:
		/* arg is a plain integer, not pointer */
		return rtc_dummy_dev_ioctl(file, RTC_EPOCH_SET, arg);
	}

	return rtc_dummy_dev_ioctl(file, cmd, (unsigned long)uarg);
}
#endif

static int rtc_dummy_dev_fasync(int fd, struct file *file, int on)
{
	struct rtc_device *rtc = file->private_data;

	dev_dbg(rtc->dev.parent, ">>%s:%d\n", __func__, __LINE__);

	return 0;
}

static int rtc_dummy_dev_release(struct inode *inode, struct file *file)
{
	struct rtc_device *rtc = file->private_data;

	clear_bit_unlock(RTC_DEV_BUSY, &rtc->flags);

	return 0;
}

static const struct file_operations rtc_dev_fops = {
	.owner		= THIS_MODULE,
	.llseek		= noop_llseek,
	.read		= rtc_dummy_dev_read,
	.poll		= rtc_dummy_dev_poll,
	.unlocked_ioctl	= rtc_dummy_dev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= rtc_dummy_dev_compat_ioctl,
#endif
	.open		= rtc_dummy_dev_open,
	.release	= rtc_dummy_dev_release,
	.fasync		= rtc_dummy_dev_fasync,
};

static int rtc_dummy_register_device(struct module *owner, struct rtc_device *rtc)
{
	dev_t rtc_devt;

	int err;

	if (!rtc->ops) {
		dev_err(&rtc->dev, "no ops set\n");
		return -EINVAL;
	}

	if (!rtc->ops->set_alarm)
		clear_bit(RTC_FEATURE_ALARM, rtc->features);

	if (rtc->ops->set_offset)
		set_bit(RTC_FEATURE_CORRECTION, rtc->features);

	rtc->owner = owner;

	if (rtc->id >= RTC_DEV_MAX) {
		dev_dbg(&rtc->dev, "too many RTC devices\n");
		return -1;
	}

	err = alloc_chrdev_region(&rtc_devt, 0, RTC_DEV_MAX, "rtc");
	if (err < 0) {
		pr_err("failed to allocate char dev region\n");
		return err;
	}

	rtc->dev.devt = MKDEV(MAJOR(rtc_devt), rtc->id);

	cdev_init(&rtc->char_dev, &rtc_dev_fops);
	rtc->char_dev.owner = rtc->owner;

	err = cdev_device_add(&rtc->char_dev, &rtc->dev);
	if (err) {
		set_bit(RTC_NO_CDEV, &rtc->flags);
		dev_err(rtc->dev.parent, "failed to add char device %d:%d err: %d\n",
			 MAJOR(rtc->dev.devt), rtc->id, err);
		return err;
	}
	dev_info(rtc->dev.parent, "char device (%d:%d) registered as %s\n",
			MAJOR(rtc->dev.devt), rtc->id, dev_name(&rtc->dev));

	return 0;
}

static void rtc_dummy_unregister_device(void *data)
{
	struct rtc_device *rtc = data;

	mutex_lock(&rtc->ops_lock);
	/*
	 * Remove innards of this RTC, then disable it, before
	 * letting any rtc_class_open() users access it again
	 */
	if (!test_bit(RTC_NO_CDEV, &rtc->flags))
		cdev_device_del(&rtc->char_dev, &rtc->dev);
	rtc->ops = NULL;
	mutex_unlock(&rtc->ops_lock);
}

static int rtc_dummy_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct rtc_dummy_data *rtd = dev_get_drvdata(dev);
	time64_t alarm;

	alarm = (rtd->alarm.expires - jiffies) / HZ;
	alarm += ktime_get_real_seconds() + rtd->offset;

	rtc_time64_to_tm(alarm, &alrm->time);
	alrm->enabled = rtd->alarm_en;

	return 0;
}

static int rtc_dummy_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct rtc_dummy_data *rtd = dev_get_drvdata(dev);
	ktime_t timeout;
	u64 expires;

	timeout = rtc_tm_to_time64(&alrm->time) - ktime_get_real_seconds();
	timeout -= rtd->offset;

	del_timer(&rtd->alarm);

	expires = jiffies + timeout * HZ;
	if (expires > U32_MAX)
		expires = U32_MAX;

	rtd->alarm.expires = expires;

	/* if alarm is enabled call add_timer() to set alarm */

	rtd->alarm_en = alrm->enabled;

	return 0;
}

static int rtc_dummy_read_time(struct device *dev, struct rtc_time *tm)
{
	int rc;
	u8 value[NUM_8_BIT_RTC_REGS];
	unsigned long secs;
	unsigned int reg;
	struct rtc_dummy_data *rtd = dev_get_drvdata(dev);
	const struct pm8xxx_rtc_regs *regs = rtd->regs;

	if (!rtd->regmap) {
		rtc_time64_to_tm(MIN_RTC_THRESHOLD_VALUE + 1, tm);
		return 0;
	}

	rc = regmap_bulk_read(rtd->regmap, regs->read, value, sizeof(value));
	if (rc) {
		dev_err(dev, "RTC read data register failed\n");
		return rc;
	}

	/*
	 * Read the LSB again and check if there has been a carry over.
	 * If there is, redo the read operation.
	 */
	rc = regmap_read(rtd->regmap, regs->read, &reg);
	if (rc < 0) {
		dev_err(dev, "RTC read data register failed\n");
		return rc;
	}

	if (unlikely(reg < value[0])) {
		rc = regmap_bulk_read(rtd->regmap, regs->read,
				      value, sizeof(value));
		if (rc) {
			dev_err(dev, "RTC read data register failed\n");
			return rc;
		}
	}

	secs = value[0] | (value[1] << 8) | (value[2] << 16) |
	       ((unsigned long)value[3] << 24);

	rtc_time64_to_tm(secs, tm);

	return 0;
}

static int rtc_dummy_set_time(struct device *dev, struct rtc_time *tm)
{
	struct rtc_dummy_data *rtd = dev_get_drvdata(dev);

	rtd->offset = rtc_tm_to_time64(tm) - ktime_get_real_seconds();

	return 0;
}

static int rtc_dummy_alarm_irq_enable(struct device *dev, unsigned int enable)
{
	struct rtc_dummy_data *rtd = dev_get_drvdata(dev);

	rtd->alarm_en = enable;
	/*
	 * To enable call add_timer()
	 * To disable call del_timer()
	 */

	return 0;
}

static const struct rtc_class_ops rtc_dummy_ops_noalm = {
	.read_time = rtc_dummy_read_time,
	.set_time = rtc_dummy_set_time,
	.alarm_irq_enable = rtc_dummy_alarm_irq_enable,
};

static const struct rtc_class_ops rtc_dummy_ops = {
	.read_time = rtc_dummy_read_time,
	.set_time = rtc_dummy_set_time,
	.read_alarm = rtc_dummy_read_alarm,
	.set_alarm = rtc_dummy_set_alarm,
	.alarm_irq_enable = rtc_dummy_alarm_irq_enable,
};

static void rtc_dummy_alarm_handler(struct timer_list *t)
{
	struct rtc_dummy_data *rtd = from_timer(rtd, t, alarm);

	rtc_update_irq(rtd->rtc, 1, RTC_AF | RTC_IRQF);
}

static const struct pm8xxx_rtc_regs pmk8350_regs = {
	.ctrl		= 0x6146,
	.write		= 0x6140,
	.read		= 0x6148,
	.alarm_rw	= 0x6240,
	.alarm_ctrl	= 0x6246,
	.alarm_ctrl2	= 0x6248,
	.alarm_en	= BIT(7),
};

static const struct of_device_id pm8xxx_id_table[] = {
	{ .compatible = "qcom,rtc-dummy", .data = &pmk8350_regs },
	{ },
};
MODULE_DEVICE_TABLE(of, pm8xxx_id_table);

static int rtc_dummy_probe(struct platform_device *plat_dev)
{
	struct rtc_dummy_data *rtd;
	const struct of_device_id *match;

	match = of_match_node(pm8xxx_id_table, plat_dev->dev.of_node);
	if (!match)
		return -ENXIO;

	rtd = devm_kzalloc(&plat_dev->dev, sizeof(*rtd), GFP_KERNEL);
	if (!rtd)
		return -ENOMEM;

	rtd->regmap = dev_get_regmap(plat_dev->dev.parent, NULL);
	if (!rtd->regmap)
		dev_dbg(&plat_dev->dev, "Parent regmap unavailable, using software clock.\n");

	platform_set_drvdata(plat_dev, rtd);

	rtd->rtc = devm_rtc_allocate_device(&plat_dev->dev);
	if (IS_ERR(rtd->rtc)) {
		dev_err(&plat_dev->dev, "%s:%d devm_rtc_allocate_device: err: %ld\n",
					__func__, __LINE__, PTR_ERR(rtd->rtc));
		return PTR_ERR(rtd->rtc);
	}

	rtd->regs = match->data;

	switch (plat_dev->id) {
	case PLATFORM_DEVID_NONE:
		rtd->rtc->ops = &rtc_dummy_ops_noalm;
		break;
	default:
		rtd->rtc->ops = &rtc_dummy_ops;
		device_init_wakeup(&plat_dev->dev, 1);
	}

	timer_setup(&rtd->alarm, rtc_dummy_alarm_handler, 0);

	return rtc_dummy_register_device(THIS_MODULE, rtd->rtc);
}

static int rtc_dummy_remove(struct platform_device *pdev)
{
	struct rtc_dummy_data *rtd = platform_get_drvdata(pdev);

	rtc_dummy_unregister_device(rtd->rtc);

	return 0;
}

static struct platform_driver rtc_dummy_driver = {
	.probe	= rtc_dummy_probe,
	.remove	= rtc_dummy_remove,
	.driver = {
		.name = "rtc-dummy",
		.of_match_table	= pm8xxx_id_table,
	},
};

module_platform_driver(rtc_dummy_driver);

MODULE_ALIAS("platform:rtc-dummy");
MODULE_DESCRIPTION("RTC dummy driver");
MODULE_LICENSE("GPL");
