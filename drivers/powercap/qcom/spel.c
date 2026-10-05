// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
/*
 * QCOM SPEL Hardware Limits Settings and Info Driver
 * Exposes:
 *   /sys/kernel/power-limits
 *     +-- priority
 *         +-- cpu
 *         +-- gpu
 *         +-- npu
 *     +-- active_limits
 *         +-- apps
 *         +-- soccp
 *     +-- constrained_duration
 *         +-- cpu
 *         +-- gpu
 *         +-- npu
 */

#define pr_fmt(fmt)	"spel: %s: " fmt, __func__

#include <linux/completion.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/ipc_logging.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#define SPEL_IPC(msg, args...)      do { \
			if (ipc_log) { \
				ipc_log_string(ipc_log, \
					"[%s]: %s: " msg, \
					current->comm, __func__, args); \
			} \
		} while (0)

#include "spel_internal.h"

#define SPEL_HW "spel-hw"

#define ROOT_NODE "root"

#define IPC_LOGPAGES             10
#define VERBOSE                  0

#define REPORT_UNITS_OFFSET      4
#define LIMITS_CAPABILITY_OFFSET 32
#define ENABLE_SPEL_OFFSET       0x84

#define ENABLE_SPEL_BIT_MASK     0x80000000
#define ENABLE_SYS_BIT_MASK      0x00000001
#define ENABLE_SOC_BIT_MASK      0x00000002
#define ENABLE_CL0_BIT_MASK      0x00000004
#define ENABLE_CL1_BIT_MASK      0x00000008
#define ENABLE_CL2_BIT_MASK      0x00000010
#define ENABLE_IGPU_BIT_MASK     0x00000020
#define ENABLE_NSP_BIT_MASK      0x00000040
#define ENABLE_MMCX_BIT_MASK     0x00000080
#define ENABLE_INFRA_BIT_MASK    0x00000100
#define ENABLE_DRAM_BIT_MASK     0x00000200
#define ENABLE_MODEM_BIT_MASK    0x00000400

#define ENABLE_SPEL_TRIGGER_NAME CHILD_NODE_SOC_NAME
#define ENABLE_SPEL_INIT_NAME    CHILD_NODE_SYS_NAME

static u32 global_enable_mask;

struct child_spec {
	enum CHILD_NODE	index;
	char		*name;
	uintptr_t	offset;
	int		nr_constraints;
	u32		enable_mask;
};

static struct child_spec children_nodes[] = {
	{ CHILD_NODE_SYS,       CHILD_NODE_SYS_NAME,       0x40, 4, ENABLE_SYS_BIT_MASK   },
	{ CHILD_NODE_SOC,       CHILD_NODE_SOC_NAME,       0x00, 4, ENABLE_SOC_BIT_MASK   },
	{ CHILD_NODE_SOC_CL0,   CHILD_NODE_SOC_CL0_NAME,   0x5C, 0, ENABLE_CL0_BIT_MASK   },
	{ CHILD_NODE_SOC_CL1,   CHILD_NODE_SOC_CL1_NAME,   0x60, 0, ENABLE_CL1_BIT_MASK   },
	{ CHILD_NODE_SOC_CL2,   CHILD_NODE_SOC_CL2_NAME,   0x64, 0, ENABLE_CL2_BIT_MASK   },
	{ CHILD_NODE_SOC_IGPU,  CHILD_NODE_SOC_IGPU_NAME,  0x08, 0, ENABLE_IGPU_BIT_MASK  },
	{ CHILD_NODE_SOC_NSP,   CHILD_NODE_SOC_NSP_NAME,   0x0C, 0, ENABLE_NSP_BIT_MASK   },
	{ CHILD_NODE_SOC_MMCX,  CHILD_NODE_SOC_MMCX_NAME,  0x10, 0, ENABLE_MMCX_BIT_MASK  },
	{ CHILD_NODE_SOC_INFRA, CHILD_NODE_SOC_INFRA_NAME, 0x18, 0, ENABLE_INFRA_BIT_MASK },
	{ CHILD_NODE_SOC_DRAM,  CHILD_NODE_SOC_DRAM_NAME,  0x1C, 0, ENABLE_DRAM_BIT_MASK  },
	{ CHILD_NODE_SOC_MODEM, CHILD_NODE_SOC_MODEM_NAME, 0x68, 0, ENABLE_MODEM_BIT_MASK },
};

struct constraint_spec {
	int		cid;
	char		*zone_name;
	char		*constraint_name;
	uintptr_t	limit_offset;
	uintptr_t	time_window_offset;
	u32		supported_mask;
	bool		supported;
};

static struct constraint_spec constraints[] = {
	{ -1, CHILD_NODE_SYS_NAME, CONSTRAINT_NAME_SYS_PL1, 0x10, 0x70, 0x01, false },
	{ -1, CHILD_NODE_SYS_NAME, CONSTRAINT_NAME_SYS_PL2, 0x14, 0x74, 0x02, false },
	{ -1, CHILD_NODE_SYS_NAME, CONSTRAINT_NAME_SYS_PL3, 0x18, 0x78, 0x04, false },
	{ -1, CHILD_NODE_SYS_NAME, CONSTRAINT_NAME_SYS_PL4, 0x1C, 0x7C, 0x08, false },
	{ -1, CHILD_NODE_SOC_NAME, CONSTRAINT_NAME_SOC_PL1, 0x00, 0x60, 0x10, false },
	{ -1, CHILD_NODE_SOC_NAME, CONSTRAINT_NAME_SOC_PL2, 0x04, 0x64, 0x20, false },
	{ -1, CHILD_NODE_SOC_NAME, CONSTRAINT_NAME_SOC_PL3, 0x08, 0x68, 0x40, false },
	{ -1, CHILD_NODE_SOC_NAME, CONSTRAINT_NAME_SOC_PL4, 0x0C, 0x6C, 0x80, false },
};

static struct spel_base_address base_address;
static struct spel_units budget_units;
static struct spel_units report_units;
static LIST_HEAD(spel_list);
static DEFINE_MUTEX(spel_list_lock);
void *ipc_log;

struct spel_hierarchy {
	int count;
	int cur_index;
	struct powercap_control_type *pct;
	struct spel_node *hierarchy;
	struct spel *root;
};

static struct spel_hierarchy *sh;

/* ===================== Global sysfs root ===================== */

static struct kobject *spel_root_kobj;

enum spel_priority_idx {
	SPEL_PRIO_CPU,
	SPEL_PRIO_GPU,
	SPEL_PRIO_NSP,
	SPEL_PRIO_MAX,
};

enum spel_limit_idx {
	SPEL_LIMIT_PM,
	SPEL_LIMIT_SOCCP,
	SPEL_LIMIT_MAX,
};

enum spel_duration_idx {
	SPEL_DUR_CPU,
	SPEL_DUR_GPU,
	SPEL_DUR_NSP,
	SPEL_DUR_MAX,
};

struct spel_kobj_attr {
	struct kobj_attribute kattr;
	int index;
};

struct spel_sysfs_data {
	struct mutex lock;

	/* names for logging */
	const char *priority_name[SPEL_PRIO_MAX];
	const char *active_limits_name[SPEL_LIMIT_MAX];
	const char *constrained_duration_name[SPEL_DUR_MAX];

	/* cached values */
	u32 priority[SPEL_PRIO_MAX];
	u32 active_limits[SPEL_LIMIT_MAX];
	u32 constrained_duration[SPEL_DUR_MAX];

	/* priority group */
	struct spel_kobj_attr priority_attrs[SPEL_PRIO_MAX];
	struct attribute *priority_group_attrs[SPEL_PRIO_MAX + 1];
	struct attribute_group priority_group;

	/* active_limits group */
	struct spel_kobj_attr active_limit_attrs[SPEL_LIMIT_MAX];
	struct attribute *active_limit_group_attrs[SPEL_LIMIT_MAX + 1];
	struct attribute_group active_limit_group;

	/* constrained_duration group */
	struct spel_kobj_attr duration_attrs[SPEL_DUR_MAX];
	struct attribute *duration_group_attrs[SPEL_DUR_MAX + 1];
	struct attribute_group duration_group;
};

static const uintptr_t priority_offset[SPEL_PRIO_MAX] = {
	0x130, 0x134, 0x138
};

static const uintptr_t active_limits_offset[SPEL_LIMIT_MAX] = {
	0x6C, 0x70
};

static const uintptr_t constrained_duration_offset[SPEL_DUR_MAX] = {
	0x80, 0x84, 0x88
};

static struct spel_sysfs_data *spel_sysfs;

static int __init spel_root_create(void)
{
	spel_root_kobj =
		kobject_create_and_add("power-limits", kernel_kobj);

	if (!spel_root_kobj)
		return -ENOMEM;

	return 0;
}

static void spel_root_destroy(void)
{
	if (spel_root_kobj) {
		kobject_put(spel_root_kobj);
		spel_root_kobj = NULL;
	}
}

static inline struct spel_kobj_attr *to_spel_attr(struct kobj_attribute *attr)
{
	return container_of(attr, struct spel_kobj_attr, kattr);
}

static u32 spel_readl(void __iomem *addr)
{
	u32 val;

	if (!addr) {
		pr_err("Invalid register address\n");
		return 0;
	}

	val = readl(addr);
	if (val == SUSPICIOUS_VALUE)
		pr_warn("Suspicious register read value 0x%X\n", SUSPICIOUS_VALUE);

	if (VERBOSE) {
		pr_debug("reading from address: %p=%u (0x%x)\n", addr, val, val);
		SPEL_IPC("reading from address: %p=%u (0x%x)\n", addr, val, val);
	}

	return val;
}

static void spel_writel(u32 val, void __iomem *addr)
{
	if (!addr) {
		pr_err("Invalid register address\n");
		return;
	}

	pr_debug("writing %u (0x%x) to address: %p\n", val, val, addr);
	SPEL_IPC("writing %u (0x%x) to address: %p\n", val, val, addr);

	writel(val, addr);
}

static ssize_t priority_show(struct kobject *kobj,
			struct kobj_attribute *attr,
			char *buf)
{
	struct spel_kobj_attr *sattr = to_spel_attr(attr);
	u32 val;

	mutex_lock(&spel_sysfs->lock);
	spel_sysfs->priority[sattr->index] =
		spel_readl(MMIO_OFFSET(base_address.constraints,
			priority_offset[sattr->index]));
	val = spel_sysfs->priority[sattr->index];
	mutex_unlock(&spel_sysfs->lock);

	/* Cap the given value at the max */
	if (val > MAX_PRIORITY)
		val = MAX_PRIORITY;

	pr_debug("%s priority is %u (0x%x)\n",
		spel_sysfs->priority_name[sattr->index], val, val);
	SPEL_IPC("%s priority is %u (0x%x)\n",
		spel_sysfs->priority_name[sattr->index], val, val);

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t priority_store(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf,
			size_t count)
{
	struct spel_kobj_attr *sattr = to_spel_attr(attr);
	u32 val;

	if (kstrtou32(buf, 0, &val))
		return -EINVAL;

	/* Cap the given value at the max */
	if (val > MAX_PRIORITY)
		val = MAX_PRIORITY;

	mutex_lock(&spel_sysfs->lock);
	spel_sysfs->priority[sattr->index] = val;

	/*
	 * Program HW register corresponding to CPU/GPU/NSP priority.
	 */
	spel_writel(val, MMIO_OFFSET(base_address.constraints,
		priority_offset[sattr->index]));

	mutex_unlock(&spel_sysfs->lock);

	pr_debug("Setting %s priority to %u (0x%x)\n",
		spel_sysfs->priority_name[sattr->index], val, val);
	SPEL_IPC("Setting %s priority to %u (0x%x)\n",
		spel_sysfs->priority_name[sattr->index], val, val);

	return count;
}

static ssize_t active_limits_show(struct kobject *kobj,
			struct kobj_attribute *attr,
			char *buf)
{
	struct spel_kobj_attr *sattr = to_spel_attr(attr);
	u32 val;

	mutex_lock(&spel_sysfs->lock);
	spel_sysfs->active_limits[sattr->index] =
		spel_readl(MMIO_OFFSET(base_address.nodes,
			active_limits_offset[sattr->index]));
	val = spel_sysfs->active_limits[sattr->index];
	mutex_unlock(&spel_sysfs->lock);

	pr_debug("%s active_limits is %u (0x%x)\n",
		spel_sysfs->active_limits_name[sattr->index], val, val);
	SPEL_IPC("%s active_limits is %u (0x%x)\n",
		spel_sysfs->active_limits_name[sattr->index], val, val);

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t duration_show(struct kobject *kobj,
			struct kobj_attribute *attr,
			char *buf)
{
	struct spel_kobj_attr *sattr = to_spel_attr(attr);
	u32 val;

	mutex_lock(&spel_sysfs->lock);
	spel_sysfs->constrained_duration[sattr->index] =
		spel_readl(MMIO_OFFSET(base_address.nodes,
			constrained_duration_offset[sattr->index]));
	val = spel_sysfs->constrained_duration[sattr->index];
	mutex_unlock(&spel_sysfs->lock);

	pr_debug("%s constrained_duration is %u (0x%x)\n",
		spel_sysfs->constrained_duration_name[sattr->index], val, val);
	SPEL_IPC("%s constrained_duration is %u (0x%x)\n",
		spel_sysfs->constrained_duration_name[sattr->index], val, val);

	return sysfs_emit(buf, "%u\n", val);
}

static int spel_init_sysfs(struct device *dev)
{
	static const char * const prio_names[] = {
		"cpu", "gpu", "npu"
	};

	static const char * const limit_names[] = {
		"apps", "soccp"
	};

	static const char * const duration_names[] = {
		"cpu", "gpu", "npu"
	};

	int ret, i;

	spel_sysfs = devm_kzalloc(dev, sizeof(*spel_sysfs), GFP_KERNEL);
	if (!spel_sysfs)
		return -ENOMEM;

	mutex_init(&spel_sysfs->lock);

	for (i = 0; i < SPEL_PRIO_MAX; i++) {
		sysfs_attr_init(&spel_sysfs->priority_attrs[i].kattr.attr);

		spel_sysfs->priority_attrs[i].kattr.attr.name =	prio_names[i];
		spel_sysfs->priority_attrs[i].kattr.attr.mode = 0644;

		spel_sysfs->priority_attrs[i].kattr.show = priority_show;
		spel_sysfs->priority_attrs[i].kattr.store = priority_store;

		spel_sysfs->priority_attrs[i].index = i;

		spel_sysfs->priority_group_attrs[i] =
			&spel_sysfs->priority_attrs[i].kattr.attr;

		spel_sysfs->priority_name[i] = prio_names[i];
	}

	spel_sysfs->priority_group_attrs[SPEL_PRIO_MAX] = NULL;

	spel_sysfs->priority_group.name = "priority";
	spel_sysfs->priority_group.attrs = spel_sysfs->priority_group_attrs;

	ret = sysfs_create_group(spel_root_kobj, &spel_sysfs->priority_group);
	if (ret)
		return ret;

	for (i = 0; i < SPEL_LIMIT_MAX; i++) {
		sysfs_attr_init(&spel_sysfs->active_limit_attrs[i].kattr.attr);

		spel_sysfs->active_limit_attrs[i].kattr.attr.name = limit_names[i];
		spel_sysfs->active_limit_attrs[i].kattr.attr.mode = 0444;

		spel_sysfs->active_limit_attrs[i].kattr.show = active_limits_show;

		spel_sysfs->active_limit_attrs[i].index = i;

		spel_sysfs->active_limit_group_attrs[i] =
			&spel_sysfs->active_limit_attrs[i].kattr.attr;

		spel_sysfs->active_limits_name[i] = limit_names[i];
	}

	spel_sysfs->active_limit_group_attrs[SPEL_LIMIT_MAX] = NULL;

	spel_sysfs->active_limit_group.name = "active_limits";
	spel_sysfs->active_limit_group.attrs = spel_sysfs->active_limit_group_attrs;

	ret = sysfs_create_group(spel_root_kobj, &spel_sysfs->active_limit_group);
	if (ret)
		goto remove_priority;

	for (i = 0; i < SPEL_DUR_MAX; i++) {
		sysfs_attr_init(&spel_sysfs->duration_attrs[i].kattr.attr);

		spel_sysfs->duration_attrs[i].kattr.attr.name = duration_names[i];
		spel_sysfs->duration_attrs[i].kattr.attr.mode = 0444;

		spel_sysfs->duration_attrs[i].kattr.show = duration_show;

		spel_sysfs->duration_attrs[i].index = i;

		spel_sysfs->duration_group_attrs[i] =
			&spel_sysfs->duration_attrs[i].kattr.attr;

		spel_sysfs->constrained_duration_name[i] = duration_names[i];
	}

	spel_sysfs->duration_group_attrs[SPEL_DUR_MAX] = NULL;

	spel_sysfs->duration_group.name = "constrained_duration";
	spel_sysfs->duration_group.attrs = spel_sysfs->duration_group_attrs;

	ret = sysfs_create_group(spel_root_kobj, &spel_sysfs->duration_group);
	if (ret)
		goto remove_active;

	return 0;

remove_active:
	sysfs_remove_group(spel_root_kobj, &spel_sysfs->active_limit_group);

remove_priority:
	sysfs_remove_group(spel_root_kobj, &spel_sysfs->priority_group);

	spel_sysfs = NULL;

	return ret;
}

static int get_spel_count(void)
{
	return ARRAY_SIZE(children_nodes);
}

static int get_constraint_count(void)
{
	return ARRAY_SIZE(constraints);
}

/******************************************************************************
 * Bits  Field Name	Description
 * 19:16 ENERGY_UNITS	Energy units used for internal budgeting calculations,
 *			in the format of 1/2^[19:16] J per LSB. This gives a
 *			maximum of 1J per LSB and min of 1/(2^15) = 30uJ per LSB.
 * NOTE:		We convert reported energy to uJ as defined by the
 *			powercap interface.
 * 11:8  TIME_UNITS	Time units in enforcing the system power limits in the
 *			format of 1/2^[11:8] usec per LSB.
 * NOTE:		The TIME_UNITS register value is unused and the time
 *			units are always in milliseconds. We convert them to
 *			microseconds as defined by the powercap interface.
 * 2:0   POWER_UNITS	Power units in the format of 1/2^[2:0] watts per LSB.
 *			This gives a maximum of 1W per LSB and min of 1/(2^7) =
 *			7.8mW per LSB.
 * NOTE:		We convert system power limits from uW to the specified
 *			power units as defined by the powercap interface.
 */

static int spel_get_units(void __iomem *address)
{
	u32 val;
	u32 energy_units, time_units, power_units;

	val = spel_readl(address);

	energy_units = (val & ENERGY_UNITS_MASK) >> ENERGY_UNITS_SHIFT;
	time_units = (val & TIME_UNITS_MASK) >> TIME_UNITS_SHIFT;
	power_units = (val & POWER_UNITS_MASK) >> POWER_UNITS_SHIFT;

	budget_units.energy = energy_units;
	budget_units.time   = time_units;
	budget_units.power  = power_units;

	val = spel_readl(MMIO_OFFSET(address, REPORT_UNITS_OFFSET));

	energy_units = (val & ENERGY_UNITS_MASK) >> ENERGY_UNITS_SHIFT;
	time_units = (val & TIME_UNITS_MASK) >> TIME_UNITS_SHIFT;
	power_units = (val & POWER_UNITS_MASK) >> POWER_UNITS_SHIFT;

	report_units.energy = energy_units;
	report_units.time   = time_units;
	report_units.power  = power_units;

	pr_debug("budget_units.energy: %u\n", budget_units.energy);
	pr_debug("budget_units.time: %u\n", budget_units.time);
	pr_debug("budget_units.power: %u\n", budget_units.power);
	pr_debug("report_units.energy: %u\n", report_units.energy);
	pr_debug("report_units.time: %u\n", report_units.time);
	pr_debug("report_units.power: %u\n", report_units.power);
	SPEL_IPC("budget_units.energy: %u\n", budget_units.energy);
	SPEL_IPC("budget_units.time: %u\n", budget_units.time);
	SPEL_IPC("budget_units.power: %u\n", budget_units.power);
	SPEL_IPC("report_units.energy: %u\n", report_units.energy);
	SPEL_IPC("report_units.time: %u\n", report_units.time);
	SPEL_IPC("report_units.power: %u\n", report_units.power);

	return 0;
}

static bool constraint_is_supported(void __iomem *address, u32 bit_mask)
{
	u32 val;

	val = spel_readl(MMIO_OFFSET(address, LIMITS_CAPABILITY_OFFSET)) & bit_mask;

	return val != 0;
}

static void update_constraints(struct child_spec *node)
{
	int cid = 0;

	for (int i = 0; i < get_constraint_count(); ++i)
		if (!strcmp(node->name, constraints[i].zone_name) &&
				constraints[i].supported) {
			constraints[i].cid = cid;
			cid++;
		}
	node->nr_constraints = cid;
}

static void spel_init_constraints(void __iomem *address)
{
	for (int i = 0; i < get_constraint_count(); ++i)
		if (constraint_is_supported(address, constraints[i].supported_mask))
			constraints[i].supported = true;

	for (int i = 0; i < get_spel_count(); ++i)
		if (children_nodes[i].nr_constraints)
			update_constraints(&children_nodes[i]);
}

static int hw_spel_set_enable(bool enable)
{
	if (enable)
		global_enable_mask |= ENABLE_SPEL_BIT_MASK;
	else
		global_enable_mask &= ~ENABLE_SPEL_BIT_MASK;

	spel_writel(global_enable_mask,
		MMIO_OFFSET(base_address.constraints, ENABLE_SPEL_OFFSET));

	pr_debug("setting SPEL to %s\n",
		enable ? "enabled" : "disabled");
	SPEL_IPC("setting SPEL to %s\n",
		enable ? "enabled" : "disabled");

	return 0;
}

static int hw_spel_get_enable(bool *enable)
{
	u32 val;

	val = spel_readl(MMIO_OFFSET(base_address.constraints, ENABLE_SPEL_OFFSET));

	*enable = (bool)((val & ENABLE_SPEL_BIT_MASK) != 0);

	pr_debug("enabled state for SPEL is %s\n",
		*enable ? "enabled" : "disabled");
	SPEL_IPC("enabled state for SPEL is %s\n",
		*enable ? "enabled" : "disabled");

	return 0;
}

static int hw_zone_set_enable(struct spel *spel, bool enable)
{
	u32 val;

	if (enable)
		global_enable_mask |= spel->enable_bitmask;
	else
		global_enable_mask &= ~spel->enable_bitmask;

	if (!strcmp(spel->name, ENABLE_SPEL_TRIGGER_NAME)) {
		if (enable) {
			spel_writel(global_enable_mask,
				MMIO_OFFSET(base_address.constraints,
					ENABLE_SPEL_OFFSET));
		} else {
			val = spel_readl(MMIO_OFFSET(base_address.constraints,
					ENABLE_SPEL_OFFSET));
			val &= ~ENABLE_SOC_BIT_MASK;
			spel_writel(val, MMIO_OFFSET(base_address.constraints,
					ENABLE_SPEL_OFFSET));
		}
	} else if (!strcmp(spel->name, ENABLE_SPEL_INIT_NAME)) {
		if (enable)
			global_enable_mask |= ~ENABLE_SPEL_BIT_MASK;
		else
			global_enable_mask &= ENABLE_SPEL_BIT_MASK;
	}

	pr_debug("setting %s to %s\n",
		spel->name,
		enable ? "enabled" : "disabled");
	SPEL_IPC("setting %s to %s\n",
		spel->name,
		enable ? "enabled" : "disabled");

	return 0;
}

static int hw_zone_get_enable(struct spel *spel, bool *enable)
{
	u32 val;

	val = spel_readl(MMIO_OFFSET(base_address.constraints, ENABLE_SPEL_OFFSET));

	*enable = (bool)((val & spel->enable_bitmask) != 0);

	pr_debug("enabled state for %s is %s\n",
		spel->name,
		*enable ? "enabled" : "disabled");
	SPEL_IPC("enabled state for %s is %s\n",
		spel->name,
		*enable ? "enabled" : "disabled");

	return 0;
}

static u64 hw_get_spel_energy_uj(struct spel *spel)
{
	/* Convert from reported units to uJ */
	return ((u64)spel_readl(spel->address) * 1000000) >> report_units.energy;
}

static void hw_release(struct spel *spel)
{
}

static int decode_time_window(u32 *time_window)
{
	u32 window = *time_window;
	u32 final_window = 0;

	final_window = (window & (TIME_WINDOW_MASK_H << 1)) >> 1;
	final_window |= window & TIME_WINDOW_MASK_L;
	final_window = final_window * 1000;	/* report in usec */
	*time_window = final_window;

	return 0;
}

static int get_time_window_us(struct powercap_zone *pcz, int cid, u64 *time_window)
{
	u32 window;
	int ret;

	for (int i = 0; i < get_constraint_count(); i++)
		if (!strcmp(pcz->name, constraints[i].zone_name) &&
				constraints[i].cid == cid) {
			window = (u32)spel_readl(MMIO_OFFSET(
				base_address.constraints,
				constraints[i].time_window_offset));
			ret = decode_time_window(&window);
			if (ret)
				return ret;
			*time_window = (u64)window;
			pr_debug("time_window for %s.%s is %u us\n",
				pcz->name,
				constraints[i].constraint_name,
				window);
			SPEL_IPC("time_window for %s.%s is %u us\n",
				pcz->name,
				constraints[i].constraint_name,
				window);
			return 0;
		}

	return -EINVAL;
}

static int encode_time_window(u32 *time_window)
{
	u32 window;
	u32 final_window = 0;

	window = (u32)div_u64(*time_window, 1000U);	/* convert from usec to msec */

	if (window < MIN_TIME_WINDOW) {
		pr_err("time window is lower than the minimum value of %u msec\n",
			MIN_TIME_WINDOW);
		return -EINVAL;
	}
	if (window > MAX_TIME_WINDOW) {
		pr_err("time window exceeds maximum value of %u msec\n",
			MAX_TIME_WINDOW);
		return -EINVAL;
	}
	final_window = (window & TIME_WINDOW_MASK_H) << 1;
	final_window |= window & TIME_WINDOW_MASK_L;
	*time_window = final_window;

	return 0;
}

static int set_time_window_us(struct powercap_zone *pcz, int cid, u64 time_window)
{
	u32 window = (u32)time_window;
	int ret;

	for (int i = 0; i < get_constraint_count(); i++)
		if (!strcmp(pcz->name, constraints[i].zone_name) &&
				constraints[i].cid == cid) {
			ret = encode_time_window(&window);
			if (ret)
				return ret;
			spel_writel(window,
				MMIO_OFFSET(base_address.constraints,
				constraints[i].time_window_offset));
			pr_debug("setting time_window for %s.%s to %u ms\n",
				pcz->name,
				constraints[i].constraint_name,
				window);
			SPEL_IPC("setting time_window for %s.%s to %u ms\n",
				pcz->name,
				constraints[i].constraint_name,
				window);
			return 0;
		}

	return -EINVAL;
}

static int decode_power_limit_uw(u64 *power_limit_uw)
{
	u64 limit = *power_limit_uw & MAX_POWER_LIMIT;

	limit = (limit * 1000000) >> budget_units.power;
	*power_limit_uw = limit;

	return 0;
}

static int get_power_limit_uw(struct powercap_zone *pcz,
			      int cid, u64 *power_limit_uw)
{
	u64 limit;
	int ret;

	for (int i = 0; i < get_constraint_count(); i++)
		if (!strcmp(pcz->name, constraints[i].zone_name) &&
				constraints[i].cid == cid) {
			limit = (u64)spel_readl(MMIO_OFFSET(
				base_address.constraints,
				constraints[i].limit_offset));
			ret = decode_power_limit_uw(&limit);
			if (ret)
				return ret;
			*power_limit_uw = limit;
			pr_debug("power limit for %s.%s is %llu uw\n",
				pcz->name,
				constraints[i].constraint_name,
				limit);
			SPEL_IPC("power limit for %s.%s is %llu uw\n",
				pcz->name,
				constraints[i].constraint_name,
				limit);
			return 0;
		}

	return -EINVAL;
}

static int encode_power_limit(u64 power_limit_uw, u32 *power_limit)
{
	u64 temp;
	u32 limit;

	temp = DIV_ROUND_UP_ULL(power_limit_uw << budget_units.power,
				1000000ULL);

	if (temp > MAX_POWER_LIMIT) {
		pr_err("power limit exceeds maximum value: %u\n",
			MAX_POWER_LIMIT);
		return -EINVAL;
	}
	limit = (u32)temp;
	limit |= ENABLE_POWER_LIMIT;
	*power_limit = limit;

	return 0;
}

static int set_power_limit_uw(struct powercap_zone *pcz,
			      int cid, u64 power_limit_uw)
{
	u32 limit;
	int ret;

	for (int i = 0; i < get_constraint_count(); i++)
		if (!strcmp(pcz->name, constraints[i].zone_name) &&
				constraints[i].cid == cid) {
			ret = encode_power_limit(power_limit_uw, &limit);
			if (ret)
				return ret;
			spel_writel(limit,
				MMIO_OFFSET(base_address.constraints,
				constraints[i].limit_offset));
			pr_debug("setting power_limit for %s.%s to %u uw\n",
				pcz->name,
				constraints[i].constraint_name,
				limit);
			SPEL_IPC("setting power_limit for %s.%s to %u uw\n",
				pcz->name,
				constraints[i].constraint_name,
				limit);
			return 0;
		}

	return -EINVAL;
}

static const char *get_constraint_name(struct powercap_zone *pcz, int cid)
{
	for (int i = 0; i < get_constraint_count(); i++)
		if (!strcmp(pcz->name, constraints[i].zone_name) &&
				constraints[i].cid == cid)
			return constraints[i].constraint_name;

	return CONSTRAINT_NAME_UNDEFINED;
}

static int get_max_power_uw(struct powercap_zone *pcz, int id, u64 *max_power)
{
	return -EOPNOTSUPP;
}

static int spel_zone_set_enable(struct powercap_zone *pcz, bool mode)
{
	struct spel *spel = to_spel(pcz);

	pr_debug("setting %s to %s\n",
		spel->name,
		mode ? "enabled" : "disabled");
	SPEL_IPC("setting %s to %s\n",
		spel->name,
		mode ? "enabled" : "disabled");

	if (spel->ops && spel->ops->set_enable)
		return spel->ops->set_enable(spel, mode);

	pr_err("failed to set %s to %s\n",
		spel->name,
		mode ? "enabled" : "disabled");

	return -EOPNOTSUPP;
}

static int spel_zone_get_enable(struct powercap_zone *pcz, bool *mode)
{
	struct spel *spel = to_spel(pcz);

	pr_debug("getting enabled state for %s\n", spel->name);
	SPEL_IPC("getting enabled state for %s\n", spel->name);

	if (spel->ops && spel->ops->get_enable)
		return spel->ops->get_enable(spel, mode);

	pr_err("failed to get enabled state for %s\n", spel->name);

	return -EOPNOTSUPP;
}

static int __get_energy_uj(struct spel *spel, u64 *energy_uj)
{
	*energy_uj = spel->ops->get_spel_energy_uj(spel);

	return 0;
}

static int get_energy_uj(struct powercap_zone *pcz, u64 *energy_uj)
{
	int ret = 0;
	struct spel *spel = to_spel(pcz);

	mutex_lock(&spel->lock);
	ret = __get_energy_uj(spel, energy_uj);
	mutex_unlock(&spel->lock);

	return ret;
}

static void spel_clean_up_constraints(struct spel *spel)
{
	struct spel_constraints *constraint, *aux;

	if (!spel->constraints_list)
		return;

	constraint = spel->constraints_list;
	list_for_each_entry_safe(constraint, aux,
			&spel->constraints_list->pzc_list, pzc_list) {
		kfree(constraint->pzc);
		constraint->pzc = NULL;
		mutex_lock(&spel_list_lock);
		list_del(&constraint->pzc_list);
		mutex_unlock(&spel_list_lock);
		kfree(constraint);
	}

	/* Free the head node as well */
	kfree(spel->constraints_list->pzc);
	kfree(spel->constraints_list);
	spel->constraints_list = NULL;
}

static int spel_release_zone(struct powercap_zone *pcz)
{
	struct spel *spel = to_spel(pcz);
	struct spel *parent = spel->parent;

	if (!list_empty(&spel->children))
		return -EBUSY;

	if (parent)
		list_del(&spel->sibling);

	if (spel->ops)
		spel->ops->release(spel);

	mutex_lock(&spel_list_lock);
	list_del(&spel->node);
	mutex_unlock(&spel_list_lock);
	spel_clean_up_constraints(spel);
	kfree(spel);

	return 0;
}

static struct powercap_zone_constraint_ops constraint_ops = {
	.set_power_limit_uw = set_power_limit_uw,
	.get_power_limit_uw = get_power_limit_uw,
	.set_time_window_us = set_time_window_us,
	.get_time_window_us = get_time_window_us,
	.get_max_power_uw = get_max_power_uw,
	.get_name = get_constraint_name,
};

static struct powercap_zone_ops zone_ops = {
	.set_enable = spel_zone_set_enable,
	.get_enable = spel_zone_get_enable,
	.get_energy_uj = get_energy_uj,
	.release = spel_release_zone,
};

static struct spel_ops hw_ops = {
	.set_enable = hw_zone_set_enable,
	.get_enable = hw_zone_get_enable,
	.get_spel_energy_uj = hw_get_spel_energy_uj,
	.release = hw_release,
};

static void spel_ops_init(struct spel *spel, struct spel_ops *ops)
{
	if (spel) {
		INIT_LIST_HEAD(&spel->children);
		INIT_LIST_HEAD(&spel->sibling);
		spel->ops = ops;
	}
}

static void spel_unregister(struct spel *spel)
{
	if (!spel)
		return;

	pr_debug("Unregistered spel node '%s'\n", spel->zone.name);
	SPEL_IPC("Unregistered spel node '%s'\n", spel->zone.name);

	powercap_unregister_zone(sh->pct, &spel->zone);
}

static int spel_add_constraint(struct spel *spel, int constraint_id)
{
	struct spel_constraints *constraints_list;
	struct powercap_zone_constraint *pzc;
	int i = constraint_id;

	constraints_list = kzalloc(sizeof(*constraints_list), GFP_KERNEL);
	if (!constraints_list)
		return -ENOMEM;

	INIT_LIST_HEAD(&constraints_list->pzc_list);

	pzc = kzalloc(sizeof(*pzc), GFP_KERNEL);
	if (!pzc) {
		kfree(constraints_list);
		return -ENOMEM;
	}

	pzc->id = constraint_id;
	pzc->power_zone = &spel->zone;
	pzc->ops = &constraint_ops;
	constraints_list->pzc = pzc;

	mutex_lock(&spel_list_lock);
	if (!spel->constraints_list)
		spel->constraints_list = constraints_list;
	else
		list_add(&constraints_list->pzc_list, &spel->constraints_list->pzc_list);
	mutex_unlock(&spel_list_lock);

	if (!strcmp(spel->name, CHILD_NODE_SOC_NAME))
		i += 4;

	pr_debug("added constraint %s.%s\n", spel->name, constraints[i].constraint_name);
	SPEL_IPC("added constraint %s.%s\n", spel->name, constraints[i].constraint_name);

	return 0;
}

static int spel_register(const char *name, struct spel *spel, struct spel *parent)
{
	struct powercap_zone *pcz;
	struct spel *prnt = NULL;
	int ret;

	if (!sh->pct)
		return -EAGAIN;

	if (!spel)
		return -EINVAL;

	if (spel->ops && !(spel->ops->get_spel_energy_uj &&
			   spel->ops->release))
		return -EINVAL;

	if (!parent || parent == sh->root)
		prnt = NULL;
	else
		prnt = parent;

	pcz = powercap_register_zone(&spel->zone, sh->pct, name,
				     prnt ? &prnt->zone : NULL,
				     &zone_ops, spel->nr_constraints,
				     &constraint_ops);
	if (IS_ERR(pcz))
		return PTR_ERR(pcz);

	if (parent) {
		list_add_tail(&spel->sibling, &parent->children);
		spel->parent = parent;
	} else {
		list_add_tail(&spel->sibling, &sh->root->children);
		spel->parent = sh->root;
	}

	for (int i = 0; i < spel->nr_constraints; i++) {
		ret = spel_add_constraint(spel, i);
		if (ret) {
			pr_err("Failed to add constraint %d: %d\n", i, ret);
			/* Clean up previously allocated constraints */
			spel_clean_up_constraints(spel);
			/* Remove from parent's children list */
			list_del(&spel->sibling);
			/* Unregister the zone */
			powercap_unregister_zone(sh->pct, &spel->zone);
			return ret;
		}
	}

	spel->enabled = true;

	pr_debug("Registered spel node '%s'\n", spel->zone.name);
	SPEL_IPC("Registered spel node '%s'\n", spel->zone.name);

	return 0;
}

static struct spel *spel_setup_virtual(struct spel_node *hierarchy,
				       struct spel *parent)
{
	struct spel *spel;
	int ret;

	spel = kzalloc(sizeof(*spel), GFP_KERNEL);
	if (!spel)
		return ERR_PTR(-ENOMEM);

	strscpy(spel->name, hierarchy->name, SPEL_NAME_MAX);
	spel->zone.name = spel->name;
	spel->np = hierarchy->np;
	spel->enable_bitmask = hierarchy->enable_bitmask;
	spel->address = hierarchy->address;
	spel->nr_constraints = hierarchy->nr_constraints;
	mutex_init(&spel->lock);
	INIT_LIST_HEAD(&spel->pz_list);
	mutex_lock(&spel_list_lock);
	list_add(&spel->node, &spel_list);
	mutex_unlock(&spel_list_lock);
	spel_ops_init(spel, &hw_ops);
	hierarchy->spel = spel;

	ret = spel_register(hierarchy->name, spel, parent);
	if (ret) {
		pr_err("Failed to register spel node '%s': %d\n",
		       hierarchy->name, ret);
		spel_clean_up_constraints(spel);
		mutex_lock(&spel_list_lock);
		list_del(&spel->node);
		mutex_unlock(&spel_list_lock);
		kfree(spel);
		return ERR_PTR(ret);
	}

	return spel;
}

static int for_spel_each_child(struct spel_node *hierarchy,
		const struct spel_node *it, struct spel *parent)
{
	struct spel *spel = NULL;
	int i, ret;

	for (i = 0; i < sh->count; i++) {
		if (hierarchy[i].parent != it)
			continue;

		if (hierarchy[i].type == SPEL_NODE_VIRTUAL) {
			spel = spel_setup_virtual(&hierarchy[i], parent);
			/*
			 * A NULL pointer means there is no children, hence we
			 * continue without going deeper in the recursivity.
			 */
			if (!spel)
				continue;
			if (IS_ERR(spel)) {
				pr_warn("Failed to create '%s' in the hierarchy\n",
					hierarchy[i].name);
				return PTR_ERR(spel);
			}
			ret = for_spel_each_child(hierarchy, &hierarchy[i], spel);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int spel_pct_set_enable(struct powercap_control_type *pct, bool mode)
{
	int ret;

	if (sh->pct != pct)
		return -EINVAL;

	ret = hw_spel_set_enable(mode);

	if (ret)
		pr_err("Failed to %s SPEL\n", mode ? "enable" : "disable");

	return ret;
}

static int spel_pct_get_enable(struct powercap_control_type *pct, bool *mode)
{
	int ret;

	if (sh->pct != pct)
		return -EINVAL;

	ret = hw_spel_get_enable(mode);

	if (ret)
		pr_err("Failed to get SPEL mode\n");

	return ret;
}

static struct powercap_control_type_ops pc_ops = {
	.set_enable = spel_pct_set_enable,
	.get_enable = spel_pct_get_enable,
};

static int spel_create_root_node(void)
{
	/*
	 * Create a root spel node and this node won't be adding
	 * into powercap sysfs. It helps to add different spel
	 * nodes as independent node under root node.
	 */
	sh->root = kzalloc(sizeof(*sh->root), GFP_KERNEL);
	if (!sh->root)
		return -ENOMEM;

	strscpy(sh->root->name, ROOT_NODE, SPEL_NAME_MAX);
	sh->root->zone.name = sh->root->name;
	sh->root->np = NULL;
	mutex_init(&sh->root->lock);
	INIT_LIST_HEAD(&sh->root->pz_list);
	mutex_lock(&spel_list_lock);
	list_add(&sh->root->node, &spel_list);
	mutex_unlock(&spel_list_lock);
	spel_ops_init(sh->root, NULL);

	return 0;
}

static int spel_create_hierarchy(struct spel_node *hierarchy)
{
	int ret;

	if (sh->pct)
		return -EBUSY;

	sh->pct = powercap_register_control_type(NULL, "qpt", &pc_ops);
	if (IS_ERR(sh->pct)) {
		pr_err("Failed to register control type\n");
		ret = PTR_ERR(sh->pct);
		goto out_pct;
	}

	if (!hierarchy) {
		ret = -EFAULT;
		goto out_err;
	}

	ret = for_spel_each_child(hierarchy, NULL, sh->root);
	if (ret)
		goto out_err;

	return 0;

out_err:
	powercap_unregister_control_type(sh->pct);
out_pct:
	sh->pct = NULL;

	return ret;
}

static void __spel_destroy_hierarchy(struct spel *spel)
{
	struct spel *child, *aux;

	list_for_each_entry_safe(child, aux, &spel->children, sibling)
		__spel_destroy_hierarchy(child);

	/*
	 * At this point, we know all children were removed from the
	 * recursive call before
	 */
	if (spel != sh->root)
		spel_unregister(spel);

	/* Root freed manually */
	if (spel == sh->root) {
		mutex_lock(&spel_list_lock);
		list_del(&spel->node);
		mutex_unlock(&spel_list_lock);
		spel_clean_up_constraints(spel);
		kfree(spel);
	}
}

static void spel_destroy_hierarchy(void)
{
	if (!sh->pct)
		return;

	__spel_destroy_hierarchy(sh->root);

	powercap_unregister_control_type(sh->pct);

	sh->pct = NULL;

	sh->root = NULL;
}

static int of_each_spel_child(void)
{
	struct spel_node *sys = NULL;
	struct spel_node *soc = NULL;

	for (int i = 0; i < sh->count; ++i) {
		strscpy(sh->hierarchy[sh->cur_index].name,
			children_nodes[i].name, SPEL_NAME_MAX);
		sh->hierarchy[sh->cur_index].type =
			SPEL_NODE_VIRTUAL;
		sh->hierarchy[sh->cur_index].enable_bitmask =
			children_nodes[i].enable_mask;
		sh->hierarchy[sh->cur_index].address = MMIO_OFFSET(
			base_address.nodes, children_nodes[i].offset);
		sh->hierarchy[sh->cur_index].nr_constraints =
			children_nodes[i].nr_constraints;
		switch (children_nodes[i].index) {
		case CHILD_NODE_SYS:
			sys = &sh->hierarchy[sh->cur_index];
			sh->hierarchy[sh->cur_index].parent = NULL;
			break;
		case CHILD_NODE_SOC:
			soc = &sh->hierarchy[sh->cur_index];
			sh->hierarchy[sh->cur_index].parent = sys;
			break;
		case CHILD_NODE_SOC_CL0:
		case CHILD_NODE_SOC_CL1:
		case CHILD_NODE_SOC_CL2:
		case CHILD_NODE_SOC_IGPU:
		case CHILD_NODE_SOC_NSP:
		case CHILD_NODE_SOC_MMCX:
		case CHILD_NODE_SOC_INFRA:
		case CHILD_NODE_SOC_DRAM:
		case CHILD_NODE_SOC_MODEM:
			sh->hierarchy[sh->cur_index].parent = soc;
			break;
		}
		sh->cur_index++;
	}

	return 0;
}

static int spel_get_mem_resource(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	void __iomem **dest;

	for (int i = 0; i < 3; ++i) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res) {
			pr_err("Cannot get MEM resource #%d\n", i);
			return -EINVAL;
		}

		pr_debug("mem@0x%llx size:%lld\n", res->start,
				resource_size(res));
		SPEL_IPC("mem@0x%llx size:%lld\n", res->start,
				resource_size(res));

		switch (i) {
		case 0:
			dest = &base_address.nodes;
			break;
		case 1:
			dest = &base_address.constraints;
			break;
		case 2:
			dest = &base_address.config;
			break;
		default:
			pr_err("Invalid resource index: %d\n", i);
			return -EINVAL;
		}

		*dest = devm_ioremap_resource(dev, res);

		pr_debug("mem@%p\n", *dest);
		SPEL_IPC("mem@%p\n", *dest);

		if (IS_ERR(*dest)) {
			pr_err("Cannot get regmap for resource #%d\n", i);
			return PTR_ERR(*dest);
		}
	}

	return 0;
}

static int spel_init(struct platform_device *pdev)
{
	int ret;

	ipc_log = ipc_log_context_create(IPC_LOGPAGES, SPEL_HW, 0);
	if (!ipc_log)
		pr_err("%s: unable to create IPC Logging for %s\n",
			__func__, SPEL_HW);

	global_enable_mask = 0;

	ret = spel_get_mem_resource(pdev);
	if (ret)
		return ret;

	ret = spel_get_units((void __iomem *)base_address.config);
	if (ret)
		return ret;

	spel_init_constraints((void __iomem *)base_address.config);

	sh = kzalloc(sizeof(*sh), GFP_KERNEL);
	if (!sh)
		return -ENOMEM;

	ret = get_spel_count();
	if (ret <= 1) {
		pr_err("Invalid count: %d\n", ret);
		ret = -ENODEV;
		goto release_sh;
	}

	sh->count = ret;
	sh->hierarchy = kcalloc(sh->count, sizeof(*sh->hierarchy), GFP_KERNEL);
	if (!sh->hierarchy) {
		ret = -ENOMEM;
		goto release_sh;
	}

	ret = spel_create_root_node();
	if (ret < 0)
		goto release_hierarchy;

	ret = of_each_spel_child();
	if (ret) {
		pr_err("Failed to read powerzones hierarchy: %d\n", ret);
		goto release_root;
	}

	ret = spel_create_hierarchy(sh->hierarchy);
	if (ret < 0)
		goto release_root;

	ret = spel_root_create();
	if (ret)
		goto release_root;

	ret = spel_init_sysfs(&pdev->dev);
	if (ret)
		goto destroy_root;

	return 0;

destroy_root:
	spel_root_destroy();
release_root:
	mutex_lock(&spel_list_lock);
	list_del(&sh->root->node);
	mutex_unlock(&spel_list_lock);
	kfree(sh->root);
release_hierarchy:
	kfree(sh->hierarchy);
release_sh:
	kfree(sh);

	return ret;
}

static void spel_exit(void)
{
	if (spel_sysfs) {
		sysfs_remove_group(spel_root_kobj, &spel_sysfs->duration_group);

		sysfs_remove_group(spel_root_kobj, &spel_sysfs->active_limit_group);

		sysfs_remove_group(spel_root_kobj, &spel_sysfs->priority_group);

		spel_sysfs = NULL;
	}

	spel_root_destroy();

	if (!sh)
		return;

	spel_destroy_hierarchy();
	kfree(sh->hierarchy);
	kfree(sh);
	sh = NULL;

	if (ipc_log)
		ipc_log_context_destroy(ipc_log);
}

static int spel_device_probe(struct platform_device *pdev)
{
	return spel_init(pdev);
}

static void spel_device_remove(struct platform_device *pdev)
{
	spel_exit();
}

static const struct of_device_id qti_spel_device_match[] = {
	{.compatible = "qcom,power-telemetry-spel"},
	{}
};

static struct platform_driver qti_spel_module = {
	.probe          = spel_device_probe,
	.remove         = spel_device_remove,
	.driver         = {
		.name   = SPEL_HW,
		.of_match_table = qti_spel_device_match,
	},
};

module_platform_driver(qti_spel_module);

MODULE_DESCRIPTION("QTI SPEL Power Telemetry interface driver");
MODULE_LICENSE("GPL");
