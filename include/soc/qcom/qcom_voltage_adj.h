/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QCOM_VADJ_H__
#define __QCOM_VADJ_H__

#define GLOBAL_VALID_MASK 0x8
#define CPU_VALID_MASK 0x80
#define GLOBAL_MASK 0x7
#define CPU_MASK 0x70
#define CPU_SHFT 0x4
#define VALID_MASK 0x88

#define SDAM2_MEM17 0x7151
#define SDAM2_MEM54 0x7176

#define MAX_IDX 7

struct qvadj_platform_data {
	struct mutex lock;
	struct regmap *regmap;
	struct dentry *root;
};

static struct qvadj_platform_data *pd;
static u32 sdam_reg;

struct qvadj_device_data {
	u32 sdam_reg;
};

#if IS_ENABLED(CONFIG_QCOM_VADJ)

int qvadj_get_cur_voltage_overrides(u8 *global_rails_index, u8 *cpu_rails_index, u32 sdam_reg);
int qvadj_set_next_voltage_overrides(u8 global_rails_index, u8 cpu_rails_index, u32 sdam_reg);

#else

int qvadj_get_cur_voltage_overrides(u8 *global_rails_index, u8 *cpu_rails_index, u32 sdam_reg)
{ return -ENODEV; }
int qvadj_set_next_voltage_overrides(u8 *global_rails_index, u8 *cpu_rails_index, u32 sdam_reg)
{ return -ENODEV; }

#endif
#endif /*__QCOM_VADJ_H__ */
