// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/kmemleak.h>
#include <trace/hooks/sched.h>
#include "walt.h"
#include "trace.h"

/*
 * These are to track following cgroups:
 * /dev/cpuctl/apps and /dev/cpuctl/apps/top-app
 */
static struct cgroup *apps_cgroup;
static struct cgroup *apps_topapp_cgroup;

/*
 * Root cgroup for adaptive cgroups. A cgroup is classified as
 * walt_adaptive_cgroup when it is created in: /dev/cpuctl/walt/
 */
static struct cgroup *root_adaptive_cgroup;

enum walt_adaptive_cgroup_states {
	WACG_STATE_INITED,
	WACG_STATE_ALIVE,
	WACG_STATE_DEAD
};

/* Lock to synchronize tunable updates for adapative cgroups */
DEFINE_MUTEX(wacg_tunables_update_lock);

/* Lock to synchronize between adapative cgroup create and free work */
static DEFINE_MUTEX(wacg_work_lock);

/* Number of adaptive cgroup free works that are queued to run */
static atomic_t nr_free_workfn_queued = ATOMIC_INIT(0);

/*
 * Completion variable to signal that all the adaptive cgroup free workers are
 * finished
 */
static DECLARE_COMPLETION(wacg_frees_done);

/* List of callbacks for the features supported by an adaptive cgroup */
static wacg_apply_feature_cb wacg_features_cb_table[WACG_MAX_FEATURES] = {
};

/* Apply adaptive cgroup features */
static inline void apply_adaptive_cgroup_features(int features, struct walt_task_struct *wts)
{
	bool enable = false;

	/* features = 0 means that we want to reset all the adaptive features for this task */
	if (!features) {
		for (int i = 0; i < WACG_MAX_FEATURES; i++) {
			wacg_apply_feature_cb func = wacg_features_cb_table[i];

			if (func)
				func(wts, 0);
		}
		wts->has_adaptive_features = false;
		return;
	}

	for (int i = 0; i < WACG_MAX_FEATURES; i++) {
		enable = features & BIT(i);
		wacg_apply_feature_cb func = wacg_features_cb_table[i];

		if (func)
			func(wts, enable);
	}
	wts->has_adaptive_features = true;
}

static int walt_adaptive_cgroup_updownmigrate_handler(const struct ctl_table *table, int write,
				void __user *buffer, size_t *lenp,
				loff_t *ppos)
{
	struct walt_adaptive_cgroup *wacg = NULL;
	int cluster_id = -1;
	int ret = -EINVAL;

	int user_input[NUM_UPDOWN_SETTINGS];
	struct ctl_table tmp_table = {
		.data	= &user_input,
		.maxlen	= sizeof(unsigned int) * NUM_UPDOWN_SETTINGS,
		.mode	= table->mode,
	};

	guard(mutex)(&wacg_tunables_update_lock);

	if (!write) {
		tmp_table.data = table->data;
		ret = proc_dointvec(&tmp_table, write, buffer, lenp, ppos);
		return ret;
	}

	/* For which cluster this handler is called? */
	if (strcmp("cluster0_updownmigrate", table->procname) == 0) {
		cluster_id = 0;
		wacg = container_of(table->data, struct walt_adaptive_cgroup,
					cluster0_updownmigrate_pct);

	} else if (strcmp("cluster1_updownmigrate", table->procname) == 0) {
		cluster_id = 1;
		wacg = container_of(table->data, struct walt_adaptive_cgroup,
					cluster1_updownmigrate_pct);

	} else if (strcmp("cluster2_updownmigrate", table->procname) == 0) {
		cluster_id = 2;
		wacg = container_of(table->data, struct walt_adaptive_cgroup,
					cluster2_updownmigrate_pct);

	} else if (strcmp("cluster3_updownmigrate", table->procname) == 0) {
		cluster_id = 3;
		wacg = container_of(table->data, struct walt_adaptive_cgroup,
					cluster3_updownmigrate_pct);
	}

	if (cluster_id < 0 || cluster_id >= num_sched_clusters)
		return -EPERM;

	ret = proc_dointvec(&tmp_table, write, buffer, lenp, ppos);
	if (ret)
		return ret;

	/* Check if the new upmigrate and downmigrate pct values are valid */
	if (user_input[0] <= 0 || user_input[0] > 100)
		return -EINVAL;

	if (user_input[1] <= 0 || user_input[1] > 100)
		return -EINVAL;

	if (user_input[0] < user_input[1])
		return -EINVAL;

	/* Update the cgroup upmigrate/downmigrate percentage and up/down capacity margins */
	wacg->capacity_down_margin[cluster_id] = SCHED_FIXEDPOINT_SCALE * 100 / user_input[1];
	wacg->capacity_up_margin[cluster_id] = SCHED_FIXEDPOINT_SCALE * 100 / user_input[0];
	((unsigned int *)table->data)[0] = user_input[0];
	((unsigned int *)table->data)[1] = user_input[1];

	return 0;
}

static int walt_adaptive_cgroup_features_handler(const struct ctl_table *table, int write,
				void __user *buffer, size_t *lenp,
				loff_t *ppos)
{

	struct cgroup_subsys_state *css = NULL;
	struct walt_adaptive_cgroup *wacg = NULL;
	struct walt_task_struct *wts;
	struct css_task_iter it;
	struct task_struct *p;
	int user_input = 0;
	int ret = -EINVAL;
	int valid_features = GENMASK_TYPE(int, WACG_MAX_FEATURES, 0);

	struct ctl_table tmp_table = {
		.data	= &user_input,
		.maxlen	= sizeof(int),
		.mode	= table->mode,
	};

	wacg = container_of(table->data, struct walt_adaptive_cgroup, features);

	if (atomic_read(&wacg->state) == WACG_STATE_DEAD)
		return -EPERM;

	guard(mutex)(&wacg_tunables_update_lock);

	if (!write) {
		tmp_table.data = table->data;
		ret = proc_dointvec(&tmp_table, write, buffer, lenp, ppos);
		return ret;
	}

	ret = proc_dointvec(&tmp_table, write, buffer, lenp, ppos);
	if (ret)
		return ret;

	/* Discard input if any unsupported feature bit is set in input */
	if (user_input & ~valid_features)
		return -EINVAL;

	if (user_input == wacg->features)
		return 0;

	wacg->features = user_input;
	/*
	 * Now we are ready to set the features for all the tasks in this cgroup
	 */
	css = wacg->css;
	css_task_iter_start(css, 0, &it);
	while ((p = css_task_iter_next(&it))) {
		wts = android_task_vendor_data(p);
		apply_adaptive_cgroup_features(user_input, wts);
	}
	css_task_iter_end(&it);

	return 0;
}

static inline void walt_adaptive_cgroup_schedule_free(struct walt_adaptive_cgroup *wacg)
{
	/*
	 * Schedules asynchronous teardown of an adaptive cgroup.
	 * Blocks future create work until teardown completes.
	 */
	atomic_inc(&nr_free_workfn_queued);
	reinit_completion(&wacg_frees_done);
	schedule_work(&wacg->cgroup_free_work);
}

static void walt_adaptive_cgroup_create_workfn(struct work_struct *work)
{
	struct walt_adaptive_cgroup *wacg = NULL;
	struct ctl_table_header *hdr = NULL;
	char path[64] = {'\0'};

	wacg = container_of(work, struct walt_adaptive_cgroup,
						cgroup_create_work);

	/* Default values for updown and downmigrate */
	wacg->cluster0_updownmigrate_pct[0] = 95;
	wacg->cluster0_updownmigrate_pct[1] = 85;
	wacg->capacity_up_margin[0] = SCHED_FIXEDPOINT_SCALE * 100 /
					wacg->cluster0_updownmigrate_pct[0];
	wacg->capacity_down_margin[0] = SCHED_FIXEDPOINT_SCALE * 100 /
					wacg->cluster0_updownmigrate_pct[1];

	wacg->cluster1_updownmigrate_pct[0] = 95;
	wacg->cluster1_updownmigrate_pct[1] = 85;
	wacg->capacity_up_margin[1] = SCHED_FIXEDPOINT_SCALE * 100 /
					wacg->cluster1_updownmigrate_pct[0];
	wacg->capacity_down_margin[1] = SCHED_FIXEDPOINT_SCALE * 100 /
					wacg->cluster1_updownmigrate_pct[1];

	wacg->cluster2_updownmigrate_pct[0] = 95;
	wacg->cluster2_updownmigrate_pct[1] = 85;
	wacg->capacity_up_margin[2] = SCHED_FIXEDPOINT_SCALE * 100 /
					wacg->cluster2_updownmigrate_pct[0];
	wacg->capacity_down_margin[2] = SCHED_FIXEDPOINT_SCALE * 100 /
					wacg->cluster2_updownmigrate_pct[1];

	wacg->cluster3_updownmigrate_pct[0] = 95;
	wacg->cluster3_updownmigrate_pct[1] = 85;
	wacg->capacity_up_margin[3] = SCHED_FIXEDPOINT_SCALE * 100 /
					wacg->cluster3_updownmigrate_pct[0];
	wacg->capacity_down_margin[3] = SCHED_FIXEDPOINT_SCALE * 100 /
					wacg->cluster3_updownmigrate_pct[1];

	/*
	 * Default value for 'features' node is 0. Each bit in this node represents
	 * a feature, so the value 0 means that no feature is set.
	 */
	wacg->features = 0;

	wacg->table[0].procname         = "features";
	wacg->table[0].data             = &wacg->features;
	wacg->table[0].maxlen           = sizeof(int);
	wacg->table[0].mode             = 0644;
	wacg->table[0].proc_handler     = walt_adaptive_cgroup_features_handler;
	wacg->table[0].extra1           = SYSCTL_ZERO;
	wacg->table[0].extra2           = SYSCTL_INT_MAX;

	wacg->table[1].procname       = "cluster0_updownmigrate";
	wacg->table[1].data           = &wacg->cluster0_updownmigrate_pct;
	wacg->table[1].maxlen         = sizeof(unsigned int) * NUM_UPDOWN_SETTINGS;
	wacg->table[1].mode           = 0644;
	wacg->table[1].proc_handler   = walt_adaptive_cgroup_updownmigrate_handler;

	wacg->table[2].procname       = "cluster1_updownmigrate";
	wacg->table[2].data           = &wacg->cluster1_updownmigrate_pct;
	wacg->table[2].maxlen         = sizeof(unsigned int) * NUM_UPDOWN_SETTINGS;
	wacg->table[2].mode           = 0644;
	wacg->table[2].proc_handler   = walt_adaptive_cgroup_updownmigrate_handler;

	wacg->table[3].procname       = "cluster2_updownmigrate";
	wacg->table[3].data           = &wacg->cluster2_updownmigrate_pct;
	wacg->table[3].maxlen         = sizeof(unsigned int) * NUM_UPDOWN_SETTINGS;
	wacg->table[3].mode           = 0644;
	wacg->table[3].proc_handler   = walt_adaptive_cgroup_updownmigrate_handler;

	wacg->table[4].procname       = "cluster3_updownmigrate";
	wacg->table[4].data           = &wacg->cluster3_updownmigrate_pct;
	wacg->table[4].maxlen         = sizeof(unsigned int) * NUM_UPDOWN_SETTINGS;
	wacg->table[4].mode           = 0644;
	wacg->table[4].proc_handler   = walt_adaptive_cgroup_updownmigrate_handler;

	/* wait for all the adaptive cgroup free work functions to finish */
	wait_for_completion(&wacg_frees_done);

	guard(mutex)(&wacg_work_lock);
	/*
	 * If the state of this adaptive cgroup is not WACG_STATE_INITED,
	 * return immediately as this means sysctl nodes are not required now.
	 * The cgroup must have been freed
	 */
	if (atomic_read(&wacg->state) != WACG_STATE_INITED) {
		css_put(wacg->css);
		return;
	}

	/* Path of this new cgroup specific tunables: /proc/sys/walt/<crgoup> */
	scnprintf(path, sizeof(path), "walt/cgroups/%s", wacg->css->cgroup->kn->name);

	/*
	 * Create the following enteries for the new cgroup:
	 * /proc/sys/walt/cgroups/<cgroup>/features
	 * /proc/sys/walt/cgroups/<cgroup>/cluster0_updownmigrate
	 * /proc/sys/walt/cgroups/<cgroup>/cluster1_updownmigrate
	 * /proc/sys/walt/cgroups/<cgroup>/cluster2_updownmigrate
	 * /proc/sys/walt/cgroups/<cgroup>/cluster3_updownmigrate
	 */
	hdr = register_sysctl(path, wacg->table);
	if (!hdr) {
		walt_adaptive_cgroup_schedule_free(wacg);
		css_put(wacg->css);
		return;
	}

	kmemleak_not_leak(hdr);
	wacg->header = hdr;
	atomic_set(&wacg->state, WACG_STATE_ALIVE);
	css_put(wacg->css);
}

/*
 * Work function that is responsible for removing the sysctl path and freeing the memory,
 * once a walt cgroup is removed.
 */
static void walt_adaptive_cgroup_free_workfn(struct work_struct *work)
{
	struct walt_adaptive_cgroup *wacg = container_of(work, struct walt_adaptive_cgroup,
						cgroup_free_work);
	struct ctl_table_header *hdr = NULL;

	guard(mutex)(&wacg_work_lock);

	hdr = wacg->header;
	if (hdr)
		unregister_sysctl_table(hdr);

	/* If nr_free_workfn_queued is 0, signal all the waiters */
	if (atomic_dec_and_test(&nr_free_workfn_queued))
		complete_all(&wacg_frees_done);

	kfree_rcu(wacg, rcu);
}

static struct walt_adaptive_cgroup *walt_adaptive_cgroup_init(struct cgroup_subsys_state *css)
{
	struct walt_adaptive_cgroup *wacg = NULL;

	if (unlikely(walt_disabled))
		return NULL;

	wacg = kzalloc(sizeof(struct walt_adaptive_cgroup), GFP_ATOMIC);
	if (!wacg)
		return NULL;

	INIT_WORK(&wacg->cgroup_free_work, walt_adaptive_cgroup_free_workfn);
	INIT_WORK(&wacg->cgroup_create_work, walt_adaptive_cgroup_create_workfn);
	atomic_set(&wacg->state, WACG_STATE_INITED);
	wacg->css = css;

	/*
	 * Take a reference of css to ensure that it is not freed before our
	 * walt_adaptive_cgroup_create_workfn is called
	 */
	css_get(css);
	schedule_work(&wacg->cgroup_create_work);
	return wacg;
}

static void walt_cgrp_remove_sysctl(struct cgroup_subsys_state *css)
{
	struct walt_task_group *wtg = (struct walt_task_group *)css_tg(css)->android_vendor_data1;
	struct walt_adaptive_cgroup *wacg = rcu_dereference_protected(wtg->adaptive_cgrp, 1);

	if (wacg) {
		int old_state = atomic_xchg(&wacg->state, WACG_STATE_DEAD);

		if (old_state == WACG_STATE_DEAD)
			return;

		rcu_assign_pointer(wtg->adaptive_cgrp, NULL);
		walt_adaptive_cgroup_schedule_free(wacg);
	}
}

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

	} else if (!strcmp(cur_cg->kn->name, "walt") && (cur_cg->level == 1)) {
		root_adaptive_cgroup = cur_cg;
		complete_all(&wacg_frees_done);

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
	 * We are tracking whether /dev/cpuctl/apps, /dev/cpuctl/apps/top-app
	 * or root adaptive cgroup is freed. If an adaptive cgroup is freed,
	 * we'll remove the corresponding sysctl enteries and free up its
	 * memory.
	 */
	if (!strcmp(cur_cg->kn->name, "apps") && (cur_cg->level == 1))
		apps_cgroup = NULL;
	else if (!strcmp(cur_cg->kn->name, "top-app") && (cur_cg->level == 2))
		apps_topapp_cgroup = NULL;
	else if (!strcmp(cur_cg->kn->name, "walt") && (cur_cg->level == 1))
		root_adaptive_cgroup = NULL;

	walt_cgrp_remove_sysctl(css);
	update_instruction_data(CPU_CGROUP_FREE);
}

static void android_rvh_cpu_cgroup_online(void *unused, struct cgroup_subsys_state *css)
{
	struct walt_adaptive_cgroup *wacg = NULL;
	struct walt_task_group *wtg = NULL;

	if (unlikely(walt_disabled))
		return;

	get_entry_instr(CPU_CGROUP_ONLINE);
	walt_update_tg_pointer(css);
	wtg = (struct walt_task_group *)css_tg(css)->android_vendor_data1;
	if (root_adaptive_cgroup && (css->cgroup != root_adaptive_cgroup) &&
			cgroup_is_descendant(css->cgroup, root_adaptive_cgroup)) {
		wacg = walt_adaptive_cgroup_init(css);
		rcu_assign_pointer(wtg->adaptive_cgrp, wacg);
	} else
		/* not a descendent of root_adaptive_cgroup, set NULL as an indicator */
		rcu_assign_pointer(wtg->adaptive_cgrp, NULL);

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
	struct walt_adaptive_cgroup *wacg = NULL;
	struct walt_task_struct *wts = NULL;

	if (unlikely(walt_disabled))
		return;

	get_entry_instr(CPU_CGROUP_ATTACH);
	cgroup_taskset_first(tset, &css);
	if (!css)
		goto out;

	rcu_read_lock();
	tg = container_of(css, struct task_group, css);
	wtg = (struct walt_task_group *) tg->android_vendor_data1;
	wacg = rcu_dereference(wtg->adaptive_cgrp);

	cgroup_taskset_for_each(task, css, tset) {
		grp_id = wtg->colocate ? DEFAULT_CGROUP_COLOC_ID : 0;
		wts = (struct walt_task_struct *)android_task_vendor_data(task);
		ret = __sched_set_group_id(task, grp_id);

		/*
		 * The destination cgroup for this task may or may not be an adaptive
		 * cgroup. If it is adaptive and has feature bits set, apply those
		 * features to the task.
		 *
		 * If the destination cgroup is not adaptive, remove any adaptive
		 * features that were previously applied to the task.
		 */
		if (wacg && wacg->features)
			apply_adaptive_cgroup_features(wacg->features, wts);
		else if (wts->has_adaptive_features)
			apply_adaptive_cgroup_features(0, wts);

		trace_sched_cgroup_attach(task, grp_id, ret);
	}
	rcu_read_unlock();
out:
	update_instruction_data(CPU_CGROUP_ATTACH);
}

void walt_cgroup_init(void)
{
	register_trace_android_rvh_cpu_cgroup_attach(android_rvh_cpu_cgroup_attach, NULL);
	register_trace_android_vh_cpu_cgroup_css_free(android_rvh_cpu_cgroup_free, NULL);
	register_trace_android_rvh_cpu_cgroup_online(android_rvh_cpu_cgroup_online, NULL);
}
