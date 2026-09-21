/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __FL7112_H_
#define __FL7112_H_

#define FL7112_FIRMWARE_INTERNAL_REGISTER_0X5000        0x5000
#define FL7112_FIRMWARE_INTERNAL_REGISTER_0X9000        0x9000
#define FL7112_FIRMWARE_INTERNAL_REGISTER_0X9001        0x9001
#define FL7112_FIRMWARE_INTERNAL_REGISTER_0X9003        0x9003
#define FL7112_FIRMWARE_INTERNAL_REGISTER_0X9010        0x9010
#define FL7112_FIRMWARE_INTERNAL_REGISTER_0X9800        0x9800
#define FL7112_FIRMWARE_INTERNAL_REGISTER_0X9B00        0x9B00
#define FL7112_FIRMWARE_INTERNAL_REGISTER_0X9BF8        0x9BF8
#define FL7112_FIRMWARE_INTERNAL_REGISTER_0XA002        0xA002
#define FL7112_FIRMWARE_INTERNAL_REGISTER_0XA003        0xA003
#define FL7112_MTP_BLOCK_SIZE                           0x1000
#define FL7112_MTP_MAX_NUMBER_OF_BLOCKS                 6

#define FL7112_MTP_MAX_SIZE_OF_FIRMWARE_BLOCK \
	(FL7112_MTP_MAX_NUMBER_OF_BLOCKS * FL7112_MTP_BLOCK_SIZE)

#define FL7112_MTP_MAX_SIZE_OF_INFORMATION_BLOCK        0x300

#define FL7112_FIRMWARE_IMAGE_SIZE \
	(FL7112_MTP_MAX_SIZE_OF_FIRMWARE_BLOCK + FL7112_MTP_MAX_SIZE_OF_INFORMATION_BLOCK)

#define FL7112_NUMBER_OF_REGISTER_COMMAND \
	((FL7112_MTP_MAX_SIZE_OF_INFORMATION_BLOCK - 24) / 4)

#define FL7112_REGISTER_COMMAND_END                     0xFFFFFFFF
#define FL7112_REGISTER_SIGNATURE                       0x96C32D0F
#define FL7112_FIRMWARE_OFFSET_0X1000                   0x1000
#define FL7112_I2C_SLAVE_ADDRESS                        0x74
#define FL7112_MTP_WRITE_RECOVERY_MAX                   3
#define FL7112_MTP_CHECK_STATUS_RETRY_MAX               20
#define FL7112_MTP_POWER_CURRENT_TABLE_SIZE             8
#define FL7112_MTP_RECOVERY                             1
#define BIT_SET(data, bit) (data |= (1 << bit))
#define IS_BIT_SET(data, bit) (data & (1 << bit))
#define EXIT_FALSE(status)  \
	do {  \
		if (!status) {           \
			pr_err("%s error line:%d\n", __func__, __LINE__); \
		}  \
	} while (0)

union fl7112_information_block {
	struct {
		u32 register_signature;
		u32 register_size_in_dword;
		u32 register_crc32;
		u32 firmware_signature;
		u32 firmware_size_in_dword;
		u32 firmware_crc32;
		u32 command[FL7112_NUMBER_OF_REGISTER_COMMAND];
	} map;
	u32 data[FL7112_NUMBER_OF_REGISTER_COMMAND + 6];
};

union dword_mapping {
	struct {
		u32 byte0:8;
		u32 byte1:8;
		u32 byte2:8;
		u32 byte3:8;
	} map;
	u8 byte_data[4];
	u16 word_data[2];
	u32 value;
};

union chartofloat {
	float f_value;
	u8 c_value[4];
};

enum fw_upgrade_status {
	UPDATE_SUCCESS = 0,
	UPDATE_RUNNING = 1,
	UPDATE_FAILED = 2,
	UPDATE_UNKNOWN = 3,
	UPDATE_SUBFAILED = 4,
};

struct fl7112 {
	struct device *dev;
	u8 i2c_addr;
	u8 i2c_wbuf[FL7112_MTP_MAX_SIZE_OF_FIRMWARE_BLOCK];
	u8 i2c_rbuf[FL7112_MTP_MAX_SIZE_OF_FIRMWARE_BLOCK];
	struct i2c_client *i2c_client;
	enum fw_upgrade_status fw_status;
	uint32_t version;
	struct workqueue_struct *wq;  /* upgrade queue */
	struct work_struct wk;        /* upgrade work */
	u32 reset_pin;   /* reset pin */
	u32 pwr_1v2;
	u32 pwr_3v3;
};

static int fl7112_read(struct fl7112 *pdata, int reg, char *buf, u32 size);
static bool fl7112_mtpread_firmware(struct fl7112 *pdata,
					int address,
					u8 *data_buffer,
					int data_buffer_length);
static bool fl7112_mtpwrite_firmware(struct fl7112 *pdata,
					int address,
					const u8 *data_buffer,
					int data_buffer_length,
					bool *is_error,
					bool *is_busy);
static void fl7112_mtppowermodedisable(struct fl7112 *pdata);
static bool fl7112_mtpwriteunlock(struct fl7112 *pdata);
static void fl7112_mtpwritelock(struct fl7112 *pdata);
static bool fl7112_mtpcheckstatus(struct fl7112 *pdata, bool *is_error, bool *is_busy);
static void fl7112_reset(struct fl7112 *pdata);

#endif /* __FL7112_H_ */
