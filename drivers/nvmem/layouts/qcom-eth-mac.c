// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
/*
 * Qualcomm Ethernet MAC address nvmem layout driver
 *
 * Supports cells where one or more output bytes are non-contiguous in eFuse.
 * Each cell node may carry "qcom,byte-fixups" — an array of <byte_idx src_offset>
 * pairs. At add_cells time the driver parses each pair from DT and stores the
 * src_offset; read_post_process reads the nvmem device at that offset and
 * applies the replacement on every cell read.
 *
 * Example (SA8797P eth1 MAC, byte[5] lives at eFuse offset 0x326):
 *   qcom,byte-fixups = <5 0x326>;
 *
 * Duplicated byte index is not allowed.
 * 4 byte word aligned and little-endian byte are used
 */
#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#define QCOM_ETH_MAC_MAX_FIXUPS	ETH_ALEN

/* cells per qcom,byte-fixups entry: <byte_idx  src_offset> */
#define QCOM_ETH_MAC_FIXUP_NCELLS    2U

/* qfprom register reads are 4-byte word aligned */
#define QCOM_QFPROM_WORD_SIZE        4U


struct qcom_eth_mac_fixup {
	u8  byte_idx;
	u32 src_offset;
	u8  byte_in_word;
};

struct qcom_eth_mac_priv {
	u32 num_fixups;
	struct nvmem_device *nvmem;
	struct device *dev;
	struct qcom_eth_mac_fixup fixups[QCOM_ETH_MAC_MAX_FIXUPS];
};

static int qcom_eth_mac_post_process(void *priv, const char *id, int index,
				     unsigned int offset, void *buf, size_t bytes)
{
	struct qcom_eth_mac_priv *p = priv;
	u8 word[QCOM_QFPROM_WORD_SIZE];
	u32 i;
	int ret;

	if (!p)
		return 0;

	for (i = 0; i < p->num_fixups; i++) {
		if (p->fixups[i].byte_idx >= bytes)
			return -EINVAL;

		ret = nvmem_device_read(p->nvmem, p->fixups[i].src_offset,
					sizeof(word), word);
		if (ret < 0)
			return ret;
		if (ret != (int)sizeof(word))
			return -EIO;

		dev_dbg(p->dev, "post_process fixup[%u]: offset=0x%x word=%02x %02x %02x %02x byte_idx=%u -> 0x%02x\n",
			i, p->fixups[i].src_offset,
			word[0], word[1], word[2], word[3],
			p->fixups[i].byte_idx, word[p->fixups[i].byte_in_word]);

		((u8 *)buf)[p->fixups[i].byte_idx] = word[p->fixups[i].byte_in_word];
	}
	return 0;
}

static int qcom_eth_mac_parse_fixups(struct device *dev,
				     struct device_node *child,
				     struct nvmem_cell_info *info,
				     resource_size_t nvmem_size)
{
	u32 raw[QCOM_ETH_MAC_MAX_FIXUPS * QCOM_ETH_MAC_FIXUP_NCELLS];
	struct qcom_eth_mac_priv *priv;
	int count, ret, i, j;

	count = of_property_count_u32_elems(child, "qcom,byte-fixups");
	if (count < 0)
		return 0;	/* property absent — no fixup needed */

	if (count == 0 || count % QCOM_ETH_MAC_FIXUP_NCELLS != 0) {
		dev_err(dev, "%pOF: qcom,byte-fixups must be pairs of <byte_idx src_offset>\n",
			child);
		return -EINVAL;
	}
	if (count > QCOM_ETH_MAC_MAX_FIXUPS * QCOM_ETH_MAC_FIXUP_NCELLS) {
		dev_err(dev, "%pOF: qcom,byte-fixups exceeds maximum (%d pairs)\n",
			child, QCOM_ETH_MAC_MAX_FIXUPS);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(child, "qcom,byte-fixups", raw, count);
	if (ret)
		return ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->num_fixups = count / QCOM_ETH_MAC_FIXUP_NCELLS;

	for (i = 0; i < priv->num_fixups; i++) {
		u32 byte_idx     = raw[QCOM_ETH_MAC_FIXUP_NCELLS * i];
		u32 src_offset   = raw[QCOM_ETH_MAC_FIXUP_NCELLS * i + 1];
		/* qfprom word_size=4: align down to 4-byte boundary */
		u32 aligned      = src_offset & ~(QCOM_QFPROM_WORD_SIZE - 1U);
		u32 byte_in_word = src_offset - aligned;

		if (byte_idx >= info->bytes) {
			dev_err(dev, "%pOF: byte_idx %u out of cell range (%u bytes)\n",
					child, byte_idx, info->bytes);
			return -EINVAL;
		}

		if (nvmem_size && src_offset >= nvmem_size) {
			dev_err(dev, "%pOF: src_offset 0x%x exceeds nvmem size 0x%llx\n",
					child, src_offset, (unsigned long long)nvmem_size);
			return -EINVAL;
		}

		/* Check for duplicate byte_idx */
		for (j = 0; j < i; j++) {
			if (priv->fixups[j].byte_idx == (u8)byte_idx) {
				dev_err(dev, "%pOF: duplicate byte_idx %u in qcom,byte-fixups\n",
					child, byte_idx);
				return -EINVAL;
			}
		}

		priv->fixups[i].byte_idx    = (u8)byte_idx;
		priv->fixups[i].src_offset  = aligned;
		priv->fixups[i].byte_in_word = (u8)byte_in_word;

	}

	info->read_post_process = qcom_eth_mac_post_process;
	info->priv = priv;
	return 0;
}

static int qcom_eth_mac_add_cells(struct nvmem_layout *layout)
{
	struct nvmem_device *nvmem = layout->nvmem;
	struct device *dev = &layout->dev;
	struct device_node *layout_node;
	struct device_node *efuse_np;
	struct device_node *child;
	resource_size_t nvmem_size = 0;
	int ret = 0;

	dev_dbg(dev, "add_cells: entry\n");

	layout_node = of_nvmem_layout_get_container(nvmem);
	if (!layout_node) {
		dev_err(dev, "add_cells: no nvmem-layout container node\n");
		return -ENOENT;
	}

	efuse_np = of_get_parent(layout_node);
	if (efuse_np) {
		struct resource res;

		if (!of_address_to_resource(efuse_np, 0, &res))
			nvmem_size = resource_size(&res);
		of_node_put(efuse_np);
	}

	for_each_child_of_node(layout_node, child) {
		struct nvmem_cell_info info = {};
		u32 reg[2];

		if (of_property_read_u32_array(child, "reg", reg, 2)) {
			dev_err(dev, "missing reg property in %pOF\n", child);
			ret = -EINVAL;
			goto out;
		}

		info.name   = child->name;
		info.offset = reg[0];
		info.bytes  = reg[1];
		info.np     = of_node_get(child);

		dev_dbg(dev, "add_cells: cell %s offset=0x%x bytes=%u\n",
			 info.name, info.offset, info.bytes);

		ret = qcom_eth_mac_parse_fixups(dev, child, &info, nvmem_size);
		if (ret) {
			of_node_put(info.np);
			goto out;
		}

		if (info.priv) {
			((struct qcom_eth_mac_priv *)info.priv)->nvmem = nvmem;
			((struct qcom_eth_mac_priv *)info.priv)->dev = dev;
		}

		ret = nvmem_add_one_cell(nvmem, &info);
		if (ret) {
			of_node_put(info.np);
			dev_err(dev, "failed to add nvmem cell %s: %d\n",
				info.name, ret);
			goto out;
		}
		dev_dbg(dev, "add_cells: cell %s added ok\n", info.name);
		/*
		 * Mark the cell fwnode as not-a-device so fw_devlink stops
		 * blocking ethernet on this supplier link.  Must be called
		 * after nvmem_add_one_cell() so the fwnode exists.
		 * fw_devlink_refresh_fwnode() cannot be used here because the
		 * efuse device is still in probe and not yet DL_DEV_DRIVER_BOUND.
		 */
		fw_devlink_purge_absent_suppliers(of_fwnode_handle(child));
	}

out:
	of_node_put(child);
	of_node_put(layout_node);
	return ret;
}

static int qcom_eth_mac_probe(struct nvmem_layout *layout)
{
	layout->add_cells = qcom_eth_mac_add_cells;

	return nvmem_layout_register(layout);
}

static void qcom_eth_mac_remove(struct nvmem_layout *layout)
{
	nvmem_layout_unregister(layout);
}

static const struct of_device_id qcom_eth_mac_of_match[] = {
	{ .compatible = "qcom,sa8797p-eth-mac" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_eth_mac_of_match);

static struct nvmem_layout_driver qcom_eth_mac_layout = {
	.driver = {
		.name           = "qcom-eth-mac-layout",
		.of_match_table = qcom_eth_mac_of_match,
	},
	.probe  = qcom_eth_mac_probe,
	.remove = qcom_eth_mac_remove,
};
module_nvmem_layout_driver(qcom_eth_mac_layout);

MODULE_DESCRIPTION("Qualcomm Ethernet MAC nvmem layout driver");
MODULE_LICENSE("GPL");
