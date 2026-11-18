// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/eventfd.h>
#include <asm/arch_timer.h>
#include "hab.h"
#include <linux/of.h>
#include "hab_virq.h"

#define VIRQ_BASE_OFFSET    4U
int irq_index;

static struct virq_handle virqid[] = {
	{ VIRQ_DISP1, -1, VIRQ_1},
	{ VIRQ_DISP2, -1, VIRQ_2},
	{ VIRQ_GFX, -1, VIRQ_3},
	{ VIRQ_MISC, -1, VIRQ_4},
	{ VIRQ_VID, -1, VIRQ_5},
	{ VIRQ_AUD, -1, VIRQ_6},
};

int qvm_get_virq_num_id(void **virqdev, int label)
{
	static int i;
	int size;
	struct hvirq_dbl *dbl;

	dbl = kzalloc(sizeof(*dbl), GFP_KERNEL);
	if (!dbl)
		return -ENOMEM;

	spin_lock_init(&dbl->dbl_lock);
	kref_init(&dbl->refcount);
	size = ARRAY_SIZE(virqid);
	if (i < size) {
		dbl->id =  (int)virqid[i].id;
		dbl->virtirq_num = virqid[i].virq_num;
		*virqdev = dbl;
		i++;
		return 0;
	}
	hab_virq_put(dbl);
	pr_err("virq lbl %d not supported\n", label);
	return -ENODEV;
}

static irqreturn_t virt_irq_handler(int irq, void *priv)
{
	struct hvirq_dbl * const dbl = priv;
	unsigned long flags;
	unsigned int status;

	spin_lock_irqsave(&dbl->dbl_lock, flags);
	status = readl(dbl->base);
	if (status == 0U) {
		spin_unlock_irqrestore(&dbl->dbl_lock, flags);
		return IRQ_NONE;
	}
	if (dbl->efd != NULL)
		eventfd_signal(dbl->efd);
	dbl->virq_recv++;
	/* to deassert interrupt status */
	writel(0x0, dbl->base);
	/* to ensure write is complete */
	mb();
	spin_unlock_irqrestore(&dbl->dbl_lock, flags);

	/* Eventually call the client cb function */
	if (dbl->client_cb != NULL)
		dbl->client_cb(irq, dbl->client_pdata, 0);

	return IRQ_HANDLED;
}

int qvm_virq_rx_register(struct hvirq_dbl *dbl, int dbl_label)
{
	int ret;

	pr_info("irq = %d id = %d irq index = %d\n", dbl->irq, dbl->id, irq_index);
	(void)snprintf(dbl->virtirq_name, sizeof(dbl->virtirq_name),
			"hab_virq_%d", dbl->virtirq_num);
	ret = request_irq(dbl->irq, virt_irq_handler, IRQF_SHARED,
			dbl->virtirq_name, (void *)dbl);
	if (ret != 0) {
		pr_err("request_irq failed ret %d\n", ret);
		return ret;
	}

	return ret;
}

int qvm_virq_rx_unregister(struct hvirq_dbl *dbl)
{
	free_irq(dbl->irq, (void *)dbl);
	return 0;
}

int qvm_virq_tx_register(struct hvirq_dbl *dbl, int dbl_label)
{
	return -ENODEV;
}

int qvm_virq_tx_unregister(struct hvirq_dbl *dbl)
{
	return -ENODEV;
}

int qvm_virq_send(struct hvirq_dbl *dbl)
{
	return -ENODEV;
}

static int qcom_virt_irq_probe(struct platform_device *pdev)
{
	int ret = 0;
	int irq;
	struct resource *mem = NULL;
	void __iomem *base = NULL;
	int vmid = 0;

	vmid = hab_driver.settings.vmid_mmid_list[0].vmid;

	/* irq_index will represent the number of irq and also irq index in dtsi */
	while ((irq = platform_get_irq(pdev, irq_index)) >= 0) {
		if (irq_index == 0) {
			/* memory location used to acknowledge/clear interrupt */
			mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
			if (mem == NULL) {
				pr_err("can not get io mem resource for virt_irq\n");
				return -EINVAL;
			}

			base = devm_ioremap(&pdev->dev, mem->start, resource_size(mem));
			if (!base) {
				pr_err("can not get ioremap resource for virt_irq\n");
				return -ENXIO;
			}
		} else {
			/* added 0x4 to get status loc for next interrupt */
			base = base + 0x4;
		}
		ret = hab_virq_alloc(irq_index, vmid, -1, irq, base);
		if (ret) {
			pr_err("virq allocation failed for irq_index %d\n", irq_index);
			return ret;
		}
		irq_index++;
	}
	pr_info("%d Virtual IRQ created successfully\n", irq_index);
	return ret;
}

static const struct of_device_id qcom_soc_match_table[] = {
	{ .compatible = "qcom,msm-virt_irq" },
	{}
};

static struct platform_driver qcom_virt_irq_driver = {
	.probe = qcom_virt_irq_probe,
	.driver = {
		.name = "msm_virt_irq",
		.of_match_table = qcom_soc_match_table,
	},
};

int qvm_init_virt_irq(void)
{
	return platform_driver_register(&qcom_virt_irq_driver);
}
