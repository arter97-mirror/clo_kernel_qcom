// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Parade ps883x usb retimer driver
 *
 * Copyright (C) 2024 Linaro Ltd.
 */

#include <drm/bridge/aux-bridge.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/usb/pd.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_retimer.h>
#include <linux/usb/typec_tbt.h>

#define REG_USB_PORT_CONN_STATUS_0		0x00

#define CONN_STATUS_0_CONNECTION_PRESENT	BIT(0)
#define CONN_STATUS_0_ORIENTATION_REVERSED	BIT(1)
#define CONN_STATUS_0_ACTIVE_CABLE		BIT(2)
#define CONN_STATUS_0_USB_3_1_CONNECTED		BIT(5)

#define REG_USB_PORT_CONN_STATUS_1		0x01

#define CONN_STATUS_1_DP_CONNECTED		BIT(0)
#define CONN_STATUS_1_DP_SINK_REQUESTED		BIT(1)
#define CONN_STATUS_1_DP_PIN_ASSIGNMENT_C_D	BIT(2)
#define CONN_STATUS_1_DP_HPD_LEVEL		BIT(7)

#define REG_USB_PORT_CONN_STATUS_2		0x02

#define CONN_STATUS_2_TBT_CONNECTED		BIT(0)
#define CONN_STATUS_2_TBT_UNIDIR_LSRX_ACT_LT	BIT(4)
#define CONN_STATUS_2_USB4_CONNECTED		BIT(7)
#define TBT_ENTER_MODE_UNI_DIR_LSRX		BIT(23)

#define PS8830_FW_DEFAULT_NAME			"ps883x/ps8830_fw.bin"
#define PS8833_FW_DEFAULT_NAME			"ps883x/ps8833_fw.bin"

#define PS883X_FW_MAGIC				"PS883XFW"
#define PS883X_FW_MAGIC_LEN			8
#define PS883X_FW_VERSION			1
#define PS883X_FW_HEADER_LEN			16

#define PS883X_FW_RAW_SIZE			0x10000

#define PS883X_FW_RAW_METADATA_OFF		0x2000
#define PS883X_FW_RAW_CHIP_ID_OFF		0x2004
#define PS883X_FW_RAW_CHIP_ID_LEN		16
#define PS883X_FW_RAW_VERSION_OFF		0x2019
#define PS883X_FW_RAW_BOOTLOADER_OFF		0x2021
#define PS883X_FW_RAW_BOOTLOADER_LEN		20
#define PS883X_FW_RAW_PROJECT_OFF		0x2035
#define PS883X_FW_RAW_PROJECT_LEN		40
#define PS883X_FW_RAW_DATE_OFF			0x205d
#define PS883X_FW_RAW_DATE_LEN			8

#define PS8830_FW_RAW_METADATA_OFF		0x9000
#define PS8830_FW_RAW_CHIP_ID_OFF		0x9004
#define PS8830_FW_RAW_CHIP_ID_LEN		16
#define PS8830_FW_RAW_VERSION_OFF		0x901b
#define PS8830_FW_RAW_BOOTLOADER_OFF		0x901f
#define PS8830_FW_RAW_BOOTLOADER_LEN		24
#define PS8830_FW_RAW_PROJECT_OFF		0x903d
#define PS8830_FW_RAW_PROJECT_LEN		36
#define PS8830_FW_RAW_DATE_OFF			0x9062
#define PS8830_FW_RAW_DATE_LEN			8

#define PS883X_FW_RAW_INFO_STR_LEN		32
#define PS883X_FW_RAW_VERSION_STR_LEN		16

#define PS8830_FW_CHIP_ID_PREFIX		"8830_"
#define PS8830_FW_CHIP_ID_ALT_PREFIX		"PS8830"
#define PS8833_FW_CHIP_ID_PREFIX		"8833_"

#define PS883X_LIVE_DEVICE_ID_PS8830		0x8830
#define PS883X_LIVE_DEVICE_ID_PS8833		0x8833
#define PS883X_LIVE_DEVICE_ID_PAGE		1
#define PS883X_LIVE_DEVICE_ID_OFF		0xfe
#define PS883X_LIVE_DEVICE_ID_LEN		2

#define PS883X_LIVE_FW_VERSION_PAGE		9
#define PS883X_LIVE_FW_VERSION_OFF		0x01
#define PS883X_LIVE_FW_VERSION_LEN		4

#define PS883X_SPI_SECTOR_SIZE			0x1000
#define PS883X_SPI_PAGE_SIZE			0x100
#define PS883X_SPI_BANK2_ADDRESS		0x00020000
#define PS883X_SPI_BOOT_DATA_ADDRESS		0x0000f000

#define PS883X_SPI_COMMAND_SECTOR_ERASE			0x20
#define PS883X_SPI_COMMAND_WRITE_ENABLE			0x06
#define PS883X_SPI_COMMAND_WRITE_STATUS_REGISTER_ENABLE	0x01

#define PS883X_POLLING_MAX_SPI			20
#define PS883X_WAIT_SPI_ROM_POLLING_MAX	1024

#define PS883X_FW_MAX_I2C_WRITE_LEN		32
#define PS883X_FW_MAX_DELAY_MS			5000

enum ps883x_fw_opcode {
	PS883X_FW_OP_I2C_WRITE		= 0x01,
	PS883X_FW_OP_READ_CHECK		= 0x02,
	PS883X_FW_OP_DELAY		= 0x03,
	PS883X_FW_OP_RESET_ASSERT	= 0x04,
	PS883X_FW_OP_RESET_RELEASE	= 0x05,
};

enum ps883x_fw_status {
	PS883X_FW_STATUS_IDLE,
	PS883X_FW_STATUS_RUNNING,
	PS883X_FW_STATUS_SUCCESS,
	PS883X_FW_STATUS_FAILED,
};

enum ps883x_fw_image_type {
	PS883X_FW_IMAGE_SCRIPT,
	PS883X_FW_IMAGE_RAW_BIN,
};

enum ps883x_chip {
	PS883X_CHIP_UNKNOWN,
	PS883X_CHIP_PS8830,
	PS883X_CHIP_PS8833,
};

struct ps883x_raw_fw_layout {
	enum ps883x_chip chip;
	u32 metadata_off;
	u32 chip_id_off;
	u32 version_off;
	u32 bootloader_off;
	u32 project_off;
	u32 date_off;
	size_t chip_id_len;
	size_t bootloader_len;
	size_t project_len;
	size_t date_len;
};

static const struct ps883x_raw_fw_layout ps883x_raw_fw_layouts[] = {
	{
		.chip = PS883X_CHIP_PS8833,
		.metadata_off = PS883X_FW_RAW_METADATA_OFF,
		.chip_id_off = PS883X_FW_RAW_CHIP_ID_OFF,
		.version_off = PS883X_FW_RAW_VERSION_OFF,
		.bootloader_off = PS883X_FW_RAW_BOOTLOADER_OFF,
		.project_off = PS883X_FW_RAW_PROJECT_OFF,
		.date_off = PS883X_FW_RAW_DATE_OFF,
		.chip_id_len = PS883X_FW_RAW_CHIP_ID_LEN,
		.bootloader_len = PS883X_FW_RAW_BOOTLOADER_LEN,
		.project_len = PS883X_FW_RAW_PROJECT_LEN,
		.date_len = PS883X_FW_RAW_DATE_LEN,
	},
	{
		.chip = PS883X_CHIP_PS8830,
		.metadata_off = PS8830_FW_RAW_METADATA_OFF,
		.chip_id_off = PS8830_FW_RAW_CHIP_ID_OFF,
		.version_off = PS8830_FW_RAW_VERSION_OFF,
		.bootloader_off = PS8830_FW_RAW_BOOTLOADER_OFF,
		.project_off = PS8830_FW_RAW_PROJECT_OFF,
		.date_off = PS8830_FW_RAW_DATE_OFF,
		.chip_id_len = PS8830_FW_RAW_CHIP_ID_LEN,
		.bootloader_len = PS8830_FW_RAW_BOOTLOADER_LEN,
		.project_len = PS8830_FW_RAW_PROJECT_LEN,
		.date_len = PS8830_FW_RAW_DATE_LEN,
	},
};

struct ps883x_fw_image_info {
	enum ps883x_fw_image_type type;
	enum ps883x_chip chip;
	bool valid;
	char chip_id[PS883X_FW_RAW_INFO_STR_LEN];
	char fw_version[PS883X_FW_RAW_VERSION_STR_LEN];
	char bootloader[PS883X_FW_RAW_INFO_STR_LEN];
	char project[PS883X_FW_RAW_INFO_STR_LEN];
	char build_date[PS883X_FW_RAW_DATE_LEN + 1];
};

struct ps883x_retimer {
	struct i2c_client *client;
	struct gpio_desc *reset_gpio;
	struct regmap *regmap;
	struct typec_switch_dev *sw;
	struct typec_retimer *retimer;
	struct clk *xo_clk;
	struct regulator *vdd_supply;
	struct regulator *vdd33_supply;
	struct regulator *vdd33_cap_supply;
	struct regulator *vddat_supply;
	struct regulator *vddar_supply;
	struct regulator *vddio_supply;

	struct typec_switch *typec_switch;
	struct typec_mux *typec_mux;

	struct mutex lock; /* protect non-concurrent retimer & switch */

	enum typec_orientation orientation;
	bool in_reset;
	bool dp_4_lane;

	enum ps883x_fw_status fw_status;
	int fw_result;
	struct ps883x_fw_image_info fw_info;
};

static int ps883x_enable_vregs(struct ps883x_retimer *retimer)
{
	struct device *dev = &retimer->client->dev;
	int ret;

	ret = regulator_enable(retimer->vdd33_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD 3.3V regulator: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(retimer->vdd33_cap_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD 3.3V CAP regulator: %d\n", ret);
		goto err_vdd33_disable;
	}

	mdelay(10);
	ret = regulator_enable(retimer->vddio_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD IO regulator: %d\n", ret);
		goto err_vdd33_cap_disable;
	}

	mdelay(10);

	ret = regulator_enable(retimer->vdd_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD regulator: %d\n", ret);
		goto err_vddio_disable;
	}

	ret = regulator_enable(retimer->vddar_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD AR regulator: %d\n", ret);
		goto err_vdd_disable;
	}

	ret = regulator_enable(retimer->vddat_supply);
	if (ret) {
		dev_err(dev, "cannot enable VDD AT regulator: %d\n", ret);
		goto err_vddar_disable;
	}

	mdelay(10);
	return 0;

err_vddar_disable:
	regulator_disable(retimer->vddar_supply);
err_vdd_disable:
	regulator_disable(retimer->vdd_supply);
err_vddio_disable:
	regulator_disable(retimer->vddio_supply);
err_vdd33_cap_disable:
	regulator_disable(retimer->vdd33_cap_supply);
err_vdd33_disable:
	regulator_disable(retimer->vdd33_supply);

	return ret;
}

static void ps883x_disable_vregs(struct ps883x_retimer *retimer)
{
	regulator_disable(retimer->vddat_supply);
	regulator_disable(retimer->vddar_supply);
	regulator_disable(retimer->vdd_supply);
	mdelay(3);

	regulator_disable(retimer->vddio_supply);
	mdelay(3);

	regulator_disable(retimer->vdd33_cap_supply);
	regulator_disable(retimer->vdd33_supply);
	mdelay(3);
}

static void ps883x_power_down(struct ps883x_retimer *retimer, bool clk_enabled)
{
	if (clk_enabled)
		clk_disable_unprepare(retimer->xo_clk);

	mdelay(2);
	ps883x_disable_vregs(retimer);
	gpiod_set_value(retimer->reset_gpio, 1);
}

static void ps883x_reset(struct ps883x_retimer *retimer)
{
	if (retimer->in_reset)
		return;

	ps883x_power_down(retimer, true);

	retimer->in_reset = true;
}

static int ps883x_reg_write(struct ps883x_retimer *retimer, int cfg0,
			    int cfg1, int cfg2)
{
	struct device *dev = &retimer->client->dev;
	int ret;

	ret = regmap_write(retimer->regmap, REG_USB_PORT_CONN_STATUS_0, cfg0);
	if (ret) {
		dev_err(dev, "failed to write conn_status_0: %d\n", ret);
		return ret;
	}

	ret = regmap_write(retimer->regmap, REG_USB_PORT_CONN_STATUS_1, cfg1);
	if (ret) {
		dev_err(dev, "failed to write conn_status_1: %d\n", ret);
		return ret;
	}

	ret = regmap_write(retimer->regmap, REG_USB_PORT_CONN_STATUS_2, cfg2);
	if (ret) {
		dev_err(dev, "failed to write conn_status_2: %d\n", ret);
		return ret;
	}

	if (retimer->dp_4_lane)
		mdelay(50);

	return 0;
}

static int ps883x_safe_mode(struct ps883x_retimer *retimer)
{
	int ret;

	ret = ps883x_reg_write(retimer, 0x01, 0x00, 0x00);
	if (ret) {
		dev_err(&retimer->client->dev, "failed to write safe mode: %d\n", ret);
		return ret;
	}
	mdelay(30);

	return 0;
}

static int ps883x_restore(struct ps883x_retimer *retimer)
{
	struct device *dev = &retimer->client->dev;
	unsigned int val;
	int ret;

	ret = ps883x_enable_vregs(retimer);
	if (ret)
		return ret;

	ret = clk_prepare_enable(retimer->xo_clk);
	if (ret) {
		dev_err(dev, "failed to enable XO: %d\n", ret);
		ps883x_power_down(retimer, false);
		retimer->in_reset = true;
		return ret;
	}

	gpiod_set_value(retimer->reset_gpio, 0);

	/* firmware initialization delay */
	msleep(65);

	/* make sure device is accessible */
	ret = regmap_read(retimer->regmap, REG_USB_PORT_CONN_STATUS_0,
			  &val);
	if (ret) {
		if (ret == -ENXIO) {
			ps883x_power_down(retimer, false);
			retimer->in_reset = true;
			ret = -EIO;
		}

		return ret;
	}

	ret = ps883x_safe_mode(retimer);
	if (ret)
		return ret;

	retimer->in_reset = false;
	return ret;
}

static int ps883x_configure(struct ps883x_retimer *retimer, int cfg0,
			    int cfg1, int cfg2, bool reset)
{
	struct device *dev = &retimer->client->dev;
	int ret;

	if (reset) {
		ps883x_reset(retimer);

		return 0;
	} else if (retimer->in_reset) {
		ret = ps883x_restore(retimer);
		if (ret) {
			dev_err(dev, "failed to restore the retimer:%d\n", ret);
			return ret;
		}
	}

	if (retimer->dp_4_lane) {
		ret = ps883x_safe_mode(retimer);
		if (ret)
			return ret;
	}

	ret = ps883x_reg_write(retimer, cfg0, cfg1, cfg2);
	if (ret) {
		dev_err(dev, "failed write the retimer config:%d\n", ret);
		return ret;
	}

	if (retimer->dp_4_lane)
		mdelay(50);

	return 0;
}

static const char *ps883x_chip_name(enum ps883x_chip chip)
{
	switch (chip) {
	case PS883X_CHIP_PS8830:
		return "PS8830";
	case PS883X_CHIP_PS8833:
		return "PS8833";
	default:
		return "PS883x";
	}
}

static const char *ps883x_chip_sysfs_name(enum ps883x_chip chip)
{
	switch (chip) {
	case PS883X_CHIP_PS8830:
		return "ps8830";
	case PS883X_CHIP_PS8833:
		return "ps8833";
	default:
		return "ps883x";
	}
}

static const char *ps883x_chip_fw_name(enum ps883x_chip chip)
{
	switch (chip) {
	case PS883X_CHIP_PS8830:
		return PS8830_FW_DEFAULT_NAME;
	case PS883X_CHIP_PS8833:
		return PS8833_FW_DEFAULT_NAME;
	default:
		return NULL;
	}
}

static enum ps883x_chip ps883x_raw_fw_chip(const char *chip_id)
{
	if (!strncmp(chip_id, PS8830_FW_CHIP_ID_PREFIX,
		     strlen(PS8830_FW_CHIP_ID_PREFIX)) ||
	    !strncmp(chip_id, PS8830_FW_CHIP_ID_ALT_PREFIX,
		     strlen(PS8830_FW_CHIP_ID_ALT_PREFIX)))
		return PS883X_CHIP_PS8830;

	if (!strncmp(chip_id, PS8833_FW_CHIP_ID_PREFIX,
		     strlen(PS8833_FW_CHIP_ID_PREFIX)))
		return PS883X_CHIP_PS8833;

	return PS883X_CHIP_UNKNOWN;
}

static enum ps883x_chip ps883x_fw_name_chip(const char *name)
{
	if (strstr(name, "ps8830"))
		return PS883X_CHIP_PS8830;

	if (strstr(name, "ps8833"))
		return PS883X_CHIP_PS8833;

	return PS883X_CHIP_UNKNOWN;
}

static u16 ps883x_get_le16(const u8 *buf)
{
	return buf[0] | ((u16)buf[1] << 8);
}

static u32 ps883x_get_le32(const u8 *buf)
{
	return buf[0] | ((u32)buf[1] << 8) | ((u32)buf[2] << 16) |
	       ((u32)buf[3] << 24);
}

static int ps883x_fw_i2c_write(struct ps883x_retimer *retimer, const u8 *data,
			       u8 len)
{
	struct device *dev = &retimer->client->dev;
	int ret;

	if (len < 2 || len > PS883X_FW_MAX_I2C_WRITE_LEN + 1)
		return -EINVAL;

	ret = i2c_master_send(retimer->client, data, len);
	if (ret < 0) {
		dev_err(dev, "firmware I2C write failed: %d\n", ret);
		return ret;
	}

	if (ret != len) {
		dev_err(dev, "firmware I2C short write: %d/%u\n", ret, len);
		return -EIO;
	}

	return 0;
}

static int ps883x_fw_i2c_read_byte(struct ps883x_retimer *retimer, u8 reg,
				   u8 *val)
{
	struct i2c_client *client = retimer->client;
	struct i2c_msg msgs[] = {
		{ .addr = client->addr, .flags = 0, .len = 1, .buf = &reg, },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = 1, .buf = val, },
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

static int ps883x_fw_read_check(struct ps883x_retimer *retimer, const u8 *data,
				u8 len)
{
	struct device *dev = &retimer->client->dev;
	u8 val;
	int ret;

	if (len != 3)
		return -EINVAL;

	ret = ps883x_fw_i2c_read_byte(retimer, data[0], &val);
	if (ret) {
		dev_err(dev, "firmware read check failed at 0x%02x: %d\n",
			data[0], ret);
		return ret;
	}

	if ((val & data[2]) != (data[1] & data[2])) {
		dev_err(dev,
			"firmware read check mismatch at 0x%02x: 0x%02x != 0x%02x mask 0x%02x\n",
			data[0], val, data[1], data[2]);
		return -EIO;
	}

	return 0;
}

static int ps883x_fw_delay(const u8 *data, u8 len)
{
	u16 delay_ms;

	if (len != 2)
		return -EINVAL;

	delay_ms = ps883x_get_le16(data);
	if (delay_ms > PS883X_FW_MAX_DELAY_MS)
		return -EINVAL;

	msleep(delay_ms);

	return 0;
}

static int ps883x_fw_reset_release(struct ps883x_retimer *retimer,
				   const u8 *data, u8 len)
{
	u16 delay_ms = 60;

	if (len) {
		if (len != 2)
			return -EINVAL;

		delay_ms = ps883x_get_le16(data);
		if (delay_ms > PS883X_FW_MAX_DELAY_MS)
			return -EINVAL;
	}

	gpiod_set_value(retimer->reset_gpio, 0);
	msleep(delay_ms);

	return 0;
}

static u8 ps883x_spi_addr_byte(u32 addr, u8 byte)
{
	return (addr >> (byte * 8)) & 0xff;
}

static int ps883x_i2c_write(struct ps883x_retimer *retimer, u8 page,
			       const u8 *data, size_t len)
{
	struct i2c_client *client = retimer->client;
	struct i2c_msg msg = {
		.addr = client->addr + page, .flags = 0, .len = len, .buf = (u8 *)data,
	};
	int ret;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		return ret;

	return ret == 1 ? 0 : -EIO;
}

static int ps883x_i2c_write_byte(struct ps883x_retimer *retimer, u8 page,
				  u8 offset, u8 val)
{
	u8 buf[] = { offset, val };

	return ps883x_i2c_write(retimer, page, buf, sizeof(buf));
}

static int ps883x_i2c_write_burst(struct ps883x_retimer *retimer, u8 page,
				   u8 offset, const u8 *data, size_t len)
{
	u8 buf[PS883X_SPI_PAGE_SIZE + 1];

	if (!len || len > PS883X_SPI_PAGE_SIZE)
		return -EINVAL;

	buf[0] = offset;
	memcpy(buf + 1, data, len);

	return ps883x_i2c_write(retimer, page, buf, len + 1);
}

static int ps883x_i2c_write_burst_skip_ff(struct ps883x_retimer *retimer,
					   u8 page, u8 offset,
					   const u8 *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (data[i] != 0xff)
			return ps883x_i2c_write_burst(retimer, page, offset,
							 data, len);
	}

	return 0;
}

static int ps883x_i2c_read_burst(struct ps883x_retimer *retimer, u8 page,
				  u8 offset, u8 *data, size_t len)
{
	struct i2c_client *client = retimer->client;
	struct i2c_msg msgs[] = {
		{ .addr = client->addr + page, .flags = 0, .len = 1, .buf = &offset, },
		{ .addr = client->addr + page, .flags = I2C_M_RD, .len = len, .buf = data, },
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

static int ps883x_i2c_read_byte(struct ps883x_retimer *retimer, u8 page,
				 u8 offset, u8 *val)
{
	return ps883x_i2c_read_burst(retimer, page, offset, val, 1);
}

static enum ps883x_chip ps883x_device_id_chip(u16 device_id)
{
	switch (device_id) {
	case PS883X_LIVE_DEVICE_ID_PS8830:
		return PS883X_CHIP_PS8830;
	case PS883X_LIVE_DEVICE_ID_PS8833:
		return PS883X_CHIP_PS8833;
	default:
		return PS883X_CHIP_UNKNOWN;
	}
}

static int ps883x_read_device_id(struct ps883x_retimer *retimer,
					 u16 *device_id)
{
	u8 id[PS883X_LIVE_DEVICE_ID_LEN];
	u8 val;
	int ret;

	/* Read page 0 first to wake paged I2C access before page 1. */
	ret = ps883x_i2c_read_byte(retimer, 0, 0x00, &val);
	if (ret)
		return ret;

	ret = ps883x_i2c_read_burst(retimer, PS883X_LIVE_DEVICE_ID_PAGE,
				       PS883X_LIVE_DEVICE_ID_OFF, id, sizeof(id));
	if (ret)
		return ret;

	*device_id = ps883x_get_le16(id);

	return 0;
}

static int ps883x_read_fw_version_bytes(struct ps883x_retimer *retimer,
					 u8 *version)
{
	u8 val;
	int ret;

	/* Read page 0 first to wake paged I2C access before page 9. */
	ret = ps883x_i2c_read_byte(retimer, 0, 0x00, &val);
	if (ret)
		return ret;

	return ps883x_i2c_read_burst(retimer, PS883X_LIVE_FW_VERSION_PAGE,
				       PS883X_LIVE_FW_VERSION_OFF, version,
				       PS883X_LIVE_FW_VERSION_LEN);
}

static int ps883x_read_chip(struct ps883x_retimer *retimer,
			    enum ps883x_chip *chip)
{
	u16 device_id;
	u8 version[PS883X_LIVE_FW_VERSION_LEN];
	int ret;

	ret = ps883x_read_device_id(retimer, &device_id);
	if (ret)
		return ret;

	*chip = ps883x_device_id_chip(device_id);
	if (*chip == PS883X_CHIP_UNKNOWN) {
		dev_warn(&retimer->client->dev,
			 "unsupported PS883x live device ID 0x%04x\n",
			 device_id);
		return -ENODEV;
	}

	ret = ps883x_read_fw_version_bytes(retimer, version);
	if (ret) {
		dev_warn(&retimer->client->dev,
			 "detected %s retimer device ID 0x%04x, failed to read live firmware version: %d\n",
			 ps883x_chip_name(*chip), device_id, ret);
		return 0;
	}

	dev_info(&retimer->client->dev,
		 "detected %s retimer device ID 0x%04x, live firmware version %02X_%X_%X_%02X\n",
		 ps883x_chip_name(*chip), device_id,
		 version[0], version[1], version[2], version[3]);

	return 0;
}

static int ps883x_read_fw_version(struct ps883x_retimer *retimer,
				  char *fw_version, size_t len)
{
	u8 version[PS883X_LIVE_FW_VERSION_LEN];
	int ret;

	if (!len)
		return -EINVAL;

	ret = ps883x_read_fw_version_bytes(retimer, version);
	if (ret)
		return ret;

	snprintf(fw_version, len, "%02X_%X_%X_%02X",
		 version[0], version[1], version[2], version[3]);

	return 0;
}

static int ps8830_escape_low_power(struct ps883x_retimer *retimer)
{
	return ps883x_i2c_write_byte(retimer, 1, 0x68, 0xa0);
}

static int ps8833_safe_mode_and_escape_low_power(struct ps883x_retimer *retimer)
{
	u8 val;
	int ret;
	int i;

	ret = ps883x_i2c_read_byte(retimer, 0, 0x00, &val);
	if (ret)
		return ret;

	if (!val) {
		ret = ps883x_i2c_write_byte(retimer, 0, 0x00, 0x01);
		if (ret)
			return ret;
	}

	for (i = 0; i < 3; i++) {
		ret = ps883x_i2c_read_byte(retimer, 1, 0x70, &val);
		if (ret)
			return ret;

		if (val) {
			ret = ps883x_i2c_write_byte(retimer, 1, 0x70, 0x00);
			if (ret)
				return ret;
		}

		msleep(50);
	}

	return 0;
}

static int ps883x_escape_low_power(struct ps883x_retimer *retimer,
				    enum ps883x_chip chip)
{
	switch (chip) {
	case PS883X_CHIP_PS8830:
		return ps8830_escape_low_power(retimer);
	case PS883X_CHIP_PS8833:
		return ps8833_safe_mode_and_escape_low_power(retimer);
	default:
		return -EINVAL;
	}
}

static int ps883x_map_spi_rom_to_page7(struct ps883x_retimer *retimer)
{
	return ps883x_i2c_write_byte(retimer, 2, 0xbd, 0x9f);
}

static int ps883x_spi_reset_interface(struct ps883x_retimer *retimer)
{
	int ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x90, 0x04);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x92, 0x00);
	if (ret)
		return ret;

	return ps883x_i2c_write_byte(retimer, 2, 0x93, 0x05);
}

static int ps883x_spi_write_enable(struct ps883x_retimer *retimer)
{
	int ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x90,
				      PS883X_SPI_COMMAND_WRITE_ENABLE);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x92, 0x00);
	if (ret)
		return ret;

	return ps883x_i2c_write_byte(retimer, 2, 0x93, 0x05);
}

static int ps883x_wait_spi_rom_ready(struct ps883x_retimer *retimer)
{
	u8 val;
	int ret;
	int count1;
	int count2;
	int count3;

	ret = ps883x_i2c_read_byte(retimer, 2, 0x9e, &val);
	if (ret)
		return ret;

	count1 = 0;
	while (val & 0x0c) {
		ret = ps883x_i2c_read_byte(retimer, 2, 0x9e, &val);
		if (ret)
			return ret;

		if (++count1 > PS883X_WAIT_SPI_ROM_POLLING_MAX)
			return -ETIMEDOUT;
	}

	val = 0x01;
	count2 = 0;
	while (val & 0x01) {
		ret = ps883x_i2c_write_byte(retimer, 2, 0x90, 0x05);
		if (ret)
			return ret;

		ret = ps883x_i2c_write_byte(retimer, 2, 0x92, 0x00);
		if (ret)
			return ret;

		ret = ps883x_i2c_write_byte(retimer, 2, 0x93, 0x01);
		if (ret)
			return ret;

		ret = ps883x_i2c_read_byte(retimer, 2, 0x93, &val);
		if (ret)
			return ret;

		count3 = 0;
		while (val & 0x01) {
			ret = ps883x_i2c_read_byte(retimer, 2, 0x93, &val);
			if (ret)
				return ret;

			if (++count3 > PS883X_WAIT_SPI_ROM_POLLING_MAX)
				return -ETIMEDOUT;
		}

		ret = ps883x_i2c_read_byte(retimer, 2, 0x91, &val);
		if (ret)
			return ret;

		if (++count2 > PS883X_WAIT_SPI_ROM_POLLING_MAX)
			return -ETIMEDOUT;
	}

	return 0;
}

static int ps883x_enable_mpu(struct ps883x_retimer *retimer)
{
	return ps883x_i2c_write_byte(retimer, 2, 0xd6, 0x00);
}

static int ps883x_disable_mpu(struct ps883x_retimer *retimer)
{
	int ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0xd6, 0xc0);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0xd6, 0x40);
	if (ret)
		return ret;

	return ps883x_spi_reset_interface(retimer);
}

static int ps883x_spi_sector_erase(struct ps883x_retimer *retimer,
				    u8 addr24, u8 addr16)
{
	int ret;

	ret = ps883x_spi_write_enable(retimer);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x90,
				      PS883X_SPI_COMMAND_SECTOR_ERASE);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x90, addr24);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x90, addr16);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x90, 0x00);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x92, 0x03);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x93, 0x05);
	if (ret)
		return ret;

	return ps883x_wait_spi_rom_ready(retimer);
}

static int ps883x_page_h_write_enable(struct ps883x_retimer *retimer)
{
	static const u8 unlock[] = { 0xaa, 0x55, 0x50, 0x41, 0x52, 0x44 };
	u8 status = 0;
	int ret;
	int i;
	int j;

	for (i = 0; i < PS883X_POLLING_MAX_SPI && status != 0x01; i++) {
		for (j = 0; j < ARRAY_SIZE(unlock); j++) {
			ret = ps883x_i2c_write_byte(retimer, 2, 0xda, unlock[j]);
			if (ret)
				return ret;
		}

		ret = ps883x_i2c_read_byte(retimer, 2, 0xda, &status);
		if (ret)
			return ret;
	}

	return status == 0x01 ? 0 : -ETIMEDOUT;
}

static int ps883x_spi_write_status_register(struct ps883x_retimer *retimer,
					    enum ps883x_chip chip,
					    u8 status1, u8 status2)
{
	int ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x90,
				      PS883X_SPI_COMMAND_WRITE_STATUS_REGISTER_ENABLE);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x90, status1);
	if (ret)
		return ret;

	/*
	 * The PS8830's SPI NOR uses the 2-byte WRSR form (status1 + status2,
	 * len=0x02); PS8833's EN25F20 sample sequence only writes status
	 * register 1 (len=0x01) and status2 is not sent on the wire.
	 */
	if (chip == PS883X_CHIP_PS8830) {
		ret = ps883x_i2c_write_byte(retimer, 2, 0x90, status2);
		if (ret)
			return ret;
	}

	ret = ps883x_i2c_write_byte(retimer, 2, 0x92,
				     chip == PS883X_CHIP_PS8830 ? 0x02 : 0x01);
	if (ret)
		return ret;

	return ps883x_i2c_write_byte(retimer, 2, 0x93, 0x05);
}

static int ps883x_spi_read_status_register1(struct ps883x_retimer *retimer,
					    u8 *status)
{
	int ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x90, 0x05);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x92, 0x00);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x93, 0x01);
	if (ret)
		return ret;

	return ps883x_i2c_read_byte(retimer, 2, 0x91, status);
}

static int ps883x_spi_read_status_register2(struct ps883x_retimer *retimer,
					    u8 *status)
{
	int ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x90, 0x35);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x92, 0x00);
	if (ret)
		return ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x93, 0x01);
	if (ret)
		return ret;

	return ps883x_i2c_read_byte(retimer, 2, 0x91, status);
}

static int ps883x_polling_status(struct ps883x_retimer *retimer, u8 *status)
{
	static const u8 unlock[] = { 0xaa, 0x55, 0x50, 0x41, 0x52, 0x44 };
	int ret;
	int i;
	int j;

	*status = 0;
	for (i = 0; i < PS883X_POLLING_MAX_SPI && *status != 0x01; i++) {
		for (j = 0; j < ARRAY_SIZE(unlock); j++) {
			ret = ps883x_i2c_write_byte(retimer, 2, 0xda, unlock[j]);
			if (ret)
				return ret;
		}

		ret = ps883x_i2c_read_byte(retimer, 2, 0xda, status);
		if (ret)
			return ret;
	}

	return 0;
}

static int ps883x_enable_write(struct ps883x_retimer *retimer,
				enum ps883x_chip chip)
{
	u8 polling_status;
	u8 status1;
	u8 status2;
	int ret;

	ret = ps883x_disable_mpu(retimer);
	if (ret)
		return ret;

	ret = ps883x_page_h_write_enable(retimer);
	if (ret)
		return ret;

	ret = ps883x_polling_status(retimer, &polling_status);
	if (ret)
		return ret;
	if (!polling_status)
		return -EIO;

	ret = ps883x_spi_write_enable(retimer);
	if (ret)
		return ret;

	ret = ps883x_spi_read_status_register1(retimer, &status1);
	if (ret)
		return ret;

	ret = ps883x_spi_read_status_register2(retimer, &status2);
	if (ret)
		return ret;

	/* Bank 2 write-enable settings, common to both chips. */
	status1 &= ~(BIT(7) | BIT(6) | BIT(5));
	status1 |= BIT(6) | BIT(5) | BIT(4) | BIT(3);
	status1 &= ~BIT(2);
	status2 &= ~BIT(6);
	status2 |= BIT(1);

	if (chip == PS883X_CHIP_PS8833)
		/* EN25F20-specific value used by the PS8833 reference updater. */
		status1 = 0x44;

	ret = ps883x_spi_write_status_register(retimer, chip, status1, status2);
	if (ret)
		return ret;

	ret = ps883x_wait_spi_rom_ready(retimer);
	if (ret)
		return ret;

	ret = ps883x_polling_status(retimer, &polling_status);
	if (ret)
		return ret;

	return polling_status ? 0 : -EIO;
}

static int ps883x_disable_write(struct ps883x_retimer *retimer,
				 enum ps883x_chip chip)
{
	u8 status1;
	u8 status2;
	int ret;

	ret = ps883x_disable_mpu(retimer);
	if (ret)
		return ret;

	ret = ps883x_spi_write_enable(retimer);
	if (ret)
		return ret;

	ret = ps883x_spi_read_status_register1(retimer, &status1);
	if (ret)
		return ret;

	ret = ps883x_spi_read_status_register2(retimer, &status2);
	if (ret)
		return ret;

	status2 &= ~BIT(6);
	status2 |= BIT(1);

	/* Bank write-protect settings, common to both chips. */
	status1 |= BIT(7) | BIT(6) | BIT(3) | BIT(2);
	status1 &= ~(BIT(5) | BIT(4));

	if (chip == PS883X_CHIP_PS8833)
		/* Protect banks 0..3, matching the PS8833 reference updater. */
		status1 = 0xdc;

	ret = ps883x_spi_write_status_register(retimer, chip, status1, status2);
	if (ret)
		return ret;

	ret = ps883x_wait_spi_rom_ready(retimer);
	if (ret)
		return ret;

	return ps883x_i2c_write_byte(retimer, 2, 0xda, 0x00);
}

static int ps883x_set_spi_address(struct ps883x_retimer *retimer, u32 addr)
{
	int ret;

	ret = ps883x_i2c_write_byte(retimer, 2, 0x8f,
				      ps883x_spi_addr_byte(addr, 2));
	if (ret)
		return ret;

	return ps883x_i2c_write_byte(retimer, 2, 0x8e,
				       ps883x_spi_addr_byte(addr, 1));
}

static int ps883x_spi_erase_by_sector(struct ps883x_retimer *retimer,
				       u32 addr, size_t len)
{
	u32 aligned_addr = addr & ~(PS883X_SPI_SECTOR_SIZE - 1);
	size_t aligned_len = ALIGN(len, PS883X_SPI_SECTOR_SIZE);
	u32 pos = aligned_addr;
	int sectors = aligned_len / PS883X_SPI_SECTOR_SIZE;
	int ret;
	int i;

	for (i = 0; i < sectors; i++) {
		ret = ps883x_spi_sector_erase(retimer,
					     ps883x_spi_addr_byte(pos, 2),
					     ps883x_spi_addr_byte(pos, 1));
		if (ret)
			return ret;

		pos += PS883X_SPI_SECTOR_SIZE;
	}

	return 0;
}

static int ps883x_program_raw_firmware(struct ps883x_retimer *retimer,
					enum ps883x_chip chip,
					const u8 *data, size_t len)
{
	static const u8 boot_data[] = { 0x55, 0xaa, 0x02, 0xff };
	struct device *dev = &retimer->client->dev;
	u8 boot_readback[sizeof(boot_data)];
	u32 spi_addr;
	int pages;
	int ret;
	int i;

	if (len != PS883X_FW_RAW_SIZE)
		return -EINVAL;

	pages = len / PS883X_SPI_PAGE_SIZE;

	ret = ps883x_escape_low_power(retimer, chip);
	if (ret)
		return ret;

	ret = ps883x_disable_mpu(retimer);
	if (ret)
		return ret;

	ret = ps883x_map_spi_rom_to_page7(retimer);
	if (ret)
		return ret;

	ret = ps883x_enable_write(retimer, chip);
	if (ret)
		return ret;

	dev_info(dev, "%s erase SPI bank2\n", ps883x_chip_name(chip));
	ret = ps883x_spi_erase_by_sector(retimer, PS883X_SPI_BANK2_ADDRESS, len);
	if (ret)
		return ret;

	dev_info(dev, "%s program SPI bank2\n", ps883x_chip_name(chip));
	spi_addr = PS883X_SPI_BANK2_ADDRESS;
	for (i = 0; i < pages; i++) {
		ret = ps883x_set_spi_address(retimer, spi_addr);
		if (ret)
			return ret;

		ret = ps883x_i2c_write_burst_skip_ff(retimer, 7, 0,
							 data, PS883X_SPI_PAGE_SIZE);
		if (ret)
			return ret;

		data += PS883X_SPI_PAGE_SIZE;
		spi_addr += PS883X_SPI_PAGE_SIZE;
	}

	dev_info(dev, "%s check boot data\n", ps883x_chip_name(chip));
	ret = ps883x_set_spi_address(retimer, PS883X_SPI_BOOT_DATA_ADDRESS);
	if (ret)
		return ret;

	ret = ps883x_i2c_read_burst(retimer, 7, 0, boot_readback,
					  sizeof(boot_readback));
	if (ret)
		return ret;

	if (memcmp(boot_readback, boot_data, sizeof(boot_data))) {
		dev_info(dev, "%s update boot data\n", ps883x_chip_name(chip));
		ret = ps883x_spi_erase_by_sector(retimer,
						 PS883X_SPI_BOOT_DATA_ADDRESS,
						 PS883X_SPI_SECTOR_SIZE);
		if (ret)
			return ret;

		ret = ps883x_set_spi_address(retimer,
					      PS883X_SPI_BOOT_DATA_ADDRESS);
		if (ret)
			return ret;

		ret = ps883x_i2c_write_burst(retimer, 7, 0, boot_data,
						 sizeof(boot_data));
		if (ret)
			return ret;
	}

	ret = ps883x_disable_write(retimer, chip);
	if (ret)
		return ret;

	return ps883x_enable_mpu(retimer);
}

static void ps883x_raw_fw_copy_string(char *dst, size_t dst_len,
				      const u8 *src, size_t src_len)
{
	size_t i;

	if (!dst_len)
		return;

	for (i = 0; i < src_len && i < dst_len - 1; i++) {
		if (!src[i])
			break;

		dst[i] = src[i] >= 0x20 && src[i] <= 0x7e ? src[i] : '?';
	}

	dst[i] = '\0';
}

static int ps883x_raw_fw_parse_info(const struct firmware *fw,
				    struct ps883x_fw_image_info *info)
{
	const struct ps883x_raw_fw_layout *layout = NULL;
	const u8 *data;
	int i;

	if (fw->size != PS883X_FW_RAW_SIZE)
		return -EINVAL;

	memset(info, 0, sizeof(*info));
	data = fw->data;

	for (i = 0; i < ARRAY_SIZE(ps883x_raw_fw_layouts); i++) {
		const struct ps883x_raw_fw_layout *tmp;
		enum ps883x_chip chip;

		tmp = &ps883x_raw_fw_layouts[i];
		if (data[tmp->metadata_off] != 0x55 ||
		    data[tmp->metadata_off + 1] != 0xaa)
			continue;

		ps883x_raw_fw_copy_string(info->chip_id,
					  sizeof(info->chip_id),
					  data + tmp->chip_id_off,
					  tmp->chip_id_len);

		chip = ps883x_raw_fw_chip(info->chip_id);
		if (chip != tmp->chip)
			continue;

		layout = tmp;
		info->chip = chip;
		break;
	}

	if (!layout)
		return -EINVAL;

	info->type = PS883X_FW_IMAGE_RAW_BIN;
	info->valid = true;

	ps883x_raw_fw_copy_string(info->bootloader,
				  sizeof(info->bootloader),
				  data + layout->bootloader_off,
				  layout->bootloader_len);
	ps883x_raw_fw_copy_string(info->project,
				  sizeof(info->project),
				  data + layout->project_off,
				  layout->project_len);
	ps883x_raw_fw_copy_string(info->build_date,
				  sizeof(info->build_date),
				  data + layout->date_off,
				  layout->date_len);

	snprintf(info->fw_version, sizeof(info->fw_version),
		 "%02X_%X_%X_%02X",
		 data[layout->version_off], data[layout->version_off + 1],
		 data[layout->version_off + 2], data[layout->version_off + 3]);

	return 0;
}

static int ps883x_raw_fw_update(struct ps883x_retimer *retimer,
				const struct firmware *fw,
				enum ps883x_chip expected_chip)
{
	struct device *dev = &retimer->client->dev;
	struct ps883x_fw_image_info info;
	int ret;

	ret = ps883x_raw_fw_parse_info(fw, &info);
	if (ret) {
		dev_err(dev, "invalid PS883x raw firmware image: %d\n", ret);
		return ret;
	}

	if (expected_chip != PS883X_CHIP_UNKNOWN && info.chip != expected_chip) {
		dev_err(dev, "%s firmware image does not match requested %s firmware\n",
			ps883x_chip_name(info.chip),
			ps883x_chip_name(expected_chip));
		return -EINVAL;
	}

	retimer->fw_info = info;

	dev_info(dev,
		 "%s raw firmware image: chip_id=\"%s\" fw_version=%s bootloader=\"%s\" project=\"%s\" build_date=%s\n",
		 ps883x_chip_name(info.chip), info.chip_id, info.fw_version,
		 info.bootloader, info.project, info.build_date);

	ret = ps883x_program_raw_firmware(retimer, info.chip, fw->data, fw->size);
	if (ret) {
		dev_err(dev, "%s firmware update failed: %d\n",
			ps883x_chip_name(info.chip), ret);
		return ret;
	}

	dev_info(dev, "%s firmware update completed\n",
		 ps883x_chip_name(info.chip));

	return 0;
}

static int ps883x_fw_validate(const struct firmware *fw, const u8 **payload,
			      size_t *payload_len)
{
	u16 version;
	u16 hdr_len;
	u32 len;

	if (!fw || fw->size < PS883X_FW_HEADER_LEN)
		return -EINVAL;

	if (memcmp(fw->data, PS883X_FW_MAGIC, PS883X_FW_MAGIC_LEN))
		return -EINVAL;

	version = ps883x_get_le16(fw->data + 8);
	hdr_len = ps883x_get_le16(fw->data + 10);
	len = ps883x_get_le32(fw->data + 12);

	if (version != PS883X_FW_VERSION || hdr_len < PS883X_FW_HEADER_LEN)
		return -EINVAL;

	if (hdr_len > fw->size || len != fw->size - hdr_len)
		return -EINVAL;

	*payload = fw->data + hdr_len;
	*payload_len = len;

	return 0;
}

static enum ps883x_fw_image_type ps883x_fw_image_type(const struct firmware *fw)
{
	if (fw->size >= PS883X_FW_MAGIC_LEN &&
	    !memcmp(fw->data, PS883X_FW_MAGIC, PS883X_FW_MAGIC_LEN))
		return PS883X_FW_IMAGE_SCRIPT;

	return PS883X_FW_IMAGE_RAW_BIN;
}

static int ps883x_fw_execute(struct ps883x_retimer *retimer,
			     const struct firmware *fw,
			     enum ps883x_chip expected_chip)
{
	struct device *dev = &retimer->client->dev;
	const u8 *payload;
	size_t len;
	int ret;

	if (ps883x_fw_image_type(fw) == PS883X_FW_IMAGE_RAW_BIN)
		return ps883x_raw_fw_update(retimer, fw, expected_chip);

	ret = ps883x_fw_validate(fw, &payload, &len);
	if (ret) {
		dev_err(dev, "invalid firmware update image: %d\n", ret);
		return ret;
	}

	memset(&retimer->fw_info, 0, sizeof(retimer->fw_info));
	retimer->fw_info.type = PS883X_FW_IMAGE_SCRIPT;
	retimer->fw_info.valid = true;

	while (len) {
		u8 opcode;
		u8 rec_len;
		const u8 *rec;

		if (len < 2)
			return -EINVAL;

		opcode = payload[0];
		rec_len = payload[1];
		payload += 2;
		len -= 2;

		if (rec_len > len)
			return -EINVAL;

		rec = payload;
		ret = 0;

		switch (opcode) {
		case PS883X_FW_OP_I2C_WRITE:
			ret = ps883x_fw_i2c_write(retimer, rec, rec_len);
			break;
		case PS883X_FW_OP_READ_CHECK:
			ret = ps883x_fw_read_check(retimer, rec, rec_len);
			break;
		case PS883X_FW_OP_DELAY:
			ret = ps883x_fw_delay(rec, rec_len);
			break;
		case PS883X_FW_OP_RESET_ASSERT:
			if (rec_len)
				ret = -EINVAL;
			else
				gpiod_set_value(retimer->reset_gpio, 1);
			break;
		case PS883X_FW_OP_RESET_RELEASE:
			ret = ps883x_fw_reset_release(retimer, rec, rec_len);
			break;
		default:
			dev_err(dev, "unsupported firmware opcode 0x%02x\n",
				opcode);
			ret = -EINVAL;
			break;
		}

		if (ret)
			return ret;

		payload += rec_len;
		len -= rec_len;
	}

	return 0;
}

static ssize_t fw_update_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct ps883x_retimer *retimer = dev_get_drvdata(dev);
	const struct firmware *fw;
	char fw_name[256];
	char *name;
	const char *fw_path;
	enum ps883x_chip expected_chip;
	bool use_default;
	bool restore_reset;
	size_t len;
	int ret;

	len = strnlen(buf, count);
	if (!len)
		return -EINVAL;

	if (len >= sizeof(fw_name))
		return -ENAMETOOLONG;

	memcpy(fw_name, buf, len);
	fw_name[len] = '\0';
	name = strim(fw_name);

	if (!name[0])
		return -EINVAL;

	use_default = !strcmp(name, "1") || !strcmp(name, "default");
	if (!use_default) {
		if (!strcmp(name, "ps8830"))
			name = PS8830_FW_DEFAULT_NAME;
		else if (!strcmp(name, "ps8833"))
			name = PS8833_FW_DEFAULT_NAME;
	}

	fw_path = name;
	expected_chip = ps883x_fw_name_chip(fw_path);

	mutex_lock(&retimer->lock);

	retimer->fw_status = PS883X_FW_STATUS_RUNNING;
	retimer->fw_result = 0;
	memset(&retimer->fw_info, 0, sizeof(retimer->fw_info));

	restore_reset = retimer->in_reset;
	if (retimer->in_reset) {
		ret = ps883x_restore(retimer);
		if (ret) {
			dev_err(dev, "failed to wake retimer for firmware update: %d\n",
				ret);
			goto out;
		}
	}

	if (use_default) {
		ret = ps883x_read_chip(retimer, &expected_chip);
		if (ret) {
			dev_err(dev, "failed to detect retimer chip: %d\n", ret);
			goto out;
		}

		fw_path = ps883x_chip_fw_name(expected_chip);
		if (!fw_path) {
			ret = -ENODEV;
			goto out;
		}

		dev_info(dev, "detected %s retimer, using firmware %s\n",
			 ps883x_chip_name(expected_chip), fw_path);
	}

	ret = request_firmware(&fw, fw_path, dev);
	if (ret) {
		dev_err(dev, "failed to request firmware %s: %d\n", fw_path, ret);
		goto out;
	}

	ret = ps883x_fw_execute(retimer, fw, expected_chip);
	release_firmware(fw);

out:
	if (restore_reset && !retimer->in_reset)
		ps883x_reset(retimer);

	if (ret) {
		retimer->fw_status = PS883X_FW_STATUS_FAILED;
		retimer->fw_result = ret;
	} else {
		retimer->fw_status = PS883X_FW_STATUS_SUCCESS;
		retimer->fw_result = 0;
	}

	mutex_unlock(&retimer->lock);

	return ret ? ret : count;
}

static ssize_t fw_update_status_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct ps883x_retimer *retimer = dev_get_drvdata(dev);

	switch (retimer->fw_status) {
	case PS883X_FW_STATUS_IDLE:
		return sysfs_emit(buf, "idle\n");
	case PS883X_FW_STATUS_RUNNING:
		return sysfs_emit(buf, "running\n");
	case PS883X_FW_STATUS_SUCCESS:
		return sysfs_emit(buf, "success\n");
	case PS883X_FW_STATUS_FAILED:
		return sysfs_emit(buf, "failed:%d\n", retimer->fw_result);
	default:
		return sysfs_emit(buf, "unknown\n");
	}
}

static ssize_t fw_update_info_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct ps883x_retimer *retimer = dev_get_drvdata(dev);
	char live_fw_version[PS883X_FW_RAW_VERSION_STR_LEN];
	struct ps883x_fw_image_info info;
	int ret = 0;

	mutex_lock(&retimer->lock);
	info = retimer->fw_info;
	if (!info.valid)
		ret = ps883x_read_fw_version(retimer, live_fw_version,
						 sizeof(live_fw_version));
	mutex_unlock(&retimer->lock);

	if (!info.valid) {
		if (ret)
			return sysfs_emit(buf,
					  "last_update=none fw_version=unavailable:%d\n",
					  ret);

		return sysfs_emit(buf, "last_update=none fw_version=%s\n",
				  live_fw_version);
	}

	if (info.type == PS883X_FW_IMAGE_SCRIPT)
		return sysfs_emit(buf, "script\n");

	return sysfs_emit(buf,
			  "%s_raw_bin chip_id=\"%s\" fw_version=%s bootloader=\"%s\" project=\"%s\" build_date=%s\n",
			  ps883x_chip_sysfs_name(info.chip), info.chip_id,
			  info.fw_version, info.bootloader, info.project,
			  info.build_date);
}

static ssize_t fw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ps883x_retimer *retimer = dev_get_drvdata(dev);
	char live_fw_version[PS883X_FW_RAW_VERSION_STR_LEN];
	int ret;

	mutex_lock(&retimer->lock);
	ret = ps883x_read_fw_version(retimer, live_fw_version,
					 sizeof(live_fw_version));
	mutex_unlock(&retimer->lock);

	if (ret)
		return sysfs_emit(buf, "unavailable:%d\n", ret);

	return sysfs_emit(buf, "%s\n", live_fw_version);
}

static DEVICE_ATTR_WO(fw_update);
static DEVICE_ATTR_RO(fw_update_info);
static DEVICE_ATTR_RO(fw_update_status);
static DEVICE_ATTR_RO(fw_version);

static struct attribute *ps883x_fw_attrs[] = {
	&dev_attr_fw_update.attr,
	&dev_attr_fw_update_info.attr,
	&dev_attr_fw_update_status.attr,
	&dev_attr_fw_version.attr,
	NULL,
};

static const struct attribute_group ps883x_fw_attr_group = {
	.attrs = ps883x_fw_attrs,
};

static int ps883x_set(struct ps883x_retimer *retimer, struct typec_retimer_state *state)
{
	struct typec_thunderbolt_data *tb_data;
	const struct enter_usb_data *eudo_data;
	int cfg0 = CONN_STATUS_0_CONNECTION_PRESENT;
	int cfg1 = 0x00;
	int cfg2 = 0x00;
	bool reset = false;

	if (retimer->orientation == TYPEC_ORIENTATION_REVERSE)
		cfg0 |= CONN_STATUS_0_ORIENTATION_REVERSED;

	retimer->dp_4_lane = false;

	if (state->alt) {
		switch (state->alt->svid) {
		case USB_TYPEC_DP_SID:
			cfg1 |= CONN_STATUS_1_DP_CONNECTED |
				CONN_STATUS_1_DP_HPD_LEVEL;

			switch (state->mode)  {
			case TYPEC_DP_STATE_C:
				cfg1 |= CONN_STATUS_1_DP_PIN_ASSIGNMENT_C_D;
				retimer->dp_4_lane = true;

				break;
			case TYPEC_DP_STATE_D:
				cfg1 |= CONN_STATUS_1_DP_PIN_ASSIGNMENT_C_D;
				cfg0 |= CONN_STATUS_0_USB_3_1_CONNECTED;
				break;
			default: /* MODE_E */
				break;
			}
			break;
		case USB_TYPEC_TBT_SID:
			tb_data = state->data;

			/* Unconditional */
			cfg2 |= CONN_STATUS_2_TBT_CONNECTED;

			if (tb_data->cable_mode & TBT_CABLE_ACTIVE_PASSIVE)
				cfg0 |= CONN_STATUS_0_ACTIVE_CABLE;

			if (tb_data->enter_vdo & TBT_ENTER_MODE_UNI_DIR_LSRX)
				cfg2 |= CONN_STATUS_2_TBT_UNIDIR_LSRX_ACT_LT;
			break;
		default:
			dev_err(&retimer->client->dev, "Got unsupported SID: 0x%x\n",
				state->alt->svid);
			return -EOPNOTSUPP;
		}
	} else {
		switch (state->mode) {
		/* SAFE can be transient or point to an actual disconnect */
		case TYPEC_STATE_SAFE:
		/* USB2 pins don't even go through this chip */
		case TYPEC_MODE_USB2:
			reset = true;
			break;
		case TYPEC_STATE_USB:
		case TYPEC_MODE_USB3:
			cfg0 |= CONN_STATUS_0_USB_3_1_CONNECTED;
			break;
		case TYPEC_MODE_USB4:
			eudo_data = state->data;

			cfg2 |= CONN_STATUS_2_USB4_CONNECTED;

			if (FIELD_GET(EUDO_CABLE_TYPE_MASK, eudo_data->eudo) != EUDO_CABLE_TYPE_PASSIVE)
				cfg0 |= CONN_STATUS_0_ACTIVE_CABLE;
			break;
		default:
			dev_err(&retimer->client->dev, "Got unsupported mode: %lu\n",
				state->mode);
			return -EOPNOTSUPP;
		}
	}

	return ps883x_configure(retimer, cfg0, cfg1, cfg2, reset);
}

static int ps883x_sw_set(struct typec_switch_dev *sw,
			 enum typec_orientation orientation)
{
	struct ps883x_retimer *retimer = typec_switch_get_drvdata(sw);
	int ret = 0;

	ret = typec_switch_set(retimer->typec_switch, orientation);
	if (ret)
		return ret;

	guard(mutex)(&retimer->lock);

	if (retimer->orientation != orientation) {
		retimer->orientation = orientation;

		/*
		 * Orientation notifications usually come prior to mode switch
		 * events. If the retimer is already in reset, we still want to
		 * cache the new orientation value for the subsequent ps883x_set().
		 */
		if (retimer->in_reset)
			return 0;

		if (orientation == TYPEC_ORIENTATION_REVERSE)
			ret = regmap_set_bits(retimer->regmap, REG_USB_PORT_CONN_STATUS_0,
					 CONN_STATUS_0_ORIENTATION_REVERSED);
		else
			ret = regmap_clear_bits(retimer->regmap, REG_USB_PORT_CONN_STATUS_0,
					 CONN_STATUS_0_ORIENTATION_REVERSED);

		if (ret)
			dev_err(&retimer->client->dev, "failed to set orientation: %d\n", ret);
	}

	return ret;
}

static int ps883x_retimer_set(struct typec_retimer *rtmr,
			      struct typec_retimer_state *state)
{
	struct ps883x_retimer *retimer = typec_retimer_get_drvdata(rtmr);
	struct typec_mux_state mux_state;
	int ret = 0;

	mutex_lock(&retimer->lock);
	ret = ps883x_set(retimer, state);
	mutex_unlock(&retimer->lock);

	if (ret)
		return ret;

	mux_state.alt = state->alt;
	mux_state.data = state->data;
	mux_state.mode = state->mode;

	return typec_mux_set(retimer->typec_mux, &mux_state);
}

static int ps883x_get_vregs(struct ps883x_retimer *retimer)
{
	struct device *dev = &retimer->client->dev;

	retimer->vdd_supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(retimer->vdd_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vdd_supply),
				     "failed to get VDD\n");

	retimer->vdd33_supply = devm_regulator_get(dev, "vdd33");
	if (IS_ERR(retimer->vdd33_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vdd33_supply),
				     "failed to get VDD 3.3V\n");

	retimer->vdd33_cap_supply = devm_regulator_get(dev, "vdd33-cap");
	if (IS_ERR(retimer->vdd33_cap_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vdd33_cap_supply),
				     "failed to get VDD CAP 3.3V\n");

	retimer->vddat_supply = devm_regulator_get(dev, "vddat");
	if (IS_ERR(retimer->vddat_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vddat_supply),
				     "failed to get VDD AT\n");

	retimer->vddar_supply = devm_regulator_get(dev, "vddar");
	if (IS_ERR(retimer->vddar_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vddar_supply),
				     "failed to get VDD AR\n");

	retimer->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(retimer->vddio_supply))
		return dev_err_probe(dev, PTR_ERR(retimer->vddio_supply),
				     "failed to get VDD IO\n");

	return 0;
}

static const struct regmap_config ps883x_retimer_regmap = {
	.max_register = 0x1f,
	.reg_bits = 8,
	.val_bits = 8,
};

static int ps883x_retimer_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct typec_switch_desc sw_desc = { };
	struct typec_retimer_desc rtmr_desc = { };
	struct ps883x_retimer *retimer;
	unsigned int val;
	bool already_configured;
	int ret;

	retimer = devm_kzalloc(dev, sizeof(*retimer), GFP_KERNEL);
	if (!retimer)
		return -ENOMEM;

	retimer->client = client;
	i2c_set_clientdata(client, retimer);

	mutex_init(&retimer->lock);

	retimer->regmap = devm_regmap_init_i2c(client, &ps883x_retimer_regmap);
	if (IS_ERR(retimer->regmap))
		return dev_err_probe(dev, PTR_ERR(retimer->regmap),
				     "failed to allocate register map\n");

	ret = ps883x_get_vregs(retimer);
	if (ret)
		return ret;

	retimer->xo_clk = devm_clk_get(dev, NULL);
	if (IS_ERR(retimer->xo_clk))
		return dev_err_probe(dev, PTR_ERR(retimer->xo_clk),
				     "failed to get xo clock\n");

	retimer->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(retimer->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(retimer->reset_gpio),
				     "failed to get reset gpio\n");

	retimer->typec_switch = typec_switch_get(dev);
	if (IS_ERR(retimer->typec_switch))
		return dev_err_probe(dev, PTR_ERR(retimer->typec_switch),
				     "failed to acquire orientation-switch\n");

	retimer->typec_mux = typec_mux_get(dev);
	if (IS_ERR(retimer->typec_mux)) {
		ret = dev_err_probe(dev, PTR_ERR(retimer->typec_mux),
				    "failed to acquire mode-mux\n");
		goto err_switch_put;
	}

#ifdef CONFIG_DRM_DP_AUX_BUS
	ret = drm_aux_bridge_register(dev);
	if (ret)
		goto err_mux_put;
#endif
	ret = ps883x_enable_vregs(retimer);
	if (ret)
		goto err_mux_put;

	ret = clk_prepare_enable(retimer->xo_clk);
	if (ret) {
		dev_err(dev, "failed to enable XO: %d\n", ret);
		goto err_vregs_disable;
	}

	already_configured = regmap_test_bits(retimer->regmap, REG_USB_PORT_CONN_STATUS_0,
					      CONN_STATUS_0_CONNECTION_PRESENT) == 1;

	/* skip resetting if already configured */
	if (already_configured) {
		gpiod_direction_output(retimer->reset_gpio, 0);
	} else {
		gpiod_direction_output(retimer->reset_gpio, 1);

		/* VDD IO supply enable to reset release delay */
		usleep_range(4000, 14000);

		gpiod_set_value(retimer->reset_gpio, 0);

		/* firmware initialization delay */
		msleep(60);
	}

	if (!already_configured) {
		/* make sure device is accessible */
		ret = regmap_read(retimer->regmap, REG_USB_PORT_CONN_STATUS_0,
				  &val);
		if (ret) {
			dev_err(dev, "failed to read conn_status_0: %d\n", ret);
			if (ret == -ENXIO)
				ret = -EIO;
			goto err_clk_disable;
		}
	}

	/* Keep the retimer in reset until a Type-C notification comes */
	ps883x_reset(retimer);

	sw_desc.drvdata = retimer;
	sw_desc.fwnode = dev_fwnode(dev);
	sw_desc.set = ps883x_sw_set;

	retimer->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(retimer->sw)) {
		ret = PTR_ERR(retimer->sw);
		dev_err(dev, "failed to register typec switch: %d\n", ret);
		goto err_mux_put;
	}

	rtmr_desc.drvdata = retimer;
	rtmr_desc.fwnode = dev_fwnode(dev);
	rtmr_desc.set = ps883x_retimer_set;

	retimer->retimer = typec_retimer_register(dev, &rtmr_desc);
	if (IS_ERR(retimer->retimer)) {
		ret = PTR_ERR(retimer->retimer);
		dev_err(dev, "failed to register typec retimer: %d\n", ret);
		goto err_switch_unregister;
	}

	ret = sysfs_create_group(&dev->kobj, &ps883x_fw_attr_group);
	if (ret) {
		dev_err(dev, "failed to add firmware update sysfs group: %d\n",
			ret);
		goto err_retimer_unregister;
	}

	return 0;

err_retimer_unregister:
	typec_retimer_unregister(retimer->retimer);
err_switch_unregister:
	typec_switch_unregister(retimer->sw);
	goto err_mux_put;
err_clk_disable:
	ps883x_power_down(retimer, true);
	goto err_mux_put;
err_vregs_disable:
	ps883x_power_down(retimer, false);
err_mux_put:
	typec_mux_put(retimer->typec_mux);
err_switch_put:
	typec_switch_put(retimer->typec_switch);

	return ret;
}

static void ps883x_retimer_remove(struct i2c_client *client)
{
	struct ps883x_retimer *retimer = i2c_get_clientdata(client);

	sysfs_remove_group(&client->dev.kobj, &ps883x_fw_attr_group);

	typec_retimer_unregister(retimer->retimer);
	typec_switch_unregister(retimer->sw);

	ps883x_reset(retimer);

	typec_mux_put(retimer->typec_mux);
	typec_switch_put(retimer->typec_switch);
}

static const struct of_device_id ps883x_retimer_of_table[] = {
	{ .compatible = "parade,ps8830" },
	{ .compatible = "parade,ps8833" },
	{ }
};
MODULE_DEVICE_TABLE(of, ps883x_retimer_of_table);

static struct i2c_driver ps883x_retimer_driver = {
	.driver = {
		.name = "ps883x_retimer",
		.of_match_table = ps883x_retimer_of_table,
	},
	.probe		= ps883x_retimer_probe,
	.remove		= ps883x_retimer_remove,
};

module_i2c_driver(ps883x_retimer_driver);

MODULE_DESCRIPTION("Parade ps883x Type-C Retimer driver");
MODULE_LICENSE("GPL");
