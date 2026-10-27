// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MHI PCI driver - MHI over PCI controller driver
 *
 * This module is a generic driver for registering MHI-over-PCI devices,
 * such as PCIe QCOM modems.
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/aer.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/mhi.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#define MHI_PCI_DEFAULT_BAR_NUM 0

#define MHI_POST_RESET_DELAY_MS 500

#define HEALTH_CHECK_PERIOD (HZ * 2)
#define CRASH_CHECK_PERIOD (HZ * 60)
#define MHI_PCI_AUTOSUSPEND_DELAY_MS 2000

/**
 * struct mhi_pci_dev_info - MHI PCI device specific information
 * @config: MHI controller configuration
 * @name: name of the PCI module
 * @fw: firmware path (if any)
 * @edl: emergency download mode firmware path (if any)
 * @bar_num: PCI base address register to use for MHI MMIO register space
 * @dma_data_width: DMA transfer word size (32 or 64 bits)
 * @sideband_wake: Devices using dedicated sideband GPIO for wakeup instead
 *		   of inband wake support (such as sdx24)
 * @auto_edl_load: Do not wait for sysfs triggers to proceed with image download
 *		   in Emergency Download Mode
 */
struct mhi_pci_dev_info {
	const struct mhi_controller_config *config;
	const char *name;
	const char *fw;
	const char *edl;
	unsigned int bar_num;
	unsigned int dma_data_width;
	bool sideband_wake;
	bool auto_edl_load;
};

#define MHI_CHANNEL_CONFIG_UL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_TO_DEVICE,			\
		.ee_mask = BIT(MHI_EE_AMSS),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}

#define MHI_CHANNEL_CONFIG_DL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_FROM_DEVICE,			\
		.ee_mask = BIT(MHI_EE_AMSS),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}

#define MHI_EVENT_CONFIG_CTRL(ev_ring, el_count) \
	{					\
		.num_elements = el_count,	\
		.irq_moderation_ms = 0,		\
		.irq = (ev_ring) + 1,		\
		.priority = 1,			\
		.mode = MHI_DB_BRST_DISABLE,	\
		.data_type = MHI_ER_CTRL,	\
		.hardware_event = false,	\
		.client_managed = false,	\
		.offload_channel = false,	\
	}

#define MHI_CHANNEL_CONFIG_HW_UL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_TO_DEVICE,			\
		.ee_mask = BIT(MHI_EE_AMSS),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_ENABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = true,		\
	}

#define MHI_CHANNEL_CONFIG_HW_DL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_FROM_DEVICE,			\
		.ee_mask = BIT(MHI_EE_AMSS),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_ENABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = true,		\
	}

#define MHI_CHANNEL_CONFIG_UL_SBL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_TO_DEVICE,			\
		.ee_mask = BIT(MHI_EE_SBL),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}

#define MHI_CHANNEL_CONFIG_DL_SBL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_FROM_DEVICE,			\
		.ee_mask = BIT(MHI_EE_SBL),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}

#define MHI_CHANNEL_CONFIG_UL_FP(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_TO_DEVICE,			\
		.ee_mask = BIT(MHI_EE_FP),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}

#define MHI_CHANNEL_CONFIG_DL_FP(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_FROM_DEVICE,			\
		.ee_mask = BIT(MHI_EE_FP),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}

#define MHI_EVENT_CONFIG_DATA(ev_ring, el_count) \
	{					\
		.num_elements = el_count,	\
		.irq_moderation_ms = 0,		\
		.irq = (ev_ring) + 1,		\
		.priority = 1,			\
		.mode = MHI_DB_BRST_DISABLE,	\
		.data_type = MHI_ER_DATA,	\
		.hardware_event = false,	\
		.client_managed = false,	\
		.offload_channel = false,	\
	}

#define MHI_EVENT_CONFIG_HW_DATA(ev_ring, el_count, ch_num, dbmode) \
	{					\
		.num_elements = el_count,	\
		.irq_moderation_ms = 5,		\
		.irq = (ev_ring) + 1,		\
		.priority = 1,			\
		.mode = dbmode,			\
		.data_type = MHI_ER_DATA,	\
		.hardware_event = true,		\
		.client_managed = false,	\
		.offload_channel = false,	\
		.channel = ch_num,		\
	}

static const struct mhi_channel_config modem_qcom_v1_mhi_channels[] = {
	MHI_CHANNEL_CONFIG_UL(0, "LOOPBACK", 32, 1),
	MHI_CHANNEL_CONFIG_DL(1, "LOOPBACK", 32, 1),
	MHI_CHANNEL_CONFIG_UL_SBL(2, "SAHARA", 64, 1),
	MHI_CHANNEL_CONFIG_DL_SBL(3, "SAHARA", 64, 1),
	MHI_CHANNEL_CONFIG_UL(4, "DIAG", 16, 2),
	MHI_CHANNEL_CONFIG_DL(5, "DIAG", 16, 2),
	MHI_CHANNEL_CONFIG_UL(8, "QDSS", 16, 2),
	MHI_CHANNEL_CONFIG_DL(9, "QDSS", 16, 2),
	MHI_CHANNEL_CONFIG_UL(12, "MBIM", 4, 0),
	MHI_CHANNEL_CONFIG_DL(13, "MBIM", 4, 0),
	MHI_CHANNEL_CONFIG_UL(14, "QMI", 4, 0),
	MHI_CHANNEL_CONFIG_DL(15, "QMI", 4, 0),
	MHI_CHANNEL_CONFIG_UL(18, "IP_CTRL", 8, 1),
	MHI_CHANNEL_CONFIG_DL(19, "IP_CTRL", 8, 1),
	MHI_CHANNEL_CONFIG_UL(20, "IPCR", 32, 3),
	MHI_CHANNEL_CONFIG_DL(21, "IPCR", 32, 3),
	MHI_CHANNEL_CONFIG_UL(32, "DUN", 32, 3),
	MHI_CHANNEL_CONFIG_DL(33, "DUN", 32, 3),
	MHI_CHANNEL_CONFIG_UL_FP(34, "FIREHOSE", 32, 1),
	MHI_CHANNEL_CONFIG_DL_FP(35, "FIREHOSE", 32, 1),
	MHI_CHANNEL_CONFIG_UL(46, "IP_SW0", 64, 1),
	MHI_CHANNEL_CONFIG_DL(47, "IP_SW0", 64, 1),
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0", 512, 4),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0", 512, 5),
	MHI_CHANNEL_CONFIG_HW_DL(102, "IP_HW_ADPL", 128, 6),
};

static struct mhi_event_config modem_qcom_v1_mhi_events[] = {
	/* first ring is control+data ring */
	MHI_EVENT_CONFIG_CTRL(0, 64),
	MHI_EVENT_CONFIG_DATA(1, 512),
	/* DIAG dedicated event ring */
	MHI_EVENT_CONFIG_DATA(2, 64),
	MHI_EVENT_CONFIG_DATA(3, 64),
	/* Hardware channels request dedicated hardware event rings */
	MHI_EVENT_CONFIG_HW_DATA(4, 1024, 100, MHI_DB_BRST_ENABLE),
	MHI_EVENT_CONFIG_HW_DATA(5, 2048, 101, MHI_DB_BRST_ENABLE),
	MHI_EVENT_CONFIG_HW_DATA(6, 1024, 102, MHI_DB_BRST_DISABLE),
};

static const struct mhi_controller_config modem_qcom_v1_mhiv_config = {
	.max_channels = 128,
	.timeout_ms = 15000,
	.num_channels = ARRAY_SIZE(modem_qcom_v1_mhi_channels),
	.ch_cfg = modem_qcom_v1_mhi_channels,
	.num_events = ARRAY_SIZE(modem_qcom_v1_mhi_events),
	.event_cfg = modem_qcom_v1_mhi_events,
};

static const struct mhi_pci_dev_info mhi_qcom_sdx65_info = {
	.name = "qcom-sdx65m",
	.fw = "qcom/sdx65m/xbl.elf",
	.edl = "qcom/sdx65m/edl.mbn",
	.config = &modem_qcom_v1_mhiv_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_qcom_sdx35_info = {
	.name = "qcom-sdx35m",
	.fw = "qcom/sdx35m/xbl.elf",
	.edl = "qcom/sdx35m/edl.mbn",
	.config = &modem_qcom_v1_mhiv_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_qcom_sdx81_info = {
	.name = "qcom-sdx81m",
	.fw = "qcom/sdx81m/xbl.elf",
	.edl = "qcom/sdx81m/edl.mbn",
	.config = &modem_qcom_v1_mhiv_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_qcom_sdx55_info = {
	.name = "qcom-sdx55m",
	.fw = "qcom/sdx55m/sbl1.mbn",
	.edl = "qcom/sdx55m/edl.mbn",
	.config = &modem_qcom_v1_mhiv_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_qcom_sdx24_info = {
	.name = "qcom-sdx24",
	.edl = "qcom/prog_firehose_sdx24.mbn",
	.config = &modem_qcom_v1_mhiv_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = true,
};

static const struct mhi_channel_config mhi_quectel_em1xx_channels[] = {
	MHI_CHANNEL_CONFIG_UL(0, "NMEA", 32, 0),
	MHI_CHANNEL_CONFIG_DL(1, "NMEA", 32, 0),
	MHI_CHANNEL_CONFIG_UL_SBL(2, "SAHARA", 32, 0),
	MHI_CHANNEL_CONFIG_DL_SBL(3, "SAHARA", 32, 0),
	MHI_CHANNEL_CONFIG_UL(4, "DIAG", 32, 1),
	MHI_CHANNEL_CONFIG_DL(5, "DIAG", 32, 1),
	MHI_CHANNEL_CONFIG_UL(12, "MBIM", 32, 0),
	MHI_CHANNEL_CONFIG_DL(13, "MBIM", 32, 0),
	MHI_CHANNEL_CONFIG_UL(32, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_DL(33, "DUN", 32, 0),
	/* The EDL firmware is a flash-programmer exposing firehose protocol */
	MHI_CHANNEL_CONFIG_UL_FP(34, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_DL_FP(35, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0_MBIM", 128, 2),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0_MBIM", 128, 3),
};

static struct mhi_event_config mhi_quectel_em1xx_events[] = {
	MHI_EVENT_CONFIG_CTRL(0, 128),
	MHI_EVENT_CONFIG_DATA(1, 128),
	MHI_EVENT_CONFIG_HW_DATA(2, 1024, 100, MHI_DB_BRST_ENABLE),
	MHI_EVENT_CONFIG_HW_DATA(3, 1024, 101, MHI_DB_BRST_ENABLE),
};

static const struct mhi_controller_config modem_quectel_em1xx_config = {
	.max_channels = 128,
	.timeout_ms = 20000,
	.num_channels = ARRAY_SIZE(mhi_quectel_em1xx_channels),
	.ch_cfg = mhi_quectel_em1xx_channels,
	.num_events = ARRAY_SIZE(mhi_quectel_em1xx_events),
	.event_cfg = mhi_quectel_em1xx_events,
};

static const struct mhi_pci_dev_info mhi_quectel_em1xx_info = {
	.name = "quectel-em1xx",
	.edl = "qcom/prog_firehose_sdx24.mbn",
	.config = &modem_quectel_em1xx_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = true,
	.auto_edl_load = true,
};

static const struct mhi_channel_config mhi_foxconn_sdx55_channels[] = {
	MHI_CHANNEL_CONFIG_UL(0, "LOOPBACK", 32, 0),
	MHI_CHANNEL_CONFIG_DL(1, "LOOPBACK", 32, 0),
	MHI_CHANNEL_CONFIG_UL(4, "DIAG", 32, 1),
	MHI_CHANNEL_CONFIG_DL(5, "DIAG", 32, 1),
	MHI_CHANNEL_CONFIG_UL(12, "MBIM", 32, 0),
	MHI_CHANNEL_CONFIG_DL(13, "MBIM", 32, 0),
	MHI_CHANNEL_CONFIG_UL(32, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_DL(33, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0_MBIM", 128, 2),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0_MBIM", 128, 3),
};

static struct mhi_event_config mhi_foxconn_sdx55_events[] = {
	MHI_EVENT_CONFIG_CTRL(0, 128),
	MHI_EVENT_CONFIG_DATA(1, 128),
	MHI_EVENT_CONFIG_HW_DATA(2, 1024, 100, MHI_DB_BRST_ENABLE),
	MHI_EVENT_CONFIG_HW_DATA(3, 1024, 101, MHI_DB_BRST_ENABLE),
};

static const struct mhi_controller_config modem_foxconn_sdx55_config = {
	.max_channels = 128,
	.timeout_ms = 20000,
	.num_channels = ARRAY_SIZE(mhi_foxconn_sdx55_channels),
	.ch_cfg = mhi_foxconn_sdx55_channels,
	.num_events = ARRAY_SIZE(mhi_foxconn_sdx55_events),
	.event_cfg = mhi_foxconn_sdx55_events,
};

static const struct mhi_pci_dev_info mhi_foxconn_sdx55_info = {
	.name = "foxconn-sdx55",
	.fw = "qcom/sdx55m/sbl1.mbn",
	.edl = "qcom/sdx55m/edl.mbn",
	.config = &modem_foxconn_sdx55_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
	.auto_edl_load = true,
};

static const struct mhi_channel_config mhi_mv31_channels[] = {
	MHI_CHANNEL_CONFIG_UL(0, "LOOPBACK", 64, 0),
	MHI_CHANNEL_CONFIG_DL(1, "LOOPBACK", 64, 0),
	/* MBIM Control Channel */
	MHI_CHANNEL_CONFIG_UL(12, "MBIM", 64, 0),
	MHI_CHANNEL_CONFIG_DL(13, "MBIM", 64, 0),
	/* MBIM Data Channel */
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0_MBIM", 512, 2),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0_MBIM", 512, 3),
};

static struct mhi_event_config mhi_mv31_events[] = {
	MHI_EVENT_CONFIG_CTRL(0, 256),
	MHI_EVENT_CONFIG_DATA(1, 256),
	MHI_EVENT_CONFIG_HW_DATA(2, 1024, 100, MHI_DB_BRST_ENABLE),
	MHI_EVENT_CONFIG_HW_DATA(3, 1024, 101, MHI_DB_BRST_ENABLE),
};

static const struct mhi_controller_config modem_mv31_config = {
	.max_channels = 128,
	.timeout_ms = 20000,
	.num_channels = ARRAY_SIZE(mhi_mv31_channels),
	.ch_cfg = mhi_mv31_channels,
	.num_events = ARRAY_SIZE(mhi_mv31_events),
	.event_cfg = mhi_mv31_events,
};

static const struct mhi_pci_dev_info mhi_mv31_info = {
	.name = "cinterion-mv31",
	.config = &modem_mv31_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.auto_edl_load = true,
};

static const struct pci_device_id mhi_pci_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_QCOM, 0x0306),
		.driver_data = (kernel_ulong_t)&mhi_qcom_sdx55_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QCOM, 0x0304),
		.driver_data = (kernel_ulong_t)&mhi_qcom_sdx24_info },
	{ PCI_DEVICE(0x1eac, 0x1001), /* EM120R-GL (sdx24) */
		.driver_data = (kernel_ulong_t)&mhi_quectel_em1xx_info },
	{ PCI_DEVICE(0x1eac, 0x1002), /* EM160R-GL (sdx24) */
		.driver_data = (kernel_ulong_t)&mhi_quectel_em1xx_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QCOM, 0x0308),
		.driver_data = (kernel_ulong_t)&mhi_qcom_sdx65_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QCOM, 0x011a), /* sdx35 */
		.driver_data = (kernel_ulong_t)&mhi_qcom_sdx35_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QCOM, 0x0309), /* sdx81 */
		.driver_data = (kernel_ulong_t)&mhi_qcom_sdx81_info },
	/* T99W175 (sdx55), Both for eSIM and Non-eSIM */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0ab),
		.driver_data = (kernel_ulong_t)&mhi_foxconn_sdx55_info },
	/* DW5930e (sdx55), With eSIM, It's also T99W175 */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0b0),
		.driver_data = (kernel_ulong_t)&mhi_foxconn_sdx55_info },
	/* DW5930e (sdx55), Non-eSIM, It's also T99W175 */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0b1),
		.driver_data = (kernel_ulong_t)&mhi_foxconn_sdx55_info },
	{  }
};
MODULE_DEVICE_TABLE(pci, mhi_pci_id_table);

enum mhi_pci_device_status {
	/* MHI controller has been registered and powered up successfully */
	MHI_PCI_DEV_STARTED,
	/* Device is parked in D3hot / runtime-suspended */
	MHI_PCI_DEV_SUSPENDED,
	/* Reset (FLR) in progress: health_check_timer/recovery_work must stay quiesced */
	MHI_PCI_DEV_RESETTING,
	/* Driver is unbinding: health_check_timer/recovery_work must never rearm again */
	MHI_PCI_DEV_REMOVING,
	/* mhi_register_controller() has completed: mhi_cntrl is fully initialized */
	MHI_PCI_DEV_REGISTERED,
};

/**
 * struct mhi_pci_device - private driver context for a MHI PCI device
 * @status: bitmask of enum mhi_pci_device_status flags, manipulated with
 *	    the atomic bit-test/set/clear helpers (test_bit(), set_bit(), ...)
 * @lock: serializes health_check_timer/recovery_work re-arming against
 *	  MHI_PCI_DEV_RESETTING/MHI_PCI_DEV_REMOVING being set, so a timer
 *	  firing or a recovery_work completing concurrently with remove()/
 *	  reset_prepare() cannot re-arm the timer or requeue the work after
 *	  those paths have started tearing down state. Taken as _bh since
 *	  health_check() runs in softirq (timer) context.
 */
struct mhi_pci_device {
	struct mhi_controller mhi_cntrl;
	struct pci_saved_state *pci_state;
	struct work_struct recovery_work;
	struct timer_list health_check_timer;
	unsigned long status;
	spinlock_t lock; /* protects health_check_timer/recovery_work re-arm vs teardown */
};

/* Whether a reset or remove is in progress/starting; call with mhi_pdev->lock held */
static bool mhi_pci_teardown_in_progress(struct mhi_pci_device *mhi_pdev)
{
	return test_bit(MHI_PCI_DEV_RESETTING, &mhi_pdev->status) ||
	       test_bit(MHI_PCI_DEV_REMOVING, &mhi_pdev->status);
}

/* Re-arm health_check_timer unless a reset or remove is in progress/starting */
static void mhi_pci_health_timer_rearm(struct mhi_pci_device *mhi_pdev,
				       unsigned long delay)
{
	struct pci_dev *pdev = to_pci_dev(mhi_pdev->mhi_cntrl.cntrl_dev);

	spin_lock_bh(&mhi_pdev->lock);
	if (!mhi_pci_teardown_in_progress(mhi_pdev))
		mod_timer(&mhi_pdev->health_check_timer, jiffies + delay);
	else
		dev_dbg_ratelimited(&pdev->dev,
				    "timer rearm skipped, reset/remove in progress\n");
	spin_unlock_bh(&mhi_pdev->lock);
}

/* Queue recovery_work unless a reset or remove is in progress/starting */
static void mhi_pci_queue_recovery(struct mhi_pci_device *mhi_pdev)
{
	struct pci_dev *pdev = to_pci_dev(mhi_pdev->mhi_cntrl.cntrl_dev);

	spin_lock_bh(&mhi_pdev->lock);
	if (!mhi_pci_teardown_in_progress(mhi_pdev))
		queue_work(system_long_wq, &mhi_pdev->recovery_work);
	else
		dev_dbg(&pdev->dev, "recovery queue skipped, reset/remove in progress\n");
	spin_unlock_bh(&mhi_pdev->lock);
}

static int mhi_pci_read_reg(struct mhi_controller *mhi_cntrl,
			    void __iomem *addr, u32 *out)
{
	*out = readl(addr);
	return 0;
}

static void mhi_pci_write_reg(struct mhi_controller *mhi_cntrl,
			      void __iomem *addr, u32 val)
{
	writel(val, addr);
}

static void mhi_pci_status_cb(struct mhi_controller *mhi_cntrl,
			      enum mhi_callback cb)
{
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);

	switch (cb) {
	case MHI_CB_FATAL_ERROR:
	case MHI_CB_SYS_ERROR:
		dev_warn(&pdev->dev, "firmware crashed (%u)\n", cb);
		fallthrough;
	case MHI_CB_EE_SBL_MODE:
		dev_dbg(&pdev->dev, "EE SBL, forbid runtime suspend\n");
		pm_runtime_forbid(&pdev->dev);
		break;
	case MHI_CB_EE_MISSION_MODE:
		dev_dbg(&pdev->dev, "EE mission mode, allow runtime suspend\n");
		pm_runtime_allow(&pdev->dev);
		break;
	case MHI_CB_FW_DL_ERR:
		dev_warn(&pdev->dev, "error requesting firmware\n");
		break;
	default:
		break;
	}
}

static void mhi_pci_wake_get_nop(struct mhi_controller *mhi_cntrl, bool force)
{
	/* no-op */
}

static void mhi_pci_wake_put_nop(struct mhi_controller *mhi_cntrl, bool override)
{
	/* no-op */
}

static void mhi_pci_wake_toggle_nop(struct mhi_controller *mhi_cntrl)
{
	/* no-op */
}

static bool mhi_pci_is_alive(struct mhi_controller *mhi_cntrl)
{
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	u16 vendor = 0;

	if (pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor))
		return false;

	if (vendor == (u16)~0 || vendor == 0)
		return false;

	return true;
}

static int mhi_pci_claim(struct mhi_controller *mhi_cntrl,
			 unsigned int bar_num, u64 dma_mask)
{
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int err;

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "failed to enable pci device: %d\n", err);
		return err;
	}

	err = pci_assign_resource(pdev, bar_num);
	if (err) {
		dev_err(&pdev->dev, "failed to assign pci resource: %d\n", err);
		return err;
	}

	err = pcim_iomap_regions(pdev, 1 << bar_num, pci_name(pdev));
	if (err) {
		dev_err(&pdev->dev, "failed to map pci region: %d\n", err);
		return err;
	}
	mhi_cntrl->regs = pcim_iomap_table(pdev)[bar_num];
	mhi_cntrl->reg_len = pci_resource_len(pdev, bar_num);

	err = dma_set_mask(&pdev->dev, dma_mask);
	if (err) {
		dev_err(&pdev->dev, "Cannot set proper DMA mask: %d\n", err);
		return err;
	}

	err = dma_set_coherent_mask(&pdev->dev, dma_mask);
	if (err) {
		dev_err(&pdev->dev, "set consistent dma mask failed: %d\n", err);
		return err;
	}

	pci_set_master(pdev);

	return 0;
}

static int mhi_pci_get_irqs(struct mhi_controller *mhi_cntrl,
			    const struct mhi_controller_config *mhi_cntrl_config)
{
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int nr_vectors, i;
	int *irq;

	/*
	 * Alloc one MSI vector for BHI + one vector per event ring, ideally...
	 * No explicit pci_free_irq_vectors required, done by pcim_release.
	 */
	mhi_cntrl->nr_irqs = 1 + mhi_cntrl_config->num_events;

	nr_vectors = pci_alloc_irq_vectors(pdev, 1, mhi_cntrl->nr_irqs, PCI_IRQ_MSI);
	if (nr_vectors < 0) {
		dev_err(&pdev->dev, "Error allocating MSI vectors %d\n",
			nr_vectors);
		return nr_vectors;
	}

	if (nr_vectors < mhi_cntrl->nr_irqs) {
		dev_warn(&pdev->dev, "using shared MSI\n");

		/* Patch msi vectors, use only one (shared) */
		for (i = 0; i < mhi_cntrl_config->num_events; i++) {
			struct mhi_event_config *cfg =
				(struct mhi_event_config *)&mhi_cntrl_config->event_cfg[i];
			cfg->irq = 0;
		}
		mhi_cntrl->nr_irqs = 1;
	}

	irq = devm_kcalloc(&pdev->dev, mhi_cntrl->nr_irqs, sizeof(int), GFP_KERNEL);
	if (!irq)
		return -ENOMEM;

	for (i = 0; i < mhi_cntrl->nr_irqs; i++) {
		int vector = i >= nr_vectors ? (nr_vectors - 1) : i;

		irq[i] = pci_irq_vector(pdev, vector);
	}

	mhi_cntrl->irq = irq;

	return 0;
}

static int mhi_pci_runtime_get(struct mhi_controller *mhi_cntrl)
{
	/* The runtime_get() MHI callback means:
	 *    Do whatever is requested to leave M3.
	 */
	return pm_runtime_get(mhi_cntrl->cntrl_dev);
}

static void mhi_pci_runtime_put(struct mhi_controller *mhi_cntrl)
{
	/* The runtime_put() MHI callback means:
	 *    Device can be moved in M3 state.
	 */
	pm_runtime_mark_last_busy(mhi_cntrl->cntrl_dev);
	pm_runtime_put(mhi_cntrl->cntrl_dev);
}

/* Prepare and power up the MHI controller; unprepares on power-up failure */
static int mhi_pci_power_up(struct mhi_controller *mhi_cntrl)
{
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int err;

	err = mhi_prepare_for_power_up(mhi_cntrl);
	if (err) {
		dev_err(&pdev->dev, "failed to prepare MHI controller\n");
		return err;
	}

	err = mhi_sync_power_up(mhi_cntrl);
	if (err) {
		dev_err(&pdev->dev, "failed to power up MHI controller\n");
		mhi_unprepare_after_power_down(mhi_cntrl);
		return err;
	}

	return 0;
}

static void mhi_pci_recovery_work(struct work_struct *work)
{
	struct mhi_pci_device *mhi_pdev = container_of(work, struct mhi_pci_device,
						       recovery_work);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int err;

	dev_warn(&pdev->dev, "device recovery started\n");

	del_timer(&mhi_pdev->health_check_timer);
	pm_runtime_forbid(&pdev->dev);

	/* Clean up MHI state */
	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, false);
		mhi_unprepare_after_power_down(mhi_cntrl);
	}

	pci_set_power_state(pdev, PCI_D0);
	pci_load_saved_state(pdev, mhi_pdev->pci_state);
	pci_restore_state(pdev);

	if (!mhi_pci_is_alive(mhi_cntrl))
		goto err_try_reset;

	err = mhi_pci_power_up(mhi_cntrl);
	if (err)
		goto err_try_reset;

	dev_dbg(&pdev->dev, "Recovery completed\n");

	set_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status);
	mhi_pci_health_timer_rearm(mhi_pdev, HEALTH_CHECK_PERIOD);
	return;

err_try_reset:
	if (pci_reset_function(pdev))
		dev_err(&pdev->dev, "Recovery failed\n");
}

static void health_check(struct timer_list *t)
{
	struct mhi_pci_device *mhi_pdev = from_timer(mhi_pdev, t, health_check_timer);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	if (!test_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status))
		return;

	/* If device is parked in d3hot, wake the device to check state */
	if (test_bit(MHI_PCI_DEV_SUSPENDED, &mhi_pdev->status)) {
		mhi_pci_runtime_get(mhi_cntrl);
		mhi_pci_runtime_put(mhi_cntrl);

		mhi_pci_health_timer_rearm(mhi_pdev, CRASH_CHECK_PERIOD);
		return;
	}

	if (!mhi_pci_is_alive(mhi_cntrl)) {
		dev_err(mhi_cntrl->cntrl_dev, "Device died\n");
		mhi_pci_queue_recovery(mhi_pdev);
		return;
	}

	/* reschedule in two seconds */
	mhi_pci_health_timer_rearm(mhi_pdev, HEALTH_CHECK_PERIOD);
}

/* Whether the device can wake the system from D3hot via PME */
static bool mhi_pci_is_wakeup_capable(struct pci_dev *pdev)
{
	return pdev->pm_cap && (pdev->pme_support & (1 << PCI_D3hot));
}

static void mhi_pci_init_controller(struct mhi_pci_device *mhi_pdev,
				    struct pci_dev *pdev,
				    const struct mhi_pci_dev_info *info)
{
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	mhi_cntrl->cntrl_dev = &pdev->dev;
	mhi_cntrl->iova_start = 0;
	mhi_cntrl->iova_stop = (dma_addr_t)DMA_BIT_MASK(info->dma_data_width);
	mhi_cntrl->fw_image = info->fw;
	mhi_cntrl->edl_image = info->edl;

	mhi_cntrl->read_reg = mhi_pci_read_reg;
	mhi_cntrl->write_reg = mhi_pci_write_reg;
	mhi_cntrl->status_cb = mhi_pci_status_cb;
	mhi_cntrl->runtime_get = mhi_pci_runtime_get;
	mhi_cntrl->runtime_put = mhi_pci_runtime_put;

	if (info->sideband_wake) {
		mhi_cntrl->wake_get = mhi_pci_wake_get_nop;
		mhi_cntrl->wake_put = mhi_pci_wake_put_nop;
		mhi_cntrl->wake_toggle = mhi_pci_wake_toggle_nop;
	}

	if (info->auto_edl_load)
		/* No consumer wired up yet; has no effect until one reads it */
		mhi_cntrl->edl_download = true;
}

/*
 * Cache pdev's PCI confspace locally for restore on a sudden PCI error, then
 * discard the PCI core's own copy so it doesn't get restored out from under us
 */
static void mhi_pci_cache_pci_state(struct mhi_pci_device *mhi_pdev, struct pci_dev *pdev)
{
	pci_save_state(pdev);
	mhi_pdev->pci_state = pci_store_saved_state(pdev);
	pci_load_saved_state(pdev, NULL);
}

/* Only allow runtime-suspend if PME capable (for wakeup) */
static void mhi_pci_setup_wakeup(struct pci_dev *pdev)
{
	if (!mhi_pci_is_wakeup_capable(pdev))
		return;

	device_init_wakeup(&pdev->dev, 1);
	pm_runtime_set_autosuspend_delay(&pdev->dev, MHI_PCI_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
}

/* Register the MHI controller, power it up and arm the health timer */
static int mhi_pci_register_and_start(struct mhi_pci_device *mhi_pdev,
				       const struct mhi_controller_config *mhi_cntrl_config)
{
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int err;

	err = mhi_register_controller(mhi_cntrl, mhi_cntrl_config);
	if (err) {
		dev_err(&pdev->dev, "failed to register MHI controller: %d\n", err);
		return err;
	}
	set_bit(MHI_PCI_DEV_REGISTERED, &mhi_pdev->status);

	/* MHI bus does not power up the controller by default */
	err = mhi_pci_power_up(mhi_cntrl);
	if (err)
		goto err_unregister;

	set_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status);

	/* start health check */
	mhi_pci_health_timer_rearm(mhi_pdev, HEALTH_CHECK_PERIOD);

	return 0;

err_unregister:
	mhi_unregister_controller(mhi_cntrl);
	return err;
}

static int mhi_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	const struct mhi_pci_dev_info *info = (struct mhi_pci_dev_info *)id->driver_data;
	const struct mhi_controller_config *mhi_cntrl_config;
	struct mhi_pci_device *mhi_pdev;
	struct mhi_controller *mhi_cntrl;
	int err;

	dev_dbg(&pdev->dev, "PCI device found: %s\n", info->name);

	/* mhi_pdev.mhi_cntrl must be zero-initialized */
	mhi_pdev = devm_kzalloc(&pdev->dev, sizeof(*mhi_pdev), GFP_KERNEL);
	if (!mhi_pdev)
		return -ENOMEM;

	INIT_WORK(&mhi_pdev->recovery_work, mhi_pci_recovery_work);
	timer_setup(&mhi_pdev->health_check_timer, health_check, 0);
	spin_lock_init(&mhi_pdev->lock);

	mhi_cntrl_config = info->config;
	mhi_cntrl = &mhi_pdev->mhi_cntrl;

	mhi_pci_init_controller(mhi_pdev, pdev, info);

	err = mhi_pci_claim(mhi_cntrl, info->bar_num, DMA_BIT_MASK(info->dma_data_width));
	if (err)
		return err;

	err = mhi_pci_get_irqs(mhi_cntrl, mhi_cntrl_config);
	if (err)
		return err;

	pci_set_drvdata(pdev, mhi_pdev);

	mhi_pci_cache_pci_state(mhi_pdev, pdev);

	err = mhi_pci_register_and_start(mhi_pdev, mhi_cntrl_config);
	if (err)
		goto err_free_pci_state;

	mhi_pci_setup_wakeup(pdev);

	return 0;

err_free_pci_state:
	kfree(mhi_pdev->pci_state);

	return err;
}

static void mhi_pci_remove(struct pci_dev *pdev)
{
	struct mhi_pci_device *mhi_pdev = pci_get_drvdata(pdev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	/*
	 * Block health_check()/mhi_pci_recovery_work() from re-arming the
	 * timer or requeuing themselves before quiescing them, otherwise a
	 * timer firing (or recovery_work already running) right before this
	 * point can re-arm/requeue after del_timer_sync()/cancel_work_sync()
	 * return, leading to a use-after-free once mhi_pdev is freed.
	 */
	spin_lock_bh(&mhi_pdev->lock);
	set_bit(MHI_PCI_DEV_REMOVING, &mhi_pdev->status);
	spin_unlock_bh(&mhi_pdev->lock);

	del_timer_sync(&mhi_pdev->health_check_timer);
	cancel_work_sync(&mhi_pdev->recovery_work);

	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, true);
		mhi_unprepare_after_power_down(mhi_cntrl);
	}

	/* balancing probe put_noidle */
	if (mhi_pci_is_wakeup_capable(pdev))
		pm_runtime_get_noresume(&pdev->dev);

	mhi_unregister_controller(mhi_cntrl);
	kfree(mhi_pdev->pci_state);
}

static void mhi_pci_shutdown(struct pci_dev *pdev)
{
	mhi_pci_remove(pdev);
	pci_set_power_state(pdev, PCI_D3hot);
}

static void mhi_pci_reset_prepare(struct pci_dev *pdev)
{
	struct mhi_pci_device *mhi_pdev = pci_get_drvdata(pdev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	dev_info(&pdev->dev, "reset\n");

	/*
	 * A reset landing before mhi_register_controller() has completed
	 * (drvdata already set, but mhi_cntrl not yet fully initialized)
	 * must not touch the partially-initialized controller. Nothing to
	 * quiesce either: health_check_timer/recovery_work are only armed
	 * after MHI_PCI_DEV_STARTED is set, which happens after registration.
	 */
	if (!test_bit(MHI_PCI_DEV_REGISTERED, &mhi_pdev->status))
		return;

	/*
	 * Prevent health_check()/mhi_pci_recovery_work() from touching
	 * mhi_cntrl concurrently with the FLR sequence below: block them from
	 * re-arming/requeuing themselves, then drain whichever of the two is
	 * already in flight before mutating any MHI state.
	 */
	spin_lock_bh(&mhi_pdev->lock);
	set_bit(MHI_PCI_DEV_RESETTING, &mhi_pdev->status);
	spin_unlock_bh(&mhi_pdev->lock);

	del_timer_sync(&mhi_pdev->health_check_timer);
	cancel_work_sync(&mhi_pdev->recovery_work);

	/* Clean up MHI state */
	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, false);
		mhi_unprepare_after_power_down(mhi_cntrl);
	}

	/* cause internal device reset */
	mhi_soc_reset(mhi_cntrl);

	/* Be sure device reset has been executed */
	msleep(MHI_POST_RESET_DELAY_MS);
}

static void mhi_pci_reset_done(struct pci_dev *pdev)
{
	struct mhi_pci_device *mhi_pdev = pci_get_drvdata(pdev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	int err;

	/* Restore initial known working PCI state */
	pci_load_saved_state(pdev, mhi_pdev->pci_state);
	pci_restore_state(pdev);

	dev_info(&pdev->dev, "reset done\n");

	/* Mirrors the early return in reset_prepare() for the same reason */
	if (!test_bit(MHI_PCI_DEV_REGISTERED, &mhi_pdev->status))
		return;

	/* Is device status available ? */
	if (!mhi_pci_is_alive(mhi_cntrl)) {
		dev_err(&pdev->dev, "reset failed\n");
		goto out_resettable;
	}

	err = mhi_pci_power_up(mhi_cntrl);
	if (err)
		goto out_resettable;

	set_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status);
	dev_dbg(&pdev->dev, "reset recovery completed\n");

out_resettable:
	/* health_check()/recovery_work may resume re-arming/requeuing themselves */
	clear_bit(MHI_PCI_DEV_RESETTING, &mhi_pdev->status);
	mhi_pci_health_timer_rearm(mhi_pdev, HEALTH_CHECK_PERIOD);
}

static pci_ers_result_t mhi_pci_error_detected(struct pci_dev *pdev,
					       pci_channel_state_t state)
{
	struct mhi_pci_device *mhi_pdev = pci_get_drvdata(pdev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	dev_err(&pdev->dev, "PCI error detected, state = %u\n", state);

	if (state == pci_channel_io_perm_failure)
		return PCI_ERS_RESULT_DISCONNECT;

	/* Clean up MHI state */
	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, false);
		mhi_unprepare_after_power_down(mhi_cntrl);
	} else {
		/* Nothing to do */
		return PCI_ERS_RESULT_RECOVERED;
	}

	pci_disable_device(pdev);

	return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t mhi_pci_slot_reset(struct pci_dev *pdev)
{
	if (pci_enable_device(pdev)) {
		dev_err(&pdev->dev, "Cannot re-enable PCI device after reset.\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}

	return PCI_ERS_RESULT_RECOVERED;
}

static void mhi_pci_io_resume(struct pci_dev *pdev)
{
	struct mhi_pci_device *mhi_pdev = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "PCI slot reset done\n");

	mhi_pci_queue_recovery(mhi_pdev);
}

static const struct pci_error_handlers mhi_pci_err_handler = {
	.error_detected = mhi_pci_error_detected,
	.slot_reset = mhi_pci_slot_reset,
	.resume = mhi_pci_io_resume,
	.reset_prepare = mhi_pci_reset_prepare,
	.reset_done = mhi_pci_reset_done,
};

static int __maybe_unused mhi_pci_runtime_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct mhi_pci_device *mhi_pdev = dev_get_drvdata(dev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	int err;

	if (test_and_set_bit(MHI_PCI_DEV_SUSPENDED, &mhi_pdev->status))
		return 0;

	del_timer_sync(&mhi_pdev->health_check_timer);
	cancel_work_sync(&mhi_pdev->recovery_work);

	if (!test_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status) ||
	    mhi_cntrl->ee != MHI_EE_AMSS)
		goto pci_suspend; /* Nothing to do at MHI level */

	/* Transition to M3 state */
	err = mhi_pm_suspend(mhi_cntrl);
	if (err) {
		dev_err(&pdev->dev, "failed to suspend device: %d\n", err);
		clear_bit(MHI_PCI_DEV_SUSPENDED, &mhi_pdev->status);
		mhi_pci_health_timer_rearm(mhi_pdev, HEALTH_CHECK_PERIOD);
		return -EBUSY;
	}

pci_suspend:
	pci_disable_device(pdev);
	err = pci_wake_from_d3(pdev, true);
	if (err)
		dev_dbg(&pdev->dev, "failed to set D3 wake-capable: %d\n", err);

	/* start crash check timer for D3hot to D0 transitions */
	mhi_pci_health_timer_rearm(mhi_pdev, CRASH_CHECK_PERIOD);

	return 0;
}

static int __maybe_unused mhi_pci_runtime_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct mhi_pci_device *mhi_pdev = dev_get_drvdata(dev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	int err;

	if (!test_and_clear_bit(MHI_PCI_DEV_SUSPENDED, &mhi_pdev->status))
		return 0;

	err = pci_enable_device(pdev);
	if (err)
		goto err_recovery;

	pci_set_master(pdev);
	err = pci_wake_from_d3(pdev, false);
	if (err)
		dev_dbg(&pdev->dev, "failed to clear D3 wake-capable: %d\n", err);

	if (!test_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status) ||
	    mhi_cntrl->ee != MHI_EE_AMSS)
		return 0; /* Nothing to do at MHI level */

	/* Exit M3, transition to M0 state */
	err = mhi_pm_resume(mhi_cntrl);
	if (err) {
		dev_err(&pdev->dev, "failed to resume device: %d\n", err);
		goto err_recovery;
	}

	/* Resume health check */
	mhi_pci_health_timer_rearm(mhi_pdev, HEALTH_CHECK_PERIOD);

	/* It can be a remote wakeup (no mhi runtime_get), update access time */
	pm_runtime_mark_last_busy(dev);

	return 0;

err_recovery:
	/* Do not fail to not mess up our PCI device state. The device may need
	 * to be reset via the recovery procedure; trigger it asynchronously to
	 * prevent delaying system suspend exit.
	 */
	mhi_pci_queue_recovery(mhi_pdev);
	pm_runtime_mark_last_busy(dev);

	return 0;
}

static int __maybe_unused mhi_pci_suspend(struct device *dev)
{
	pm_runtime_disable(dev);
	return mhi_pci_runtime_suspend(dev);
}

static int __maybe_unused mhi_pci_resume(struct device *dev)
{
	int ret;

	/* Depending on the platform, device may need to be resumed to check
	 * its state and recover when necessary.
	 */
	ret = mhi_pci_runtime_resume(dev);
	pm_runtime_enable(dev);

	return ret;
}

static int __maybe_unused mhi_pci_freeze(struct device *dev)
{
	struct mhi_pci_device *mhi_pdev = dev_get_drvdata(dev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	flush_work(&mhi_pdev->recovery_work);
	/* We want to stop all operations, hibernation does not guarantee that
	 * device will be in the same state as before freezing, especially if
	 * the intermediate restore kernel reinitializes MHI device with new
	 * context.
	 */
	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, true);
		mhi_unprepare_after_power_down(mhi_cntrl);
	}

	return 0;
}

static int __maybe_unused mhi_pci_restore(struct device *dev)
{
	struct mhi_pci_device *mhi_pdev = dev_get_drvdata(dev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	/* Clean up MHI state */
	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, true);
		mhi_unprepare_after_power_down(mhi_cntrl);
	}

	/* Reinitialize the device */
	mhi_pci_queue_recovery(mhi_pdev);

	return 0;
}

static const struct dev_pm_ops mhi_pci_pm_ops = {
	SET_RUNTIME_PM_OPS(mhi_pci_runtime_suspend, mhi_pci_runtime_resume, NULL)
#ifdef CONFIG_PM_SLEEP
	.suspend = mhi_pci_suspend,
	.resume = mhi_pci_resume,
	.freeze = mhi_pci_freeze,
	.thaw = mhi_pci_restore,
	.poweroff = mhi_pci_freeze,
	.restore = mhi_pci_restore,
#endif
};

static struct pci_driver mhi_pci_driver = {
	.name		= "mhi-pci-generic",
	.id_table	= mhi_pci_id_table,
	.probe		= mhi_pci_probe,
	.remove		= mhi_pci_remove,
	.shutdown	= mhi_pci_shutdown,
	.err_handler	= &mhi_pci_err_handler,
	.driver.pm	= &mhi_pci_pm_ops
};
module_pci_driver(mhi_pci_driver);

MODULE_DESCRIPTION("QTI Modem Host Interface (MHI) PCI controller driver");
MODULE_LICENSE("GPL");
