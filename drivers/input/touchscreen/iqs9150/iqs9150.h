/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#ifndef __IQS9150_H__
#define __IQS9150_H__

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>

#define FINGER_DATA_OFFSET	64

/*add by hqm  start */
#define IQS9150_DATA_ADDR	0x101C
#define FTS_PINCTRL_EN	1
#define IQS_GPIO_FREE(pin)                 gpio_free(pin)
#define IQS_DEBUG_EN	1
#define IQS9150_REG_ID	0x1000
#define MAX_TOUCH_REPORT	7
#define CONFIG_SETTINGS_1_SUSPEND          0x00
#define CONFIG_SETTINGS_1_RESUME	0x04
#define EVENT_BUFFER_COUNT                 64
#define INIT_BUFFER_COUNT                  64
#define RESUME_BUFFER_COUNT                10
#define SUSPEND_BUFFER_COUNT               10
#define TOUCH_MAX_X                        1440
#define TOUCH_MAX_Y                        2880
#define I2C_RETRY_NUMBER                   3
#define XFER_COUNT                         2
/* add for log debug control */
#ifdef IQS_DEBUG_EN
#define IQS_DEBUG(fmt, args...)	   pr_info("[IQS-DBG][%s:%d] "fmt"\n", __func__, __LINE__, ##args)
#endif
#define IQS_FUNC_ENTER()                   pr_err("[IQS-INF][%s:%d]  Enter\n", __func__, __LINE__)
#define IQS_FUNC_EXIT()		   pr_err("[IQS-INF][%s:%d]   Exit\n", __func__, __LINE__)
#define IQS_INFO(fmt, args...) \
	pr_err("[IQS-INF][%s:%d] " fmt "\n", __func__, __LINE__, ##args)
#define IQS_ERROR(fmt, args...)	   pr_err("[IQS-ERR][%s:%d] "fmt"\n", __func__, __LINE__, ##args)
/* add  by hqm end */

#define X_RESOLUTION 25
#define Y_RESOLUTION 22

#define NO_FINGER_TOUCH 0

#define IQS9150_PRODUCT_ID 0x076A
#define IQS9150_VENDOR_ID 0x3591
#define DATA_POS_BEGIN 8
#define FINGER_DATA_LEN 8

#define NO_REFERENCE_DATA_I0 0
#define NO_REFERENCE_DATA_I1 24
#define NO_REFERENCE_DATA_I2 125
#define NO_REFERENCE_DATA_I3 150
#define NO_REFERENCE_DATA_I4 275
#define NO_REFERENCE_DATA_I5 299

#define READ_BYTES_NUM_EACH_COUNT 128
#define PER_REGISTER_OCCUPY_BYTES 2
#define CONSISTENCY_BUFFER_MAX_LEN 1024
#define IQS_REFERENCE_DATA_MIN 150
#define IQS_REFERENCE_DATA_MAX 250
#define IQS_DELTA_DATA_MAX_UNSIGNED 24
#define IQS_DELTA_DATA_MIN_UNSIGNED 65512
#define IQS_ATI_COMPENSATION_DIVIDER_DATA_MIN 0
#define IQS_ATI_COMPENSATION_DIVIDER_DATA_MAX 31
#define IQS_ATI_COMPENSATION_DATA_MIN 150
#define IQS_ATI_COMPENSATION_DATA_MAX 850
#define IQS_ATI_COMPENSATION_OCCUPY_BIT 10
#define IQS_ATI_COMPENSATION_REGISTER_MSB 0xD0
#define IQS_ATI_COMPENSATION_REGISTER_LSB 0x80
#define IQS_DELTA_REGISTER_MSB 0xC0
#define IQS_DELTA_REGISTER_LSB 0x80
#define IQS_REFERENCE_REGISTER_MSB 0xB0
#define IQS_REFERENCE_REGISTER_LSB 0x80
#define PRODUCT_NUMBER_REGISTER_ADDRESS_MSB 0x10
#define PRODUCT_NUMBER_REGISTER_ADDRESS_LSB 0x00
#define INFO_FLAG_REGISTER_ADDRESS_MSB 0x10
#define INFO_FLAG_REGISTER_ADDRESS_LSB 0x20
#define PRODUCT_NUMBER_DATA_MSB 0x07
#define PRODUCT_NUMBER_DATA_LSB 0x6A
#define FINGER_DATA_REGISTER_ADDRESS_MSB 0x10
#define FINGER_DATA_REGISTER_ADDRESS_LSB 0x1C
#define CHIP_RESET_OCCURRED 0X80
#define ATI_ERROR 0x08
#define ATI_ACTIVE 0x10

#define DELAY_BETWEEN_GPIO_HIGH_LOW 2
#define DELAY_CALL_PARSE_PANEL_WORK_WHEN_FAIL 500
#define DELAY_WAIT_FOR_TWO_RDY 1000
#define DELAY_CALL_PARSE_PANEL_WORK_IN_PROBE 3000
#define DELAY_US_AFTER_RW_REG_BEGIN 200
#define DELAY_US_AFTER_RW_REG_END 300
#define DELAY_AFTER_RESET_DONE 100
#define DELAY_TO_CHECK_PRODUCT_NUMBER 5000

#define GPIO_OUTPUT_HIGH 1
#define GPIO_OUTPUT_LOW 0

enum iqs9150_state_enum {
	IQS_COMMS_WINDOW = 0,
	IQS_RESET_INIT,
	IQS_NORMAL,
	IQS_PRODUCTION_TESTING
};

enum iqs9150_test_id_enum {
	IQS_CHIP_ID = 0,
	IQS_ATI_STATE,
	IQS_REFERENCE,
	IQS_DELTA,
	IQS_ATI_COMPENSATION
};

enum iqs9150_error_code_enum {
	IQS_TEST_PASS = 0,
	IQS_CHIP_ID_PASS,
	IQS_CHIP_ID_ERROR,
	IQS_ATI_PASS,
	IQS_ATI_ERROR,
	IQS_REFERENCE_PASS,
	IQS_REFERENCE_ERROR,
	IQS_DELTA_PASS,
	IQS_DELTA_ERROR,
	IQS_ATI_COMPENSATION_PASS,
	IQS_ATI_COMPENSATION_ERROR
};

struct iqs9150_ts_data;

struct iqs9150_ts_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct device *dev;
	struct kobject kobj;
	const struct iqs9150_chip_data *chip;
	struct mutex lock;
	int irq;
	int irq_gpio;
	u32 irq_gpio_flags;
	int reset_gpio;
	u32 reset_gpio_flags;
	int iovdd_gpio;
	u32 iovdd_gpio_flags;
	u8 *cmd_buf;
	uint8_t max_touch_num;
	int suspend_state;

	/* Power management related */
	void *notifier_cookie;
	struct notifier_block drm_notifier;
	struct work_struct fb_notify_work;
	struct delayed_work panel_check_work;

	/* Status flags */
	bool initialized;
	bool input_dev_registered;
	bool irq_requested;
	bool panel_registered;
	bool workqueue_created;
	bool resume_in_workqueue;
	bool workqueue_initialized;

	/* Debug counters */
	unsigned int event_count;
	unsigned int error_count;

	struct delayed_work reg_read_dwork;

	struct drm_panel *active_panel;
	struct workqueue_struct *workqueue;
	int panel_unblank;
	int prevent_accidental_touch;
	bool get_rdy_fail;
	uint8_t production_testing_flag;
	uint8_t iqs9150_state;
	atomic_t is_process_event_now;
	int last_touch_num;
	uint8_t test_id;

	struct mutex i2c_mutex;

	#if FTS_PINCTRL_EN
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_active;
	struct pinctrl_state *pins_suspend;
	struct pinctrl_state *pins_release;
	#endif

};

/* ALP ATI Compensation */
/* Memory Map Position 0x115C - 0x1175 */
#define ALP_COMPENSATION_RX0_0                   0x71
#define ALP_COMPENSATION_RX0_1                   0x0E
#define ALP_COMPENSATION_RX1_0                   0x5C
#define ALP_COMPENSATION_RX1_1                   0x0E
#define ALP_COMPENSATION_RX2_0                   0x78
#define ALP_COMPENSATION_RX2_1                   0x0E
#define ALP_COMPENSATION_RX3_0                   0x1D
#define ALP_COMPENSATION_RX3_1                   0x13
#define ALP_COMPENSATION_RX4_0                   0xFF
#define ALP_COMPENSATION_RX4_1                   0x7F
#define ALP_COMPENSATION_RX5_0                   0xFF
#define ALP_COMPENSATION_RX5_1                   0x7F
#define ALP_COMPENSATION_RX6_0                   0xFF
#define ALP_COMPENSATION_RX6_1                   0x03
#define ALP_COMPENSATION_RX7_0                   0x83
#define ALP_COMPENSATION_RX7_1                   0x0E
#define ALP_COMPENSATION_RX8_0                   0x61
#define ALP_COMPENSATION_RX8_1                   0x0E
#define ALP_COMPENSATION_RX9_0                   0xFF
#define ALP_COMPENSATION_RX9_1                   0x7F
#define ALP_COMPENSATION_RX10_0                  0xFF
#define ALP_COMPENSATION_RX10_1                  0x7F
#define ALP_COMPENSATION_RX11_0                  0xFF
#define ALP_COMPENSATION_RX11_1                  0x03
#define ALP_COMPENSATION_RX12_0                  0xFF
#define ALP_COMPENSATION_RX12_1                  0x03

/* I2C Slave Address */
/* Memory Map Position 0x1176 - 0x1177 */
#define I2C_KEY                                  0xFF
#define I2C_SLAVE_ADR                            0xFF

/* Settings Version Numbers */
/* Memory Map Position 0x1178 - 0x1179 */
#define MINOR_VERSION                            0x01
#define MAJOR_VERSION                            0x00

/* ATI Multipliers / Dividers */
/* Memory Map Position 0x117A - 0x1195 */
#define TP_ATI_MULTDIV_L                         0xC1
#define TP_ATI_MULTDIV_H                         0x44
#define ALP_ATI_COARSE_RX0_L                     0x9B
#define ALP_ATI_COARSE_RX0_H                     0x58
#define ALP_ATI_COARSE_RX1_L                     0x9B
#define ALP_ATI_COARSE_RX1_H                     0x5A
#define ALP_ATI_COARSE_RX2_L                     0x9B
#define ALP_ATI_COARSE_RX2_H                     0x58
#define ALP_ATI_COARSE_RX3_L                     0x9B
#define ALP_ATI_COARSE_RX3_H                     0x5A
#define ALP_ATI_COARSE_RX4_L                     0x21
#define ALP_ATI_COARSE_RX4_H                     0x41
#define ALP_ATI_COARSE_RX5_L                     0x21
#define ALP_ATI_COARSE_RX5_H                     0x41
#define ALP_ATI_COARSE_RX6_L                     0x21
#define ALP_ATI_COARSE_RX6_H                     0x41
#define ALP_ATI_COARSE_RX7_L                     0x8E
#define ALP_ATI_COARSE_RX7_H                     0x58
#define ALP_ATI_COARSE_RX8_L                     0x8E
#define ALP_ATI_COARSE_RX8_H                     0x5A
#define ALP_ATI_COARSE_RX9_L                     0x21
#define ALP_ATI_COARSE_RX9_H                     0x41
#define ALP_ATI_COARSE_RX10_L                    0x21
#define ALP_ATI_COARSE_RX10_H                    0x41
#define ALP_ATI_COARSE_RX11_L                    0x21
#define ALP_ATI_COARSE_RX11_H                    0x41
#define ALP_ATI_COARSE_RX12_L                    0x21
#define ALP_ATI_COARSE_RX12_H                    0x41

/* ATI Settings */
/* Memory Map Position 0x1196 - 0x11A1 */
#define TP_ATI_TARGET_0                          0xC8
#define TP_ATI_TARGET_1                          0x00
#define ALP_ATI_TARGET_0                         0x2C
#define ALP_ATI_TARGET_1                         0x01
#define ALP_BASE_TARGET_0                        0x32
#define ALP_BASE_TARGET_1                        0x00
#define TP_NEG_DELTA_REATI_0                     0x32
#define TP_NEG_DELTA_REATI_1                     0x00
#define TP_POS_DELTA_REATI_0                     0xE8
#define TP_POS_DELTA_REATI_1                     0x03
#define TP_REF_DRIFT_LIMIT                       0x32
#define ALP_LTA_DRIFT_LIMIT                      0x14

/* Sampling Periods and Timing */
/* Memory Map Position 0x11A2 - 0x11BB */
#define ACTIVE_MODE_SAMPLING_PERIOD_0            0x0A
#define ACTIVE_MODE_SAMPLING_PERIOD_1            0x00
#define IDLE_TOUCH_MODE_SAMPLING_PERIOD_0        0x60
#define IDLE_TOUCH_MODE_SAMPLING_PERIOD_1        0x00
#define IDLE_MODE_SAMPLING_PERIOD_0              0x30
#define IDLE_MODE_SAMPLING_PERIOD_1              0x00
#define LP1_MODE_SAMPLING_PERIOD_0               0x30
#define LP1_MODE_SAMPLING_PERIOD_1               0x00
#define LP2_MODE_SAMPLING_PERIOD_0               0x60
#define LP2_MODE_SAMPLING_PERIOD_1               0x00
#define STATIONARY_TOUCH_TIMEOUT_0               0x08
#define STATIONARY_TOUCH_TIMEOUT_1               0x00
#define IDLE_TOUCH_MODE_TIMEOUT_0                0x3C
#define IDLE_TOUCH_MODE_TIMEOUT_1                0x00
#define IDLE_MODE_TIMEOUT_0                      0x08
#define IDLE_MODE_TIMEOUT_1                      0x00
#define LP1_MODE_TIMEOUT_0                       0x00
#define LP1_MODE_TIMEOUT_1                       0x00
#define ACTIVE_MODE_TIMEOUT_0                    0xDC
#define ACTIVE_MODE_TIMEOUT_1                    0x05
#define REATI_RETRY_TIME                         0x03
#define REF_UPDATE_TIME                          0x04
#define I2C_TIMEOUT_0                            0x64
#define I2C_TIMEOUT_1                            0x00
#define SNAP_TIMEOUT                             0x10
#define OPEN_TIMING                              0x00

/* System Settings */
/* Memory Map Position 0x11BC - 0x11C1 */
#define SYSTEM_CONTROL_0                         0x50
#define SYSTEM_CONTROL_1                         0x00
#define CONFIG_SETTINGS_0                        0x6F
#define CONFIG_SETTINGS_1                        0x27
#define OTHER_SETTINGS_0                         0x30
#define OTHER_SETTINGS_1                         0x00

/* ALP Settings */
/* Memory Map Position 0x11C2 - 0x11CB */
#define ALP_SETUP_0                              0x0F
#define ALP_SETUP_1                              0xE1
#define ALP_SETUP_2                              0x11
#define ALP_SETUP_3                              0x70
#define ALP_TX_ENABLE_0                          0x00
#define ALP_TX_ENABLE_1                          0x1E
#define ALP_TX_ENABLE_2                          0xE0
#define ALP_TX_ENABLE_3                          0xC3
#define ALP_TX_ENABLE_4                          0xFD
#define ALP_TX_ENABLE_5                          0x0F

/* Thresholds and Debounce Settings */
/* Memory Map Position 0x11CC - 0x11D3 */
#define TRACKPAD_TOUCH_SET_THRESHOLD             0x30
#define TRACKPAD_TOUCH_CLEAR_THRESHOLD           0x2C
#define ALP_THRESHOLD                            0x08
#define ALP_AUTOPROX_THRESHOLD                   0x08
#define ALP_SET_DEBOUNCE                         0x01
#define ALP_CLEAR_DEBOUNCE                       0x01
#define SNAP_SET_THRESHOLD                       0x32
#define SNAP_CLEAR_THRESHOLD                     0x32

/* ALP count and LTA Betas */
/* Memory Map Position 0x11D4 - 0x11D7 */
#define ALP_COUNT_BETA_LP1                       0x46
#define ALP_LTA_BETA_LP1                         0x04
#define ALP_COUNT_BETA_LP2                       0xB4
#define ALP_LTA_BETA_LP2                         0x64

/* Hardware Settings */
/* Memory Map Position 0x11D8 - 0x11E1 */
#define TP_FRAC                                  0x28
#define TP_PERIOD1                               0x02
#define TP_PERIOD2                               0x02
#define ALP_FRAC                                 0x28
#define ALP_PERIOD1                              0x02
#define ALP_PERIOD2                              0x02
#define TRACKPAD_HARDWARE_SETTINGS_0             0x40
#define TRACKPAD_HARDWARE_SETTINGS_1             0x43
#define ALP_HARDWARE_SETTINGS_0                  0x40
#define ALP_HARDWARE_SETTINGS_1                  0x4B

/* Trackpad Settings */
/* Memory Map Position 0x11E2 - 0x11F5 */
#define TRACKPAD_SETTINGS_0_0                    0x6C
#define TRACKPAD_SETTINGS_0_1                    0x19
#define TRACKPAD_SETTINGS_1_0                    0x0C
#define TRACKPAD_SETTINGS_1_1                    0x07
#define X_RESOLUTION_0                           0xA0
#define X_RESOLUTION_1                           0x05
#define Y_RESOLUTION_0                           0x40
#define Y_RESOLUTION_1                           0x0B
#define XY_DYNAMIC_FILTER_BOTTOM_SPEED_0         0x06
#define XY_DYNAMIC_FILTER_BOTTOM_SPEED_1         0x00
#define XY_DYNAMIC_FILTER_TOP_SPEED_0            0x7C
#define XY_DYNAMIC_FILTER_TOP_SPEED_1            0x00
#define XY_DYNAMIC_FILTER_BOTTOM_BETA            0x07
#define XY_DYNAMIC_FILTER_STATIC_FILTER_BETA     0x80
#define STATIONARY_TOUCH_MOV_THRESHOLD           0x14
#define FINGER_SPLIT_FACTOR                      0x06
#define X_TRIM_VALUE                             0x14
#define Y_TRIM_VALUE                             0x14
#define JITTER_FILTER_DELTA                      0x0A
#define FINGER_CONFIDENCE_THRESHOLD              0x14

/* Gesture Settings */
/* Memory Map Position 0x11F6 - 0x1217 */
#define GESTURE_ENABLE_0                         0x1F
#define GESTURE_ENABLE_1                         0xFF
#define GESTURE_ENABLE_2F_0                      0xFF
#define GESTURE_ENABLE_2F_1                      0x00
#define TAP_TOUCH_TIME_0                         0xFA
#define TAP_TOUCH_TIME_1                         0x00
#define TAP_WAIT_TIME_0                          0x2C
#define TAP_WAIT_TIME_1                          0x01
#define TAP_DISTANCE_0                           0xA0
#define TAP_DISTANCE_1                           0x00
#define HOLD_TIME_0                              0xE8
#define HOLD_TIME_1                              0x03
#define SWIPE_TIME_0                             0xF4
#define SWIPE_TIME_1                             0x01
#define SWIPE_X_DISTANCE_0                       0xE0
#define SWIPE_X_DISTANCE_1                       0x01
#define SWIPE_Y_DISTANCE_0                       0xE0
#define SWIPE_Y_DISTANCE_1                       0x01
#define SWIPE_X_CONS_DIST_0                      0xD0
#define SWIPE_X_CONS_DIST_1                      0x07
#define SWIPE_Y_CONS_DIST_0                      0x25
#define SWIPE_Y_CONS_DIST_1                      0x25
#define SWIPE_ANGLE                              0xA0
#define SCROLL_ANGLE                             0x00
#define ZOOM_INIT_DIST_0                         0x50
#define ZOOM_INIT_DIST_1                         0x00
#define ZOOM_CONSECUTIVE_DIST_0                  0x50
#define ZOOM_CONSECUTIVE_DIST_1                  0x00
#define SCROLL_INIT_DIST_0                       0x20
#define SCROLL_INIT_DIST_1                       0x00
#define SCROLL_CONSECUTIVE_DIST_0                0x64
#define SCROLL_CONSECUTIVE_DIST_1                0x00
#define PALM_GESTURE_THRESHOLD_0                 0x20
#define PALM_GESTURE_THRESHOLD_1                 0x00

/* Rx/Tx Mapping */
/* Memory Map Position 0x1218 - 0x1245 */
#define RX_TX_MAP_0                              0x15
#define RX_TX_MAP_1                              0x09
#define RX_TX_MAP_2                              0x16
#define RX_TX_MAP_3                              0x06
#define RX_TX_MAP_4                              0x13
#define RX_TX_MAP_5                              0x07
#define RX_TX_MAP_6                              0x14
#define RX_TX_MAP_7                              0x08
#define RX_TX_MAP_8                              0x0A
#define RX_TX_MAP_9                              0x17
#define RX_TX_MAP_10                             0x0B
#define RX_TX_MAP_11                             0x0C
#define RX_TX_MAP_12                             0x01
#define RX_TX_MAP_13                             0x0D
#define RX_TX_MAP_14                             0x00
#define RX_TX_MAP_15                             0x12
#define RX_TX_MAP_16                             0x05
#define RX_TX_MAP_17                             0x11
#define RX_TX_MAP_18                             0x04
#define RX_TX_MAP_19                             0x10
#define RX_TX_MAP_20                             0x03
#define RX_TX_MAP_21                             0x0F
#define RX_TX_MAP_22                             0x02
#define RX_TX_MAP_23                             0x0E
#define RX_TX_MAP_24                             0x18
#define RX_TX_MAP_25                             0x20
#define RX_TX_MAP_26                             0x21
#define RX_TX_MAP_27                             0x23
#define RX_TX_MAP_28                             0x22
#define RX_TX_MAP_29                             0x2B
#define RX_TX_MAP_30                             0x2A
#define RX_TX_MAP_31                             0x24
#define RX_TX_MAP_32                             0x25
#define RX_TX_MAP_33                             0x26
#define RX_TX_MAP_34                             0x27
#define RX_TX_MAP_35                             0x28
#define RX_TX_MAP_36                             0x29
#define RX_TX_MAP_37                             0x20
#define RX_TX_MAP_38                             0x1B
#define RX_TX_MAP_39                             0x1C
#define RX_TX_MAP_40                             0x1A
#define RX_TX_MAP_41                             0x1E
#define RX_TX_MAP_42                             0x1D
#define RX_TX_MAP_43                             0x1F
#define RX_TX_MAP_44                             0x00
#define RX_TX_OPEN                               0x00

/* TP Channel Disables */
/* Memory Map Position 0x1246 - 0x129D */
#define TPCHANNELDISABLE_0                       0x01
#define TPCHANNELDISABLE_1                       0x00
#define TPCHANNELDISABLE_2                       0x00
#define TPCHANNELDISABLE_3                       0x01
#define TPCHANNELDISABLE_4                       0x00
#define TPCHANNELDISABLE_5                       0x00
#define TPCHANNELDISABLE_6                       0x00
#define TPCHANNELDISABLE_7                       0x00
#define TPCHANNELDISABLE_8                       0x00
#define TPCHANNELDISABLE_9                       0x00
#define TPCHANNELDISABLE_10                      0x00
#define TPCHANNELDISABLE_11                      0x00
#define TPCHANNELDISABLE_12                      0x00
#define TPCHANNELDISABLE_13                      0x00
#define TPCHANNELDISABLE_14                      0x00
#define TPCHANNELDISABLE_15                      0x00
#define TPCHANNELDISABLE_16                      0x00
#define TPCHANNELDISABLE_17                      0x00
#define TPCHANNELDISABLE_18                      0x00
#define TPCHANNELDISABLE_19                      0x00
#define TPCHANNELDISABLE_20                      0x01
#define TPCHANNELDISABLE_21                      0x00
#define TPCHANNELDISABLE_22                      0x00
#define TPCHANNELDISABLE_23                      0x00
#define TPCHANNELDISABLE_24                      0x01
#define TPCHANNELDISABLE_25                      0x00
#define TPCHANNELDISABLE_26                      0x00
#define TPCHANNELDISABLE_27                      0x00
#define TPCHANNELDISABLE_28                      0x00
#define TPCHANNELDISABLE_29                      0x00
#define TPCHANNELDISABLE_30                      0x00
#define TPCHANNELDISABLE_31                      0x00
#define TPCHANNELDISABLE_32                      0x00
#define TPCHANNELDISABLE_33                      0x00
#define TPCHANNELDISABLE_34                      0x00
#define TPCHANNELDISABLE_35                      0x00
#define TPCHANNELDISABLE_36                      0x00
#define TPCHANNELDISABLE_37                      0x00
#define TPCHANNELDISABLE_38                      0x00
#define TPCHANNELDISABLE_39                      0x00
#define TPCHANNELDISABLE_40                      0x00
#define TPCHANNELDISABLE_41                      0x00
#define TPCHANNELDISABLE_42                      0x00
#define TPCHANNELDISABLE_43                      0x00
#define TPCHANNELDISABLE_44                      0x01
#define TPCHANNELDISABLE_45                      0x00
#define TPCHANNELDISABLE_46                      0x00
#define TPCHANNELDISABLE_47                      0x01
#define TPCHANNELDISABLE_48                      0x00
#define TPCHANNELDISABLE_49                      0x00
#define TPCHANNELDISABLE_50                      0x00
#define TPCHANNELDISABLE_51                      0x00
#define TPCHANNELDISABLE_52                      0x00
#define TPCHANNELDISABLE_53                      0x00
#define TPCHANNELDISABLE_54                      0x00
#define TPCHANNELDISABLE_55                      0x00
#define TPCHANNELDISABLE_56                      0x00
#define TPCHANNELDISABLE_57                      0x00
#define TPCHANNELDISABLE_58                      0x00
#define TPCHANNELDISABLE_59                      0x00
#define TPCHANNELDISABLE_60                      0x00
#define TPCHANNELDISABLE_61                      0x00
#define TPCHANNELDISABLE_62                      0x00
#define TPCHANNELDISABLE_63                      0x00
#define TPCHANNELDISABLE_64                      0x00
#define TPCHANNELDISABLE_65                      0x00
#define TPCHANNELDISABLE_66                      0x00
#define TPCHANNELDISABLE_67                      0x00
#define TPCHANNELDISABLE_68                      0x00
#define TPCHANNELDISABLE_69                      0x00
#define TPCHANNELDISABLE_70                      0x00
#define TPCHANNELDISABLE_71                      0x00
#define TPCHANNELDISABLE_72                      0x00
#define TPCHANNELDISABLE_73                      0x00
#define TPCHANNELDISABLE_74                      0x00
#define TPCHANNELDISABLE_75                      0x00
#define TPCHANNELDISABLE_76                      0x00
#define TPCHANNELDISABLE_77                      0x00
#define TPCHANNELDISABLE_78                      0x00
#define TPCHANNELDISABLE_79                      0x00
#define TPCHANNELDISABLE_80                      0x00
#define TPCHANNELDISABLE_81                      0x00
#define TPCHANNELDISABLE_82                      0x00
#define TPCHANNELDISABLE_83                      0x00
#define TPCHANNELDISABLE_84                      0x00
#define TPCHANNELDISABLE_85                      0x00
#define TPCHANNELDISABLE_86                      0x00
#define TPCHANNELDISABLE_87                      0x00

/* TP Snap Enable */
/* Memory Map Position 0x129E - 0x12F5 */
#define SNAPCHANNELENABLE_0                      0x00
#define SNAPCHANNELENABLE_1                      0x00
#define SNAPCHANNELENABLE_2                      0x00
#define SNAPCHANNELENABLE_3                      0x00
#define SNAPCHANNELENABLE_4                      0x00
#define SNAPCHANNELENABLE_5                      0x00
#define SNAPCHANNELENABLE_6                      0x00
#define SNAPCHANNELENABLE_7                      0x00
#define SNAPCHANNELENABLE_8                      0x00
#define SNAPCHANNELENABLE_9                      0x00
#define SNAPCHANNELENABLE_10                     0x00
#define SNAPCHANNELENABLE_11                     0x00
#define SNAPCHANNELENABLE_12                     0x00
#define SNAPCHANNELENABLE_13                     0x00
#define SNAPCHANNELENABLE_14                     0x00
#define SNAPCHANNELENABLE_15                     0x00
#define SNAPCHANNELENABLE_16                     0x00
#define SNAPCHANNELENABLE_17                     0x00
#define SNAPCHANNELENABLE_18                     0x00
#define SNAPCHANNELENABLE_19                     0x00
#define SNAPCHANNELENABLE_20                     0x00
#define SNAPCHANNELENABLE_21                     0x00
#define SNAPCHANNELENABLE_22                     0x00
#define SNAPCHANNELENABLE_23                     0x00
#define SNAPCHANNELENABLE_24                     0x00
#define SNAPCHANNELENABLE_25                     0x00
#define SNAPCHANNELENABLE_26                     0x00
#define SNAPCHANNELENABLE_27                     0x00
#define SNAPCHANNELENABLE_28                     0x00
#define SNAPCHANNELENABLE_29                     0x00
#define SNAPCHANNELENABLE_30                     0x00
#define SNAPCHANNELENABLE_31                     0x00
#define SNAPCHANNELENABLE_32                     0x00
#define SNAPCHANNELENABLE_33                     0x00
#define SNAPCHANNELENABLE_34                     0x00
#define SNAPCHANNELENABLE_35                     0x00
#define SNAPCHANNELENABLE_36                     0x00
#define SNAPCHANNELENABLE_37                     0x00
#define SNAPCHANNELENABLE_38                     0x00
#define SNAPCHANNELENABLE_39                     0x00
#define SNAPCHANNELENABLE_40                     0x00
#define SNAPCHANNELENABLE_41                     0x00
#define SNAPCHANNELENABLE_42                     0x00
#define SNAPCHANNELENABLE_43                     0x00
#define SNAPCHANNELENABLE_44                     0x00
#define SNAPCHANNELENABLE_45                     0x00
#define SNAPCHANNELENABLE_46                     0x00
#define SNAPCHANNELENABLE_47                     0x00
#define SNAPCHANNELENABLE_48                     0x00
#define SNAPCHANNELENABLE_49                     0x00
#define SNAPCHANNELENABLE_50                     0x00
#define SNAPCHANNELENABLE_51                     0x00
#define SNAPCHANNELENABLE_52                     0x00
#define SNAPCHANNELENABLE_53                     0x00
#define SNAPCHANNELENABLE_54                     0x00
#define SNAPCHANNELENABLE_55                     0x00
#define SNAPCHANNELENABLE_56                     0x00
#define SNAPCHANNELENABLE_57                     0x00
#define SNAPCHANNELENABLE_58                     0x00
#define SNAPCHANNELENABLE_59                     0x00
#define SNAPCHANNELENABLE_60                     0x00
#define SNAPCHANNELENABLE_61                     0x00
#define SNAPCHANNELENABLE_62                     0x00
#define SNAPCHANNELENABLE_63                     0x00
#define SNAPCHANNELENABLE_64                     0x00
#define SNAPCHANNELENABLE_65                     0x00
#define SNAPCHANNELENABLE_66                     0x00
#define SNAPCHANNELENABLE_67                     0x00
#define SNAPCHANNELENABLE_68                     0x00
#define SNAPCHANNELENABLE_69                     0x00
#define SNAPCHANNELENABLE_70                     0x00
#define SNAPCHANNELENABLE_71                     0x00
#define SNAPCHANNELENABLE_72                     0x00
#define SNAPCHANNELENABLE_73                     0x00
#define SNAPCHANNELENABLE_74                     0x00
#define SNAPCHANNELENABLE_75                     0x00
#define SNAPCHANNELENABLE_76                     0x00
#define SNAPCHANNELENABLE_77                     0x00
#define SNAPCHANNELENABLE_78                     0x00
#define SNAPCHANNELENABLE_79                     0x00
#define SNAPCHANNELENABLE_80                     0x00
#define SNAPCHANNELENABLE_81                     0x00
#define SNAPCHANNELENABLE_82                     0x00
#define SNAPCHANNELENABLE_83                     0x00
#define SNAPCHANNELENABLE_84                     0x00
#define SNAPCHANNELENABLE_85                     0x00
#define SNAPCHANNELENABLE_86                     0x00
#define SNAPCHANNELENABLE_87                     0x00

/* Individual Touch Threshold Adjustments */
/* Memory Map Position 0x12F6 - 0x14EF */
#define TOUCHTHRADJUST_0                         0x00
#define TOUCHTHRADJUST_1                         0x00
#define TOUCHTHRADJUST_2                         0x00
#define TOUCHTHRADJUST_3                         0x00
#define TOUCHTHRADJUST_4                         0x00
#define TOUCHTHRADJUST_5                         0x00
#define TOUCHTHRADJUST_6                         0x00
#define TOUCHTHRADJUST_7                         0x00
#define TOUCHTHRADJUST_8                         0x00
#define TOUCHTHRADJUST_9                         0x00
#define TOUCHTHRADJUST_10                        0x00
#define TOUCHTHRADJUST_11                        0x00
#define TOUCHTHRADJUST_12                        0x00
#define TOUCHTHRADJUST_13                        0x00
#define TOUCHTHRADJUST_14                        0x00
#define TOUCHTHRADJUST_15                        0x00
#define TOUCHTHRADJUST_16                        0x00
#define TOUCHTHRADJUST_17                        0x00
#define TOUCHTHRADJUST_18                        0x00
#define TOUCHTHRADJUST_19                        0x00
#define TOUCHTHRADJUST_20                        0x00
#define TOUCHTHRADJUST_21                        0x00
#define TOUCHTHRADJUST_22                        0x00
#define TOUCHTHRADJUST_23                        0x00
#define TOUCHTHRADJUST_24                        0x00
#define TOUCHTHRADJUST_25                        0x00
#define TOUCHTHRADJUST_26                        0x00
#define TOUCHTHRADJUST_27                        0x00
#define TOUCHTHRADJUST_28                        0x00
#define TOUCHTHRADJUST_29                        0x00
#define TOUCHTHRADJUST_30                        0x00
#define TOUCHTHRADJUST_31                        0x00
#define TOUCHTHRADJUST_32                        0x00
#define TOUCHTHRADJUST_33                        0x00
#define TOUCHTHRADJUST_34                        0x00
#define TOUCHTHRADJUST_35                        0x00
#define TOUCHTHRADJUST_36                        0x00
#define TOUCHTHRADJUST_37                        0x00
#define TOUCHTHRADJUST_38                        0x00
#define TOUCHTHRADJUST_39                        0x00
#define TOUCHTHRADJUST_40                        0x00
#define TOUCHTHRADJUST_41                        0x00
#define TOUCHTHRADJUST_42                        0x00
#define TOUCHTHRADJUST_43                        0x00
#define TOUCHTHRADJUST_44                        0x00
#define TOUCHTHRADJUST_45                        0x00
#define TOUCHTHRADJUST_46                        0x00
#define TOUCHTHRADJUST_47                        0x00
#define TOUCHTHRADJUST_48                        0x00
#define TOUCHTHRADJUST_49                        0x00
#define TOUCHTHRADJUST_50                        0x00
#define TOUCHTHRADJUST_51                        0x00
#define TOUCHTHRADJUST_52                        0x00
#define TOUCHTHRADJUST_53                        0x00
#define TOUCHTHRADJUST_54                        0x00
#define TOUCHTHRADJUST_55                        0x00
#define TOUCHTHRADJUST_56                        0x00
#define TOUCHTHRADJUST_57                        0x00
#define TOUCHTHRADJUST_58                        0x00
#define TOUCHTHRADJUST_59                        0x00
#define TOUCHTHRADJUST_60                        0x00
#define TOUCHTHRADJUST_61                        0x00
#define TOUCHTHRADJUST_62                        0x00
#define TOUCHTHRADJUST_63                        0x00
#define TOUCHTHRADJUST_64                        0x00
#define TOUCHTHRADJUST_65                        0x00
#define TOUCHTHRADJUST_66                        0x00
#define TOUCHTHRADJUST_67                        0x00
#define TOUCHTHRADJUST_68                        0x00
#define TOUCHTHRADJUST_69                        0x00
#define TOUCHTHRADJUST_70                        0x00
#define TOUCHTHRADJUST_71                        0x00
#define TOUCHTHRADJUST_72                        0x00
#define TOUCHTHRADJUST_73                        0x00
#define TOUCHTHRADJUST_74                        0x00
#define TOUCHTHRADJUST_75                        0x00
#define TOUCHTHRADJUST_76                        0x00
#define TOUCHTHRADJUST_77                        0x00
#define TOUCHTHRADJUST_78                        0x00
#define TOUCHTHRADJUST_79                        0x00
#define TOUCHTHRADJUST_80                        0x00
#define TOUCHTHRADJUST_81                        0x00
#define TOUCHTHRADJUST_82                        0x00
#define TOUCHTHRADJUST_83                        0x00
#define TOUCHTHRADJUST_84                        0x00
#define TOUCHTHRADJUST_85                        0x00
#define TOUCHTHRADJUST_86                        0x00
#define TOUCHTHRADJUST_87                        0x00
#define TOUCHTHRADJUST_88                        0x00
#define TOUCHTHRADJUST_89                        0x00
#define TOUCHTHRADJUST_90                        0x00
#define TOUCHTHRADJUST_91                        0x00
#define TOUCHTHRADJUST_92                        0x00
#define TOUCHTHRADJUST_93                        0x00
#define TOUCHTHRADJUST_94                        0x00
#define TOUCHTHRADJUST_95                        0x00
#define TOUCHTHRADJUST_96                        0x00
#define TOUCHTHRADJUST_97                        0x00
#define TOUCHTHRADJUST_98                        0x00
#define TOUCHTHRADJUST_99                        0x00
#define TOUCHTHRADJUST_100                       0x00
#define TOUCHTHRADJUST_101                       0x00
#define TOUCHTHRADJUST_102                       0x00
#define TOUCHTHRADJUST_103                       0x00
#define TOUCHTHRADJUST_104                       0x00
#define TOUCHTHRADJUST_105                       0x00
#define TOUCHTHRADJUST_106                       0x00
#define TOUCHTHRADJUST_107                       0x00
#define TOUCHTHRADJUST_108                       0x00
#define TOUCHTHRADJUST_109                       0x00
#define TOUCHTHRADJUST_110                       0x00
#define TOUCHTHRADJUST_111                       0x00
#define TOUCHTHRADJUST_112                       0x00
#define TOUCHTHRADJUST_113                       0x00
#define TOUCHTHRADJUST_114                       0x00
#define TOUCHTHRADJUST_115                       0x00
#define TOUCHTHRADJUST_116                       0x00
#define TOUCHTHRADJUST_117                       0x00
#define TOUCHTHRADJUST_118                       0x00
#define TOUCHTHRADJUST_119                       0x00
#define TOUCHTHRADJUST_120                       0x00
#define TOUCHTHRADJUST_121                       0x00
#define TOUCHTHRADJUST_122                       0x00
#define TOUCHTHRADJUST_123                       0x00
#define TOUCHTHRADJUST_124                       0x00
#define TOUCHTHRADJUST_125                       0x00
#define TOUCHTHRADJUST_126                       0x00
#define TOUCHTHRADJUST_127                       0x00
#define TOUCHTHRADJUST_128                       0x00
#define TOUCHTHRADJUST_129                       0x00
#define TOUCHTHRADJUST_130                       0x00
#define TOUCHTHRADJUST_131                       0x00
#define TOUCHTHRADJUST_132                       0x00
#define TOUCHTHRADJUST_133                       0x00
#define TOUCHTHRADJUST_134                       0x00
#define TOUCHTHRADJUST_135                       0x00
#define TOUCHTHRADJUST_136                       0x00
#define TOUCHTHRADJUST_137                       0x00
#define TOUCHTHRADJUST_138                       0x00
#define TOUCHTHRADJUST_139                       0x00
#define TOUCHTHRADJUST_140                       0x00
#define TOUCHTHRADJUST_141                       0x00
#define TOUCHTHRADJUST_142                       0x00
#define TOUCHTHRADJUST_143                       0x00
#define TOUCHTHRADJUST_144                       0x00
#define TOUCHTHRADJUST_145                       0x00
#define TOUCHTHRADJUST_146                       0x00
#define TOUCHTHRADJUST_147                       0x00
#define TOUCHTHRADJUST_148                       0x00
#define TOUCHTHRADJUST_149                       0x00
#define TOUCHTHRADJUST_150                       0x00
#define TOUCHTHRADJUST_151                       0x00
#define TOUCHTHRADJUST_152                       0x00
#define TOUCHTHRADJUST_153                       0x00
#define TOUCHTHRADJUST_154                       0x00
#define TOUCHTHRADJUST_155                       0x00
#define TOUCHTHRADJUST_156                       0x00
#define TOUCHTHRADJUST_157                       0x00
#define TOUCHTHRADJUST_158                       0x00
#define TOUCHTHRADJUST_159                       0x00
#define TOUCHTHRADJUST_160                       0x00
#define TOUCHTHRADJUST_161                       0x00
#define TOUCHTHRADJUST_162                       0x00
#define TOUCHTHRADJUST_163                       0x00
#define TOUCHTHRADJUST_164                       0x00
#define TOUCHTHRADJUST_165                       0x00
#define TOUCHTHRADJUST_166                       0x00
#define TOUCHTHRADJUST_167                       0x00
#define TOUCHTHRADJUST_168                       0x00
#define TOUCHTHRADJUST_169                       0x00
#define TOUCHTHRADJUST_170                       0x00
#define TOUCHTHRADJUST_171                       0x00
#define TOUCHTHRADJUST_172                       0x00
#define TOUCHTHRADJUST_173                       0x00
#define TOUCHTHRADJUST_174                       0x00
#define TOUCHTHRADJUST_175                       0x00
#define TOUCHTHRADJUST_176                       0x00
#define TOUCHTHRADJUST_177                       0x00
#define TOUCHTHRADJUST_178                       0x00
#define TOUCHTHRADJUST_179                       0x00
#define TOUCHTHRADJUST_180                       0x00
#define TOUCHTHRADJUST_181                       0x00
#define TOUCHTHRADJUST_182                       0x00
#define TOUCHTHRADJUST_183                       0x00
#define TOUCHTHRADJUST_184                       0x00
#define TOUCHTHRADJUST_185                       0x00
#define TOUCHTHRADJUST_186                       0x00
#define TOUCHTHRADJUST_187                       0x00
#define TOUCHTHRADJUST_188                       0x00
#define TOUCHTHRADJUST_189                       0x00
#define TOUCHTHRADJUST_190                       0x00
#define TOUCHTHRADJUST_191                       0x00
#define TOUCHTHRADJUST_192                       0x00
#define TOUCHTHRADJUST_193                       0x00
#define TOUCHTHRADJUST_194                       0x00
#define TOUCHTHRADJUST_195                       0x00
#define TOUCHTHRADJUST_196                       0x00
#define TOUCHTHRADJUST_197                       0x00
#define TOUCHTHRADJUST_198                       0x00
#define TOUCHTHRADJUST_199                       0x00
#define TOUCHTHRADJUST_200                       0x00
#define TOUCHTHRADJUST_201                       0x00
#define TOUCHTHRADJUST_202                       0x00
#define TOUCHTHRADJUST_203                       0x00
#define TOUCHTHRADJUST_204                       0x00
#define TOUCHTHRADJUST_205                       0x00
#define TOUCHTHRADJUST_206                       0x00
#define TOUCHTHRADJUST_207                       0x00
#define TOUCHTHRADJUST_208                       0x00
#define TOUCHTHRADJUST_209                       0x00
#define TOUCHTHRADJUST_210                       0x00
#define TOUCHTHRADJUST_211                       0x00
#define TOUCHTHRADJUST_212                       0x00
#define TOUCHTHRADJUST_213                       0x00
#define TOUCHTHRADJUST_214                       0x00
#define TOUCHTHRADJUST_215                       0x00
#define TOUCHTHRADJUST_216                       0x00
#define TOUCHTHRADJUST_217                       0x00
#define TOUCHTHRADJUST_218                       0x00
#define TOUCHTHRADJUST_219                       0x00
#define TOUCHTHRADJUST_220                       0x00
#define TOUCHTHRADJUST_221                       0x00
#define TOUCHTHRADJUST_222                       0x00
#define TOUCHTHRADJUST_223                       0x00
#define TOUCHTHRADJUST_224                       0x00
#define TOUCHTHRADJUST_225                       0x00
#define TOUCHTHRADJUST_226                       0x00
#define TOUCHTHRADJUST_227                       0x00
#define TOUCHTHRADJUST_228                       0x00
#define TOUCHTHRADJUST_229                       0x00
#define TOUCHTHRADJUST_230                       0x00
#define TOUCHTHRADJUST_231                       0x00
#define TOUCHTHRADJUST_232                       0x00
#define TOUCHTHRADJUST_233                       0x00
#define TOUCHTHRADJUST_234                       0x00
#define TOUCHTHRADJUST_235                       0x00
#define TOUCHTHRADJUST_236                       0x00
#define TOUCHTHRADJUST_237                       0x00
#define TOUCHTHRADJUST_238                       0x00
#define TOUCHTHRADJUST_239                       0x00
#define TOUCHTHRADJUST_240                       0x00
#define TOUCHTHRADJUST_241                       0x00
#define TOUCHTHRADJUST_242                       0x00
#define TOUCHTHRADJUST_243                       0x00
#define TOUCHTHRADJUST_244                       0x00
#define TOUCHTHRADJUST_245                       0x00
#define TOUCHTHRADJUST_246                       0x00
#define TOUCHTHRADJUST_247                       0x00
#define TOUCHTHRADJUST_248                       0x00
#define TOUCHTHRADJUST_249                       0x00
#define TOUCHTHRADJUST_250                       0x00
#define TOUCHTHRADJUST_251                       0x00
#define TOUCHTHRADJUST_252                       0x00
#define TOUCHTHRADJUST_253                       0x00
#define TOUCHTHRADJUST_254                       0x00
#define TOUCHTHRADJUST_255                       0x00
#define TOUCHTHRADJUST_256                       0x00
#define TOUCHTHRADJUST_257                       0x00
#define TOUCHTHRADJUST_258                       0x00
#define TOUCHTHRADJUST_259                       0x00
#define TOUCHTHRADJUST_260                       0x00
#define TOUCHTHRADJUST_261                       0x00
#define TOUCHTHRADJUST_262                       0x00
#define TOUCHTHRADJUST_263                       0x00
#define TOUCHTHRADJUST_264                       0x00
#define TOUCHTHRADJUST_265                       0x00
#define TOUCHTHRADJUST_266                       0x00
#define TOUCHTHRADJUST_267                       0x00
#define TOUCHTHRADJUST_268                       0x00
#define TOUCHTHRADJUST_269                       0x00
#define TOUCHTHRADJUST_270                       0x00
#define TOUCHTHRADJUST_271                       0x00
#define TOUCHTHRADJUST_272                       0x00
#define TOUCHTHRADJUST_273                       0x00
#define TOUCHTHRADJUST_274                       0x00
#define TOUCHTHRADJUST_275                       0x00
#define TOUCHTHRADJUST_276                       0x00
#define TOUCHTHRADJUST_277                       0x00
#define TOUCHTHRADJUST_278                       0x00
#define TOUCHTHRADJUST_279                       0x00
#define TOUCHTHRADJUST_280                       0x00
#define TOUCHTHRADJUST_281                       0x00
#define TOUCHTHRADJUST_282                       0x00
#define TOUCHTHRADJUST_283                       0x00
#define TOUCHTHRADJUST_284                       0x00
#define TOUCHTHRADJUST_285                       0x00
#define TOUCHTHRADJUST_286                       0x00
#define TOUCHTHRADJUST_287                       0x00
#define TOUCHTHRADJUST_288                       0x00
#define TOUCHTHRADJUST_289                       0x00
#define TOUCHTHRADJUST_290                       0x00
#define TOUCHTHRADJUST_291                       0x00
#define TOUCHTHRADJUST_292                       0x00
#define TOUCHTHRADJUST_293                       0x00
#define TOUCHTHRADJUST_294                       0x00
#define TOUCHTHRADJUST_295                       0x00
#define TOUCHTHRADJUST_296                       0x00
#define TOUCHTHRADJUST_297                       0x00
#define TOUCHTHRADJUST_298                       0x00
#define TOUCHTHRADJUST_299                       0x00
#define TOUCHTHRADJUST_300                       0x00
#define TOUCHTHRADJUST_301                       0x00
#define TOUCHTHRADJUST_302                       0x00
#define TOUCHTHRADJUST_303                       0x00
#define TOUCHTHRADJUST_304                       0x00
#define TOUCHTHRADJUST_305                       0x00
#define TOUCHTHRADJUST_306                       0x00
#define TOUCHTHRADJUST_307                       0x00
#define TOUCHTHRADJUST_308                       0x00
#define TOUCHTHRADJUST_309                       0x00
#define TOUCHTHRADJUST_310                       0x00
#define TOUCHTHRADJUST_311                       0x00
#define TOUCHTHRADJUST_312                       0x00
#define TOUCHTHRADJUST_313                       0x00
#define TOUCHTHRADJUST_314                       0x00
#define TOUCHTHRADJUST_315                       0x00
#define TOUCHTHRADJUST_316                       0x00
#define TOUCHTHRADJUST_317                       0x00
#define TOUCHTHRADJUST_318                       0x00
#define TOUCHTHRADJUST_319                       0x00
#define TOUCHTHRADJUST_320                       0x00
#define TOUCHTHRADJUST_321                       0x00
#define TOUCHTHRADJUST_322                       0x00
#define TOUCHTHRADJUST_323                       0x00
#define TOUCHTHRADJUST_324                       0x00
#define TOUCHTHRADJUST_325                       0x00
#define TOUCHTHRADJUST_326                       0x00
#define TOUCHTHRADJUST_327                       0x00
#define TOUCHTHRADJUST_328                       0x00
#define TOUCHTHRADJUST_329                       0x00
#define TOUCHTHRADJUST_330                       0x00
#define TOUCHTHRADJUST_331                       0x00
#define TOUCHTHRADJUST_332                       0x00
#define TOUCHTHRADJUST_333                       0x00
#define TOUCHTHRADJUST_334                       0x00
#define TOUCHTHRADJUST_335                       0x00
#define TOUCHTHRADJUST_336                       0x00
#define TOUCHTHRADJUST_337                       0x00
#define TOUCHTHRADJUST_338                       0x00
#define TOUCHTHRADJUST_339                       0x00
#define TOUCHTHRADJUST_340                       0x00
#define TOUCHTHRADJUST_341                       0x00
#define TOUCHTHRADJUST_342                       0x00
#define TOUCHTHRADJUST_343                       0x00
#define TOUCHTHRADJUST_344                       0x00
#define TOUCHTHRADJUST_345                       0x00
#define TOUCHTHRADJUST_346                       0x00
#define TOUCHTHRADJUST_347                       0x00
#define TOUCHTHRADJUST_348                       0x00
#define TOUCHTHRADJUST_349                       0x00
#define TOUCHTHRADJUST_350                       0x00
#define TOUCHTHRADJUST_351                       0x00
#define TOUCHTHRADJUST_352                       0x00
#define TOUCHTHRADJUST_353                       0x00
#define TOUCHTHRADJUST_354                       0x00
#define TOUCHTHRADJUST_355                       0x00
#define TOUCHTHRADJUST_356                       0x00
#define TOUCHTHRADJUST_357                       0x00
#define TOUCHTHRADJUST_358                       0x00
#define TOUCHTHRADJUST_359                       0x00
#define TOUCHTHRADJUST_360                       0x00
#define TOUCHTHRADJUST_361                       0x00
#define TOUCHTHRADJUST_362                       0x00
#define TOUCHTHRADJUST_363                       0x00
#define TOUCHTHRADJUST_364                       0x00
#define TOUCHTHRADJUST_365                       0x00
#define TOUCHTHRADJUST_366                       0x00
#define TOUCHTHRADJUST_367                       0x00
#define TOUCHTHRADJUST_368                       0x00
#define TOUCHTHRADJUST_369                       0x00
#define TOUCHTHRADJUST_370                       0x00
#define TOUCHTHRADJUST_371                       0x00
#define TOUCHTHRADJUST_372                       0x00
#define TOUCHTHRADJUST_373                       0x00
#define TOUCHTHRADJUST_374                       0x00
#define TOUCHTHRADJUST_375                       0x00
#define TOUCHTHRADJUST_376                       0x00
#define TOUCHTHRADJUST_377                       0x00
#define TOUCHTHRADJUST_378                       0x00
#define TOUCHTHRADJUST_379                       0x00
#define TOUCHTHRADJUST_380                       0x00
#define TOUCHTHRADJUST_381                       0x00
#define TOUCHTHRADJUST_382                       0x00
#define TOUCHTHRADJUST_383                       0x00
#define TOUCHTHRADJUST_384                       0x00
#define TOUCHTHRADJUST_385                       0x00
#define TOUCHTHRADJUST_386                       0x00
#define TOUCHTHRADJUST_387                       0x00
#define TOUCHTHRADJUST_388                       0x00
#define TOUCHTHRADJUST_389                       0x00
#define TOUCHTHRADJUST_390                       0x00
#define TOUCHTHRADJUST_391                       0x00
#define TOUCHTHRADJUST_392                       0x00
#define TOUCHTHRADJUST_393                       0x00
#define TOUCHTHRADJUST_394                       0x00
#define TOUCHTHRADJUST_395                       0x00
#define TOUCHTHRADJUST_396                       0x00
#define TOUCHTHRADJUST_397                       0x00
#define TOUCHTHRADJUST_398                       0x00
#define TOUCHTHRADJUST_399                       0x00
#define TOUCHTHRADJUST_400                       0x00
#define TOUCHTHRADJUST_401                       0x00
#define TOUCHTHRADJUST_402                       0x00
#define TOUCHTHRADJUST_403                       0x00
#define TOUCHTHRADJUST_404                       0x00
#define TOUCHTHRADJUST_405                       0x00
#define TOUCHTHRADJUST_406                       0x00
#define TOUCHTHRADJUST_407                       0x00
#define TOUCHTHRADJUST_408                       0x00
#define TOUCHTHRADJUST_409                       0x00
#define TOUCHTHRADJUST_410                       0x00
#define TOUCHTHRADJUST_411                       0x00
#define TOUCHTHRADJUST_412                       0x00
#define TOUCHTHRADJUST_413                       0x00
#define TOUCHTHRADJUST_414                       0x00
#define TOUCHTHRADJUST_415                       0x00
#define TOUCHTHRADJUST_416                       0x00
#define TOUCHTHRADJUST_417                       0x00
#define TOUCHTHRADJUST_418                       0x00
#define TOUCHTHRADJUST_419                       0x00
#define TOUCHTHRADJUST_420                       0x00
#define TOUCHTHRADJUST_421                       0x00
#define TOUCHTHRADJUST_422                       0x00
#define TOUCHTHRADJUST_423                       0x00
#define TOUCHTHRADJUST_424                       0x00
#define TOUCHTHRADJUST_425                       0x00
#define TOUCHTHRADJUST_426                       0x00
#define TOUCHTHRADJUST_427                       0x00
#define TOUCHTHRADJUST_428                       0x00
#define TOUCHTHRADJUST_429                       0x00
#define TOUCHTHRADJUST_430                       0x00
#define TOUCHTHRADJUST_431                       0x00
#define TOUCHTHRADJUST_432                       0x00
#define TOUCHTHRADJUST_433                       0x00
#define TOUCHTHRADJUST_434                       0x00
#define TOUCHTHRADJUST_435                       0x00
#define TOUCHTHRADJUST_436                       0x00
#define TOUCHTHRADJUST_437                       0x00
#define TOUCHTHRADJUST_438                       0x00
#define TOUCHTHRADJUST_439                       0x00
#define TOUCHTHRADJUST_440                       0x00
#define TOUCHTHRADJUST_441                       0x00
#define TOUCHTHRADJUST_442                       0x00
#define TOUCHTHRADJUST_443                       0x00
#define TOUCHTHRADJUST_444                       0x00
#define TOUCHTHRADJUST_445                       0x00
#define TOUCHTHRADJUST_446                       0x00
#define TOUCHTHRADJUST_447                       0x00
#define TOUCHTHRADJUST_448                       0x00
#define TOUCHTHRADJUST_449                       0x00
#define TOUCHTHRADJUST_450                       0x00
#define TOUCHTHRADJUST_451                       0x00
#define TOUCHTHRADJUST_452                       0x00
#define TOUCHTHRADJUST_453                       0x00
#define TOUCHTHRADJUST_454                       0x00
#define TOUCHTHRADJUST_455                       0x00
#define TOUCHTHRADJUST_456                       0x00
#define TOUCHTHRADJUST_457                       0x00
#define TOUCHTHRADJUST_458                       0x00
#define TOUCHTHRADJUST_459                       0x00
#define TOUCHTHRADJUST_460                       0x00
#define TOUCHTHRADJUST_461                       0x00
#define TOUCHTHRADJUST_462                       0x00
#define TOUCHTHRADJUST_463                       0x00
#define TOUCHTHRADJUST_464                       0x00
#define TOUCHTHRADJUST_465                       0x00
#define TOUCHTHRADJUST_466                       0x00
#define TOUCHTHRADJUST_467                       0x00
#define TOUCHTHRADJUST_468                       0x00
#define TOUCHTHRADJUST_469                       0x00
#define TOUCHTHRADJUST_470                       0x00
#define TOUCHTHRADJUST_471                       0x00
#define TOUCHTHRADJUST_472                       0x00
#define TOUCHTHRADJUST_473                       0x00
#define TOUCHTHRADJUST_474                       0x00
#define TOUCHTHRADJUST_475                       0x00
#define TOUCHTHRADJUST_476                       0x00
#define TOUCHTHRADJUST_477                       0x00
#define TOUCHTHRADJUST_478                       0x00
#define TOUCHTHRADJUST_479                       0x00
#define TOUCHTHRADJUST_480                       0x00
#define TOUCHTHRADJUST_481                       0x00
#define TOUCHTHRADJUST_482                       0x00
#define TOUCHTHRADJUST_483                       0x00
#define TOUCHTHRADJUST_484                       0x00
#define TOUCHTHRADJUST_485                       0x00
#define TOUCHTHRADJUST_486                       0x00
#define TOUCHTHRADJUST_487                       0x00
#define TOUCHTHRADJUST_488                       0x00
#define TOUCHTHRADJUST_489                       0x00
#define TOUCHTHRADJUST_490                       0x00
#define TOUCHTHRADJUST_491                       0x00
#define TOUCHTHRADJUST_492                       0x00
#define TOUCHTHRADJUST_493                       0x00
#define TOUCHTHRADJUST_494                       0x00
#define TOUCHTHRADJUST_495                       0x00
#define TOUCHTHRADJUST_496                       0x00
#define TOUCHTHRADJUST_497                       0x00
#define TOUCHTHRADJUST_498                       0x00
#define TOUCHTHRADJUST_499                       0x00
#define TOUCHTHRADJUST_500                       0x00
#define TOUCHTHRADJUST_501                       0x00
#define TOUCHTHRADJUST_502                       0x00
#define TOUCHTHRADJUST_503                       0x00
#define TOUCHTHRADJUST_504                       0x00
#define TOUCHTHRADJUST_505                       0x00

/* Virtual Buttons 0 */
/* Memory Map Position 0x14F0 - 0x1511 */
#define NUMBER_OF_VIRTUAL_SENSORS_0              0x00
#define NUMBER_OF_VIRTUAL_SENSORS_1              0x00
#define B0_TOP_LEFT_X_0                          0x00
#define B0_TOP_LEFT_X_1                          0x00
#define B0_TOP_LEFT_Y_0                          0x00
#define B0_TOP_LEFT_Y_1                          0x00
#define B0_BOT_RIGHT_X_0                         0x00
#define B0_BOT_RIGHT_X_1                         0x00
#define B0_BOT_RIGHT_Y_0                         0x00
#define B0_BOT_RIGHT_Y_1                         0x00
#define B1_TOP_LEFT_X_0                          0x00
#define B1_TOP_LEFT_X_1                          0x00
#define B1_TOP_LEFT_Y_0                          0x00
#define B1_TOP_LEFT_Y_1                          0x00
#define B1_BOT_RIGHT_X_0                         0x00
#define B1_BOT_RIGHT_X_1                         0x00
#define B1_BOT_RIGHT_Y_0                         0x00
#define B1_BOT_RIGHT_Y_1                         0x00
#define B2_TOP_LEFT_X_0                          0x00
#define B2_TOP_LEFT_X_1                          0x00
#define B2_TOP_LEFT_Y_0                          0x00
#define B2_TOP_LEFT_Y_1                          0x00
#define B2_BOT_RIGHT_X_0                         0x00
#define B2_BOT_RIGHT_X_1                         0x00
#define B2_BOT_RIGHT_Y_0                         0x00
#define B2_BOT_RIGHT_Y_1                         0x00
#define B3_TOP_LEFT_X_0                          0x00
#define B3_TOP_LEFT_X_1                          0x00
#define B3_TOP_LEFT_Y_0                          0x00
#define B3_TOP_LEFT_Y_1                          0x00
#define B3_BOT_RIGHT_X_0                         0x00
#define B3_BOT_RIGHT_X_1                         0x00
#define B3_BOT_RIGHT_Y_0                         0x00
#define B3_BOT_RIGHT_Y_1                         0x00

/* Virtual Buttons 1 */
/* Memory Map Position 0x1512 - 0x1541 */
#define B4_TOP_LEFT_X_0                          0x00
#define B4_TOP_LEFT_X_1                          0x00
#define B4_TOP_LEFT_Y_0                          0x00
#define B4_TOP_LEFT_Y_1                          0x00
#define B4_BOT_RIGHT_X_0                         0x00
#define B4_BOT_RIGHT_X_1                         0x00
#define B4_BOT_RIGHT_Y_0                         0x00
#define B4_BOT_RIGHT_Y_1                         0x00
#define B5_TOP_LEFT_X_0                          0x00
#define B5_TOP_LEFT_X_1                          0x00
#define B5_TOP_LEFT_Y_0                          0x00
#define B5_TOP_LEFT_Y_1                          0x00
#define B5_BOT_RIGHT_X_0                         0x00
#define B5_BOT_RIGHT_X_1                         0x00
#define B5_BOT_RIGHT_Y_0                         0x00
#define B5_BOT_RIGHT_Y_1                         0x00
#define B6_TOP_LEFT_X_0                          0x00
#define B6_TOP_LEFT_X_1                          0x00
#define B6_TOP_LEFT_Y_0                          0x00
#define B6_TOP_LEFT_Y_1                          0x00
#define B6_BOT_RIGHT_X_0                         0x00
#define B6_BOT_RIGHT_X_1                         0x00
#define B6_BOT_RIGHT_Y_0                         0x00
#define B6_BOT_RIGHT_Y_1                         0x00
#define B7_TOP_LEFT_X_0                          0x00
#define B7_TOP_LEFT_X_1                          0x00
#define B7_TOP_LEFT_Y_0                          0x00
#define B7_TOP_LEFT_Y_1                          0x00
#define B7_BOT_RIGHT_X_0                         0x00
#define B7_BOT_RIGHT_X_1                         0x00
#define B7_BOT_RIGHT_Y_0                         0x00
#define B7_BOT_RIGHT_Y_1                         0x00
#define B8_TOP_LEFT_X_0                          0x00
#define B8_TOP_LEFT_X_1                          0x00
#define B8_TOP_LEFT_Y_0                          0x00
#define B8_TOP_LEFT_Y_1                          0x00
#define B8_BOT_RIGHT_X_0                         0x00
#define B8_BOT_RIGHT_X_1                         0x00
#define B8_BOT_RIGHT_Y_0                         0x00
#define B8_BOT_RIGHT_Y_1                         0x00
#define B9_TOP_LEFT_X_0                          0x00
#define B9_TOP_LEFT_X_1                          0x00
#define B9_TOP_LEFT_Y_0                          0x00
#define B9_TOP_LEFT_Y_1                          0x00
#define B9_BOT_RIGHT_X_0                         0x00
#define B9_BOT_RIGHT_X_1                         0x00
#define B9_BOT_RIGHT_Y_0                         0x00
#define B9_BOT_RIGHT_Y_1                         0x00

/* Virtual Buttons 2 */
/* Memory Map Position 0x1542 - 0x1571 */
#define B10_TOP_LEFT_X_0                         0x00
#define B10_TOP_LEFT_X_1                         0x00
#define B10_TOP_LEFT_Y_0                         0x00
#define B10_TOP_LEFT_Y_1                         0x00
#define B10_BOT_RIGHT_X_0                        0x00
#define B10_BOT_RIGHT_X_1                        0x00
#define B10_BOT_RIGHT_Y_0                        0x00
#define B10_BOT_RIGHT_Y_1                        0x00
#define B11_TOP_LEFT_X_0                         0x00
#define B11_TOP_LEFT_X_1                         0x00
#define B11_TOP_LEFT_Y_0                         0x00
#define B11_TOP_LEFT_Y_1                         0x00
#define B11_BOT_RIGHT_X_0                        0x00
#define B11_BOT_RIGHT_X_1                        0x00
#define B11_BOT_RIGHT_Y_0                        0x00
#define B11_BOT_RIGHT_Y_1                        0x00
#define B12_TOP_LEFT_X_0                         0x00
#define B12_TOP_LEFT_X_1                         0x00
#define B12_TOP_LEFT_Y_0                         0x00
#define B12_TOP_LEFT_Y_1                         0x00
#define B12_BOT_RIGHT_X_0                        0x00
#define B12_BOT_RIGHT_X_1                        0x00
#define B12_BOT_RIGHT_Y_0                        0x00
#define B12_BOT_RIGHT_Y_1                        0x00
#define B13_TOP_LEFT_X_0                         0x00
#define B13_TOP_LEFT_X_1                         0x00
#define B13_TOP_LEFT_Y_0                         0x00
#define B13_TOP_LEFT_Y_1                         0x00
#define B13_BOT_RIGHT_X_0                        0x00
#define B13_BOT_RIGHT_X_1                        0x00
#define B13_BOT_RIGHT_Y_0                        0x00
#define B13_BOT_RIGHT_Y_1                        0x00
#define B14_TOP_LEFT_X_0                         0x00
#define B14_TOP_LEFT_X_1                         0x00
#define B14_TOP_LEFT_Y_0                         0x00
#define B14_TOP_LEFT_Y_1                         0x00
#define B14_BOT_RIGHT_X_0                        0x00
#define B14_BOT_RIGHT_X_1                        0x00
#define B14_BOT_RIGHT_Y_0                        0x00
#define B14_BOT_RIGHT_Y_1                        0x00
#define B15_TOP_LEFT_X_0                         0x00
#define B15_TOP_LEFT_X_1                         0x00
#define B15_TOP_LEFT_Y_0                         0x00
#define B15_TOP_LEFT_Y_1                         0x00
#define B15_BOT_RIGHT_X_0                        0x00
#define B15_BOT_RIGHT_X_1                        0x00
#define B15_BOT_RIGHT_Y_0                        0x00
#define B15_BOT_RIGHT_Y_1                        0x00

/* Virtual Sliders 0 */
/* Memory Map Position 0x1572 - 0x15A5 */
#define SLIDER_DEADZONE_0                        0x00
#define SLIDER_DEADZONE_1                        0x00
#define S0_TOP_LEFT_X_0                          0x00
#define S0_TOP_LEFT_X_1                          0x00
#define S0_TOP_LEFT_Y_0                          0x00
#define S0_TOP_LEFT_Y_1                          0x00
#define S0_BOT_RIGHT_X_0                         0x00
#define S0_BOT_RIGHT_X_1                         0x00
#define S0_BOT_RIGHT_Y_0                         0x00
#define S0_BOT_RIGHT_Y_1                         0x00
#define S0_RESOLUTION_0                          0x00
#define S0_RESOLUTION_1                          0x00
#define S1_TOP_LEFT_X_0                          0x00
#define S1_TOP_LEFT_X_1                          0x00
#define S1_TOP_LEFT_Y_0                          0x00
#define S1_TOP_LEFT_Y_1                          0x00
#define S1_BOT_RIGHT_X_0                         0x00
#define S1_BOT_RIGHT_X_1                         0x00
#define S1_BOT_RIGHT_Y_0                         0x00
#define S1_BOT_RIGHT_Y_1                         0x00
#define S1_RESOLUTION_0                          0x00
#define S1_RESOLUTION_1                          0x00
#define S2_TOP_LEFT_X_0                          0x00
#define S2_TOP_LEFT_X_1                          0x00
#define S2_TOP_LEFT_Y_0                          0x00
#define S2_TOP_LEFT_Y_1                          0x00
#define S2_BOT_RIGHT_X_0                         0x00
#define S2_BOT_RIGHT_X_1                         0x00
#define S2_BOT_RIGHT_Y_0                         0x00
#define S2_BOT_RIGHT_Y_1                         0x00
#define S2_RESOLUTION_0                          0x00
#define S2_RESOLUTION_1                          0x00
#define S3_TOP_LEFT_X_0                          0x00
#define S3_TOP_LEFT_X_1                          0x00
#define S3_TOP_LEFT_Y_0                          0x00
#define S3_TOP_LEFT_Y_1                          0x00
#define S3_BOT_RIGHT_X_0                         0x00
#define S3_BOT_RIGHT_X_1                         0x00
#define S3_BOT_RIGHT_Y_0                         0x00
#define S3_BOT_RIGHT_Y_1                         0x00
#define S3_RESOLUTION_0                          0x00
#define S3_RESOLUTION_1                          0x00
#define S4_TOP_LEFT_X_0                          0x00
#define S4_TOP_LEFT_X_1                          0x00
#define S4_TOP_LEFT_Y_0                          0x00
#define S4_TOP_LEFT_Y_1                          0x00
#define S4_BOT_RIGHT_X_0                         0x00
#define S4_BOT_RIGHT_X_1                         0x00
#define S4_BOT_RIGHT_Y_0                         0x00
#define S4_BOT_RIGHT_Y_1                         0x00
#define S4_RESOLUTION_0                          0x00
#define S4_RESOLUTION_1                          0x00

/* Virtual Sliders 1 */
/* Memory Map Position 0x15A6 - 0x15C3 */
#define S5_TOP_LEFT_X_0                          0x00
#define S5_TOP_LEFT_X_1                          0x00
#define S5_TOP_LEFT_Y_0                          0x00
#define S5_TOP_LEFT_Y_1                          0x00
#define S5_BOT_RIGHT_X_0                         0x00
#define S5_BOT_RIGHT_X_1                         0x00
#define S5_BOT_RIGHT_Y_0                         0x00
#define S5_BOT_RIGHT_Y_1                         0x00
#define S5_RESOLUTION_0                          0x00
#define S5_RESOLUTION_1                          0x00
#define S6_TOP_LEFT_X_0                          0x00
#define S6_TOP_LEFT_X_1                          0x00
#define S6_TOP_LEFT_Y_0                          0x00
#define S6_TOP_LEFT_Y_1                          0x00
#define S6_BOT_RIGHT_X_0                         0x00
#define S6_BOT_RIGHT_X_1                         0x00
#define S6_BOT_RIGHT_Y_0                         0x00
#define S6_BOT_RIGHT_Y_1                         0x00
#define S6_RESOLUTION_0                          0x00
#define S6_RESOLUTION_1                          0x00
#define S7_TOP_LEFT_X_0                          0x00
#define S7_TOP_LEFT_X_1                          0x00
#define S7_TOP_LEFT_Y_0                          0x00
#define S7_TOP_LEFT_Y_1                          0x00
#define S7_BOT_RIGHT_X_0                         0x00
#define S7_BOT_RIGHT_X_1                         0x00
#define S7_BOT_RIGHT_Y_0                         0x00
#define S7_BOT_RIGHT_Y_1                         0x00
#define S7_RESOLUTION_0                          0x00
#define S7_RESOLUTION_1                          0x00

/* Virtual Wheels */
/* Memory Map Position 0x15C4 - 0x15EB */
#define W0_CENTRE_X_0                            0x00
#define W0_CENTRE_X_1                            0x00
#define W0_CENTRE_Y_0                            0x00
#define W0_CENTRE_Y_1                            0x00
#define W0_INNER_RADIUS_0                        0x00
#define W0_INNER_RADIUS_1                        0x00
#define W0_OUTER_RADIUS_0                        0x00
#define W0_OUTER_RADIUS_1                        0x00
#define W0_RESOLUTION_0                          0x00
#define W0_RESOLUTION_1                          0x00
#define W1_CENTRE_X_0                            0x00
#define W1_CENTRE_X_1                            0x00
#define W1_CENTRE_Y_0                            0x00
#define W1_CENTRE_Y_1                            0x00
#define W1_INNER_RADIUS_0                        0x00
#define W1_INNER_RADIUS_1                        0x00
#define W1_OUTER_RADIUS_0                        0x00
#define W1_OUTER_RADIUS_1                        0x00
#define W1_RESOLUTION_0                          0x00
#define W1_RESOLUTION_1                          0x00
#define W2_CENTRE_X_0                            0x00
#define W2_CENTRE_X_1                            0x00
#define W2_CENTRE_Y_0                            0x00
#define W2_CENTRE_Y_1                            0x00
#define W2_INNER_RADIUS_0                        0x00
#define W2_INNER_RADIUS_1                        0x00
#define W2_OUTER_RADIUS_0                        0x00
#define W2_OUTER_RADIUS_1                        0x00
#define W2_RESOLUTION_0                          0x00
#define W2_RESOLUTION_1                          0x00
#define W3_CENTRE_X_0                            0x00
#define W3_CENTRE_X_1                            0x00
#define W3_CENTRE_Y_0                            0x00
#define W3_CENTRE_Y_1                            0x00
#define W3_INNER_RADIUS_0                        0x00
#define W3_INNER_RADIUS_1                        0x00
#define W3_OUTER_RADIUS_0                        0x00
#define W3_OUTER_RADIUS_1                        0x00
#define W3_RESOLUTION_0                          0x00
#define W3_RESOLUTION_1                          0x00

/* Eng Settings */
/* Memory Map Position 0x2000 - 0x2005 */
#define ENG_CONFIG_0                             0xEE
#define ENG_CONFIG_1                             0xEE
#define ENG_0                                    0xEE
#define ENG_1                                    0xEE
#define ENG_2                                    0xEE
#define ENG_3                                    0xEE

static int iqs9150_read_reg(struct iqs9150_ts_data *ts, u8 reg_high,
				u8 reg_low, u16 len, void *val);
static int iqs9150_write_reg(struct i2c_client *client, u16 reg, u16 len, const void *val);
static int iqs_gpio_configure(struct iqs9150_ts_data *ts);
static int iqs9150_configure_inpudev(struct iqs9150_ts_data *ts);
static int iqs9150_init(struct iqs9150_ts_data *ts);
static int iqs9150_stop_bit_disabled(struct iqs9150_ts_data *ts);
static int iqs9150_stop_i2c_comm_window(struct iqs9150_ts_data *ts);
static int iqs_parse_dt(struct device *dev, struct iqs9150_ts_data *pdata);
static int iqs9150_force_i2c_comm_window(struct iqs9150_ts_data *ts);
static uint8_t iqs9150_iqs_production_testing(struct iqs9150_ts_data *ts);

#endif
