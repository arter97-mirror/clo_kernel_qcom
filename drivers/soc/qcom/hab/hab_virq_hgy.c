// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>

#include "hab.h"
#include "hab_virq.h"
#include "hab_virq_hgy.h"

/* These values are configured in device tree of HAB
 * under virt-irq option
 */
#define GH_HYP_IRQ1 0x9
#define GH_HYP_IRQ2 0xA
#define GH_HYP_IRQ3 0xB
#define GH_HYP_IRQ4 0xC
#define GH_HYP_IRQ5 0xD
#define GH_HYP_IRQ6 0xE
#define GH_HYP_IRQ7 0xF

static struct virq_handle virqid[] = {
	{ VIRQ_MISC, GH_HYP_IRQ1, VIRQ_1},
	{ VIRQ_DISP1, GH_HYP_IRQ2, VIRQ_2},
	{ VIRQ_DISP2, GH_HYP_IRQ3, VIRQ_3},
	{ VIRQ_DPRX, GH_HYP_IRQ4, VIRQ_4},
	{ VIRQ_DISP1, GH_HYP_IRQ5, VIRQ_5},
	{ VIRQ_DISP2, GH_HYP_IRQ6, VIRQ_6},
	{ VIRQ_DPRX, GH_HYP_IRQ7, VIRQ_7},
};

static int hgy_irq_index;

int hgy_get_virq_num_id(void **virqdev, int label)
{
	int i = 0;
	struct hvirq_dbl *dbl;

	if (!virqdev || *virqdev != NULL) {
		pr_err("invalid input arg for lbl %d\n", label);
		return -EINVAL;
	}

	dbl = kzalloc(sizeof(*dbl), GFP_KERNEL);
	if (dbl == NULL)
		return -ENOMEM;

	spin_lock_init(&dbl->dbl_lock);
	kref_init(&dbl->refcount);
	for (i = 0 ; i < (int)ARRAY_SIZE(virqid); i++) {
		if (label == virqid[i].virq_label) {
			dbl->id = (int)virqid[i].id;
			dbl->virtirq_num = virqid[i].virq_num;
			*virqdev = dbl;
			return 0;
		} else {
			/* continue searching */
		}
	}
	hab_virq_put(dbl);
	pr_err("virq lbl %d not supported\n", label);
	return -ENODEV;
}

/* callback function for receiving doorball */
static irqreturn_t gh_dbl_recv_cb(int irq, void *data)
{
	int ret = 0;
	unsigned long flags;
	struct hvirq_dbl *dbl;

	dbl = (struct hvirq_dbl *)data;

	spin_lock_irqsave(&dbl->dbl_lock, flags);
	if (dbl->efd != NULL) {
		pr_debug("fd is %d for id %d lbl %d\n", dbl->fd, dbl->id, dbl->virtirq_label);
		eventfd_signal(dbl->efd);
	} else {
		/* no eventfd registered */
	}
	dbl->virq_recv++;
	spin_unlock_irqrestore(&dbl->dbl_lock, flags);

	/* Eventually call the client cb function */
	if ((dbl->client_cb != NULL) && (dbl->client_pdata != NULL)) {
		ret = dbl->client_cb(irq, dbl->client_pdata, dbl->flags);
		if (ret != 0) {
			pr_err("cb fail for id %d lbl %d ret: %d\n", dbl->id,
					dbl->virtirq_label, ret);
		} else {
			/* callback succeeded */
		}
	} else {
		pr_err("client cb not registered for id %d lbl %d ret: %d\n", dbl->id,
				dbl->virtirq_label, ret);
	}

	return IRQ_HANDLED;
}

int hgy_virq_tx_register(struct hvirq_dbl *dbl, int dbl_label)
{
	return -ENODEV;
}

int hgy_virq_rx_register(struct hvirq_dbl *dbl, int dbl_label)
{
	int ret;

	pr_debug("irq = %d id = %d\n", dbl->irq, dbl->id);
	snprintf(dbl->virtirq_name, sizeof(dbl->virtirq_name), "hab_virq_%d", dbl->virtirq_num);
	ret = request_irq(dbl->irq, gh_dbl_recv_cb, IRQF_SHARED, dbl->virtirq_name, (void *)dbl);

	if (ret) {
		pr_err("request_irq failed ret %d\n", ret);
		return ret;
	} else {
		/* registered successfully */
	}

	return ret;
}

int hgy_virq_send(struct hvirq_dbl *dbl)
{
	return -ENODEV;
}

int hgy_virq_tx_unregister(struct hvirq_dbl *dbl)
{
	return -ENODEV;
}

int hgy_virq_rx_unregister(struct hvirq_dbl *dbl)
{
	free_irq(dbl->irq, (void *)dbl);
	return 0;
}

static int qcom_virt_hgy_irq_probe(struct platform_device *pdev)
{
	int ret = 0;
	int irq;
	int vmid = 0;
	struct device *dev = &pdev->dev;
	struct device_node *virq_node = dev->of_node;
	u32 label;

	vmid = hab_driver.settings.vmid_mmid_list[0].vmid;
	if (vmid == HABCFG_VMID_INVALID || vmid >= HABCFG_VMID_MAX) {
		pr_err("invalid vmid %d from settings\n", vmid);
		return -EINVAL;
	}

	/* hgy_irq_index will represent the number of irq and
	 * also irq index in dtsi
	 */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		pr_err("failed to get irq: %d\n", irq);
		return irq;
	}

	ret = of_property_read_u32(virq_node, "qcom,label", &label);
	if (ret) {
		pr_err("missing qcom,label\n");
		return ret;
	}

	pr_debug("qcom,label = 0x%x\n", label);

	if (hgy_irq_index >= ARRAY_SIZE(virqid)) {
		pr_err("irq index %d exceeds max %zu\n", hgy_irq_index,
				ARRAY_SIZE(virqid));
		return -EINVAL;
	}

	ret = hab_virq_alloc(hgy_irq_index, vmid, label, irq, NULL);
	if (ret) {
		pr_err("virq allocation failed %d\n", ret);
		return ret;
	}

	pr_info("Virtual IRQ created successfully irq = %d at hgy_irq_index = %d\n",
				irq, hgy_irq_index);
	hgy_irq_index++;

	return ret;
}

static const struct of_device_id qcom_soc_hgy_match_table[] = {
	{ .compatible = "qcom,msm-virt_irq" },
	{}
};

static struct platform_driver qcom_virt_hgy_irq_driver = {
	.probe = qcom_virt_hgy_irq_probe,
	.driver = {
		.name = "msm_virt_hgy_irq",
		.of_match_table = qcom_soc_hgy_match_table,
	},
};

int hgy_init_virt_irq(void)
{
	return platform_driver_register(&qcom_virt_hgy_irq_driver);

}
