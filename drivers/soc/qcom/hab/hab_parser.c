// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2018, 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/of.h>
#include "hab.h"
#include "hab_virq.h"

#define HAB_MMID_RANGE_DIVISOR  100U
#define HAB_VMIDS_MAX           16U
#define HAB_ROLE_BE             "be"

struct local_virq virqsettings = {0};
/*
 * set valid mmid value in tbl to show this is valid entry. All inputs here are
 * normalized to 1 based integer
 */
static int fill_vmid_mmid_tbl(struct vmid_mmid_desc *tbl, int32_t vm_start,
				   int32_t vm_range, int32_t mmid_start,
				   int32_t mmid_range, int32_t be, int kernel_only,
				   bool dma_coherent)
{
	int i, j;

	for (i = vm_start; i < (vm_start + vm_range); i++) {
		tbl[i].vmid = i; /* set valid vmid value to make it usable */
		for (j = mmid_start; j < (mmid_start + mmid_range); j++) {
			/* sanity check */
			if (tbl[i].mmid[j] != HABCFG_VMID_INVALID) {
				pr_err("overwrite previous setting vmid %d, mmid %d, be %d, kernel only %d\n",
					i, j, tbl[i].is_listener[j], tbl[i].kernel_only[j]);
			}
			tbl[i].mmid[j] = j;
			tbl[i].is_listener[j] = be; /* BE IS listen */
			tbl[i].kernel_only[j] = kernel_only;
			tbl[i].dma_coherent[j] = dma_coherent;
		}
	}

	return 0;
}

void dump_settings(struct local_vmid *settings)
{
	pr_debug("self vmid is %d\n", settings->self);
}


#ifdef CONFIG_MSM_VHOST_HAB
int fill_default_gvm_settings(struct local_vmid *settings, int vmid_default,
		int mmid_start, int mmid_end)
{
	const int32_t be = HABCFG_BE_TRUE;
	const int32_t range = 1;
	const int32_t vmremote = vmid_default;
	/* pchan is not kernel only by default */
	const int32_t kernel_only = 0;
	/* dma_coherent is false by default */
	const bool dma_coherent = false;

	/* default gvm always talks to host as vm0 */
	settings->self = 0;
	return fill_vmid_mmid_tbl(settings->vmid_mmid_list, vmremote, range,
		(mmid_start / (int)HAB_MMID_RANGE_DIVISOR),
		(((mmid_end - mmid_start) / (int)HAB_MMID_RANGE_DIVISOR) + 1),
		be, kernel_only, dma_coherent);
}
#else
int fill_default_gvm_settings(struct local_vmid *settings, int vmid_local,
		int mmid_start, int mmid_end)
{
	const int32_t be = HABCFG_BE_FALSE;
	const int32_t range = 1;
	const int32_t vmremote = 0; /* default to host[0] as local is guest[2] */
	const int32_t kernel_only = 0;
	const bool dma_coherent = false;

	settings->self = vmid_local;
	/* default gvm always talks to host as vm0 */
	return fill_vmid_mmid_tbl(settings->vmid_mmid_list, vmremote, range,
		(mmid_start / (int)HAB_MMID_RANGE_DIVISOR),
		(((mmid_end - mmid_start) / (int)HAB_MMID_RANGE_DIVISOR) + 1),
		be, kernel_only, dma_coherent);
}
#endif

int hab_count_pchan(void)
{
	int result, i;
	struct device_node *hab_node;
	struct device_node *mmid_grp_node;
	int32_t grp_start_id;
	u32 tmp;
	int count = 0;

	/* parse device tree*/
	hab_node = of_find_compatible_node(NULL, NULL, "qcom,hab");
	if (hab_node == NULL) {
		pr_err("no hab device tree node\n");
		return -ENODEV;
	}

	for_each_child_of_node(hab_node, mmid_grp_node) {
		/* read the group starting id */
		result = of_property_read_u32(mmid_grp_node,
				"grp-start-id", &tmp);
		if (result != 0) {
			pr_err("failed to read grp-start-id, result = %d\n",
				result);
			return result;
		}

		grp_start_id = (int32_t)tmp;
		grp_start_id++;
		for (i = 0; i < hab_driver.ndevices; i++) {
			if (hab_driver.devp[i].id == (u32)grp_start_id) {
				count++;
				grp_start_id++;
			}
		}
	}
	return count;
}

/* device tree based parser */
static int hab_parse_dt(struct local_vmid *settings)
{
	int result, i;
	struct device_node *hab_node;
	struct device_node *mmid_grp_node;
	const char *role = NULL;
	u32 vmids[HAB_VMIDS_MAX];
	int vmids_num;
	u32 tmp;
	int32_t grp_start_id, be;
	int kernel_only;
	bool dma_coherent;
	struct property *prop;

	/* parse device tree*/
	pr_debug("parsing hab node in device tree...\n");
	hab_node = of_find_compatible_node(NULL, NULL, "qcom,hab");
	if (hab_node == NULL) {
		pr_err("no hab device tree node\n");
		return -ENODEV;
	}

	/* read the local vmid of this VM, like 0 for host, 1 for AGL GVM */
	result = of_property_read_u32(hab_node, "vmid", &tmp);
	if (result != 0) {
		pr_err("failed to read local vmid, result = %d\n", result);
		return result;
	}

	pr_debug("local vmid = %d\n", tmp);
	settings->self = (int)tmp;

	prop = of_find_property(hab_node, "PCHAN_RX_PENDING_SZ_MAX", NULL);
	if (prop != NULL) {
		result = of_property_read_u32(hab_node, "RX_PENDING_SZ_MAX", &tmp);
		if (result != 0) {
			pr_err("error %d getting pending msg mem size limit\n", result);
		} else {
			hab_driver.pchan_rx_pending_sz_max = (int)tmp;
			pr_debug("setting pending msg mem limit to %d\n", tmp);
		}
	}

	prop = of_find_property(hab_node, "VCHAN_RX_PENDING_CNT_MAX", NULL);
	if (prop != NULL) {
		result = of_property_read_u32(hab_node, "VCHAN_RX_PENDING_CNT_MAX", &tmp);
		if (result != 0) {
			pr_err("error %d getting pending msg cnt limit\n", result);
		} else {
			hab_driver.vchan_rx_pending_cnt_max = (int)tmp;
			pr_debug("setting pending msg cnt limit to %d\n", tmp);
		}
	}

	for_each_child_of_node(hab_node, mmid_grp_node) {
		/* read the group starting id */
		result = of_property_read_u32(mmid_grp_node,
				"grp-start-id", &tmp);
		if (result != 0) {
			pr_err("failed to read grp-start-id, result = %d\n",
				result);
			return result;
		}

		pr_debug("grp-start-id = %d\n", tmp);
		grp_start_id = (int32_t)tmp;

		if (of_property_read_bool(mmid_grp_node, "dma-coherent"))
			dma_coherent = true;
		else
			dma_coherent = false;
		pr_debug("coherent is %d in mmid grp %d\n", dma_coherent, tmp);

		/* read the role(fe/be) of these pchans in this mmid group */
		result = of_property_read_string(mmid_grp_node, "role", &role);
		if (result != 0) {
			pr_err("failed to get role, result = %d\n", result);
			return result;
		}

		pr_debug("local role of this mmid group is %s\n", role);
		if (strcmp(role, HAB_ROLE_BE) == 0)
			be = 1;
		else
			be = 0;

		/* read the remote vmids for these pchans in this mmid group */
		vmids_num = of_property_count_elems_of_size(mmid_grp_node,
					"remote-vmids", sizeof(u32));

		result = of_property_read_u32_array(mmid_grp_node,
					"remote-vmids", vmids, (size_t)vmids_num);
		if (result != 0) {
			pr_err("failed to read remote-vmids, result = %d\n",
				result);
			return result;
		}

		/* check the kernel_only flag for these pchans in this mmid group */
		result = of_property_read_bool(mmid_grp_node, "kernel_only");
		if (result) {
			kernel_only = 1;
			pr_debug("kernel_only flag is set for this mmid group\n");
		} else {
			kernel_only = 0;
			pr_debug("kernel_only flag is not set for this mmid group\n");
		}

		for (i = 0; i < vmids_num; i++) {
			pr_debug("vmids_num = %d, vmids[%d] = %d\n",
				vmids_num, i, vmids[i]);

			result = fill_vmid_mmid_tbl(
					settings->vmid_mmid_list,
					(int32_t)vmids[i], 1,
					grp_start_id / (int32_t)HAB_MMID_RANGE_DIVISOR,
					1, be, kernel_only, dma_coherent);
			if (result != 0) {
				pr_err("fill_vmid_mmid_tbl failed\n");
				return result;
			}
		}

	}

	dump_settings(settings);
	return 0;
}

/*
 * 0: successful
 * negative: various failure core
 */
int hab_parse(struct local_vmid *settings)
{
	int ret;

	ret = hab_parse_dt(settings);

	return ret;
}
