// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/msm_pcie.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "../pci.h"

#define PRSNT_HP_MAX_RETRIES		5
#define PRSNT_HP_RETRY_DELAY_MS		100

struct pci_prsnt_hp {
	/* PCIe root complex port; child endpoints enumerate below it */
	struct pci_dev *rc_dev;
	/* platform device */
	struct device *dev;
	/* PCIe controller instance; assumed == PCI domain on MSM controllers */
	u32 pci_instance;
	/* PRSNT# presence-detect GPIO */
	struct gpio_desc *prsnt_gpio;
	/* IRQ mapped from prsnt_gpio; cached for symmetric enable/disable */
	int irq;
	/* true if enable_irq_wake() succeeded in probe */
	bool wake_enabled;
};

static int pci_prsnt_hp_remove_children(struct pci_prsnt_hp *hp)
{
	struct pci_dev *child;

	pci_lock_rescan_remove();
	if (!hp->rc_dev->subordinate) {
		pci_unlock_rescan_remove();
		dev_err(hp->dev, "No subordinate bus found (instance %u)\n",
			hp->pci_instance);
		return -ENODEV;
	}

	/*
	 * GL9767 bridge dynamically reconfigures its PCIe endpoint on
	 * card insertion/removal (e.g., switches to NVMe endpoint for SDXC),
	 * causing a link-down and topology change.
	 *
	 * As part of hotplug handling, we must explicitly remove all existing
	 * child devices to:
	 *  - cleanly unregister old endpoints
	 *  - avoid stale PCI state
	 *  - allow proper re-enumeration after link retrain and rescan
	 */
	while (!list_empty(&hp->rc_dev->subordinate->devices)) {
		child = list_last_entry(&hp->rc_dev->subordinate->devices,
					 struct pci_dev, bus_list);
		pci_dev_get(child);
		/*
		 * pci_stop_and_remove_bus_device_locked() acquires the
		 * rescan/remove lock itself, so it must be released here
		 * before calling it and re-acquired afterwards to continue
		 * the list walk; holding it across the call would deadlock.
		 */
		pci_unlock_rescan_remove();
		dev_dbg(hp->dev, "removing child %s\n", pci_name(child));
		/*
		 * pci_stop_and_remove_bus_device_locked() does not mark the device
		 * disconnected on its own; without this call, any config-space access
		 * racing the removal (e.g. from a driver's ->remove()) would still try
		 * to reach hardware that the GL9767 is in the process of
		 * switching away from.
		 */
		pci_dev_set_disconnected(child, NULL);
		pci_stop_and_remove_bus_device_locked(child);
		pci_dev_put(child);
		pci_lock_rescan_remove();
	}
	pci_unlock_rescan_remove();

	return 0;
}

static int pci_prsnt_hp_retrain_link(struct pci_prsnt_hp *hp)
{
	int ret, retry = 0;

	/*
	 * Bring down the link to allow the bridge chip to complete its
	 * internal switch between Card Reader and SD Express modes.
	 */
	dev_dbg(hp->dev, "suspending link (instance %u)\n", hp->pci_instance);
	ret = msm_pcie_pm_control(MSM_PCIE_SUSPEND, hp->rc_dev->bus->number,
				  hp->rc_dev, NULL, MSM_PCIE_CONFIG_FORCE_SUSP);
	if (ret) {
		dev_err(hp->dev, "Link suspend failed (instance %u): %d\n",
			hp->pci_instance, ret);
		return ret;
	}

	/*
	 * Retrain the link; GL9767 internal switching makes the first attempt
	 * unreliable, so retry up to PRSNT_HP_MAX_RETRIES times with a
	 * settling delay between each attempt.
	 */
	do {
		dev_dbg(hp->dev, "retrain attempt %d/%d\n",
			retry + 1, PRSNT_HP_MAX_RETRIES);
		ret = msm_pcie_pm_control(MSM_PCIE_RESUME, hp->rc_dev->bus->number,
					  hp->rc_dev, NULL, 0);
		if (!ret)
			break;
		retry++;
		if (retry < PRSNT_HP_MAX_RETRIES)
			msleep(PRSNT_HP_RETRY_DELAY_MS);
	} while (retry < PRSNT_HP_MAX_RETRIES);

	if (ret) {
		dev_err(hp->dev, "Link retrain failed after %d retries (instance %u): %d\n",
			retry, hp->pci_instance, ret);
		return ret;
	}

	dev_dbg(hp->dev, "link retrained after %d retries\n", retry);
	return 0;
}

static void pci_prsnt_hp_rescan(struct pci_prsnt_hp *hp)
{
	pci_lock_rescan_remove();
	dev_dbg(hp->dev, "rescanning bus after link retrain\n");
	/*
	 * GL9767 reconfigures its PCIe endpoint on card insertion/removal,
	 * causing topology change (bridge <-> NVMe).
	 *
	 * After link retraining, rescan is required to detect and enumerate
	 * the new endpoint and bind the correct driver.
	 */
	pci_rescan_bus(hp->rc_dev->bus);
	if (hp->rc_dev->subordinate)
		dev_dbg(hp->dev, "rescan complete: %s\n",
			list_empty(&hp->rc_dev->subordinate->devices) ?
			"no devices" : "device(s) found");
	else
		dev_dbg(hp->dev, "rescan complete: no subordinate bus\n");
	pci_unlock_rescan_remove();
}

static void pci_prsnt_hp_enable_aspm(struct pci_prsnt_hp *hp)
{
	int ret;

	/*
	 * Re-enable ASPM after rescan to restore PCIe low power states
	 * affected during link retraining. The new pci_dev is only created
	 * during the resume phase, so ASPM cannot be armed until enumeration
	 * is done.
	 */
	ret = msm_pcie_pm_control(MSM_PCIE_ENABLE_ASPM, hp->rc_dev->bus->number,
				  hp->rc_dev, NULL, 0);
	if (ret)
		dev_warn(hp->dev, "ASPM re-enable failed (instance %u): %d\n",
			 hp->pci_instance, ret);
}

static irqreturn_t pci_prsnt_hp_irq_handler(int irq, void *dev_id)
{
	struct pci_prsnt_hp *hp = dev_id;
	int ret;

	dev_dbg(hp->dev, "PRSNT# IRQ: hotplug cycle start (instance %u)\n",
		hp->pci_instance);

	ret = pci_prsnt_hp_remove_children(hp);
	if (ret)
		return IRQ_HANDLED;

	ret = pci_prsnt_hp_retrain_link(hp);
	if (ret) {
		/*
		 * Link never came back up. Rescan anyway so the bus is not
		 * left permanently empty with the link down; the next
		 * PRSNT# edge retries the full cycle regardless of whether
		 * this rescan finds anything.
		 */
		pci_prsnt_hp_rescan(hp);
		return IRQ_HANDLED;
	}

	pci_prsnt_hp_rescan(hp);
	pci_prsnt_hp_enable_aspm(hp);

	dev_info(hp->dev, "hotplug cycle complete (instance %u)\n", hp->pci_instance);
	return IRQ_HANDLED;
}

static int pci_prsnt_hp_probe(struct platform_device *pdev)
{
	struct pci_prsnt_hp *hp;
	int ret;

	hp = devm_kzalloc(&pdev->dev, sizeof(*hp), GFP_KERNEL);
	if (!hp)
		return -ENOMEM;

	hp->dev = &pdev->dev;

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,pcie-instance",
				   &hp->pci_instance);
	if (ret) {
		dev_err(hp->dev, "qcom,pcie-instance missing\n");
		return -EINVAL;
	}

	hp->prsnt_gpio = devm_gpiod_get_from_of_node(&pdev->dev,
						      pdev->dev.of_node,
						      "qcom,card-presence-pin",
						      0, GPIOD_IN, "PRSNT#");
	if (IS_ERR(hp->prsnt_gpio)) {
		dev_err(hp->dev, "Failed to get PRSNT# GPIO: %ld\n",
			PTR_ERR(hp->prsnt_gpio));
		return PTR_ERR(hp->prsnt_gpio);
	}

	ret = msm_pcie_enumerate(hp->pci_instance);
	if (ret) {
		dev_err(hp->dev, "PCIe enumeration failed: %d\n", ret);
		return ret;
	}

	/*
	 * Root complex port is always at bus 0, slot 0 on MSM PCIe controllers.
	 * pci_instance (from qcom,pcie-instance) is passed as the PCI domain
	 * number here: on MSM controllers the controller instance index and
	 * the PCI domain number are the same value by construction.
	 */
	hp->rc_dev = pci_get_domain_bus_and_slot(hp->pci_instance, 0, 0);
	if (!hp->rc_dev) {
		dev_err(hp->dev, "PCI device not found after enumeration\n");
		return -ENODEV;
	}

	hp->irq = gpiod_to_irq(hp->prsnt_gpio);
	if (hp->irq < 0) {
		dev_err(hp->dev, "Failed to map PRSNT# GPIO to IRQ: %d\n", hp->irq);
		ret = hp->irq;
		goto err_put_pci_dev;
	}

	/*
	 * Rising edge (PRSNT# de-asserted) is the only trigger needed for all
	 * runtime hotplug events. The GL9767 de-asserts PRSNT# whenever it
	 * switches its internal PCIe path — both on SDXC card insertion
	 * (Card Reader -> NVMe endpoint) and on SDXC card removal
	 * (NVMe -> Card Reader endpoint). The IRQ handler is endpoint-agnostic:
	 * it removes whatever is currently enumerated, retrains the link, and
	 * rescans, so the same rising-edge IRQ covers insertion, removal, and
	 * re-insertion equally.
	 *
	 * Falling-edge detection is not needed for runtime events because both
	 * transitions produce a rising edge. The only case that never generates
	 * a rising edge is the initial card presence at boot; that is handled
	 * explicitly by msm_pcie_enumerate() in probe().
	 */
	ret = devm_request_threaded_irq(hp->dev, hp->irq, NULL,
					pci_prsnt_hp_irq_handler,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"PRSNT", hp);
	if (ret < 0) {
		dev_err(hp->dev, "IRQ request failed: %d\n", ret);
		goto err_put_pci_dev;
	}

	ret = enable_irq_wake(hp->irq);
	if (ret)
		dev_warn(hp->dev, "enable_irq_wake failed: %d\n", ret);
	else
		hp->wake_enabled = true;

	dev_set_drvdata(&pdev->dev, hp);
	dev_info(hp->dev, "PRSNT hotplug ready: irq=%d instance=%u\n",
		 hp->irq, hp->pci_instance);
	return 0;

err_put_pci_dev:
	pci_dev_put(hp->rc_dev);
	return ret;
}

static int pci_prsnt_hp_remove(struct platform_device *pdev)
{
	struct pci_prsnt_hp *hp = dev_get_drvdata(&pdev->dev);

	dev_dbg(hp->dev, "removing PRSNT hotplug driver (instance %u)\n",
		hp->pci_instance);

	/*
	 * Quiesce the IRQ synchronously before releasing hp->rc_dev.
	 * devm_request_threaded_irq() tears down the IRQ only after
	 * remove() returns; without an explicit disable here the handler
	 * can fire against a freed pci_dev in that window.
	 */
	if (hp->wake_enabled)
		disable_irq_wake(hp->irq);
	disable_irq(hp->irq);
	pci_dev_put(hp->rc_dev);
	return 0;
}

static const struct of_device_id pci_prsnt_hp_match[] = {
	{ .compatible = "qcom,pci-prsnt-hp", },
	{}
};
MODULE_DEVICE_TABLE(of, pci_prsnt_hp_match);

static struct platform_driver pci_prsnt_hp_platform_driver = {
	.probe  = pci_prsnt_hp_probe,
	.remove = pci_prsnt_hp_remove,
	.driver = {
		.name           = "pci-prsnt-hp",
		.of_match_table = pci_prsnt_hp_match,
		.probe_type     = PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(pci_prsnt_hp_platform_driver);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. PCI PRSNT hotplug driver");
MODULE_LICENSE("GPL");
