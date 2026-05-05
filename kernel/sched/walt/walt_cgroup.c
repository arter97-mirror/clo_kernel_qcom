// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <trace/hooks/sched.h>
#include "walt.h"
#include "trace.h"

/*
 * These are to track following cgroups:
 * /dev/cpuctl/apps and /dev/cpuctl/apps/top-app
 */
static struct cgroup *apps_cgroup;
static struct cgroup *apps_topapp_cgroup;

static __always_inline bool is_top_app_cgroup_descendant(struct cgroup *cgroup)
{
	if (apps_topapp_cgroup &&
			cgroup_is_descendant(cgroup, apps_topapp_cgroup) &&
			(strcmp(cgroup->kn->name, "normal") == 0))
		return true;

	if (apps_topapp_cgroup &&
			cgroup_is_descendant(cgroup, apps_topapp_cgroup) &&
			(strcmp(cgroup->kn->name, "latency-sensitive") == 0))
		return true;

	return false;
}

static void walt_update_tg_pointer(struct cgroup_subsys_state *css)
{
	struct cgroup *cur_cg = css->cgroup;

	if (!strcmp(cur_cg->kn->name, "apps") && (cur_cg->level == 1)) {
		apps_cgroup = cur_cg;

	} else if (!strcmp(cur_cg->kn->name, "top-app")) {

		if (cur_cg->level == 1) {
			/*
			 * 'top-app' cgroup that is created at the root level is
			 * considered as top-app group. Example: /dev/cpuctl/top-app
			 */
			walt_init_topapp_tg(css_tg(css));

		} else if ((cur_cg->level == 2) && apps_cgroup &&
				cgroup_is_descendant(cur_cg, apps_cgroup)) {
			/*
			 * 'top-app' cgroup that is created in 'apps' cgroup is
			 * considered as top-app group. Example: /dev/cpuctl/apps/top-app
			 */
			apps_topapp_cgroup = cur_cg;
			walt_init_topapp_tg(css_tg(css));

		} else {
			/*
			 * Any other 'top-app' cgroup will not be considered as top-app group
			 */
			walt_init_tg(css_tg(css));
		}
	} else if (apps_cgroup && is_top_app_cgroup_descendant(cur_cg)) {
		walt_init_topapp_tg(css_tg(css));

	} else if (!strcmp(cur_cg->kn->name, "foreground")) {
		walt_init_foreground_tg(css_tg(css));

	} else if (!strcmp(css->cgroup->kn->name, "background")) {
		walt_init_background_tg(css_tg(css));

	} else {
		walt_init_tg(css_tg(css));
	}
}

void walt_init_tg_pointers(void)
{
	struct cgroup_subsys_state *css = &root_task_group.css;
	struct cgroup_subsys_state *top_css = css;

	rcu_read_lock();
	css_for_each_child(css, top_css)
		walt_update_tg_pointer(css);
	rcu_read_unlock();
}

static void android_rvh_cpu_cgroup_free(void *unused, struct cgroup_subsys_state *css)
{
	struct cgroup *cur_cg = css->cgroup;

	get_entry_instr(CPU_CGROUP_FREE);
	/*
	 * We are only tracking whether /dev/cpuctl/apps or
	 * /dev/cpuctl/apps/top-app cgroups are freed
	 */
	if (!strcmp(cur_cg->kn->name, "apps") && (cur_cg->level == 1))
		apps_cgroup = NULL;
	else if (!strcmp(cur_cg->kn->name, "top-app") && (cur_cg->level == 2))
		apps_topapp_cgroup = NULL;
	update_instruction_data(CPU_CGROUP_FREE);
}

static void android_rvh_cpu_cgroup_online(void *unused, struct cgroup_subsys_state *css)
{
	if (unlikely(walt_disabled))
		return;

	get_entry_instr(CPU_CGROUP_ONLINE);
	walt_update_tg_pointer(css);
	update_instruction_data(CPU_CGROUP_ONLINE);
}

static void android_rvh_cpu_cgroup_attach(void *unused,
						struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *css;
	struct task_group *tg;
	struct walt_task_group *wtg;
	unsigned int grp_id;
	int ret;

	if (unlikely(walt_disabled))
		return;

	get_entry_instr(CPU_CGROUP_ATTACH);
	cgroup_taskset_first(tset, &css);
	if (!css)
		goto out;

	tg = container_of(css, struct task_group, css);
	wtg = (struct walt_task_group *) tg->android_vendor_data1;

	cgroup_taskset_for_each(task, css, tset) {
		grp_id = wtg->colocate ? DEFAULT_CGROUP_COLOC_ID : 0;
		ret = __sched_set_group_id(task, grp_id);
		trace_sched_cgroup_attach(task, grp_id, ret);
	}
out:
	update_instruction_data(CPU_CGROUP_ATTACH);
}

void walt_cgroup_init(void)
{
	register_trace_android_rvh_cpu_cgroup_attach(android_rvh_cpu_cgroup_attach, NULL);
	register_trace_android_vh_cpu_cgroup_css_free(android_rvh_cpu_cgroup_free, NULL);
	register_trace_android_rvh_cpu_cgroup_online(android_rvh_cpu_cgroup_online, NULL);
}
