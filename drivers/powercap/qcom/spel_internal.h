/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __SPEL_INTERNAL_H__
#define __SPEL_INTERNAL_H__

#include <linux/powercap.h>

#define MMIO_OFFSET(base, offset) ((void __iomem *)((uintptr_t)(base) + (uintptr_t)(offset)))

#define SUSPICIOUS_VALUE   0xFFFFFFFF

#define SPEL_NAME_MAX      32

#define ENERGY_UNITS_MASK  0xF0000
#define TIME_UNITS_MASK    0xF00
#define POWER_UNITS_MASK   0x7

#define ENERGY_UNITS_SHIFT 16
#define TIME_UNITS_SHIFT   8
#define POWER_UNITS_SHIFT  0

#define MAX_POWER_LIMIT    0x00007FFF
#define ENABLE_POWER_LIMIT 0x80000000

#define MAX_TIME_WINDOW    0x003FFFFF
#define MIN_TIME_WINDOW    0x0000000A
#define TIME_WINDOW_MASK_L 0x00007FFF
#define TIME_WINDOW_MASK_H 0x003F8000
#define MAX_PRIORITY       0x0000001F

#define CHILD_NODE_SYS_NAME "sys"
#define CHILD_NODE_SOC_NAME "soc"
#define CHILD_NODE_SOC_CL0_NAME "cg0"
#define CHILD_NODE_SOC_CL1_NAME "cg1"
#define CHILD_NODE_SOC_CL2_NAME "cg2"
#define CHILD_NODE_SOC_IGPU_NAME "gpu"
#define CHILD_NODE_SOC_NSP_NAME "npu"
#define CHILD_NODE_SOC_MMCX_NAME "mm"
#define CHILD_NODE_SOC_INFRA_NAME "infra"
#define CHILD_NODE_SOC_DRAM_NAME "dram"
#define CHILD_NODE_SOC_MODEM_NAME "modem"

#define CONSTRAINT_NAME_SYS_PL1 "sys_pl1"
#define CONSTRAINT_NAME_SYS_PL2 "sys_pl2"
#define CONSTRAINT_NAME_SYS_PL3 "sys_pl3"
#define CONSTRAINT_NAME_SYS_PL4 "sys_pl4"
#define CONSTRAINT_NAME_SOC_PL1 "soc_pl1"
#define CONSTRAINT_NAME_SOC_PL2 "soc_pl2"
#define CONSTRAINT_NAME_SOC_PL3 "soc_pl3"
#define CONSTRAINT_NAME_SOC_PL4 "soc_pl4"
#define CONSTRAINT_NAME_UNDEFINED "unknown"

enum CHILD_NODE {
	CHILD_NODE_SYS,
	CHILD_NODE_SOC,
	CHILD_NODE_SOC_CL0,
	CHILD_NODE_SOC_CL1,
	CHILD_NODE_SOC_CL2,
	CHILD_NODE_SOC_IGPU,
	CHILD_NODE_SOC_NSP,
	CHILD_NODE_SOC_MMCX,
	CHILD_NODE_SOC_INFRA,
	CHILD_NODE_SOC_DRAM,
	CHILD_NODE_SOC_MODEM,
};

enum SPEL_NODE_TYPE {
	SPEL_NODE_VIRTUAL = 0,
	SPEL_NODE_DT,
};

struct spel_ops;

struct spel_units {
	u32 energy;
	u32 time;
	u32 power;
};

struct spel_constraints {
	struct list_head pzc_list;
	struct powercap_zone_constraint *pzc;
};

struct spel {
	struct powercap_zone zone;
	struct list_head sibling;
	struct list_head children;
	struct spel_ops *ops;
	char name[SPEL_NAME_MAX];
	struct device_node *np;
	int count;
	bool enabled;
	u32 enable_bitmask;
	void __iomem *address;
	int nr_constraints;
	struct list_head pz_list;
	struct list_head node;
	struct mutex lock;
	struct spel *parent;
	struct spel_constraints *constraints_list;
};

struct spel_ops {
	int (*set_enable)(struct spel *spel, bool enable);
	int (*get_enable)(struct spel *spel, bool *enable);
	u64 (*get_spel_energy_uj)(struct spel *spel);
	void (*release)(struct spel *spel);
};

struct spel_node {
	enum SPEL_NODE_TYPE type;
	char name[SPEL_NAME_MAX];
	struct spel_node *parent;
	struct spel *spel;
	struct device_node *np;
	u32 enable_bitmask;
	void __iomem *address;
	int nr_constraints;
};

struct spel_base_address {
	void __iomem *nodes;
	void __iomem *constraints;
	void __iomem *config;
};

static inline struct spel *to_spel(struct powercap_zone *zone)
{
	return container_of(zone, struct spel, zone);
}
#endif /* __SPEL_INTERNAL_H__ */
