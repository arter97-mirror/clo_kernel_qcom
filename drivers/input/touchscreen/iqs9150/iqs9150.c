// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <linux/kernel.h>
#include <linux/dmi.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/of.h>
#include <asm/unaligned.h>
#include <linux/power_supply.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include <drm/drm_panel.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/atomic.h>

#include "iqs9150.h"

static int iqs9150_suspend(struct device *dev);
static int iqs9150_resume(struct device *dev);

static int iqs9150_read_reg(struct iqs9150_ts_data *ts, u8 reg_high, u8 reg_low, u16 len, void *val)
{
	struct i2c_msg xfer[XFER_COUNT] = {0};
	int ret = 0, i = 0;
	u8 *buf = ts->cmd_buf;

	struct i2c_client *client = ts->client;

	*buf = reg_low & 0xff;
	*(buf + 1) = reg_high & 0xff;
	/* Write register */
	xfer[0].addr = client->addr;
	xfer[0].flags = 0;
	xfer[0].len = sizeof(reg_high) + sizeof(reg_low);
	xfer[0].buf = buf;
	/* Read data */
	xfer[1].addr = client->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = len;
	xfer[1].buf = val;

	for (i = 0; i < I2C_RETRY_NUMBER; i++) {
		mutex_lock(&ts->i2c_mutex);
		ret = i2c_transfer(client->adapter, xfer, XFER_COUNT);
		mutex_unlock(&ts->i2c_mutex);
		if (ret == XFER_COUNT) {
			ret = 0;
			break;
		}
		if (ret >= 0)
			ret = -EIO;
		IQS_ERROR("i2c transfer failed (%d), retry:%d ", ret, i);
		usleep_range(DELAY_US_AFTER_RW_REG_BEGIN, DELAY_US_AFTER_RW_REG_END);
	}

	return ret;
}

static int iqs9150_write_reg(struct i2c_client *client, u16 reg, u16 len, const void *val)
{
	u8 *buf = NULL;
	size_t count = 0;
	int ret = 0;

	struct iqs9150_ts_data *ts = i2c_get_clientdata(client);

	count = len + 2;
	buf = kmalloc(count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = reg & 0xff;
	buf[1] = (reg >> 8) & 0xff;
	memcpy(&buf[2], val, len);

	for (i = 0; i < I2C_RETRY_NUMBER; i++) {
		mutex_lock(&ts->i2c_mutex);
		ret = i2c_master_send(client, buf, count);
		mutex_unlock(&ts->i2c_mutex);
		if (ret == count) {
			ret = 0;
				break;
		}
		if (ret >= 0)
			ret = -EIO;
		IQS_ERROR("i2c send failed (%d), retry:%d", ret, i);
		usleep_range(DELAY_US_AFTER_RW_REG_BEGIN, DELAY_US_AFTER_RW_REG_END);
	}

	kfree(buf);
	return ret;
}

static void iqs9150_reg_read_work_handler(struct work_struct *work)
{
	struct iqs9150_ts_data *ts =
		container_of(work, struct iqs9150_ts_data, reg_read_dwork.work);
	uint8_t buf[2] = {0};
	int ret = 0;

	if (atomic_read(&ts->is_process_event_now)) {
		queue_delayed_work(ts->workqueue,
					    &ts->reg_read_dwork,
					    msecs_to_jiffies(DELAY_TO_CHECK_PRODUCT_NUMBER));
		return;
	}

	ret = iqs9150_read_reg(ts, PRODUCT_NUMBER_REGISTER_ADDRESS_MSB,
				 PRODUCT_NUMBER_REGISTER_ADDRESS_LSB, 2, buf);

	if (!ret && (buf[0] == PRODUCT_NUMBER_DATA_LSB)
		&& (buf[1] == PRODUCT_NUMBER_DATA_MSB))
		IQS_INFO("Product_number register read: 0x%02X 0x%02X\n", buf[0], buf[1]);
	else {
		IQS_ERROR("Product_number register read failed: %d, reset chip\n", ret);
		disable_irq(ts->irq);

		if (gpio_is_valid(ts->iovdd_gpio)) {
			gpio_direction_output(ts->iovdd_gpio, GPIO_OUTPUT_LOW);
			msleep(DELAY_BETWEEN_GPIO_HIGH_LOW);
			gpio_direction_output(ts->iovdd_gpio, GPIO_OUTPUT_HIGH);
		}

		ts->iqs9150_state = IQS_COMMS_WINDOW;
		enable_irq(ts->irq);
	}
	queue_delayed_work(ts->workqueue,
				    &ts->reg_read_dwork,
				    msecs_to_jiffies(DELAY_TO_CHECK_PRODUCT_NUMBER));

}

static int stop_iqs9150_iqs_production_testing(struct iqs9150_ts_data *ts)
{
	int ret = 0;
	uint8_t buffer[2] = {0};

	buffer[0] = CONFIG_SETTINGS_0|0x40;
	buffer[1] = CONFIG_SETTINGS_1|0x01;
	ret = iqs9150_write_reg(ts->client, 0x11BE, 2, &buffer[0]);
	if (ret)
		IQS_ERROR("iqs9150_write_reg  11BE");

	ts->iqs9150_state = IQS_NORMAL;
	queue_delayed_work(ts->workqueue,
				    &ts->reg_read_dwork,
				    msecs_to_jiffies(DELAY_TO_CHECK_PRODUCT_NUMBER));

	return ret;
}

static int check_rdy(struct iqs9150_ts_data *ts)
{
	if (gpio_get_value(ts->irq_gpio)) {
		IQS_ERROR("RDY pin is high, the device is not ready");
		return -1;
	}
	return 0;
}

static int map_to_tool(int touch_num)
{
	switch (touch_num) {
	case 0:
			return NO_FINGER_TOUCH;
	case 1:
			return BTN_TOOL_FINGER;
	case 2:
			return BTN_TOOL_DOUBLETAP;
	case 3:
			return BTN_TOOL_TRIPLETAP;
	default:
			/*
			 * If it is greater than or equal to four fingers,
			 * still report according to four fingers.
			 */
			return BTN_TOOL_QUADTAP;
	}
}

static void iqs9150_report_tool_count(struct iqs9150_ts_data *ts,
				      struct input_dev *input_dev, int touch_num)
{
	int new_tool = map_to_tool(touch_num);
	int old_tool = map_to_tool(ts->last_touch_num);

	if (new_tool != old_tool) {
		if (old_tool != NO_FINGER_TOUCH)
			input_report_key(input_dev, old_tool, false);

		if (new_tool != NO_FINGER_TOUCH)
			input_report_key(input_dev, new_tool, true);
	}

	ts->last_touch_num = touch_num;
}

static void iqs9150_report_touch_slot(struct input_dev *input_dev, int slot,
				      uint16_t x, uint16_t y,
				      uint16_t pressure)
{
	input_mt_slot(input_dev, slot);

	if (x == 0xFFFF) {
		input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, false);
		input_report_abs(input_dev, ABS_MT_PRESSURE, 0);
		return;
	}

	input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, true);
	input_report_abs(input_dev, ABS_MT_PRESSURE, pressure);
	input_report_abs(input_dev, ABS_MT_POSITION_X, x);
	input_report_abs(input_dev, ABS_MT_POSITION_Y, y);
}

static void iqs9150_report_touch_slots(struct input_dev *input_dev, uint8_t *data)
{
	uint16_t x;
	uint16_t y;
	uint16_t pressure;
	int data_pos;
	int i;

	for (i = 0; i < MAX_TOUCH_REPORT; i++) {
		data_pos = DATA_POS_BEGIN + i * FINGER_DATA_LEN;

		x = ((uint16_t)data[data_pos + 1] << 8) + data[data_pos];
		y = ((uint16_t)data[data_pos + 3] << 8) + data[data_pos + 2];
		pressure = ((uint16_t)data[data_pos + 5] << 8) + data[data_pos + 4];

		iqs9150_report_touch_slot(input_dev, i, x, y, pressure);
	}
}

/*
 * iqs9150_process_events - Process incoming events
 *
 * @ts: our iqs9150_ts_data pointer
 *
 * Called when the IRQ is triggered. Read the current device state, and push
 * the input events to the user space.
 */
static void iqs9150_process_events(struct iqs9150_ts_data *ts)
{
	uint8_t iqs9150_testing_code = 0;

	uint8_t buffer[2] = {0};

	uint8_t system_data_buf[EVENT_BUFFER_COUNT] = {0};

	int error = 0;
	int ret = 0;

	int touch_num = 0;

	if (!ts || !ts->client) {
		IQS_ERROR("%s: Invalid ts data (ts=%p, client=%p)\n",
			   __func__, ts, ts ? ts->client : NULL);
		return;
	}

	if (check_rdy(ts) == 0) {
		atomic_set(&ts->is_process_event_now, 1);
		switch (ts->iqs9150_state) {
		case IQS_COMMS_WINDOW:
			ret = iqs9150_stop_bit_disabled(ts);
			if (ret) {
				IQS_ERROR("iqs9150_stop_bit_disabled failed");
				goto out;
			}
			ts->iqs9150_state = IQS_RESET_INIT;
			IQS_INFO("IQS9150_Stop_Bit_Disabled_finished");
			break;

		case IQS_RESET_INIT:
			ret = iqs9150_init(ts);
			if (ret) {
				IQS_ERROR("iqs9150_init failed");
				iqs9150_stop_i2c_comm_window(ts);
				goto out;
			}
			ret = iqs9150_stop_i2c_comm_window(ts);
			if (ret) {
				IQS_ERROR("iqs9150_stop_i2c_comm_window failed");
				goto out;
			}
			ts->get_rdy_fail = false;
			ts->iqs9150_state = IQS_NORMAL;

			IQS_INFO("IQS9150_Init_finished");

			break;
		case IQS_NORMAL:
			if (!ts->input_dev) {
				IQS_ERROR("%s: Invalid ts data (input_dev=%p)\n",
					__func__, ts ? ts->input_dev : NULL);
				goto out;
			}
			error = iqs9150_read_reg(ts, FINGER_DATA_REGISTER_ADDRESS_MSB,
				FINGER_DATA_REGISTER_ADDRESS_LSB,
				FINGER_DATA_OFFSET,
				&system_data_buf[0]);

			if (error) {
				IQS_ERROR("read TP_data 0x101C error: %d", error);
				goto out;
			}

			if (system_data_buf[4] & CHIP_RESET_OCCURRED) {
				ts->iqs9150_state = IQS_COMMS_WINDOW;
				IQS_ERROR("IQS9150_Reset_occurred");
			} else {
				if (ts->production_testing_flag) {
					ts->production_testing_flag = 0;
					ts->iqs9150_state = IQS_PRODUCTION_TESTING;

					buffer[0] = CONFIG_SETTINGS_0 | 0x40;
					buffer[1] = CONFIG_SETTINGS_1 & 0xFE;
					ret = iqs9150_write_reg(ts->client, 0x11BE, 2, &buffer[0]);
					if (ret) {
						IQS_ERROR("Please restart Production_Testing");
						iqs9150_stop_i2c_comm_window(ts);
						goto out;
					}
					IQS_INFO("Production_Testing_Start");
				}

				if (system_data_buf[4] & ATI_ERROR) {
					IQS_ERROR("IQS9150_ATI_error");
					buffer[0] = SYSTEM_CONTROL_0|0x80|0x20;
					ret = iqs9150_write_reg(ts->client, 0x11BC, 1, &buffer[0]);
					if (ret)
						IQS_ERROR("iqs9150_write_reg  11BC");
				} else if (system_data_buf[4] & ATI_ACTIVE) {
					IQS_ERROR("IQS9150_ATI_active");
				} else {
					touch_num = (int)(system_data_buf[6] & 0x0F);

					iqs9150_report_tool_count(ts, ts->input_dev, touch_num);

					iqs9150_report_touch_slots(ts->input_dev,
						    system_data_buf);
					if (touch_num)
						input_report_key(ts->input_dev,
								 BTN_TOUCH, true);
					else
						input_report_key(ts->input_dev,
								 BTN_TOUCH, false);

					input_mt_sync_frame(ts->input_dev);
					input_sync(ts->input_dev);
				}
				iqs9150_stop_i2c_comm_window(ts);
			}
			break;
		case IQS_PRODUCTION_TESTING:
			iqs9150_testing_code = iqs9150_iqs_production_testing(ts);

			if (iqs9150_testing_code == IQS_CHIP_ID_ERROR ||
			    iqs9150_testing_code == IQS_ATI_ERROR ||
			    iqs9150_testing_code == IQS_REFERENCE_ERROR ||
			    iqs9150_testing_code == IQS_DELTA_ERROR ||
			    iqs9150_testing_code == IQS_ATI_COMPENSATION_ERROR) {
				stop_iqs9150_iqs_production_testing(ts);
				IQS_ERROR("TP Consistency Test Fail");
			} else if (iqs9150_testing_code == IQS_TEST_PASS) {
				stop_iqs9150_iqs_production_testing(ts);
				IQS_INFO("TP Consistency Test Pass");
			}

			iqs9150_stop_i2c_comm_window(ts);
			IQS_ERROR("IQS9150_Testing_Result_Code: %d", iqs9150_testing_code);
			break;
		default:
			ts->iqs9150_state = IQS_COMMS_WINDOW;
			break;
		}
		goto out;
	}
out:
	atomic_set(&ts->is_process_event_now, 0);
}

/*
 * iqs9150_ts_irq_handler - The IRQ handler
 *
 * @irq: interrupt number.
 * @dev_id: private data pointer.
 */
static irqreturn_t iqs9150_ts_irq_handler(int irq, void *dev_id)
{
	struct iqs9150_ts_data *ts = dev_id;

	iqs9150_process_events(ts);

	return IRQ_HANDLED;
}

#if FTS_PINCTRL_EN
static int fts_pinctrl_init(struct iqs9150_ts_data *ts)
{
	int ret = 0;

	IQS_INFO("enter\n");
	ts->pinctrl = devm_pinctrl_get(ts->dev);
	if (IS_ERR_OR_NULL(ts->pinctrl)) {
		IQS_ERROR("Failed to get pinctrl, please check dts");
		ret = PTR_ERR(ts->pinctrl);
		goto err_pinctrl_get;
	}

	ts->pins_active = pinctrl_lookup_state(ts->pinctrl, "pmx_ts_active");
	if (IS_ERR_OR_NULL(ts->pins_active)) {
		IQS_ERROR("Pin state[active] not found");
		ret = PTR_ERR(ts->pins_active);
		goto err_pinctrl_lookup;
	}

	ts->pins_suspend = pinctrl_lookup_state(ts->pinctrl, "pmx_ts_suspend");
	if (IS_ERR_OR_NULL(ts->pins_suspend)) {
		IQS_ERROR("Pin state[suspend] not found");
		ret = PTR_ERR(ts->pins_suspend);
		goto err_pinctrl_lookup;
	}

	ts->pins_release = pinctrl_lookup_state(ts->pinctrl, "pmx_ts_release");
	if (IS_ERR_OR_NULL(ts->pins_release)) {
		IQS_ERROR("Pin state[release] not found");
		ret = PTR_ERR(ts->pins_release);
	}
	IQS_INFO("enter   End\n");

	return 0;

err_pinctrl_lookup:
	if (ts->pinctrl)
		devm_pinctrl_put(ts->pinctrl);
err_pinctrl_get:
	ts->pinctrl = NULL;
	ts->pins_release = NULL;
	ts->pins_suspend = NULL;
	ts->pins_active = NULL;
	return ret;
}

static int fts_pinctrl_select_normal(struct iqs9150_ts_data *ts)
{
	int ret = 0;

	IQS_INFO("enter\n");
	if (ts->pinctrl && ts->pins_active) {
		ret = pinctrl_select_state(ts->pinctrl, ts->pins_active);
		if (ret < 0)
			IQS_ERROR("Set normal pin state error:%d", ret);
	}

	return ret;
}

#endif /* FTS_PINCTRL_EN */

static int iqs_gpio_configure(struct iqs9150_ts_data *ts)
{
	int ret = 0;

	/* request rdy  gpio */
	if (gpio_is_valid(ts->irq_gpio)) {
		ret = gpio_request(ts->irq_gpio, "iqs_irq_gpio");
		if (ret) {
			IQS_ERROR("RDY  gpio request failed, %d ", ret);
			goto err_irq_gpio_req;
		}

		ret = gpio_direction_input(ts->irq_gpio);
		if (ret) {
			IQS_ERROR("Set_direction for RDY gpio failed");
			goto err_irq_gpio_dir;
		}

#if FTS_PINCTRL_EN
	IQS_INFO("enter Pinctrl init\n");
	fts_pinctrl_init(ts);
	fts_pinctrl_select_normal(ts);
#endif
	}

	/* request reset gpio */
	if (gpio_is_valid(ts->reset_gpio)) {
		ret = gpio_request(ts->reset_gpio, "iqs_reset_gpio");
		if (ret) {
			IQS_ERROR("Reset gpio request failed");
			goto err_irq_gpio_dir;
		}

		ret = gpio_direction_output(ts->reset_gpio, GPIO_OUTPUT_HIGH);
		if (ret) {
			IQS_ERROR("Set_direction for Reset gpio failed");
			goto err_reset_gpio_dir;
		}
	}

	ts->irq = gpio_to_irq(ts->irq_gpio);
	ts->irq_gpio_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
	IQS_INFO("Request IRQ:irq:%d, flag:%x ", ts->irq, ts->irq_gpio_flags);

	ret = request_threaded_irq(ts->irq, NULL,
						   iqs9150_ts_irq_handler,
						   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						   ts->client->name, ts);
	if (ret < 0) {
		IQS_ERROR("Request IRQ-0 failed: %d", ret);
		goto err_irq_gpio_dir;
	}

	/* request idvdd  gpio */
	if (gpio_is_valid(ts->iovdd_gpio)) {
		ret = gpio_request(ts->iovdd_gpio, "iqs_iovdd_gpio");
		if (ret) {
			IQS_ERROR("[GPIO]set_direction for iovdd_gpio gpio failed");
			goto err_iovdd_gpio_dir;
		}

		IQS_INFO("I Will conctrl iovdd gpio");
		gpio_direction_output(ts->iovdd_gpio, GPIO_OUTPUT_LOW);
		msleep(DELAY_BETWEEN_GPIO_HIGH_LOW);
		gpio_direction_output(ts->iovdd_gpio, GPIO_OUTPUT_HIGH);
	}

	return 0;

err_reset_gpio_dir:
	if (gpio_is_valid(ts->reset_gpio))
		gpio_free(ts->reset_gpio);
err_irq_gpio_dir:
	if (gpio_is_valid(ts->irq_gpio))
		gpio_free(ts->irq_gpio);
err_iovdd_gpio_dir:
	if (gpio_is_valid(ts->reset_gpio))
		gpio_free(ts->reset_gpio);
	if (gpio_is_valid(ts->irq_gpio))
		gpio_free(ts->irq_gpio);
err_irq_gpio_req:
	IQS_ERROR("EXIT  error");
	return ret;
}

/*
 * *
 * iqs9150_configure_dev - Finish device initialization
 *
 * @ts: our iqs9150_ts_data pointer
 *
 * Must be called from probe to finish initialization of the device.
 * Contains the common initialization code for both devices that
 * declare gpio pins and devices that do not. It is either called
 * directly from probe or from request_firmware_wait callback.
 */
static int iqs9150_configure_inpudev(struct iqs9150_ts_data *ts)
{
	int error = 0;

	ts->input_dev = input_allocate_device();
	if (!ts->input_dev) {
		IQS_ERROR("Failed to allocate input device.");
		return -ENOMEM;
	}

	ts->input_dev->name = "IQS9150";
	ts->input_dev->phys = "input/ts";
	ts->input_dev->id.product = IQS9150_PRODUCT_ID;
	ts->input_dev->id.vendor = IQS9150_VENDOR_ID;
	ts->input_dev->id.bustype = BUS_I2C;
	input_set_drvdata(ts->input_dev, ts);

	__set_bit(EV_SYN, ts->input_dev->evbit);
	__set_bit(EV_ABS, ts->input_dev->evbit);
	__set_bit(EV_KEY, ts->input_dev->evbit);
	__set_bit(BTN_TOUCH, ts->input_dev->keybit);
	__set_bit(INPUT_PROP_DIRECT, ts->input_dev->propbit);
	__set_bit(BTN_TOOL_FINGER, ts->input_dev->keybit);
	__set_bit(BTN_TOOL_DOUBLETAP, ts->input_dev->keybit);
	__set_bit(BTN_TOOL_TRIPLETAP, ts->input_dev->keybit);
	__set_bit(BTN_TOOL_QUADTAP, ts->input_dev->keybit);

	error = input_mt_init_slots(ts->input_dev, ts->max_touch_num,
					INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error) {
		IQS_ERROR("Failed to initialize MT slots: %d", error);
		return error;
	}

	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X,
			0, TOUCH_MAX_X, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y,
			0, TOUCH_MAX_Y, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 0xFF, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE, 0, 0xFFFF, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TRACKING_ID, 0, ts->max_touch_num-1, 0, 0);

	input_abs_set_res(ts->input_dev, ABS_MT_POSITION_X, X_RESOLUTION);
	input_abs_set_res(ts->input_dev, ABS_MT_POSITION_Y, Y_RESOLUTION);

	error = input_register_device(ts->input_dev);
	if (error) {
		IQS_ERROR("Failed to register input device: %d", error);
		goto err_register_input;
	}

	return 0;

err_register_input:
	input_set_drvdata(ts->input_dev, NULL);
	input_free_device(ts->input_dev);
	ts->input_dev = NULL;

	return error;
}

static int iqs9150_init(struct iqs9150_ts_data *ts)
{
	uint8_t buffer[INIT_BUFFER_COUNT] = {0};
	int ret = 0;

	IQS_FUNC_ENTER();
	/* Change the ALP ATI Compensation */
	/* Memory Map Position 0x115C - 0x1175 */
	buffer[0] = ALP_COMPENSATION_RX0_0;
	buffer[1] = ALP_COMPENSATION_RX0_1;
	buffer[2] = ALP_COMPENSATION_RX1_0;
	buffer[3] = ALP_COMPENSATION_RX1_1;
	buffer[4] = ALP_COMPENSATION_RX2_0;
	buffer[5] = ALP_COMPENSATION_RX2_1;
	buffer[6] = ALP_COMPENSATION_RX3_0;
	buffer[7] = ALP_COMPENSATION_RX3_1;
	buffer[8] = ALP_COMPENSATION_RX4_0;
	buffer[9] = ALP_COMPENSATION_RX4_1;
	buffer[10] = ALP_COMPENSATION_RX5_0;
	buffer[11] = ALP_COMPENSATION_RX5_1;
	buffer[12] = ALP_COMPENSATION_RX6_0;
	buffer[13] = ALP_COMPENSATION_RX6_1;
	buffer[14] = ALP_COMPENSATION_RX7_0;
	buffer[15] = ALP_COMPENSATION_RX7_1;
	buffer[16] = ALP_COMPENSATION_RX8_0;
	buffer[17] = ALP_COMPENSATION_RX8_1;
	buffer[18] = ALP_COMPENSATION_RX9_0;
	buffer[19] = ALP_COMPENSATION_RX9_1;
	buffer[20] = ALP_COMPENSATION_RX10_0;
	buffer[21] = ALP_COMPENSATION_RX10_1;
	buffer[22] = ALP_COMPENSATION_RX11_0;
	buffer[23] = ALP_COMPENSATION_RX11_1;
	buffer[24] = ALP_COMPENSATION_RX12_0;
	buffer[25] = ALP_COMPENSATION_RX12_1;
	ret = iqs9150_write_reg(ts->client, 0x115C, 26, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x115C-0x1175 failed");
		return ret;
	}

	/* Change the I2C Slave Address */
	/* Memory Map Position 0x1176 - 0x1177 */
	buffer[0] = I2C_KEY;
	buffer[1] = I2C_SLAVE_ADR;
	ret = iqs9150_write_reg(ts->client, 0x1176, 2, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1176-0x1177 failed");
		return ret;
	}

	/* Change the Settings Version Numbers */
	/* Memory Map Position 0x1178 - 0x1179 */
	buffer[0] = MINOR_VERSION;
	buffer[1] = MAJOR_VERSION;
	ret = iqs9150_write_reg(ts->client, 0x1178, 2, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1178-0x1179 failed");
		return ret;
	}

	/* Change the ATI Multipliers / Dividers */
	/* Memory Map Position 0x117A - 0x1195 */
	buffer[0] = TP_ATI_MULTDIV_L;
	buffer[1] = TP_ATI_MULTDIV_H;
	buffer[2] = ALP_ATI_COARSE_RX0_L;
	buffer[3] = ALP_ATI_COARSE_RX0_H;
	buffer[4] = ALP_ATI_COARSE_RX1_L;
	buffer[5] = ALP_ATI_COARSE_RX1_H;
	buffer[6] = ALP_ATI_COARSE_RX2_L;
	buffer[7] = ALP_ATI_COARSE_RX2_H;
	buffer[8] = ALP_ATI_COARSE_RX3_L;
	buffer[9] = ALP_ATI_COARSE_RX3_H;
	buffer[10] = ALP_ATI_COARSE_RX4_L;
	buffer[11] = ALP_ATI_COARSE_RX4_H;
	buffer[12] = ALP_ATI_COARSE_RX5_L;
	buffer[13] = ALP_ATI_COARSE_RX5_H;
	buffer[14] = ALP_ATI_COARSE_RX6_L;
	buffer[15] = ALP_ATI_COARSE_RX6_H;
	buffer[16] = ALP_ATI_COARSE_RX7_L;
	buffer[17] = ALP_ATI_COARSE_RX7_H;
	buffer[18] = ALP_ATI_COARSE_RX8_L;
	buffer[19] = ALP_ATI_COARSE_RX8_H;
	buffer[20] = ALP_ATI_COARSE_RX9_L;
	buffer[21] = ALP_ATI_COARSE_RX9_H;
	buffer[22] = ALP_ATI_COARSE_RX10_L;
	buffer[23] = ALP_ATI_COARSE_RX10_H;
	buffer[24] = ALP_ATI_COARSE_RX11_L;
	buffer[25] = ALP_ATI_COARSE_RX11_H;
	buffer[26] = ALP_ATI_COARSE_RX12_L;
	buffer[27] = ALP_ATI_COARSE_RX12_H;
	ret = iqs9150_write_reg(ts->client, 0x117A, 28, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x117A-0x1195 failed");
		return ret;
	}

	/* Change the ATI Settings */
	/* Memory Map Position 0x1196 - 0x11A1 */
	buffer[0] = TP_ATI_TARGET_0;
	buffer[1] = TP_ATI_TARGET_1;
	buffer[2] = ALP_ATI_TARGET_0;
	buffer[3] = ALP_ATI_TARGET_1;
	buffer[4] = ALP_BASE_TARGET_0;
	buffer[5] = ALP_BASE_TARGET_1;
	buffer[6] = TP_NEG_DELTA_REATI_0;
	buffer[7] = TP_NEG_DELTA_REATI_1;
	buffer[8] = TP_POS_DELTA_REATI_0;
	buffer[9] = TP_POS_DELTA_REATI_1;
	buffer[10] = TP_REF_DRIFT_LIMIT;
	buffer[11] = ALP_LTA_DRIFT_LIMIT;
	ret = iqs9150_write_reg(ts->client, 0x1196, 12, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1196-0x11A1 failed");
		return ret;
	}

	/* Change the Sampling Periods and Timing */
	/* Memory Map Position 0x11A2 - 0x11BB */
	buffer[0] = ACTIVE_MODE_SAMPLING_PERIOD_0;
	buffer[1] = ACTIVE_MODE_SAMPLING_PERIOD_1;
	buffer[2] = IDLE_TOUCH_MODE_SAMPLING_PERIOD_0;
	buffer[3] = IDLE_TOUCH_MODE_SAMPLING_PERIOD_1;
	buffer[4] = IDLE_MODE_SAMPLING_PERIOD_0;
	buffer[5] = IDLE_MODE_SAMPLING_PERIOD_1;
	buffer[6] = LP1_MODE_SAMPLING_PERIOD_0;
	buffer[7] = LP1_MODE_SAMPLING_PERIOD_1;
	buffer[8] = LP2_MODE_SAMPLING_PERIOD_0;
	buffer[9] = LP2_MODE_SAMPLING_PERIOD_1;
	buffer[10] = STATIONARY_TOUCH_TIMEOUT_0;
	buffer[11] = STATIONARY_TOUCH_TIMEOUT_1;
	buffer[12] = IDLE_TOUCH_MODE_TIMEOUT_0;
	buffer[13] = IDLE_TOUCH_MODE_TIMEOUT_1;
	buffer[14] = IDLE_MODE_TIMEOUT_0;
	buffer[15] = IDLE_MODE_TIMEOUT_1;
	buffer[16] = LP1_MODE_TIMEOUT_0;
	buffer[17] = LP1_MODE_TIMEOUT_1;
	buffer[18] = ACTIVE_MODE_TIMEOUT_0;
	buffer[19] = ACTIVE_MODE_TIMEOUT_1;
	buffer[20] = REATI_RETRY_TIME;
	buffer[21] = REF_UPDATE_TIME;
	buffer[22] = I2C_TIMEOUT_0;
	buffer[23] = I2C_TIMEOUT_1;
	buffer[24] = SNAP_TIMEOUT;
	buffer[25] = OPEN_TIMING;
	ret = iqs9150_write_reg(ts->client, 0x11A2, 26, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x11A2-0x11BB failed");
		return ret;
	}

	/* Change the System Settings */
	/* Memory Map Position 0x11BC - 0x11C1 */
	/*
	 * 0x80: Ack Reset - Acknowledge a reset,
	 * 0x20: TP Re-ATI - Queue a re-ATI on trackpad channels
	 */
	buffer[0] = SYSTEM_CONTROL_0|0x80|0x20;
	buffer[1] = SYSTEM_CONTROL_1;
	/*
	 * Bit 6: Terminate Comms Window - 1: Terminate Comms Window command,
	 *         followed by an I2C STOP
	 * end comms
	 */
	buffer[2] = CONFIG_SETTINGS_0|0x40;
	buffer[3] = CONFIG_SETTINGS_1|0x01;  /* 0x01: Event Mode- Enable event mode communication */
	buffer[4] = OTHER_SETTINGS_0;
	buffer[5] = OTHER_SETTINGS_1;
	ret = iqs9150_write_reg(ts->client, 0x11BC, 6, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x11BC-0x11C1 failed");
		return ret;
	}

	/* Change the ALP Settings */
	/* Memory Map Position 0x11C2 - 0x11CB */
	buffer[0] = ALP_SETUP_0;
	buffer[1] = ALP_SETUP_1;
	buffer[2] = ALP_SETUP_2;
	buffer[3] = ALP_SETUP_3;
	buffer[4] = ALP_TX_ENABLE_0;
	buffer[5] = ALP_TX_ENABLE_1;
	buffer[6] = ALP_TX_ENABLE_2;
	buffer[7] = ALP_TX_ENABLE_3;
	buffer[8] = ALP_TX_ENABLE_4;
	buffer[9] = ALP_TX_ENABLE_5;
	ret = iqs9150_write_reg(ts->client, 0x11C2, 10, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x11C2-0x11CB failed");
		return ret;
	}

	/*
	 * Change the Thresholds and Debounce Settings
	 * Memory Map Position 0x11CC - 0x11D3
	 */
	buffer[0] = TRACKPAD_TOUCH_SET_THRESHOLD;
	buffer[1] = TRACKPAD_TOUCH_CLEAR_THRESHOLD;
	buffer[2] = ALP_THRESHOLD;
	buffer[3] = ALP_AUTOPROX_THRESHOLD;
	buffer[4] = ALP_SET_DEBOUNCE;
	buffer[5] = ALP_CLEAR_DEBOUNCE;
	buffer[6] = SNAP_SET_THRESHOLD;
	buffer[7] = SNAP_CLEAR_THRESHOLD;
	ret = iqs9150_write_reg(ts->client, 0x11CC, 8, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x11CC-0x11D3 failed");
		return ret;
	}

	/* Change the ALP count and LTA Betas */
	/* Memory Map Position 0x11D4 - 0x11D7 */
	buffer[0] = ALP_COUNT_BETA_LP1;
	buffer[1] = ALP_LTA_BETA_LP1;
	buffer[2] = ALP_COUNT_BETA_LP2;
	buffer[3] = ALP_LTA_BETA_LP2;
	ret = iqs9150_write_reg(ts->client, 0x11D4, 4, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x11D4-0x11D7 failed");
		return ret;
	}

	/* Change the Hardware Settings */
	/* Memory Map Position 0x11D8 - 0x11E1 */
	buffer[0] = TP_FRAC;
	buffer[1] = TP_PERIOD1;
	buffer[2] = TP_PERIOD2;
	buffer[3] = ALP_FRAC;
	buffer[4] = ALP_PERIOD1;
	buffer[5] = ALP_PERIOD2;
	buffer[6] = TRACKPAD_HARDWARE_SETTINGS_0;
	buffer[7] = TRACKPAD_HARDWARE_SETTINGS_1;
	buffer[8] = ALP_HARDWARE_SETTINGS_0;
	buffer[9] = ALP_HARDWARE_SETTINGS_1;
	ret = iqs9150_write_reg(ts->client, 0x11D8, 10, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x11D8-0x11E1 failed");
		return ret;
	}

	/* Change the Trackpad Settings */
	/* Memory Map Position 0x11E2 - 0x11F5 */
	buffer[0] = TRACKPAD_SETTINGS_0_0;
	buffer[1] = TRACKPAD_SETTINGS_0_1;
	buffer[2] = TRACKPAD_SETTINGS_1_0;
	buffer[3] = TRACKPAD_SETTINGS_1_1;
	buffer[4] = X_RESOLUTION_0;
	buffer[5] = X_RESOLUTION_1;
	buffer[6] = Y_RESOLUTION_0;
	buffer[7] = Y_RESOLUTION_1;
	buffer[8] = XY_DYNAMIC_FILTER_BOTTOM_SPEED_0;
	buffer[9] = XY_DYNAMIC_FILTER_BOTTOM_SPEED_1;
	buffer[10] = XY_DYNAMIC_FILTER_TOP_SPEED_0;
	buffer[11] = XY_DYNAMIC_FILTER_TOP_SPEED_1;
	buffer[12] = XY_DYNAMIC_FILTER_BOTTOM_BETA;
	buffer[13] = XY_DYNAMIC_FILTER_STATIC_FILTER_BETA;
	buffer[14] = STATIONARY_TOUCH_MOV_THRESHOLD;
	buffer[15] = FINGER_SPLIT_FACTOR;
	buffer[16] = X_TRIM_VALUE;
	buffer[17] = Y_TRIM_VALUE;
	buffer[18] = JITTER_FILTER_DELTA;
	buffer[19] = FINGER_CONFIDENCE_THRESHOLD;
	ret = iqs9150_write_reg(ts->client, 0x11E2, 20, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x11E2-0x11F5 failed");
		return ret;
	}

	/* Change the Gesture Settings */
	/* Memory Map Position 0x11F6 - 0x1217 */
	buffer[0] = GESTURE_ENABLE_0;
	buffer[1] = GESTURE_ENABLE_1;
	buffer[2] = GESTURE_ENABLE_2F_0;
	buffer[3] = GESTURE_ENABLE_2F_1;
	buffer[4] = TAP_TOUCH_TIME_0;
	buffer[5] = TAP_TOUCH_TIME_1;
	buffer[6] = TAP_WAIT_TIME_0;
	buffer[7] = TAP_WAIT_TIME_1;
	buffer[8] = TAP_DISTANCE_0;
	buffer[9] = TAP_DISTANCE_1;
	buffer[10] = HOLD_TIME_0;
	buffer[11] = HOLD_TIME_1;
	buffer[12] = SWIPE_TIME_0;
	buffer[13] = SWIPE_TIME_1;
	buffer[14] = SWIPE_X_DISTANCE_0;
	buffer[15] = SWIPE_X_DISTANCE_1;
	buffer[16] = SWIPE_Y_DISTANCE_0;
	buffer[17] = SWIPE_Y_DISTANCE_1;
	buffer[18] = SWIPE_X_CONS_DIST_0;
	buffer[19] = SWIPE_X_CONS_DIST_1;
	buffer[20] = SWIPE_ANGLE;
	buffer[21] = SCROLL_ANGLE;
	buffer[22] = ZOOM_INIT_DIST_0;
	buffer[23] = ZOOM_INIT_DIST_1;
	buffer[24] = ZOOM_CONSECUTIVE_DIST_0;
	buffer[25] = ZOOM_CONSECUTIVE_DIST_1;
	buffer[26] = SCROLL_INIT_DIST_0;
	buffer[27] = SCROLL_INIT_DIST_1;
	buffer[28] = SCROLL_CONSECUTIVE_DIST_0;
	buffer[29] = SCROLL_CONSECUTIVE_DIST_1;
	buffer[30] = PALM_GESTURE_THRESHOLD_0;
	buffer[31] = PALM_GESTURE_THRESHOLD_1;
	ret = iqs9150_write_reg(ts->client, 0x11F6, 32, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x11F6-0x1217 failed");
		return ret;
	}

	/* Change the Rx/Tx Mapping */
	/* Memory Map Position 0x1218 - 0x1245 */
	buffer[0] = RX_TX_MAP_0;
	buffer[1] = RX_TX_MAP_1;
	buffer[2] = RX_TX_MAP_2;
	buffer[3] = RX_TX_MAP_3;
	buffer[4] = RX_TX_MAP_4;
	buffer[5] = RX_TX_MAP_5;
	buffer[6] = RX_TX_MAP_6;
	buffer[7] = RX_TX_MAP_7;
	buffer[8] = RX_TX_MAP_8;
	buffer[9] = RX_TX_MAP_9;
	buffer[10] = RX_TX_MAP_10;
	buffer[11] = RX_TX_MAP_11;
	buffer[12] = RX_TX_MAP_12;
	buffer[13] = RX_TX_MAP_13;
	buffer[14] = RX_TX_MAP_14;
	buffer[15] = RX_TX_MAP_15;
	buffer[16] = RX_TX_MAP_16;
	buffer[17] = RX_TX_MAP_17;
	buffer[18] = RX_TX_MAP_18;
	buffer[19] = RX_TX_MAP_19;
	buffer[20] = RX_TX_MAP_20;
	buffer[21] = RX_TX_MAP_21;
	buffer[22] = RX_TX_MAP_22;
	buffer[23] = RX_TX_MAP_23;
	buffer[24] = RX_TX_MAP_24;
	buffer[25] = RX_TX_MAP_25;
	buffer[26] = RX_TX_MAP_26;
	buffer[27] = RX_TX_MAP_27;
	buffer[28] = RX_TX_MAP_28;
	buffer[29] = RX_TX_MAP_29;
	buffer[30] = RX_TX_MAP_30;
	buffer[31] = RX_TX_MAP_31;
	buffer[32] = RX_TX_MAP_32;
	buffer[33] = RX_TX_MAP_33;
	buffer[34] = RX_TX_MAP_34;
	buffer[35] = RX_TX_MAP_35;
	buffer[36] = RX_TX_MAP_36;
	buffer[37] = RX_TX_MAP_37;
	buffer[38] = RX_TX_MAP_38;
	buffer[39] = RX_TX_MAP_39;
	buffer[40] = RX_TX_MAP_40;
	buffer[41] = RX_TX_MAP_41;
	buffer[42] = RX_TX_MAP_42;
	buffer[43] = RX_TX_MAP_43;
	buffer[44] = RX_TX_MAP_44;
	buffer[45] = RX_TX_OPEN;
	ret = iqs9150_write_reg(ts->client, 0x1218, 46, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1218-0x1245 failed");
		return ret;
	}


	/* Change the TP Channel Disables 0 */
	/* Memory Map Position 0x1246 - 0x1271 */
	buffer[0] = TPCHANNELDISABLE_0;
	buffer[1] = TPCHANNELDISABLE_1;
	buffer[2] = TPCHANNELDISABLE_2;
	buffer[3] = TPCHANNELDISABLE_3;
	buffer[4] = TPCHANNELDISABLE_4;
	buffer[5] = TPCHANNELDISABLE_5;
	buffer[6] = TPCHANNELDISABLE_6;
	buffer[7] = TPCHANNELDISABLE_7;
	buffer[8] = TPCHANNELDISABLE_8;
	buffer[9] = TPCHANNELDISABLE_9;
	buffer[10] = TPCHANNELDISABLE_10;
	buffer[11] = TPCHANNELDISABLE_11;
	buffer[12] = TPCHANNELDISABLE_12;
	buffer[13] = TPCHANNELDISABLE_13;
	buffer[14] = TPCHANNELDISABLE_14;
	buffer[15] = TPCHANNELDISABLE_15;
	buffer[16] = TPCHANNELDISABLE_16;
	buffer[17] = TPCHANNELDISABLE_17;
	buffer[18] = TPCHANNELDISABLE_18;
	buffer[19] = TPCHANNELDISABLE_19;
	buffer[20] = TPCHANNELDISABLE_20;
	buffer[21] = TPCHANNELDISABLE_21;
	buffer[22] = TPCHANNELDISABLE_22;
	buffer[23] = TPCHANNELDISABLE_23;
	buffer[24] = TPCHANNELDISABLE_24;
	buffer[25] = TPCHANNELDISABLE_25;
	buffer[26] = TPCHANNELDISABLE_26;
	buffer[27] = TPCHANNELDISABLE_27;
	buffer[28] = TPCHANNELDISABLE_28;
	buffer[29] = TPCHANNELDISABLE_29;
	buffer[30] = TPCHANNELDISABLE_30;
	buffer[31] = TPCHANNELDISABLE_31;
	buffer[32] = TPCHANNELDISABLE_32;
	buffer[33] = TPCHANNELDISABLE_33;
	buffer[34] = TPCHANNELDISABLE_34;
	buffer[35] = TPCHANNELDISABLE_35;
	buffer[36] = TPCHANNELDISABLE_36;
	buffer[37] = TPCHANNELDISABLE_37;
	buffer[38] = TPCHANNELDISABLE_38;
	buffer[39] = TPCHANNELDISABLE_39;
	buffer[40] = TPCHANNELDISABLE_40;
	buffer[41] = TPCHANNELDISABLE_41;
	buffer[42] = TPCHANNELDISABLE_42;
	buffer[43] = TPCHANNELDISABLE_43;
	ret = iqs9150_write_reg(ts->client, 0x1246, 44, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1246-0x1271 failed");
		return ret;
	}

	/* Change the TP Channel Disables 1 */
	/* Memory Map Position 0x1272 - 0x129D */
	buffer[0] = TPCHANNELDISABLE_44;
	buffer[1] = TPCHANNELDISABLE_45;
	buffer[2] = TPCHANNELDISABLE_46;
	buffer[3] = TPCHANNELDISABLE_47;
	buffer[4] = TPCHANNELDISABLE_48;
	buffer[5] = TPCHANNELDISABLE_49;
	buffer[6] = TPCHANNELDISABLE_50;
	buffer[7] = TPCHANNELDISABLE_51;
	buffer[8] = TPCHANNELDISABLE_52;
	buffer[9] = TPCHANNELDISABLE_53;
	buffer[10] = TPCHANNELDISABLE_54;
	buffer[11] = TPCHANNELDISABLE_55;
	buffer[12] = TPCHANNELDISABLE_56;
	buffer[13] = TPCHANNELDISABLE_57;
	buffer[14] = TPCHANNELDISABLE_58;
	buffer[15] = TPCHANNELDISABLE_59;
	buffer[16] = TPCHANNELDISABLE_60;
	buffer[17] = TPCHANNELDISABLE_61;
	buffer[18] = TPCHANNELDISABLE_62;
	buffer[19] = TPCHANNELDISABLE_63;
	buffer[20] = TPCHANNELDISABLE_64;
	buffer[21] = TPCHANNELDISABLE_65;
	buffer[22] = TPCHANNELDISABLE_66;
	buffer[23] = TPCHANNELDISABLE_67;
	buffer[24] = TPCHANNELDISABLE_68;
	buffer[25] = TPCHANNELDISABLE_69;
	buffer[26] = TPCHANNELDISABLE_70;
	buffer[27] = TPCHANNELDISABLE_71;
	buffer[28] = TPCHANNELDISABLE_72;
	buffer[29] = TPCHANNELDISABLE_73;
	buffer[30] = TPCHANNELDISABLE_74;
	buffer[31] = TPCHANNELDISABLE_75;
	buffer[32] = TPCHANNELDISABLE_76;
	buffer[33] = TPCHANNELDISABLE_77;
	buffer[34] = TPCHANNELDISABLE_78;
	buffer[35] = TPCHANNELDISABLE_79;
	buffer[36] = TPCHANNELDISABLE_80;
	buffer[37] = TPCHANNELDISABLE_81;
	buffer[38] = TPCHANNELDISABLE_82;
	buffer[39] = TPCHANNELDISABLE_83;
	buffer[40] = TPCHANNELDISABLE_84;
	buffer[41] = TPCHANNELDISABLE_85;
	buffer[42] = TPCHANNELDISABLE_86;
	buffer[43] = TPCHANNELDISABLE_87;
	ret = iqs9150_write_reg(ts->client, 0x1272, 44, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1272-0x129D failed");
		return ret;
	}

	/* Change the TP Snap Enable 0 */
	/* Memory Map Position 0x129E - 0x12C9 */
	buffer[0] = SNAPCHANNELENABLE_0;
	buffer[1] = SNAPCHANNELENABLE_1;
	buffer[2] = SNAPCHANNELENABLE_2;
	buffer[3] = SNAPCHANNELENABLE_3;
	buffer[4] = SNAPCHANNELENABLE_4;
	buffer[5] = SNAPCHANNELENABLE_5;
	buffer[6] = SNAPCHANNELENABLE_6;
	buffer[7] = SNAPCHANNELENABLE_7;
	buffer[8] = SNAPCHANNELENABLE_8;
	buffer[9] = SNAPCHANNELENABLE_9;
	buffer[10] = SNAPCHANNELENABLE_10;
	buffer[11] = SNAPCHANNELENABLE_11;
	buffer[12] = SNAPCHANNELENABLE_12;
	buffer[13] = SNAPCHANNELENABLE_13;
	buffer[14] = SNAPCHANNELENABLE_14;
	buffer[15] = SNAPCHANNELENABLE_15;
	buffer[16] = SNAPCHANNELENABLE_16;
	buffer[17] = SNAPCHANNELENABLE_17;
	buffer[18] = SNAPCHANNELENABLE_18;
	buffer[19] = SNAPCHANNELENABLE_19;
	buffer[20] = SNAPCHANNELENABLE_20;
	buffer[21] = SNAPCHANNELENABLE_21;
	buffer[22] = SNAPCHANNELENABLE_22;
	buffer[23] = SNAPCHANNELENABLE_23;
	buffer[24] = SNAPCHANNELENABLE_24;
	buffer[25] = SNAPCHANNELENABLE_25;
	buffer[26] = SNAPCHANNELENABLE_26;
	buffer[27] = SNAPCHANNELENABLE_27;
	buffer[28] = SNAPCHANNELENABLE_28;
	buffer[29] = SNAPCHANNELENABLE_29;
	buffer[30] = SNAPCHANNELENABLE_30;
	buffer[31] = SNAPCHANNELENABLE_31;
	buffer[32] = SNAPCHANNELENABLE_32;
	buffer[33] = SNAPCHANNELENABLE_33;
	buffer[34] = SNAPCHANNELENABLE_34;
	buffer[35] = SNAPCHANNELENABLE_35;
	buffer[36] = SNAPCHANNELENABLE_36;
	buffer[37] = SNAPCHANNELENABLE_37;
	buffer[38] = SNAPCHANNELENABLE_38;
	buffer[39] = SNAPCHANNELENABLE_39;
	buffer[40] = SNAPCHANNELENABLE_40;
	buffer[41] = SNAPCHANNELENABLE_41;
	buffer[42] = SNAPCHANNELENABLE_42;
	buffer[43] = SNAPCHANNELENABLE_43;
	ret = iqs9150_write_reg(ts->client, 0x129E, 44, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x129E-0x12C9 failed");
		return ret;
	}

	/* Change the TP Snap Enable 1 */
	/* Memory Map Position 0x12CA - 0x12F5 */
	buffer[0] = SNAPCHANNELENABLE_44;
	buffer[1] = SNAPCHANNELENABLE_45;
	buffer[2] = SNAPCHANNELENABLE_46;
	buffer[3] = SNAPCHANNELENABLE_47;
	buffer[4] = SNAPCHANNELENABLE_48;
	buffer[5] = SNAPCHANNELENABLE_49;
	buffer[6] = SNAPCHANNELENABLE_50;
	buffer[7] = SNAPCHANNELENABLE_51;
	buffer[8] = SNAPCHANNELENABLE_52;
	buffer[9] = SNAPCHANNELENABLE_53;
	buffer[10] = SNAPCHANNELENABLE_54;
	buffer[11] = SNAPCHANNELENABLE_55;
	buffer[12] = SNAPCHANNELENABLE_56;
	buffer[13] = SNAPCHANNELENABLE_57;
	buffer[14] = SNAPCHANNELENABLE_58;
	buffer[15] = SNAPCHANNELENABLE_59;
	buffer[16] = SNAPCHANNELENABLE_60;
	buffer[17] = SNAPCHANNELENABLE_61;
	buffer[18] = SNAPCHANNELENABLE_62;
	buffer[19] = SNAPCHANNELENABLE_63;
	buffer[20] = SNAPCHANNELENABLE_64;
	buffer[21] = SNAPCHANNELENABLE_65;
	buffer[22] = SNAPCHANNELENABLE_66;
	buffer[23] = SNAPCHANNELENABLE_67;
	buffer[24] = SNAPCHANNELENABLE_68;
	buffer[25] = SNAPCHANNELENABLE_69;
	buffer[26] = SNAPCHANNELENABLE_70;
	buffer[27] = SNAPCHANNELENABLE_71;
	buffer[28] = SNAPCHANNELENABLE_72;
	buffer[29] = SNAPCHANNELENABLE_73;
	buffer[30] = SNAPCHANNELENABLE_74;
	buffer[31] = SNAPCHANNELENABLE_75;
	buffer[32] = SNAPCHANNELENABLE_76;
	buffer[33] = SNAPCHANNELENABLE_77;
	buffer[34] = SNAPCHANNELENABLE_78;
	buffer[35] = SNAPCHANNELENABLE_79;
	buffer[36] = SNAPCHANNELENABLE_80;
	buffer[37] = SNAPCHANNELENABLE_81;
	buffer[38] = SNAPCHANNELENABLE_82;
	buffer[39] = SNAPCHANNELENABLE_83;
	buffer[40] = SNAPCHANNELENABLE_84;
	buffer[41] = SNAPCHANNELENABLE_85;
	buffer[42] = SNAPCHANNELENABLE_86;
	buffer[43] = SNAPCHANNELENABLE_87;
	ret = iqs9150_write_reg(ts->client, 0x12CA, 44, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x12CA-0x12F5 failed");
		return ret;
	}

	/* Change the Individual Touch Threshold Adjustments */
	/* Memory Map Position 0x12F6 - 0x1327 */
	buffer[0] = TOUCHTHRADJUST_0;
	buffer[1] = TOUCHTHRADJUST_1;
	buffer[2] = TOUCHTHRADJUST_2;
	buffer[3] = TOUCHTHRADJUST_3;
	buffer[4] = TOUCHTHRADJUST_4;
	buffer[5] = TOUCHTHRADJUST_5;
	buffer[6] = TOUCHTHRADJUST_6;
	buffer[7] = TOUCHTHRADJUST_7;
	buffer[8] = TOUCHTHRADJUST_8;
	buffer[9] = TOUCHTHRADJUST_9;
	buffer[10] = TOUCHTHRADJUST_10;
	buffer[11] = TOUCHTHRADJUST_11;
	buffer[12] = TOUCHTHRADJUST_12;
	buffer[13] = TOUCHTHRADJUST_13;
	buffer[14] = TOUCHTHRADJUST_14;
	buffer[15] = TOUCHTHRADJUST_15;
	buffer[16] = TOUCHTHRADJUST_16;
	buffer[17] = TOUCHTHRADJUST_17;
	buffer[18] = TOUCHTHRADJUST_18;
	buffer[19] = TOUCHTHRADJUST_19;
	buffer[20] = TOUCHTHRADJUST_20;
	buffer[21] = TOUCHTHRADJUST_21;
	buffer[22] = TOUCHTHRADJUST_22;
	buffer[23] = TOUCHTHRADJUST_23;
	buffer[24] = TOUCHTHRADJUST_24;
	buffer[25] = TOUCHTHRADJUST_25;
	buffer[26] = TOUCHTHRADJUST_26;
	buffer[27] = TOUCHTHRADJUST_27;
	buffer[28] = TOUCHTHRADJUST_28;
	buffer[29] = TOUCHTHRADJUST_29;
	buffer[30] = TOUCHTHRADJUST_30;
	buffer[31] = TOUCHTHRADJUST_31;
	buffer[32] = TOUCHTHRADJUST_32;
	buffer[33] = TOUCHTHRADJUST_33;
	buffer[34] = TOUCHTHRADJUST_34;
	buffer[35] = TOUCHTHRADJUST_35;
	buffer[36] = TOUCHTHRADJUST_36;
	buffer[37] = TOUCHTHRADJUST_37;
	buffer[38] = TOUCHTHRADJUST_38;
	buffer[39] = TOUCHTHRADJUST_39;
	buffer[40] = TOUCHTHRADJUST_40;
	buffer[41] = TOUCHTHRADJUST_41;
	buffer[42] = TOUCHTHRADJUST_42;
	buffer[43] = TOUCHTHRADJUST_43;
	buffer[44] = TOUCHTHRADJUST_44;
	buffer[45] = TOUCHTHRADJUST_45;
	buffer[46] = TOUCHTHRADJUST_46;
	buffer[47] = TOUCHTHRADJUST_47;
	buffer[48] = TOUCHTHRADJUST_48;
	buffer[49] = TOUCHTHRADJUST_49;
	ret = iqs9150_write_reg(ts->client, 0x12F6, 50, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x12F6-0x1327 failed");
		return ret;
	}

	/* Change the Individual Touch Threshold Adjustments */
	/* Memory Map Position 0x1328 - 0x1359 */
	buffer[0] = TOUCHTHRADJUST_50;
	buffer[1] = TOUCHTHRADJUST_51;
	buffer[2] = TOUCHTHRADJUST_52;
	buffer[3] = TOUCHTHRADJUST_53;
	buffer[4] = TOUCHTHRADJUST_54;
	buffer[5] = TOUCHTHRADJUST_55;
	buffer[6] = TOUCHTHRADJUST_56;
	buffer[7] = TOUCHTHRADJUST_57;
	buffer[8] = TOUCHTHRADJUST_58;
	buffer[9] = TOUCHTHRADJUST_59;
	buffer[10] = TOUCHTHRADJUST_60;
	buffer[11] = TOUCHTHRADJUST_61;
	buffer[12] = TOUCHTHRADJUST_62;
	buffer[13] = TOUCHTHRADJUST_63;
	buffer[14] = TOUCHTHRADJUST_64;
	buffer[15] = TOUCHTHRADJUST_65;
	buffer[16] = TOUCHTHRADJUST_66;
	buffer[17] = TOUCHTHRADJUST_67;
	buffer[18] = TOUCHTHRADJUST_68;
	buffer[19] = TOUCHTHRADJUST_69;
	buffer[20] = TOUCHTHRADJUST_70;
	buffer[21] = TOUCHTHRADJUST_71;
	buffer[22] = TOUCHTHRADJUST_72;
	buffer[23] = TOUCHTHRADJUST_73;
	buffer[24] = TOUCHTHRADJUST_74;
	buffer[25] = TOUCHTHRADJUST_75;
	buffer[26] = TOUCHTHRADJUST_76;
	buffer[27] = TOUCHTHRADJUST_77;
	buffer[28] = TOUCHTHRADJUST_78;
	buffer[29] = TOUCHTHRADJUST_79;
	buffer[30] = TOUCHTHRADJUST_80;
	buffer[31] = TOUCHTHRADJUST_81;
	buffer[32] = TOUCHTHRADJUST_82;
	buffer[33] = TOUCHTHRADJUST_83;
	buffer[34] = TOUCHTHRADJUST_84;
	buffer[35] = TOUCHTHRADJUST_85;
	buffer[36] = TOUCHTHRADJUST_86;
	buffer[37] = TOUCHTHRADJUST_87;
	buffer[38] = TOUCHTHRADJUST_88;
	buffer[39] = TOUCHTHRADJUST_89;
	buffer[40] = TOUCHTHRADJUST_90;
	buffer[41] = TOUCHTHRADJUST_91;
	buffer[42] = TOUCHTHRADJUST_92;
	buffer[43] = TOUCHTHRADJUST_93;
	buffer[44] = TOUCHTHRADJUST_94;
	buffer[45] = TOUCHTHRADJUST_95;
	buffer[46] = TOUCHTHRADJUST_96;
	buffer[47] = TOUCHTHRADJUST_97;
	buffer[48] = TOUCHTHRADJUST_98;
	buffer[49] = TOUCHTHRADJUST_99;
	ret = iqs9150_write_reg(ts->client, 0x1328, 50, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1328-0x1359 failed");
		return ret;
	}

	/* Change the Individual Touch Threshold Adjustments */
	/* Memory Map Position 0x135A - 0x138B */
	buffer[0] = TOUCHTHRADJUST_100;
	buffer[1] = TOUCHTHRADJUST_101;
	buffer[2] = TOUCHTHRADJUST_102;
	buffer[3] = TOUCHTHRADJUST_103;
	buffer[4] = TOUCHTHRADJUST_104;
	buffer[5] = TOUCHTHRADJUST_105;
	buffer[6] = TOUCHTHRADJUST_106;
	buffer[7] = TOUCHTHRADJUST_107;
	buffer[8] = TOUCHTHRADJUST_108;
	buffer[9] = TOUCHTHRADJUST_109;
	buffer[10] = TOUCHTHRADJUST_110;
	buffer[11] = TOUCHTHRADJUST_111;
	buffer[12] = TOUCHTHRADJUST_112;
	buffer[13] = TOUCHTHRADJUST_113;
	buffer[14] = TOUCHTHRADJUST_114;
	buffer[15] = TOUCHTHRADJUST_115;
	buffer[16] = TOUCHTHRADJUST_116;
	buffer[17] = TOUCHTHRADJUST_117;
	buffer[18] = TOUCHTHRADJUST_118;
	buffer[19] = TOUCHTHRADJUST_119;
	buffer[20] = TOUCHTHRADJUST_120;
	buffer[21] = TOUCHTHRADJUST_121;
	buffer[22] = TOUCHTHRADJUST_122;
	buffer[23] = TOUCHTHRADJUST_123;
	buffer[24] = TOUCHTHRADJUST_124;
	buffer[25] = TOUCHTHRADJUST_125;
	buffer[26] = TOUCHTHRADJUST_126;
	buffer[27] = TOUCHTHRADJUST_127;
	buffer[28] = TOUCHTHRADJUST_128;
	buffer[29] = TOUCHTHRADJUST_129;
	buffer[30] = TOUCHTHRADJUST_130;
	buffer[31] = TOUCHTHRADJUST_131;
	buffer[32] = TOUCHTHRADJUST_132;
	buffer[33] = TOUCHTHRADJUST_133;
	buffer[34] = TOUCHTHRADJUST_134;
	buffer[35] = TOUCHTHRADJUST_135;
	buffer[36] = TOUCHTHRADJUST_136;
	buffer[37] = TOUCHTHRADJUST_137;
	buffer[38] = TOUCHTHRADJUST_138;
	buffer[39] = TOUCHTHRADJUST_139;
	buffer[40] = TOUCHTHRADJUST_140;
	buffer[41] = TOUCHTHRADJUST_141;
	buffer[42] = TOUCHTHRADJUST_142;
	buffer[43] = TOUCHTHRADJUST_143;
	buffer[44] = TOUCHTHRADJUST_144;
	buffer[45] = TOUCHTHRADJUST_145;
	buffer[46] = TOUCHTHRADJUST_146;
	buffer[47] = TOUCHTHRADJUST_147;
	buffer[48] = TOUCHTHRADJUST_148;
	buffer[49] = TOUCHTHRADJUST_149;
	ret = iqs9150_write_reg(ts->client, 0x135A, 50, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x135A-0x138B failed");
		return ret;
	}

	/* Change the Individual Touch Threshold Adjustments */
	/* Memory Map Position 0x138C - 0x13BD */
	buffer[0] = TOUCHTHRADJUST_150;
	buffer[1] = TOUCHTHRADJUST_151;
	buffer[2] = TOUCHTHRADJUST_152;
	buffer[3] = TOUCHTHRADJUST_153;
	buffer[4] = TOUCHTHRADJUST_154;
	buffer[5] = TOUCHTHRADJUST_155;
	buffer[6] = TOUCHTHRADJUST_156;
	buffer[7] = TOUCHTHRADJUST_157;
	buffer[8] = TOUCHTHRADJUST_158;
	buffer[9] = TOUCHTHRADJUST_159;
	buffer[10] = TOUCHTHRADJUST_160;
	buffer[11] = TOUCHTHRADJUST_161;
	buffer[12] = TOUCHTHRADJUST_162;
	buffer[13] = TOUCHTHRADJUST_163;
	buffer[14] = TOUCHTHRADJUST_164;
	buffer[15] = TOUCHTHRADJUST_165;
	buffer[16] = TOUCHTHRADJUST_166;
	buffer[17] = TOUCHTHRADJUST_167;
	buffer[18] = TOUCHTHRADJUST_168;
	buffer[19] = TOUCHTHRADJUST_169;
	buffer[20] = TOUCHTHRADJUST_170;
	buffer[21] = TOUCHTHRADJUST_171;
	buffer[22] = TOUCHTHRADJUST_172;
	buffer[23] = TOUCHTHRADJUST_173;
	buffer[24] = TOUCHTHRADJUST_174;
	buffer[25] = TOUCHTHRADJUST_175;
	buffer[26] = TOUCHTHRADJUST_176;
	buffer[27] = TOUCHTHRADJUST_177;
	buffer[28] = TOUCHTHRADJUST_178;
	buffer[29] = TOUCHTHRADJUST_179;
	buffer[30] = TOUCHTHRADJUST_180;
	buffer[31] = TOUCHTHRADJUST_181;
	buffer[32] = TOUCHTHRADJUST_182;
	buffer[33] = TOUCHTHRADJUST_183;
	buffer[34] = TOUCHTHRADJUST_184;
	buffer[35] = TOUCHTHRADJUST_185;
	buffer[36] = TOUCHTHRADJUST_186;
	buffer[37] = TOUCHTHRADJUST_187;
	buffer[38] = TOUCHTHRADJUST_188;
	buffer[39] = TOUCHTHRADJUST_189;
	buffer[40] = TOUCHTHRADJUST_190;
	buffer[41] = TOUCHTHRADJUST_191;
	buffer[42] = TOUCHTHRADJUST_192;
	buffer[43] = TOUCHTHRADJUST_193;
	buffer[44] = TOUCHTHRADJUST_194;
	buffer[45] = TOUCHTHRADJUST_195;
	buffer[46] = TOUCHTHRADJUST_196;
	buffer[47] = TOUCHTHRADJUST_197;
	buffer[48] = TOUCHTHRADJUST_198;
	buffer[49] = TOUCHTHRADJUST_199;
	ret = iqs9150_write_reg(ts->client, 0x138C, 50, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x138C-0x13BD failed");
		return ret;
	}

	/* Change the Individual Touch Threshold Adjustments */
	/* Memory Map Position 0x13BE - 0x13EF */
	buffer[0] = TOUCHTHRADJUST_200;
	buffer[1] = TOUCHTHRADJUST_201;
	buffer[2] = TOUCHTHRADJUST_202;
	buffer[3] = TOUCHTHRADJUST_203;
	buffer[4] = TOUCHTHRADJUST_204;
	buffer[5] = TOUCHTHRADJUST_205;
	buffer[6] = TOUCHTHRADJUST_206;
	buffer[7] = TOUCHTHRADJUST_207;
	buffer[8] = TOUCHTHRADJUST_208;
	buffer[9] = TOUCHTHRADJUST_209;
	buffer[10] = TOUCHTHRADJUST_210;
	buffer[11] = TOUCHTHRADJUST_211;
	buffer[12] = TOUCHTHRADJUST_212;
	buffer[13] = TOUCHTHRADJUST_213;
	buffer[14] = TOUCHTHRADJUST_214;
	buffer[15] = TOUCHTHRADJUST_215;
	buffer[16] = TOUCHTHRADJUST_216;
	buffer[17] = TOUCHTHRADJUST_217;
	buffer[18] = TOUCHTHRADJUST_218;
	buffer[19] = TOUCHTHRADJUST_219;
	buffer[20] = TOUCHTHRADJUST_220;
	buffer[21] = TOUCHTHRADJUST_221;
	buffer[22] = TOUCHTHRADJUST_222;
	buffer[23] = TOUCHTHRADJUST_223;
	buffer[24] = TOUCHTHRADJUST_224;
	buffer[25] = TOUCHTHRADJUST_225;
	buffer[26] = TOUCHTHRADJUST_226;
	buffer[27] = TOUCHTHRADJUST_227;
	buffer[28] = TOUCHTHRADJUST_228;
	buffer[29] = TOUCHTHRADJUST_229;
	buffer[30] = TOUCHTHRADJUST_230;
	buffer[31] = TOUCHTHRADJUST_231;
	buffer[32] = TOUCHTHRADJUST_232;
	buffer[33] = TOUCHTHRADJUST_233;
	buffer[34] = TOUCHTHRADJUST_234;
	buffer[35] = TOUCHTHRADJUST_235;
	buffer[36] = TOUCHTHRADJUST_236;
	buffer[37] = TOUCHTHRADJUST_237;
	buffer[38] = TOUCHTHRADJUST_238;
	buffer[39] = TOUCHTHRADJUST_239;
	buffer[40] = TOUCHTHRADJUST_240;
	buffer[41] = TOUCHTHRADJUST_241;
	buffer[42] = TOUCHTHRADJUST_242;
	buffer[43] = TOUCHTHRADJUST_243;
	buffer[44] = TOUCHTHRADJUST_244;
	buffer[45] = TOUCHTHRADJUST_245;
	buffer[46] = TOUCHTHRADJUST_246;
	buffer[47] = TOUCHTHRADJUST_247;
	buffer[48] = TOUCHTHRADJUST_248;
	buffer[49] = TOUCHTHRADJUST_249;
	ret = iqs9150_write_reg(ts->client, 0x13BE, 50, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x13BE-0x13EF failed");
		return ret;
	}

	/* Change the Individual Touch Threshold Adjustments */
	/* Memory Map Position 0x13F0 - 0x1421 */
	buffer[0] = TOUCHTHRADJUST_250;
	buffer[1] = TOUCHTHRADJUST_251;
	buffer[2] = TOUCHTHRADJUST_252;
	buffer[3] = TOUCHTHRADJUST_253;
	buffer[4] = TOUCHTHRADJUST_254;
	buffer[5] = TOUCHTHRADJUST_255;
	buffer[6] = TOUCHTHRADJUST_256;
	buffer[7] = TOUCHTHRADJUST_257;
	buffer[8] = TOUCHTHRADJUST_258;
	buffer[9] = TOUCHTHRADJUST_259;
	buffer[10] = TOUCHTHRADJUST_260;
	buffer[11] = TOUCHTHRADJUST_261;
	buffer[12] = TOUCHTHRADJUST_262;
	buffer[13] = TOUCHTHRADJUST_263;
	buffer[14] = TOUCHTHRADJUST_264;
	buffer[15] = TOUCHTHRADJUST_265;
	buffer[16] = TOUCHTHRADJUST_266;
	buffer[17] = TOUCHTHRADJUST_267;
	buffer[18] = TOUCHTHRADJUST_268;
	buffer[19] = TOUCHTHRADJUST_269;
	buffer[20] = TOUCHTHRADJUST_270;
	buffer[21] = TOUCHTHRADJUST_271;
	buffer[22] = TOUCHTHRADJUST_272;
	buffer[23] = TOUCHTHRADJUST_273;
	buffer[24] = TOUCHTHRADJUST_274;
	buffer[25] = TOUCHTHRADJUST_275;
	buffer[26] = TOUCHTHRADJUST_276;
	buffer[27] = TOUCHTHRADJUST_277;
	buffer[28] = TOUCHTHRADJUST_278;
	buffer[29] = TOUCHTHRADJUST_279;
	buffer[30] = TOUCHTHRADJUST_280;
	buffer[31] = TOUCHTHRADJUST_281;
	buffer[32] = TOUCHTHRADJUST_282;
	buffer[33] = TOUCHTHRADJUST_283;
	buffer[34] = TOUCHTHRADJUST_284;
	buffer[35] = TOUCHTHRADJUST_285;
	buffer[36] = TOUCHTHRADJUST_286;
	buffer[37] = TOUCHTHRADJUST_287;
	buffer[38] = TOUCHTHRADJUST_288;
	buffer[39] = TOUCHTHRADJUST_289;
	buffer[40] = TOUCHTHRADJUST_290;
	buffer[41] = TOUCHTHRADJUST_291;
	buffer[42] = TOUCHTHRADJUST_292;
	buffer[43] = TOUCHTHRADJUST_293;
	buffer[44] = TOUCHTHRADJUST_294;
	buffer[45] = TOUCHTHRADJUST_295;
	buffer[46] = TOUCHTHRADJUST_296;
	buffer[47] = TOUCHTHRADJUST_297;
	buffer[48] = TOUCHTHRADJUST_298;
	buffer[49] = TOUCHTHRADJUST_299;
	ret = iqs9150_write_reg(ts->client, 0x13F0, 50, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x13F0-0x1421 failed");
		return ret;
	}

	/* Change the Individual Touch Threshold Adjustments */
	/* Memory Map Position 0x1422 - 0x1453 */
	buffer[0] = TOUCHTHRADJUST_300;
	buffer[1] = TOUCHTHRADJUST_301;
	buffer[2] = TOUCHTHRADJUST_302;
	buffer[3] = TOUCHTHRADJUST_303;
	buffer[4] = TOUCHTHRADJUST_304;
	buffer[5] = TOUCHTHRADJUST_305;
	buffer[6] = TOUCHTHRADJUST_306;
	buffer[7] = TOUCHTHRADJUST_307;
	buffer[8] = TOUCHTHRADJUST_308;
	buffer[9] = TOUCHTHRADJUST_309;
	buffer[10] = TOUCHTHRADJUST_310;
	buffer[11] = TOUCHTHRADJUST_311;
	buffer[12] = TOUCHTHRADJUST_312;
	buffer[13] = TOUCHTHRADJUST_313;
	buffer[14] = TOUCHTHRADJUST_314;
	buffer[15] = TOUCHTHRADJUST_315;
	buffer[16] = TOUCHTHRADJUST_316;
	buffer[17] = TOUCHTHRADJUST_317;
	buffer[18] = TOUCHTHRADJUST_318;
	buffer[19] = TOUCHTHRADJUST_319;
	buffer[20] = TOUCHTHRADJUST_320;
	buffer[21] = TOUCHTHRADJUST_321;
	buffer[22] = TOUCHTHRADJUST_322;
	buffer[23] = TOUCHTHRADJUST_323;
	buffer[24] = TOUCHTHRADJUST_324;
	buffer[25] = TOUCHTHRADJUST_325;
	buffer[26] = TOUCHTHRADJUST_326;
	buffer[27] = TOUCHTHRADJUST_327;
	buffer[28] = TOUCHTHRADJUST_328;
	buffer[29] = TOUCHTHRADJUST_329;
	buffer[30] = TOUCHTHRADJUST_330;
	buffer[31] = TOUCHTHRADJUST_331;
	buffer[32] = TOUCHTHRADJUST_332;
	buffer[33] = TOUCHTHRADJUST_333;
	buffer[34] = TOUCHTHRADJUST_334;
	buffer[35] = TOUCHTHRADJUST_335;
	buffer[36] = TOUCHTHRADJUST_336;
	buffer[37] = TOUCHTHRADJUST_337;
	buffer[38] = TOUCHTHRADJUST_338;
	buffer[39] = TOUCHTHRADJUST_339;
	buffer[40] = TOUCHTHRADJUST_340;
	buffer[41] = TOUCHTHRADJUST_341;
	buffer[42] = TOUCHTHRADJUST_342;
	buffer[43] = TOUCHTHRADJUST_343;
	buffer[44] = TOUCHTHRADJUST_344;
	buffer[45] = TOUCHTHRADJUST_345;
	buffer[46] = TOUCHTHRADJUST_346;
	buffer[47] = TOUCHTHRADJUST_347;
	buffer[48] = TOUCHTHRADJUST_348;
	buffer[49] = TOUCHTHRADJUST_349;
	ret = iqs9150_write_reg(ts->client, 0x1422, 50, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1422-0x1453 failed");
		return ret;
	}

	/* Change the Individual Touch Threshold Adjustments */
	/* Memory Map Position 0x1454 - 0x1485 */
	buffer[0] = TOUCHTHRADJUST_350;
	buffer[1] = TOUCHTHRADJUST_351;
	buffer[2] = TOUCHTHRADJUST_352;
	buffer[3] = TOUCHTHRADJUST_353;
	buffer[4] = TOUCHTHRADJUST_354;
	buffer[5] = TOUCHTHRADJUST_355;
	buffer[6] = TOUCHTHRADJUST_356;
	buffer[7] = TOUCHTHRADJUST_357;
	buffer[8] = TOUCHTHRADJUST_358;
	buffer[9] = TOUCHTHRADJUST_359;
	buffer[10] = TOUCHTHRADJUST_360;
	buffer[11] = TOUCHTHRADJUST_361;
	buffer[12] = TOUCHTHRADJUST_362;
	buffer[13] = TOUCHTHRADJUST_363;
	buffer[14] = TOUCHTHRADJUST_364;
	buffer[15] = TOUCHTHRADJUST_365;
	buffer[16] = TOUCHTHRADJUST_366;
	buffer[17] = TOUCHTHRADJUST_367;
	buffer[18] = TOUCHTHRADJUST_368;
	buffer[19] = TOUCHTHRADJUST_369;
	buffer[20] = TOUCHTHRADJUST_370;
	buffer[21] = TOUCHTHRADJUST_371;
	buffer[22] = TOUCHTHRADJUST_372;
	buffer[23] = TOUCHTHRADJUST_373;
	buffer[24] = TOUCHTHRADJUST_374;
	buffer[25] = TOUCHTHRADJUST_375;
	buffer[26] = TOUCHTHRADJUST_376;
	buffer[27] = TOUCHTHRADJUST_377;
	buffer[28] = TOUCHTHRADJUST_378;
	buffer[29] = TOUCHTHRADJUST_379;
	buffer[30] = TOUCHTHRADJUST_380;
	buffer[31] = TOUCHTHRADJUST_381;
	buffer[32] = TOUCHTHRADJUST_382;
	buffer[33] = TOUCHTHRADJUST_383;
	buffer[34] = TOUCHTHRADJUST_384;
	buffer[35] = TOUCHTHRADJUST_385;
	buffer[36] = TOUCHTHRADJUST_386;
	buffer[37] = TOUCHTHRADJUST_387;
	buffer[38] = TOUCHTHRADJUST_388;
	buffer[39] = TOUCHTHRADJUST_389;
	buffer[40] = TOUCHTHRADJUST_390;
	buffer[41] = TOUCHTHRADJUST_391;
	buffer[42] = TOUCHTHRADJUST_392;
	buffer[43] = TOUCHTHRADJUST_393;
	buffer[44] = TOUCHTHRADJUST_394;
	buffer[45] = TOUCHTHRADJUST_395;
	buffer[46] = TOUCHTHRADJUST_396;
	buffer[47] = TOUCHTHRADJUST_397;
	buffer[48] = TOUCHTHRADJUST_398;
	buffer[49] = TOUCHTHRADJUST_399;
	ret = iqs9150_write_reg(ts->client, 0x1454, 50, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1454-0x1485 failed");
		return ret;
	}

	/* Change the Individual Touch Threshold Adjustments */
	/* Memory Map Position 0x1486 - 0x14B7 */
	buffer[0] = TOUCHTHRADJUST_400;
	buffer[1] = TOUCHTHRADJUST_401;
	buffer[2] = TOUCHTHRADJUST_402;
	buffer[3] = TOUCHTHRADJUST_403;
	buffer[4] = TOUCHTHRADJUST_404;
	buffer[5] = TOUCHTHRADJUST_405;
	buffer[6] = TOUCHTHRADJUST_406;
	buffer[7] = TOUCHTHRADJUST_407;
	buffer[8] = TOUCHTHRADJUST_408;
	buffer[9] = TOUCHTHRADJUST_409;
	buffer[10] = TOUCHTHRADJUST_410;
	buffer[11] = TOUCHTHRADJUST_411;
	buffer[12] = TOUCHTHRADJUST_412;
	buffer[13] = TOUCHTHRADJUST_413;
	buffer[14] = TOUCHTHRADJUST_414;
	buffer[15] = TOUCHTHRADJUST_415;
	buffer[16] = TOUCHTHRADJUST_416;
	buffer[17] = TOUCHTHRADJUST_417;
	buffer[18] = TOUCHTHRADJUST_418;
	buffer[19] = TOUCHTHRADJUST_419;
	buffer[20] = TOUCHTHRADJUST_420;
	buffer[21] = TOUCHTHRADJUST_421;
	buffer[22] = TOUCHTHRADJUST_422;
	buffer[23] = TOUCHTHRADJUST_423;
	buffer[24] = TOUCHTHRADJUST_424;
	buffer[25] = TOUCHTHRADJUST_425;
	buffer[26] = TOUCHTHRADJUST_426;
	buffer[27] = TOUCHTHRADJUST_427;
	buffer[28] = TOUCHTHRADJUST_428;
	buffer[29] = TOUCHTHRADJUST_429;
	buffer[30] = TOUCHTHRADJUST_430;
	buffer[31] = TOUCHTHRADJUST_431;
	buffer[32] = TOUCHTHRADJUST_432;
	buffer[33] = TOUCHTHRADJUST_433;
	buffer[34] = TOUCHTHRADJUST_434;
	buffer[35] = TOUCHTHRADJUST_435;
	buffer[36] = TOUCHTHRADJUST_436;
	buffer[37] = TOUCHTHRADJUST_437;
	buffer[38] = TOUCHTHRADJUST_438;
	buffer[39] = TOUCHTHRADJUST_439;
	buffer[40] = TOUCHTHRADJUST_440;
	buffer[41] = TOUCHTHRADJUST_441;
	buffer[42] = TOUCHTHRADJUST_442;
	buffer[43] = TOUCHTHRADJUST_443;
	buffer[44] = TOUCHTHRADJUST_444;
	buffer[45] = TOUCHTHRADJUST_445;
	buffer[46] = TOUCHTHRADJUST_446;
	buffer[47] = TOUCHTHRADJUST_447;
	buffer[48] = TOUCHTHRADJUST_448;
	buffer[49] = TOUCHTHRADJUST_449;
	ret = iqs9150_write_reg(ts->client, 0x1486, 50, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1486-0x14B7 failed");
		return ret;
	}

	/* Change the Individual Touch Threshold Adjustments */
	/* Memory Map Position 0x14B8 - 0x14E9 */
	buffer[0] = TOUCHTHRADJUST_450;
	buffer[1] = TOUCHTHRADJUST_451;
	buffer[2] = TOUCHTHRADJUST_452;
	buffer[3] = TOUCHTHRADJUST_453;
	buffer[4] = TOUCHTHRADJUST_454;
	buffer[5] = TOUCHTHRADJUST_455;
	buffer[6] = TOUCHTHRADJUST_456;
	buffer[7] = TOUCHTHRADJUST_457;
	buffer[8] = TOUCHTHRADJUST_458;
	buffer[9] = TOUCHTHRADJUST_459;
	buffer[10] = TOUCHTHRADJUST_460;
	buffer[11] = TOUCHTHRADJUST_461;
	buffer[12] = TOUCHTHRADJUST_462;
	buffer[13] = TOUCHTHRADJUST_463;
	buffer[14] = TOUCHTHRADJUST_464;
	buffer[15] = TOUCHTHRADJUST_465;
	buffer[16] = TOUCHTHRADJUST_466;
	buffer[17] = TOUCHTHRADJUST_467;
	buffer[18] = TOUCHTHRADJUST_468;
	buffer[19] = TOUCHTHRADJUST_469;
	buffer[20] = TOUCHTHRADJUST_470;
	buffer[21] = TOUCHTHRADJUST_471;
	buffer[22] = TOUCHTHRADJUST_472;
	buffer[23] = TOUCHTHRADJUST_473;
	buffer[24] = TOUCHTHRADJUST_474;
	buffer[25] = TOUCHTHRADJUST_475;
	buffer[26] = TOUCHTHRADJUST_476;
	buffer[27] = TOUCHTHRADJUST_477;
	buffer[28] = TOUCHTHRADJUST_478;
	buffer[29] = TOUCHTHRADJUST_479;
	buffer[30] = TOUCHTHRADJUST_480;
	buffer[31] = TOUCHTHRADJUST_481;
	buffer[32] = TOUCHTHRADJUST_482;
	buffer[33] = TOUCHTHRADJUST_483;
	buffer[34] = TOUCHTHRADJUST_484;
	buffer[35] = TOUCHTHRADJUST_485;
	buffer[36] = TOUCHTHRADJUST_486;
	buffer[37] = TOUCHTHRADJUST_487;
	buffer[38] = TOUCHTHRADJUST_488;
	buffer[39] = TOUCHTHRADJUST_489;
	buffer[40] = TOUCHTHRADJUST_490;
	buffer[41] = TOUCHTHRADJUST_491;
	buffer[42] = TOUCHTHRADJUST_492;
	buffer[43] = TOUCHTHRADJUST_493;
	buffer[44] = TOUCHTHRADJUST_494;
	buffer[45] = TOUCHTHRADJUST_495;
	buffer[46] = TOUCHTHRADJUST_496;
	buffer[47] = TOUCHTHRADJUST_497;
	buffer[48] = TOUCHTHRADJUST_498;
	buffer[49] = TOUCHTHRADJUST_499;
	ret = iqs9150_write_reg(ts->client, 0x14B8, 50, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x14B8-0x14E9 failed");
		return ret;
	}

	/* Change the Individual Touch Threshold Adjustments */
	/* Memory Map Position 0x14EA - 0x14EF */
	buffer[0] = TOUCHTHRADJUST_500;
	buffer[1] = TOUCHTHRADJUST_501;
	buffer[2] = TOUCHTHRADJUST_502;
	buffer[3] = TOUCHTHRADJUST_503;
	buffer[4] = TOUCHTHRADJUST_504;
	buffer[5] = TOUCHTHRADJUST_505;
	ret = iqs9150_write_reg(ts->client, 0x14EA, 6, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x14EA-0x14EF failed");
		return ret;
	}
	/* Change the Virtual Buttons 0 */
	/* Memory Map Position 0x14F0 - 0x1511 */
	buffer[0] = NUMBER_OF_VIRTUAL_SENSORS_0;
	buffer[1] = NUMBER_OF_VIRTUAL_SENSORS_1;
	buffer[2] = B0_TOP_LEFT_X_0;
	buffer[3] = B0_TOP_LEFT_X_1;
	buffer[4] = B0_TOP_LEFT_Y_0;
	buffer[5] = B0_TOP_LEFT_Y_1;
	buffer[6] = B0_BOT_RIGHT_X_0;
	buffer[7] = B0_BOT_RIGHT_X_1;
	buffer[8] = B0_BOT_RIGHT_Y_0;
	buffer[9] = B0_BOT_RIGHT_Y_1;
	buffer[10] = B1_TOP_LEFT_X_0;
	buffer[11] = B1_TOP_LEFT_X_1;
	buffer[12] = B1_TOP_LEFT_Y_0;
	buffer[13] = B1_TOP_LEFT_Y_1;
	buffer[14] = B1_BOT_RIGHT_X_0;
	buffer[15] = B1_BOT_RIGHT_X_1;
	buffer[16] = B1_BOT_RIGHT_Y_0;
	buffer[17] = B1_BOT_RIGHT_Y_1;
	buffer[18] = B2_TOP_LEFT_X_0;
	buffer[19] = B2_TOP_LEFT_X_1;
	buffer[20] = B2_TOP_LEFT_Y_0;
	buffer[21] = B2_TOP_LEFT_Y_1;
	buffer[22] = B2_BOT_RIGHT_X_0;
	buffer[23] = B2_BOT_RIGHT_X_1;
	buffer[24] = B2_BOT_RIGHT_Y_0;
	buffer[25] = B2_BOT_RIGHT_Y_1;
	buffer[26] = B3_TOP_LEFT_X_0;
	buffer[27] = B3_TOP_LEFT_X_1;
	buffer[28] = B3_TOP_LEFT_Y_0;
	buffer[29] = B3_TOP_LEFT_Y_1;
	buffer[30] = B3_BOT_RIGHT_X_0;
	buffer[31] = B3_BOT_RIGHT_X_1;
	buffer[32] = B3_BOT_RIGHT_Y_0;
	buffer[33] = B3_BOT_RIGHT_Y_1;
	ret = iqs9150_write_reg(ts->client, 0x14F0, 34, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x14F0-0x1511 failed");
		return ret;
	}

	/* Change the Virtual Buttons 1 */
	/* Memory Map Position 0x1512 - 0x1541 */
	buffer[0] = B4_TOP_LEFT_X_0;
	buffer[1] = B4_TOP_LEFT_X_1;
	buffer[2] = B4_TOP_LEFT_Y_0;
	buffer[3] = B4_TOP_LEFT_Y_1;
	buffer[4] = B4_BOT_RIGHT_X_0;
	buffer[5] = B4_BOT_RIGHT_X_1;
	buffer[6] = B4_BOT_RIGHT_Y_0;
	buffer[7] = B4_BOT_RIGHT_Y_1;
	buffer[8] = B5_TOP_LEFT_X_0;
	buffer[9] = B5_TOP_LEFT_X_1;
	buffer[10] = B5_TOP_LEFT_Y_0;
	buffer[11] = B5_TOP_LEFT_Y_1;
	buffer[12] = B5_BOT_RIGHT_X_0;
	buffer[13] = B5_BOT_RIGHT_X_1;
	buffer[14] = B5_BOT_RIGHT_Y_0;
	buffer[15] = B5_BOT_RIGHT_Y_1;
	buffer[16] = B6_TOP_LEFT_X_0;
	buffer[17] = B6_TOP_LEFT_X_1;
	buffer[18] = B6_TOP_LEFT_Y_0;
	buffer[19] = B6_TOP_LEFT_Y_1;
	buffer[20] = B6_BOT_RIGHT_X_0;
	buffer[21] = B6_BOT_RIGHT_X_1;
	buffer[22] = B6_BOT_RIGHT_Y_0;
	buffer[23] = B6_BOT_RIGHT_Y_1;
	buffer[24] = B7_TOP_LEFT_X_0;
	buffer[25] = B7_TOP_LEFT_X_1;
	buffer[26] = B7_TOP_LEFT_Y_0;
	buffer[27] = B7_TOP_LEFT_Y_1;
	buffer[28] = B7_BOT_RIGHT_X_0;
	buffer[29] = B7_BOT_RIGHT_X_1;
	buffer[30] = B7_BOT_RIGHT_Y_0;
	buffer[31] = B7_BOT_RIGHT_Y_1;
	buffer[32] = B8_TOP_LEFT_Y_0;
	buffer[33] = B8_TOP_LEFT_Y_1;
	buffer[34] = B8_BOT_RIGHT_X_0;
	buffer[35] = B8_BOT_RIGHT_X_1;
	buffer[36] = B8_BOT_RIGHT_Y_0;
	buffer[37] = B8_BOT_RIGHT_Y_1;
	buffer[38] = B8_TOP_LEFT_X_0;
	buffer[39] = B8_TOP_LEFT_X_1;
	buffer[40] = B9_TOP_LEFT_Y_0;
	buffer[41] = B9_TOP_LEFT_Y_1;
	buffer[42] = B9_BOT_RIGHT_X_0;
	buffer[43] = B9_BOT_RIGHT_X_1;
	buffer[44] = B9_BOT_RIGHT_Y_0;
	buffer[45] = B9_BOT_RIGHT_Y_1;
	buffer[46] = B9_TOP_LEFT_X_0;
	buffer[47] = B9_TOP_LEFT_X_1;
	ret = iqs9150_write_reg(ts->client, 0x1512, 48, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1512-0x1541 failed");
		return ret;
	}

	/* Change the Virtual Buttons 2 */
	/* Memory Map Position 0x1542 - 0x1571 */
	buffer[0] = B10_TOP_LEFT_X_0;
	buffer[1] = B10_TOP_LEFT_X_1;
	buffer[2] = B10_TOP_LEFT_Y_0;
	buffer[3] = B10_TOP_LEFT_Y_1;
	buffer[4] = B10_BOT_RIGHT_X_0;
	buffer[5] = B10_BOT_RIGHT_X_1;
	buffer[6] = B10_BOT_RIGHT_Y_0;
	buffer[7] = B10_BOT_RIGHT_Y_1;
	buffer[8] = B11_TOP_LEFT_X_0;
	buffer[9] = B11_TOP_LEFT_X_1;
	buffer[10] = B11_TOP_LEFT_Y_0;
	buffer[11] = B11_TOP_LEFT_Y_1;
	buffer[12] = B11_BOT_RIGHT_X_0;
	buffer[13] = B11_BOT_RIGHT_X_1;
	buffer[14] = B11_BOT_RIGHT_Y_0;
	buffer[15] = B11_BOT_RIGHT_Y_1;
	buffer[16] = B12_TOP_LEFT_X_0;
	buffer[17] = B12_TOP_LEFT_X_1;
	buffer[18] = B12_TOP_LEFT_Y_0;
	buffer[19] = B12_TOP_LEFT_Y_1;
	buffer[20] = B12_BOT_RIGHT_X_0;
	buffer[21] = B12_BOT_RIGHT_X_1;
	buffer[22] = B12_BOT_RIGHT_Y_0;
	buffer[23] = B12_BOT_RIGHT_Y_1;
	buffer[24] = B13_TOP_LEFT_X_0;
	buffer[25] = B13_TOP_LEFT_X_1;
	buffer[26] = B13_TOP_LEFT_Y_0;
	buffer[27] = B13_TOP_LEFT_Y_1;
	buffer[28] = B13_BOT_RIGHT_X_0;
	buffer[29] = B13_BOT_RIGHT_X_1;
	buffer[30] = B13_BOT_RIGHT_Y_0;
	buffer[31] = B13_BOT_RIGHT_Y_1;
	buffer[32] = B14_TOP_LEFT_Y_0;
	buffer[33] = B14_TOP_LEFT_Y_1;
	buffer[34] = B14_BOT_RIGHT_X_0;
	buffer[35] = B14_BOT_RIGHT_X_1;
	buffer[36] = B14_BOT_RIGHT_Y_0;
	buffer[37] = B14_BOT_RIGHT_Y_1;
	buffer[38] = B14_TOP_LEFT_X_0;
	buffer[39] = B14_TOP_LEFT_X_1;
	buffer[40] = B15_TOP_LEFT_Y_0;
	buffer[41] = B15_TOP_LEFT_Y_1;
	buffer[42] = B15_BOT_RIGHT_X_0;
	buffer[43] = B15_BOT_RIGHT_X_1;
	buffer[44] = B15_BOT_RIGHT_Y_0;
	buffer[45] = B15_BOT_RIGHT_Y_1;
	buffer[46] = B15_TOP_LEFT_X_0;
	buffer[47] = B15_TOP_LEFT_X_1;
	ret = iqs9150_write_reg(ts->client, 0x1542, 48, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1542-0x1571 failed");
		return ret;
	}

	/* Change the Virtual Sliders 0 */
	/* Memory Map Position 0x1572 - 0x15A5 */
	buffer[0] = SLIDER_DEADZONE_0;
	buffer[1] = SLIDER_DEADZONE_1;
	buffer[2] = S0_TOP_LEFT_X_0;
	buffer[3] = S0_TOP_LEFT_X_1;
	buffer[4] = S0_TOP_LEFT_Y_0;
	buffer[5] = S0_TOP_LEFT_Y_1;
	buffer[6] = S0_BOT_RIGHT_X_0;
	buffer[7] = S0_BOT_RIGHT_X_1;
	buffer[8] = S0_BOT_RIGHT_Y_0;
	buffer[9] = S0_BOT_RIGHT_Y_1;
	buffer[10] = S0_RESOLUTION_0;
	buffer[11] = S0_RESOLUTION_1;
	buffer[12] = S1_TOP_LEFT_X_0;
	buffer[13] = S1_TOP_LEFT_X_1;
	buffer[14] = S1_TOP_LEFT_Y_0;
	buffer[15] = S1_TOP_LEFT_Y_1;
	buffer[16] = S1_BOT_RIGHT_X_0;
	buffer[17] = S1_BOT_RIGHT_X_1;
	buffer[18] = S1_BOT_RIGHT_Y_0;
	buffer[19] = S1_BOT_RIGHT_Y_1;
	buffer[20] = S1_RESOLUTION_0;
	buffer[21] = S1_RESOLUTION_1;
	buffer[22] = S2_TOP_LEFT_X_0;
	buffer[23] = S2_TOP_LEFT_X_1;
	buffer[24] = S2_TOP_LEFT_Y_0;
	buffer[25] = S2_TOP_LEFT_Y_1;
	buffer[26] = S2_BOT_RIGHT_X_0;
	buffer[27] = S2_BOT_RIGHT_X_1;
	buffer[28] = S2_BOT_RIGHT_Y_0;
	buffer[29] = S2_BOT_RIGHT_Y_1;
	buffer[30] = S2_RESOLUTION_0;
	buffer[31] = S2_RESOLUTION_1;
	buffer[32] = S3_TOP_LEFT_X_0;
	buffer[33] = S3_TOP_LEFT_X_1;
	buffer[34] = S3_TOP_LEFT_Y_0;
	buffer[35] = S3_TOP_LEFT_Y_1;
	buffer[36] = S3_BOT_RIGHT_X_0;
	buffer[37] = S3_BOT_RIGHT_X_1;
	buffer[38] = S3_BOT_RIGHT_Y_0;
	buffer[39] = S3_BOT_RIGHT_Y_1;
	buffer[40] = S3_RESOLUTION_0;
	buffer[41] = S3_RESOLUTION_1;
	buffer[42] = S4_TOP_LEFT_X_0;
	buffer[43] = S4_TOP_LEFT_X_1;
	buffer[44] = S4_TOP_LEFT_Y_0;
	buffer[45] = S4_TOP_LEFT_Y_1;
	buffer[46] = S4_BOT_RIGHT_X_0;
	buffer[47] = S4_BOT_RIGHT_X_1;
	buffer[48] = S4_BOT_RIGHT_Y_0;
	buffer[49] = S4_BOT_RIGHT_Y_1;
	buffer[50] = S4_RESOLUTION_0;
	buffer[51] = S4_RESOLUTION_1;
	ret = iqs9150_write_reg(ts->client, 0x1572, 52, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x1572-0x15A5 failed");
		return ret;
	}

	/* Change the Virtual Sliders 1 */
	/* Memory Map Position 0x15A6 - 0x15C3 */
	buffer[0] = S5_TOP_LEFT_X_0;
	buffer[1] = S5_TOP_LEFT_X_1;
	buffer[2] = S5_TOP_LEFT_Y_0;
	buffer[3] = S5_TOP_LEFT_Y_1;
	buffer[4] = S5_BOT_RIGHT_X_0;
	buffer[5] = S5_BOT_RIGHT_X_1;
	buffer[6] = S5_BOT_RIGHT_Y_0;
	buffer[7] = S5_BOT_RIGHT_Y_1;
	buffer[8] = S5_RESOLUTION_0;
	buffer[9] = S5_RESOLUTION_1;
	buffer[10] = S6_TOP_LEFT_X_0;
	buffer[11] = S6_TOP_LEFT_X_1;
	buffer[12] = S6_TOP_LEFT_Y_0;
	buffer[13] = S6_TOP_LEFT_Y_1;
	buffer[14] = S6_BOT_RIGHT_X_0;
	buffer[15] = S6_BOT_RIGHT_X_1;
	buffer[16] = S6_BOT_RIGHT_Y_0;
	buffer[17] = S6_BOT_RIGHT_Y_1;
	buffer[18] = S6_RESOLUTION_0;
	buffer[19] = S6_RESOLUTION_1;
	buffer[20] = S7_TOP_LEFT_X_0;
	buffer[21] = S7_TOP_LEFT_X_1;
	buffer[22] = S7_TOP_LEFT_Y_0;
	buffer[23] = S7_TOP_LEFT_Y_1;
	buffer[24] = S7_BOT_RIGHT_X_0;
	buffer[25] = S7_BOT_RIGHT_X_1;
	buffer[26] = S7_BOT_RIGHT_Y_0;
	buffer[27] = S7_BOT_RIGHT_Y_1;
	buffer[28] = S7_RESOLUTION_0;
	buffer[29] = S7_RESOLUTION_1;
	ret = iqs9150_write_reg(ts->client, 0x15A6, 30, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x15A6-0x15C3 failed");
		return ret;
	}

	/* Change the Virtual Wheels */
	/* Memory Map Position 0x15C4 - 0x15EB */
	buffer[0] = W0_CENTRE_X_0;
	buffer[1] = W0_CENTRE_X_1;
	buffer[2] = W0_CENTRE_Y_0;
	buffer[3] = W0_CENTRE_Y_1;
	buffer[4] = W0_INNER_RADIUS_0;
	buffer[5] = W0_INNER_RADIUS_1;
	buffer[6] = W0_OUTER_RADIUS_0;
	buffer[7] = W0_OUTER_RADIUS_1;
	buffer[8] = W0_RESOLUTION_0;
	buffer[9] = W0_RESOLUTION_1;
	buffer[10] = W1_CENTRE_X_0;
	buffer[11] = W1_CENTRE_X_1;
	buffer[12] = W1_CENTRE_Y_0;
	buffer[13] = W1_CENTRE_Y_1;
	buffer[14] = W1_INNER_RADIUS_0;
	buffer[15] = W1_INNER_RADIUS_1;
	buffer[16] = W1_OUTER_RADIUS_0;
	buffer[17] = W1_OUTER_RADIUS_1;
	buffer[18] = W1_RESOLUTION_0;
	buffer[19] = W1_RESOLUTION_1;
	buffer[20] = W2_CENTRE_X_0;
	buffer[21] = W2_CENTRE_X_1;
	buffer[22] = W2_CENTRE_Y_0;
	buffer[23] = W2_CENTRE_Y_1;
	buffer[24] = W2_INNER_RADIUS_0;
	buffer[25] = W2_INNER_RADIUS_1;
	buffer[26] = W2_OUTER_RADIUS_0;
	buffer[27] = W2_OUTER_RADIUS_1;
	buffer[28] = W2_RESOLUTION_0;
	buffer[29] = W2_RESOLUTION_1;
	buffer[30] = W3_CENTRE_X_0;
	buffer[31] = W3_CENTRE_X_1;
	buffer[32] = W3_CENTRE_Y_0;
	buffer[33] = W3_CENTRE_Y_1;
	buffer[34] = W3_INNER_RADIUS_0;
	buffer[35] = W3_INNER_RADIUS_1;
	buffer[36] = W3_OUTER_RADIUS_0;
	buffer[37] = W3_OUTER_RADIUS_1;
	buffer[38] = W3_RESOLUTION_0;
	buffer[39] = W3_RESOLUTION_1;
	ret = iqs9150_write_reg(ts->client, 0x15C4, 40, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x15C4-0x15EB failed");
		return ret;
	}

	/* Eng Settings
	 * Memory Map Position 0x2000 - 0x2005
	 */

	buffer[0] = ENG_CONFIG_0;
	buffer[1] = ENG_CONFIG_1;
	buffer[2] = ENG_0;
	buffer[3] = ENG_1;
	buffer[4] = ENG_2;
	buffer[5] = ENG_3;

	ret = iqs9150_write_reg(ts->client, 0x2000, 6, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg 0x2000-0x2005 failed");
		return ret;
	}
	IQS_FUNC_EXIT();

	return 0;
}

static int iqs9150_stop_bit_disabled(struct iqs9150_ts_data *ts)
{
	uint8_t buffer[2];
	int ret;

	buffer[0] = CONFIG_SETTINGS_0|0x40;

	ret = iqs9150_write_reg(ts->client, 0x11BE, 1, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg  11BE");
		return ret;
	}

	return 0;
}

static int iqs9150_stop_i2c_comm_window(struct iqs9150_ts_data *ts)
{
	uint8_t buffer[2] = {0};
	int ret = 0;

	buffer[0] = 0x00;

	ret = iqs9150_write_reg(ts->client, 0xEEEE, 1, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg  EEEE");
		return ret;
	}

	return 0;
}

static int iqs9150_force_i2c_comm_window(struct iqs9150_ts_data *ts)
{
	uint8_t buffer[2] = {0};
	int ret = 0;

	buffer[0] = 0x00;
	ret = iqs9150_write_reg(ts->client, 0x00FF, 0, &buffer[0]);
	if (ret) {
		IQS_ERROR("iqs9150_write_reg  00FF");
		return ret;
	}

	return 0;
}

static uint8_t iqs9150_iqs_production_testing(struct iqs9150_ts_data *ts)
{
	uint8_t iqs9150_error_code = IQS_TEST_PASS;
	uint8_t buffer[CONSISTENCY_BUFFER_MAX_LEN] = {0};
	uint16_t iqs9150_trackpad_channel = TRACKPAD_SETTINGS_0_1 * TRACKPAD_SETTINGS_1_0;
	uint8_t iqs9150_trackpad_line_count = iqs9150_trackpad_channel / READ_BYTES_NUM_EACH_COUNT;
	uint8_t iqs9150_trackpad_remainder_count =
		iqs9150_trackpad_channel % READ_BYTES_NUM_EACH_COUNT;
	uint8_t count = 0;
	uint16_t value = 0xFFFF;

	int error = 0;
	int i = 0;
	int value_integer = 0;
	int value_remainder = 0;
	int index = 0;
	int offset = 0;

	cancel_delayed_work_sync(&ts->reg_read_dwork);

	iqs9150_trackpad_line_count = iqs9150_trackpad_line_count * PER_REGISTER_OCCUPY_BYTES;
	iqs9150_trackpad_remainder_count =
		iqs9150_trackpad_remainder_count * PER_REGISTER_OCCUPY_BYTES;

	switch (ts->test_id) {
	case IQS_CHIP_ID:
		error = iqs9150_read_reg(ts, PRODUCT_NUMBER_REGISTER_ADDRESS_MSB,
			PRODUCT_NUMBER_REGISTER_ADDRESS_LSB, PER_REGISTER_OCCUPY_BYTES, &buffer[0]);
		if (error) {
			IQS_ERROR("read IQS_CHIP_ID error: %d", error);
			iqs9150_error_code = IQS_CHIP_ID_ERROR;
			ts->test_id = IQS_CHIP_ID;
			return iqs9150_error_code;
		}
		if ((buffer[0] == PRODUCT_NUMBER_DATA_LSB) &&
		    (buffer[1] == PRODUCT_NUMBER_DATA_MSB)) {
			iqs9150_error_code = IQS_CHIP_ID_PASS;
			ts->test_id = IQS_ATI_STATE;
		} else {
			iqs9150_error_code = IQS_CHIP_ID_ERROR;
			ts->test_id = IQS_CHIP_ID;
			return iqs9150_error_code;
		}
		break;

	case IQS_ATI_STATE:
		error = iqs9150_read_reg(ts, INFO_FLAG_REGISTER_ADDRESS_MSB,
			INFO_FLAG_REGISTER_ADDRESS_LSB, PER_REGISTER_OCCUPY_BYTES, &buffer[0]);
		if (error) {
			IQS_ERROR("read IQS_ATI_STATE error: %d", error);
			iqs9150_error_code = IQS_ATI_ERROR;
			ts->test_id = IQS_CHIP_ID;
			return iqs9150_error_code;
		}
		if (buffer[0] & ATI_ERROR) {
			iqs9150_error_code = IQS_ATI_ERROR;
			ts->test_id = IQS_CHIP_ID;
			return iqs9150_error_code;
		}
		iqs9150_error_code = IQS_ATI_PASS;
		ts->test_id = IQS_REFERENCE;
		break;
	case IQS_REFERENCE:
		index = 0;
		for (count = 0; iqs9150_trackpad_line_count > count; count++) {
			error = iqs9150_read_reg(ts, IQS_REFERENCE_REGISTER_MSB + (count/2),
				IQS_REFERENCE_REGISTER_LSB * (count%2),
				READ_BYTES_NUM_EACH_COUNT,
				&buffer[READ_BYTES_NUM_EACH_COUNT * count]);
			if (error) {
				IQS_ERROR("read IQS_REFERENCE error: %d", error);
				iqs9150_error_code = IQS_REFERENCE_ERROR;
				ts->test_id = IQS_CHIP_ID;
				return iqs9150_error_code;
			}
			for (i = 0; i < READ_BYTES_NUM_EACH_COUNT;
			     i += PER_REGISTER_OCCUPY_BYTES) {
				offset = READ_BYTES_NUM_EACH_COUNT * count + i;
				value = ((uint16_t)buffer[offset + 1] << 8) +
					buffer[offset];
				IQS_DEBUG("IQS_REFERENCE-Data: %d", value);
				if (value < IQS_REFERENCE_DATA_MIN ||
				    value > IQS_REFERENCE_DATA_MAX) {
					if (index != NO_REFERENCE_DATA_I0 &&
					    index != NO_REFERENCE_DATA_I1 &&
					    index != NO_REFERENCE_DATA_I2 &&
					    index != NO_REFERENCE_DATA_I3 &&
					    index != NO_REFERENCE_DATA_I4 &&
					    index != NO_REFERENCE_DATA_I5) {
						iqs9150_error_code = IQS_REFERENCE_ERROR;
						IQS_ERROR("IQS_REFERENCE-Data invalid");
					}
				}
				index++;
			}
		}

		if (iqs9150_trackpad_remainder_count) {
			error = iqs9150_read_reg(ts, IQS_REFERENCE_REGISTER_MSB +
				(iqs9150_trackpad_line_count/2), IQS_REFERENCE_REGISTER_LSB *
				(iqs9150_trackpad_line_count%2), iqs9150_trackpad_remainder_count,
				&buffer[READ_BYTES_NUM_EACH_COUNT *
					iqs9150_trackpad_line_count]);
			if (error) {
				IQS_ERROR("read IQS_REFERENCE error: %d", error);
				iqs9150_error_code = IQS_REFERENCE_ERROR;
				ts->test_id = IQS_CHIP_ID;
				return iqs9150_error_code;
			}

			for (i = 0; i < iqs9150_trackpad_remainder_count;
			     i += PER_REGISTER_OCCUPY_BYTES) {
				offset = READ_BYTES_NUM_EACH_COUNT *
					iqs9150_trackpad_line_count + i;
				value = ((uint16_t)buffer[offset + 1] << 8) +
					buffer[offset];
				IQS_DEBUG("IQS_REFERENCE-Remainder Data: %d", value);
				if (value < IQS_REFERENCE_DATA_MIN ||
				    value > IQS_REFERENCE_DATA_MAX) {
					if (index != NO_REFERENCE_DATA_I0 &&
					    index != NO_REFERENCE_DATA_I1 &&
					    index != NO_REFERENCE_DATA_I2 &&
					    index != NO_REFERENCE_DATA_I3 &&
					    index != NO_REFERENCE_DATA_I4 &&
					    index != NO_REFERENCE_DATA_I5) {
						IQS_ERROR("IQS_REFERENCE-remainder-Data invalid");
						iqs9150_error_code = IQS_REFERENCE_ERROR;
					}
				}
				index++;
			}
		}
		if (iqs9150_error_code == IQS_REFERENCE_ERROR) {
			ts->test_id = IQS_CHIP_ID;
			return iqs9150_error_code;
		}
		iqs9150_error_code = IQS_REFERENCE_PASS;
		ts->test_id = IQS_DELTA;
		break;

	case IQS_DELTA:
		for (count = 0; iqs9150_trackpad_line_count > count; count++) {
			error = iqs9150_read_reg(ts, IQS_DELTA_REGISTER_MSB + (count/2),
				IQS_DELTA_REGISTER_LSB * (count%2), READ_BYTES_NUM_EACH_COUNT,
				&buffer[READ_BYTES_NUM_EACH_COUNT * count]);
			if (error) {
				IQS_ERROR("read IQS_DELTA error: %d", error);
				iqs9150_error_code = IQS_DELTA_ERROR;
				ts->test_id = IQS_CHIP_ID;
				return iqs9150_error_code;
			}
			for (i = 0; i < READ_BYTES_NUM_EACH_COUNT;
			     i += PER_REGISTER_OCCUPY_BYTES) {
				offset = READ_BYTES_NUM_EACH_COUNT * count + i;
				value = ((uint16_t)buffer[offset + 1] << 8) +
					buffer[offset];
				IQS_DEBUG("IQS_DELTA-Data: %d", value);
				if (value < IQS_DELTA_DATA_MIN_UNSIGNED
					&& value > IQS_DELTA_DATA_MAX_UNSIGNED) {
					IQS_ERROR("IQS_DELTA-Data invalid");
					iqs9150_error_code = IQS_DELTA_ERROR;
				}
			}
		}

		if (iqs9150_trackpad_remainder_count) {
			error = iqs9150_read_reg(ts, IQS_DELTA_REGISTER_MSB +
				(iqs9150_trackpad_line_count/2),
				IQS_DELTA_REGISTER_LSB *
				(iqs9150_trackpad_line_count%2),
				iqs9150_trackpad_remainder_count,
				&buffer[READ_BYTES_NUM_EACH_COUNT *
					iqs9150_trackpad_line_count]);
			if (error) {
				IQS_ERROR("read IQS_DELTA error: %d", error);
				iqs9150_error_code = IQS_DELTA_ERROR;
				ts->test_id = IQS_CHIP_ID;
				return iqs9150_error_code;
			}
			for (i = 0; i < iqs9150_trackpad_remainder_count;
			     i += PER_REGISTER_OCCUPY_BYTES) {
				offset = READ_BYTES_NUM_EACH_COUNT *
					iqs9150_trackpad_line_count + i;
				value = ((uint16_t)buffer[offset + 1] << 8) +
					buffer[offset];
				IQS_DEBUG("IQS_DELTA-Remainder Data: %d", value);
				if (value < IQS_DELTA_DATA_MIN_UNSIGNED
					&& value > IQS_DELTA_DATA_MAX_UNSIGNED) {
					IQS_ERROR("IQS_DELTA-Remainder Data invalid");
					iqs9150_error_code = IQS_DELTA_ERROR;
				}
			}
		}

		if (iqs9150_error_code == IQS_DELTA_ERROR) {
			ts->test_id = IQS_CHIP_ID;
			return iqs9150_error_code;
		}
		iqs9150_error_code = IQS_DELTA_PASS;
		ts->test_id = IQS_ATI_COMPENSATION;

		break;
	case IQS_ATI_COMPENSATION:
		for (count = 0; iqs9150_trackpad_line_count > count; count++) {
			error = iqs9150_read_reg(ts, IQS_ATI_COMPENSATION_REGISTER_MSB + (count/2),
				IQS_ATI_COMPENSATION_REGISTER_LSB * (count%2),
				READ_BYTES_NUM_EACH_COUNT,
				&buffer[READ_BYTES_NUM_EACH_COUNT * count]);
			if (error) {
				IQS_ERROR("read IQS_ATI_COMPENSATION error: %d", error);
				iqs9150_error_code = IQS_ATI_COMPENSATION_ERROR;
				ts->test_id = IQS_CHIP_ID;
				return iqs9150_error_code;
			}

			for (i = 0; i < READ_BYTES_NUM_EACH_COUNT;
			     i += PER_REGISTER_OCCUPY_BYTES) {
				offset = READ_BYTES_NUM_EACH_COUNT * count + i;
				value = ((uint16_t)(buffer[offset + 1] & 0x7F) << 8) +
					buffer[offset];
				value_integer =
					(int)value / (1 << IQS_ATI_COMPENSATION_OCCUPY_BIT);
				value_remainder =
					(int)value % (1 << IQS_ATI_COMPENSATION_OCCUPY_BIT);

				IQS_DEBUG("IQS_ATI_COMPENSATION-Divider-Data: %d", value_integer);
				IQS_DEBUG("IQS_ATI_COMPENSATION-Data: %d", value_remainder);
				if (value_integer > IQS_ATI_COMPENSATION_DIVIDER_DATA_MIN
					&& value_integer < IQS_ATI_COMPENSATION_DIVIDER_DATA_MAX) {
					continue;
				} else if (value_integer == IQS_ATI_COMPENSATION_DIVIDER_DATA_MIN
					&& value_remainder < IQS_ATI_COMPENSATION_DATA_MAX) {
					continue;
				} else if (value_integer == IQS_ATI_COMPENSATION_DIVIDER_DATA_MAX
					&& value_remainder > IQS_ATI_COMPENSATION_DATA_MIN) {
					continue;
				} else {
					iqs9150_error_code = IQS_ATI_COMPENSATION_ERROR;
					IQS_ERROR("IQS_ATI_COMPENSATION Data invalid");
				}
			}
		}

		if (iqs9150_trackpad_remainder_count) {
			error = iqs9150_read_reg(ts, IQS_ATI_COMPENSATION_REGISTER_MSB +
				(iqs9150_trackpad_line_count/2),
				IQS_ATI_COMPENSATION_REGISTER_LSB *
				(iqs9150_trackpad_line_count%2),
				iqs9150_trackpad_remainder_count,
				&buffer[READ_BYTES_NUM_EACH_COUNT *
					iqs9150_trackpad_line_count]);
			if (error) {
				IQS_ERROR("read IQS_ATI_COMPENSATION error: %d", error);
				iqs9150_error_code = IQS_ATI_COMPENSATION_ERROR;
				ts->test_id = IQS_CHIP_ID;
				return iqs9150_error_code;
			}
			for (i = 0; i < iqs9150_trackpad_remainder_count;
			     i += PER_REGISTER_OCCUPY_BYTES) {
				offset = READ_BYTES_NUM_EACH_COUNT *
					iqs9150_trackpad_line_count + i;
				value = ((uint16_t)(buffer[offset + 1] & 0x7F) << 8) +
					buffer[offset];
				value_integer =
					(int)value / (1 << IQS_ATI_COMPENSATION_OCCUPY_BIT);
				value_remainder =
					(int)value % (1 << IQS_ATI_COMPENSATION_OCCUPY_BIT);

				IQS_DEBUG("IQS_ATI_COMPENSATION-Divider-Remainder-Data: %d",
					  value_integer);
				IQS_DEBUG("IQS_ATI_COMPENSATION-Remainder-Data: %d",
					  value_remainder);
				if (value_integer > IQS_ATI_COMPENSATION_DIVIDER_DATA_MIN &&
				    value_integer < IQS_ATI_COMPENSATION_DIVIDER_DATA_MAX) {
					continue;
				} else if (value_integer == IQS_ATI_COMPENSATION_DIVIDER_DATA_MIN
					&& value_remainder < IQS_ATI_COMPENSATION_DATA_MAX) {
					continue;
				} else if (value_integer == IQS_ATI_COMPENSATION_DIVIDER_DATA_MAX
					&& value_remainder > IQS_ATI_COMPENSATION_DATA_MIN) {
					continue;
				} else {
					iqs9150_error_code = IQS_ATI_COMPENSATION_ERROR;
					IQS_ERROR("IQS_ATI_COMPENSATION-Remainder Data invalid");
				}
			}
		}
		if (iqs9150_error_code == IQS_ATI_COMPENSATION_ERROR) {
			ts->test_id = IQS_CHIP_ID;
			return iqs9150_error_code;
		}
		iqs9150_error_code = IQS_TEST_PASS;
		ts->test_id = IQS_CHIP_ID;
		break;
	default:
			ts->test_id = IQS_CHIP_ID;
			break;
	}

	return iqs9150_error_code;
}

static int iqs_parse_dt(struct device *dev, struct iqs9150_ts_data *pdata)
{
	struct device_node *np = dev->of_node;

	IQS_FUNC_ENTER();

	if (!np) {
		IQS_ERROR(" np is NULL ");
		return -EINVAL;
	}
	pdata->reset_gpio =
		of_get_named_gpio_flags(np, "iqs9150, reset-gpio",
					0, &pdata->reset_gpio_flags);

	if (pdata->reset_gpio < 0)
		IQS_ERROR("Unable to get reset_gpio ");

	pdata->irq_gpio =
		of_get_named_gpio_flags(np, "iqs9150, irq-gpio",
					0, &pdata->irq_gpio_flags);
	if (pdata->irq_gpio < 0)
		IQS_ERROR("Unable to get irq_gpio");

	pdata->iovdd_gpio =
		of_get_named_gpio_flags(np, "iqs9150, iovdd-gpio",
					0, &pdata->iovdd_gpio_flags);
	if (pdata->iovdd_gpio < 0)
		IQS_ERROR("Unable to get iovdd_gpio ");

	IQS_INFO(" irq gpio:%d, reset gpio:%d, iovdd_gpio:%d",
		 pdata->irq_gpio, pdata->reset_gpio, pdata->iovdd_gpio);

	IQS_FUNC_EXIT();
	return 0;
}

/* drm notifier block control the suspend/resume procedure */
static void iqs9150_fb_notifier_callback(
	enum panel_event_notifier_tag tag,
	struct panel_event_notification *notification, void *data)
{
	struct iqs9150_ts_data *ts = data;

	if (!notification) {
		IQS_INFO("Invalid panel notification\n");
		return;
	}
	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_UNBLANK:
			IQS_INFO("Resume by drm notifier.");
			ts->panel_unblank = 1;
			if (ts->resume_in_workqueue)
				schedule_work(&ts->fb_notify_work);
			else
				iqs9150_resume(&ts->client->dev);
			break;
	case DRM_PANEL_EVENT_BLANK:
			IQS_INFO("Suspend by drm notifier.");
			if (ts->resume_in_workqueue)
				flush_work(&ts->fb_notify_work);
			ts->panel_unblank = 0;
			iqs9150_suspend(&ts->client->dev);
			break;
	default:
			IQS_INFO("FB BLANK(%d) do not need process", notification->notif_type);
			break;
	}
}

static void fb_notify_resume_work(struct work_struct *work)
{
	struct iqs9150_ts_data *ts = container_of(work, struct iqs9150_ts_data, fb_notify_work);

	IQS_INFO("try resume in workqueue\n");
	iqs9150_resume(&ts->client->dev);
}

static int iqs9150_register_powermanager(struct iqs9150_ts_data *ts)
{
	int rc = 0;

	void *cookie = NULL;

	INIT_WORK(&ts->fb_notify_work, fb_notify_resume_work);
	cookie = panel_event_notifier_register(
			PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_PRIMARY_TOUCH,
			ts->active_panel,
			iqs9150_fb_notifier_callback,
			(void *)ts);
	if (IS_ERR(cookie)) {
		rc = PTR_ERR(cookie);
		IQS_ERROR("Failed to register panel event notifier, rc=%d\n", rc);
		return rc;
	}
	pr_debug("register panel notifier successful\n");
	ts->notifier_cookie = cookie;
	return rc;
}

static int iqs9150_unregister_powermanager(struct iqs9150_ts_data *ts)
{
	if (ts->notifier_cookie)
		panel_event_notifier_unregister(ts->notifier_cookie);
	return 0;
}

int iqs9150_call_parse_panel_work(struct iqs9150_ts_data *ts, unsigned long delay)
{
	return queue_delayed_work(ts->workqueue,
		&ts->panel_check_work, msecs_to_jiffies(delay));
}

int iqs9150_parse_panel(struct device *dev, struct device_node *np)
{
	int i;
	int count;

	struct device_node *node;
	struct drm_panel *panel;

	count = of_count_phandle_with_args(np, "panel", NULL);
	if (count <= 0) {
		IQS_INFO("panel count is 0");
		return -1;
	}
	IQS_INFO("panel count is %d", count);
	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "panel", i);
		if (!node) {
			IQS_ERROR("parse panel node failed");
			return -EINVAL;
		}
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel)) {
			ts->active_panel = panel;
			return 0;
		}
	}
	return PTR_ERR(panel);
}

void iqs9150_parse_panel_work(struct work_struct *work)
{
	int ret = 0;

	static int cnt;
	struct device_node *np;
	struct device *dev = NULL;
	struct iqs9150_ts_data *ts = NULL;
	struct delayed_work *ts_delayed_work = NULL;

	ts_delayed_work = container_of(work, struct delayed_work, work);
	if (!ts_delayed_work) {
		IQS_ERROR("ts_delayed_work is null!");
				return;
	}
	ts = container_of(ts_delayed_work, struct iqs9150_ts_data, panel_check_work);
	if (!ts) {
		IQS_ERROR("ts is null!");
		return;
	}
	dev = &ts->client->dev;
	if (!dev) {
		IQS_ERROR("dev is null!");
		return;
	}
	np = dev->of_node;
	ret = iqs9150_parse_panel(dev, np);
	if (ret == 0) {
		IQS_INFO("parse drm panel success");
		iqs9150_register_powermanager(ts);
	} else {
		IQS_ERROR("parse drm panel failed");
		if (++cnt < 10) {
			iqs9150_call_parse_panel_work(ts, DELAY_CALL_PARSE_PANEL_WORK_WHEN_FAIL);
		} else {
			cnt = 0;
			iqs9150_register_powermanager(ts);
		}
	}
}

struct iqs9150_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *kobj,
		struct attribute *attr, const char *buf, size_t count);
};

#define to_iqs9150_attr(_attr) container_of(_attr, struct iqs9150_attribute, attr)
#define to_iqs9150_device(o) container_of(o, struct iqs9150_ts_data, kobj)

static ssize_t attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct iqs9150_attribute *p_iqs9150_attr = to_iqs9150_attr(attr);
	ssize_t ret = -EIO;

	if (p_iqs9150_attr->show)
		ret = p_iqs9150_attr->show(kobj, attr, buf);
	return ret;
}

static ssize_t attr_store(struct kobject *kobj,
					struct attribute *this,
					const char *buf, size_t count)
{
	struct attribute attr;
	struct iqs9150_attribute *p_iqs9150_attr = to_iqs9150_attr(attr);
	ssize_t ret = -EIO;

	if (p_iqs9150_attr->store)
		ret = p_iqs9150_attr->store(kobj, attr, buf, count);
	return ret;
}

static const struct sysfs_ops iqs9150_sysfs_ops = {
	.show	= attr_show,
	.store	= attr_store,
};

static struct kobj_type iqs9150_kobj_type = {
	.sysfs_ops	= &iqs9150_sysfs_ops,
};

static ssize_t tp_control_show(struct kobject *kobj, struct attribute *this, char *buf)
{
	struct iqs9150_ts_data *iqs9150 = to_iqs9150_device(kobj);

	if (!iqs9150)
		return scnprintf(buf, PAGE_SIZE, "iqs9150 NULL\n");

	return snprintf(buf, PAGE_SIZE, "%d\n", !ts->prevent_accidental_touch);
}

static ssize_t tp_control_store(struct kobject *kobj,
					struct attribute *this,
					const char *buf, size_t count)
{
	struct iqs9150_ts_data *iqs9150 = to_iqs9150_device(kobj);
	int val = -1;

	if (!iqs9150)
		return -EINVAL;
	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	if (val) {
		ts->prevent_accidental_touch = 0;
		iqs9150_resume(&iqs9150->client->dev);
	} else {
		ts->prevent_accidental_touch = 1;
		iqs9150_suspend(&iqs9150->client->dev);
	}

	return count;
}

static ssize_t test_mode_show(struct kobject *kobj, struct attribute *this, char *buf)
{
	struct iqs9150_ts_data *iqs9150 = to_iqs9150_device(kobj);

	if (!iqs9150)
		return scnprintf(buf, PAGE_SIZE, "iqs9150 NULL\n");

	return snprintf(buf, PAGE_SIZE, "%d\n", ts->production_testing_flag);
}

static ssize_t test_mode_store(struct kobject *kobj,
					struct attribute *this,
					const char *buf, size_t count)
{
	struct iqs9150_ts_data *iqs9150 = to_iqs9150_device(kobj);
	int val = -1;

	if (!iqs9150)
		return -EINVAL;
	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	if (ts->get_rdy_fail) {
		IQS_ERROR("get rdy fail, can't enter test mode");
		return -1;
	}

	if (val) {
		ts->production_testing_flag = 1;
		iqs9150_force_i2c_comm_window(iqs9150);
	} else {
		ts->production_testing_flag = 0;
		stop_iqs9150_iqs_production_testing(iqs9150);
		iqs9150_stop_i2c_comm_window(iqs9150);
	}

	return count;
}

static struct iqs9150_attribute attr_tp_control = __ATTR_RW(tp_control);
static struct iqs9150_attribute attr_test_mode = __ATTR_RW(test_mode);
static const struct attribute *iqs9150_attrs[] = {
	&attr_tp_control.attr,
	&attr_test_mode.attr,
	NULL
};

static int iqs9150_ts_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct iqs9150_ts_data *ts = NULL;
	int ret = 0;

	IQS_FUNC_ENTER();

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		IQS_ERROR("I2C check functionality failed.");
		return -ENXIO;
	}

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return dev_err_probe(&client->dev, -ENOMEM, "failed to allocate device data\n");

	ts->cmd_buf = devm_kzalloc(&client->dev, 64, GFP_KERNEL);
	if (!ts->cmd_buf)
		return dev_err_probe(&client->dev, -ENOMEM, "failed to allocate cmd_buf\n");

	ts->client = client;
	ts->dev = &client->dev;
	i2c_set_clientdata(client, ts);
	ts->max_touch_num = MAX_TOUCH_REPORT;
	ts->resume_in_workqueue = false;
	ts->initialized = false;
	ts->panel_unblank = 1;
	ts->get_rdy_fail = true;
	ts->iqs9150_state = IQS_COMMS_WINDOW;
	ts->test_id = IQS_CHIP_ID;
	atomic_set(&ts->is_process_event_now, 0);

	if (client->dev.of_node) {
		ret = iqs_parse_dt(&client->dev, ts);
		if (ret)
			IQS_ERROR("device-tree parse fail");
	} else {
		IQS_ERROR("no valid device tree node found");
		return -ENODEV;
	}

	ret = iqs_gpio_configure(ts);
	if (ret) {
		IQS_ERROR(" error");
		goto err_free_ts;
	}

	msleep(DELAY_WAIT_FOR_TWO_RDY);

	if (ts->get_rdy_fail) {
		IQS_ERROR("get_rdy error");
		goto err_free_gpio;
	}

	ret = iqs9150_configure_inpudev(ts);
	if (ret < 0) {
		IQS_ERROR("iqs9150_configure_inpudev error");
		goto err_free_gpio;
	}

	ts->input_dev_registered = true;

	ts->workqueue = create_singlethread_workqueue("iqs9150_workthread");
	if (!ts->workqueue) {
		IQS_ERROR("Create workqueue failed!");
		ret = -ENOMEM;
		goto err_unreg_input;
	}

	INIT_DELAYED_WORK(&ts->panel_check_work, iqs9150_parse_panel_work);
	ts->workqueue_initialized = true;
	/* optimize the delay time */
	iqs9150_call_parse_panel_work(ts, DELAY_CALL_PARSE_PANEL_WORK_IN_PROBE);

	mutex_init(&ts->i2c_mutex);
	INIT_DELAYED_WORK(&ts->reg_read_dwork, iqs9150_reg_read_work_handler);
	queue_delayed_work(ts->workqueue,
				    &ts->reg_read_dwork,
				    msecs_to_jiffies(DELAY_TO_CHECK_PRODUCT_NUMBER));

	ts->initialized = true;

	/* create node */
	ret = kobject_init_and_add(&ts->kobj, &iqs9150_kobj_type, kernel_kobj, "iqs9150");
	if (ret) {
		dev_err(ts->dev, "Error in creation kobject_add, ret=%d.\n", ret);
		kobject_put(&ts->kobj);
		return ret;
	}
	ret = sysfs_create_files(&ts->kobj, iqs9150_attrs);
	if (ret) {
		dev_err(ts->dev, "Error in create sysfs, ret=%d.\n", ret);
		kobject_del(&ts->kobj);
	}

	IQS_FUNC_EXIT();
	return 0;

err_unreg_input:
	if (ts->input_dev_registered) {
		input_unregister_device(ts->input_dev);
		ts->input_dev_registered = false;
	}

	if (ts->workqueue) {
		cancel_delayed_work_sync(&ts->panel_check_work);
		destroy_workqueue(ts->workqueue);
	}

err_free_gpio:
	free_irq(ts->irq, ts);
	IQS_GPIO_FREE(ts->irq_gpio);
	gpio_set_value(ts->iovdd_gpio, GPIO_OUTPUT_LOW);
	gpio_set_value(ts->reset_gpio, GPIO_OUTPUT_LOW);
	IQS_GPIO_FREE(ts->iovdd_gpio);
	IQS_GPIO_FREE(ts->reset_gpio);

err_free_ts:
	devm_kfree(&client->dev, ts);
	return ret;
}

static void iqs9150_ts_remove(struct i2c_client *client)
{
	struct iqs9150_ts_data *ts = i2c_get_clientdata(client);

	IQS_FUNC_ENTER();

	if (!ts) {
		IQS_ERROR("ts is NULL");
		return;
	}

	iqs9150_unregister_powermanager(ts);

	if (ts->workqueue) {
		cancel_delayed_work_sync(&ts->panel_check_work);
		cancel_delayed_work_sync(&ts->reg_read_dwork);
		destroy_workqueue(ts->workqueue);
		ts->workqueue = NULL;
	}

	if (ts->irq > 0)
		free_irq(ts->irq, ts);

	if (gpio_is_valid(ts->irq_gpio))
		IQS_GPIO_FREE(ts->irq_gpio);

	if (ts->input_dev_registered)
		input_unregister_device(ts->input_dev);
	else if (ts->input_dev)
		input_free_device(ts->input_dev);

	mutex_destroy(&ts->lock);
	devm_kfree(&client->dev, ts);
	ts->get_rdy_fail = true;
	IQS_FUNC_EXIT();
}

static int iqs9150_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct iqs9150_ts_data *ts = i2c_get_clientdata(client);
	int error = 0;
	uint8_t buffer[RESUME_BUFFER_COUNT] = {0};

	if (!ts || !client || !ts->client) {
		IQS_ERROR("ts/client/ts->client is null!");
		return 0;
	}

	buffer[0] = SYSTEM_CONTROL_0;
	buffer[1] = SYSTEM_CONTROL_1|0x08;

	IQS_FUNC_ENTER();

	if (ts->suspend_state) {
		IQS_ERROR("Already in suspend state");
		return 0;
	}

	if (ts->get_rdy_fail) {
		IQS_ERROR("get rdy fail, disable suspend function");
		return 0;
	}

	cancel_delayed_work_sync(&ts->reg_read_dwork);

	mutex_lock(&ts->lock);

	if (ts->irq > 0)
		disable_irq(ts->irq);

	error = iqs9150_write_reg(ts->client, 0x11BC, 2, &buffer[0]);
	if (error)
		IQS_ERROR("iqs9150_write_reg  suspend 11BC");

	usleep_range(DELAY_US_AFTER_RW_REG_BEGIN, DELAY_US_AFTER_RW_REG_END);
	mutex_unlock(&ts->lock);
	ts->suspend_state = 1;
	IQS_FUNC_EXIT();

	return 0;
}

static int iqs9150_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct iqs9150_ts_data *ts = i2c_get_clientdata(client);
	int error = 0;
	uint8_t buffer[SUSPEND_BUFFER_COUNT] = {0};

	if (!ts || !client || !ts->client) {
		IQS_ERROR("ts/client/ts->client is null!");
		return 0;
	}

	if (ts->prevent_accidental_touch || !ts->panel_unblank) {
		IQS_INFO("resume blocked: prevent touch or panel unblank state");
		return 0;
	}

	buffer[0] = SYSTEM_CONTROL_0;
	buffer[1] = SYSTEM_CONTROL_1;
	buffer[2] = CONFIG_SETTINGS_0&0xF3;
	buffer[3] = CONFIG_SETTINGS_1;

	IQS_FUNC_ENTER();
	if (!ts->suspend_state) {
		IQS_ERROR("Already in resume state");
		return 0;
	}
	if (ts->get_rdy_fail) {
		IQS_ERROR("get rdy fail, disable resume function");
		return 0;
	}
	mutex_lock(&ts->lock);

	error = iqs9150_write_reg(ts->client, 0x11BC, 4, &buffer[0]);
	if (error)
		IQS_ERROR("iqs9150_write_reg  suspend 11BC");

	usleep_range(DELAY_US_AFTER_RW_REG_BEGIN, DELAY_US_AFTER_RW_REG_END);

	if (ts->irq > 0)
		enable_irq(ts->irq);
	mutex_unlock(&ts->lock);

	queue_delayed_work(ts->workqueue,
				    &ts->reg_read_dwork,
				    msecs_to_jiffies(DELAY_TO_CHECK_PRODUCT_NUMBER));
	ts->suspend_state = 0;
	IQS_FUNC_EXIT();

	return 0;
}

static SIMPLE_DEV_PM_OPS(iqs9150_pm_ops, iqs9150_suspend, iqs9150_resume);

static const struct i2c_device_id iqs9150_ts_id[] = {
	{ "iqs9150", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, iqs9150_ts_id);

static const struct of_device_id iqs9150_of_match[] = {
	{.compatible = "iqs9150, iqs9150", },
	{}
};

MODULE_DEVICE_TABLE(of, iqs9150_of_match);

static struct i2c_driver iqs9150_ts_driver = {
	.probe = iqs9150_ts_probe,
	.remove = iqs9150_ts_remove,
	.id_table = iqs9150_ts_id,
	.driver = {
		.name = "iqs9150",
		.of_match_table = of_match_ptr(iqs9150_of_match),
		.pm = &iqs9150_pm_ops,
	},
};

module_i2c_driver(iqs9150_ts_driver);

MODULE_DESCRIPTION("IQS9150 touchscreen driver");
MODULE_LICENSE("GPL");

