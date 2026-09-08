// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/usb/audio.h>
#include <linux/usb/audio-v2.h>
#include <linux/uaccess.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/core.h>
#include <sound/asound.h>
#include <linux/usb.h>
#include <linux/init.h>
#include <linux/usb/hcd.h>
#include <linux/usb/xhci-sideband.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/iommu.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/usb/audio-v3.h>
#include <linux/ipc_logging.h>
#include <linux/workqueue.h>

#include "sound/usb/usbaudio.h"
#include "sound/usb/card.h"
#include "sound/usb/endpoint.h"
#include "sound/usb/helper.h"
#include "sound/usb/pcm.h"
#include "sound/usb/power.h"
#include "usb_audio_qmi_v01.h"
#include <linux/usb/dwc3-msm.h>
#include "../../../drivers/usb/host/xhci.h"
#include <trace/hooks/usb.h>

#define QSRAM_ADSP_READY_REG	7
#define QSRAM_ADSP_READY_BIT	BIT(0)
#define QSRAM_OFFLOAD_ACK_REG	8

#define PAGE_4K		4096
#define BUS_INTERVAL_FULL_SPEED 1000 /* in us */
#define BUS_INTERVAL_HIGHSPEED_AND_ABOVE 125 /* in us */
#define MAX_BINTERVAL_ISOC_EP 16
#define DEV_RELEASE_WAIT_TIMEOUT 10000 /* in ms */

#define SND_PCM_CARD_NUM_MASK 0xffff0000
#define SND_PCM_DEV_NUM_MASK 0xff00
#define SND_PCM_STREAM_DIRECTION 0xff

#define PREPEND_SID_TO_IOVA(iova, sid) ((u64)(((u64)(iova)) | \
					(((u64)sid) << 32)))

/*  event ring iova base address */
#define IOVA_BASE 0x1000

#define IOVA_XFER_RING_BASE (IOVA_BASE + PAGE_4K * (SNDRV_CARDS + 1))
#define IOVA_XFER_BUF_BASE (IOVA_XFER_RING_BASE + PAGE_4K * SNDRV_CARDS * 32)
#define IOVA_XFER_RING_MAX (IOVA_XFER_BUF_BASE - PAGE_4K)

#define MAX_XFER_BUFF_LEN (24 * PAGE_4K)
#define EP_MASK 0x7F

#define IOVA_PSEUDO_EVT_RING_BASE (IOVA_XFER_BUF_BASE)
#define IOVA_XFER_BUF_START (IOVA_PSEUDO_EVT_RING_BASE + PAGE_4K)
#define IOVA_XFER_BUF_MAX (0xfffff000 - PAGE_4K)

#define MAX_DRAIN_EVT_RING_NUM 20
struct xhci_ring;

struct iova_info {
	struct list_head list;
	unsigned long start_iova;
	size_t size;
	bool in_use;
};

struct intf_info {
	unsigned long data_xfer_ring_va;
	size_t data_xfer_ring_size;
	unsigned long sync_xfer_ring_va;
	size_t sync_xfer_ring_size;
	unsigned long xfer_buf_va;
	size_t xfer_buf_size;
	phys_addr_t xfer_buf_pa;
	unsigned int data_ep_pipe;
	unsigned int sync_ep_pipe;
	unsigned int data_ep_idx;
	unsigned int sync_ep_idx;
	u8 *xfer_buf;
	u8 intf_num;
	u8 pcm_card_num;
	u8 pcm_dev_num;
	u8 direction;
	bool in_use;
};

/* Private data structure for audio offload */
struct audio_offload_data {
	u32 active;
	void *event_buffer;
	unsigned long event_buffer_va;
	struct xhci_ring *sw_event_ring;
	union xhci_trb *sw_enqueue;
	union xhci_trb *sw_dequeue;
};

#define SW_EVENT_RING_SIZE (4 * 1024)
#define SW_EVENT_RING_NUM_TRBS (SW_EVENT_RING_SIZE / sizeof(union xhci_trb))
/*
 * Maximum number of pending SW event ring entries (enqueue - dequeue,
 * accounting for wraparound) that can be self-drained/handled when there
 * is no active offload stream driving the normal interrupt path (e.g. the
 * QSRAM poll work draining stray events, or xhci_handle_offload() draining
 * a pending/inactive ring during teardown). If more entries than this are
 * pending, an interrupt is expected to arrive and trigger the normal
 * offload handling path instead, so processing bails out / stops after
 * this many entries rather than draining everything.
 */
#define UAUDIO_QSRAM_POLL_MAX_PENDING_EVENTS	5

struct uaudio_dev {
	struct usb_device *udev;
	struct mutex mutex;
	/* audio control interface */
	struct usb_host_interface *ctrl_intf;
	unsigned int card_num;
	unsigned int usb_core_id;
	atomic_t in_use;
	wait_queue_head_t disconnect_wq;

	/* xhci sideband */
	struct xhci_sideband *sb;
	/* Audio offload private data */
	struct audio_offload_data offload_data;
	struct xhci_hcd *xhci;

	/* interface specific */
	int num_intf;
	struct intf_info *info;
	struct snd_usb_audio *chip;
	bool disconnect_wq_init;
};

static struct uaudio_dev uadev[SNDRV_CARDS];
/*
 * Set by uaudio_disconnect() before it takes uadev[card].mutex
 * uaudio_disconnect() -> USB_DEVICE_REMOVE -> dwc3 host-stop teardown.
 */
static atomic_t card_disconnecting[SNDRV_CARDS];

struct stop_ep_monitor {
	struct work_struct work;
	int card_num;
	atomic_t stop_completed;
	/*
	 * Set to 1 by handle_uaudio_stream_req once disable_audio_stream()
	 * has completed.  stop_ep_monitor_work checks this first and skips
	 * the adsp_state poll loop when the stream is already torn down.
	 */
	atomic_t disable_completed;
};

struct uaudio_qmi_dev {
	struct device *dev;
	u32 sid;
	u32 intr_num;
	struct xhci_ring *sec_ring;
	struct iommu_domain *domain;

	/* list to keep track of available iova */
	struct list_head xfer_ring_list;
	size_t xfer_ring_iova_size;
	unsigned long curr_xfer_ring_iova;
	struct list_head xfer_buf_list;
	size_t xfer_buf_iova_size;
	unsigned long curr_xfer_buf_iova;
	/* bit fields representing pcm card enabled */
	unsigned long card_slot;
	/* indicate event ring mapped or not */
	bool er_mapped;
	struct qsram_xhci __iomem *qsram;

	/* Pseudo event ring - separate pool, independent lifecycle */
	struct list_head pseudo_evt_ring_list;
	size_t pseudo_evt_ring_iova_size;
	unsigned long curr_pseudo_evt_ring_iova;
	bool pseudo_evt_ring_mapped;
	void *pseudo_evt_ring_buffer;
	phys_addr_t pseudo_evt_ring_pa;
	unsigned long pseudo_evt_ring_va;
	struct usb_device *pseudo_evt_ring_udev;

	struct work_struct offload_ready_work;
	/*
	 * High priority workqueue for offload_ready_work (avoid system_wq delays)
	 */
	struct workqueue_struct *offload_ready_wq;
	bool poll_active;
	struct delayed_work offload_qsram_poll_work;
	bool offload_qsram_poll_active;
	bool sw_evt_ring_freed;
	struct workqueue_struct *stop_monitor_wq;
	struct stop_ep_monitor stop_monitor;
	bool in_disconnect;
	bool adsp_offload_active;
};

static struct uaudio_qmi_dev *uaudio_qdev;

struct uaudio_qmi_svc {
	struct qmi_handle *uaudio_svc_hdl;
	struct sockaddr_qrtr client_sq;
	bool client_connected;
	void *uaudio_ipc_log;
};

static struct uaudio_qmi_svc *uaudio_svc;
static void handle_uaudio_stream_req(struct qmi_handle *handle,
			struct sockaddr_qrtr *sq,
			struct qmi_txn *txn,
			const void *decoded_msg);

static struct qmi_msg_handler uaudio_stream_req_handlers = {
	.type = QMI_REQUEST,
	.msg_id = QMI_UAUDIO_STREAM_REQ_V01,
	.ei = qmi_uaudio_stream_req_msg_v01_ei,
	.decoded_size = QMI_UAUDIO_STREAM_REQ_MSG_V01_MAX_MSG_LEN,
	.fn = handle_uaudio_stream_req,
};

enum mem_type {
	MEM_EVENT_RING,
	MEM_XFER_RING,
	MEM_XFER_BUF,
	MEM_PSEUDO_EVT_RING,
};

enum usb_qmi_audio_format {
	USB_QMI_PCM_FORMAT_S8 = 0,
	USB_QMI_PCM_FORMAT_U8,
	USB_QMI_PCM_FORMAT_S16_LE,
	USB_QMI_PCM_FORMAT_S16_BE,
	USB_QMI_PCM_FORMAT_U16_LE,
	USB_QMI_PCM_FORMAT_U16_BE,
	USB_QMI_PCM_FORMAT_S24_LE,
	USB_QMI_PCM_FORMAT_S24_BE,
	USB_QMI_PCM_FORMAT_U24_LE,
	USB_QMI_PCM_FORMAT_U24_BE,
	USB_QMI_PCM_FORMAT_S24_3LE,
	USB_QMI_PCM_FORMAT_S24_3BE,
	USB_QMI_PCM_FORMAT_U24_3LE,
	USB_QMI_PCM_FORMAT_U24_3BE,
	USB_QMI_PCM_FORMAT_S32_LE,
	USB_QMI_PCM_FORMAT_S32_BE,
	USB_QMI_PCM_FORMAT_U32_LE,
	USB_QMI_PCM_FORMAT_U32_BE,
};


#define uaudio_info(fmt, ...) do { \
	ipc_log_string(uaudio_svc->uaudio_ipc_log, \
	"%s: " fmt, __func__, ##__VA_ARGS__); \
	pr_info("%s: " fmt, __func__, ##__VA_ARGS__); \
	} while (0)

#define uaudio_err(fmt, ...) do { \
	ipc_log_string(uaudio_svc->uaudio_ipc_log, \
		"%s: " fmt, __func__, ##__VA_ARGS__); \
	pr_err("%s: " fmt, __func__, ##__VA_ARGS__); \
	} while (0)

#ifdef CONFIG_DYNAMIC_DEBUG
#define uaudio_dbg(fmt, ...) do { \
	ipc_log_string(uaudio_svc->uaudio_ipc_log, \
	"%s: " fmt, __func__, ##__VA_ARGS__); \
	dynamic_pr_debug("%s: " fmt, __func__, ##__VA_ARGS__); \
	} while (0)
#else
#define uaudio_dbg(fmt, ...) do { \
	ipc_log_string(uaudio_svc->uaudio_ipc_log, \
		"%s: " fmt, __func__, ##__VA_ARGS__); \
	pr_debug("%s: " fmt, __func__, ##__VA_ARGS__); \
	} while (0)
#endif

#define NUM_LOG_PAGES		10

static void uaudio_iommu_unmap(enum mem_type mtype, unsigned long va,
	size_t iova_size, size_t mapped_iova_size);

static struct qsram_xhci __iomem *uaudio_get_qsram(struct device *dev);

static enum usb_audio_device_speed_enum_v01
get_speed_info(enum usb_device_speed udev_speed)
{
	switch (udev_speed) {
	case USB_SPEED_LOW:
		return USB_AUDIO_DEVICE_SPEED_LOW_V01;
	case USB_SPEED_FULL:
		return USB_AUDIO_DEVICE_SPEED_FULL_V01;
	case USB_SPEED_HIGH:
		return USB_AUDIO_DEVICE_SPEED_HIGH_V01;
	case USB_SPEED_SUPER:
		return USB_AUDIO_DEVICE_SPEED_SUPER_V01;
	case USB_SPEED_SUPER_PLUS:
		return USB_AUDIO_DEVICE_SPEED_SUPER_PLUS_V01;
	default:
		uaudio_err("udev speed %d\n", udev_speed);
		return USB_AUDIO_DEVICE_SPEED_INVALID_V01;
	}
}

dma_addr_t xhci_sw_trb_virt_to_dma(struct xhci_segment *seg,
		union xhci_trb *trb)
{
	unsigned long segment_offset;

	if (!seg || !trb || trb < seg->trbs)
		return 0;
	/* offset in TRBs */
	segment_offset = trb - seg->trbs;
	if (segment_offset >= TRBS_PER_SEGMENT)
		return 0;
	return seg->dma + (segment_offset * sizeof(*trb));
}

/*
 * Fallback: rebuild event ring dequeue + cycle_state
 * and apply to ring + HW ERDP.
 *
 * Only used when original ERDP is invalid.
 * Only supports single-segment event ring.
 */
static int xhci_rebuild_erdp_cycle(struct xhci_hcd *xhci,
		struct xhci_interrupter *ir)
{
	struct xhci_ring *ring = ir->event_ring;
	struct xhci_segment *seg;
	union xhci_trb *trbs;
	u32 seg_trbs;
	u32 i;

	dma_addr_t hw_deq_dma;
	u64 hw_deq;
	u32 trb_index;
	unsigned long offset;

	if (!ring || !ring->first_seg) {
		pr_err("UGMI: rebuild: invalid ring\n");
		return -EINVAL;
	}

	if (ring->num_segs != 1) {
		pr_err("UGMI: rebuild: invalid ring seg %d.\n", ring->num_segs);
		return -EPERM;
	}

	seg = ring->first_seg;
	trbs = seg->trbs;
	seg_trbs = ir->erst.entries[0].seg_size;

	/* =========================
	 * Step1: scan 找 boundary
	 * =========================
	 */
	for (i = 0; i + 1 < seg_trbs; i++) {
		u32 cur_d3  = le32_to_cpu(trbs[i].generic.field[3]);
		u32 next_d3 = le32_to_cpu(trbs[i + 1].generic.field[3]);
		u32 cur_cycle = cur_d3 & 1;
		u32 next_cycle = next_d3 & 1;

		if (next_cycle != cur_cycle) {
			hw_deq_dma = seg->dma +
				(i + 1) * sizeof(union xhci_trb);
			ring->cycle_state = cur_cycle;

			pr_err("UGMI: rebuild: boundary i=%u -> idx=%u dma=%pa cycle=%u\n",
				   i, i + 1, &hw_deq_dma, cur_cycle);
			goto apply;
		}
	}

	/* =========================
	 * Step2: all same case
	 * =========================
	 */
	{
		u32 d3 = le32_to_cpu(trbs[0].generic.field[3]);
		u32 c0 = d3 & 1;

		hw_deq_dma = seg->dma;
		ring->cycle_state = c0 ^ 1;

		pr_err("UGMI: rebuild: no boundary all_cycle=%u -> idx=0 dma=%pa cycle=%u\n",
			   c0, &hw_deq_dma, ring->cycle_state);
	}

apply:
	/* =========================
	 * Step3: offset 修正
	 * =========================
	 */
	hw_deq = (u64)hw_deq_dma;

	if (((hw_deq_dma&0xfff) != 0xff0) && (uaudio_qdev->in_disconnect)) {
		if (hw_deq_dma >= seg->dma + 4 * sizeof(union xhci_trb)) {
			hw_deq -= 4 * sizeof(union xhci_trb);
			hw_deq_dma = (dma_addr_t)(hw_deq & ERST_PTR_MASK);
		} else if (hw_deq_dma >= seg->dma + 2 * sizeof(union xhci_trb)) {
			hw_deq -= 2 * sizeof(union xhci_trb);
			hw_deq_dma = (dma_addr_t)(hw_deq & ERST_PTR_MASK);
		}
	}

	/* =========================
	 * Step4: DMA → VA 映射
	 * =========================
	 */
	if (hw_deq_dma < seg->dma ||
		hw_deq_dma >= seg->dma + TRB_SEGMENT_SIZE) {
		pr_err("UGMI: rebuild dma out of range %pa\n", &hw_deq_dma);
		return -EINVAL;
	}

	offset = hw_deq_dma - seg->dma;
	trb_index = offset / sizeof(union xhci_trb);

	ring->deq_seg = seg;
	ring->dequeue = &seg->trbs[trb_index];

	pr_err("UGMI: rebuild final: idx=%u dma=%pa cycle=%u\n",
		   trb_index, &hw_deq_dma, ring->cycle_state);

	/* =========================
	 * Step5: 写 HW ERDP + clear EHB
	 * =========================
	 */
	hw_deq |= ERST_EHB;
	xhci_write_64(xhci, hw_deq, &ir->ir_set->erst_dequeue);

	return 0;
}

static int xhci_sync_erst_dequeue(struct xhci_hcd *xhci,
				  struct xhci_interrupter *ir)
{

	struct xhci_ring *ring = ir->event_ring;
	struct xhci_segment *seg;
	dma_addr_t hw_deq_dma, seg_dma;
	unsigned long offset;
	unsigned int trb_index;
	u64 hw_deq, hw_deq_erst, hw_deq_qsram10;
	int i = 0;

	uaudio_qdev->qsram = uaudio_get_qsram(uaudio_qdev->dev);
	if (!ring || !ring->first_seg)
		return -EINVAL;

	/* Read hardware dequeue pointer (DMA address) */
	hw_deq_erst = xhci_read_64(xhci, &ir->ir_set->erst_dequeue);
	hw_deq = (u64)readl(&uaudio_qdev->qsram->data[3]);
	hw_deq_qsram10 = (u64)readl(&uaudio_qdev->qsram->data[10]);
	uaudio_dbg("hw_deq=%llx hw_deq_10=%llx erst=%llx\n", (unsigned long long)hw_deq,
		(unsigned long long)hw_deq_qsram10, (unsigned long long)hw_deq_erst);
	if (hw_deq_qsram10 != 0) {
		hw_deq_dma = (dma_addr_t)(hw_deq_qsram10 & ERST_PTR_MASK);
		hw_deq = hw_deq_qsram10;
	} else {
		hw_deq_dma = (dma_addr_t)(hw_deq & ERST_PTR_MASK);
	}
	/* if it's ff0, means cycle state is about to change */
	if (((hw_deq_dma&0xfff) != 0xff0) && (uaudio_qdev->in_disconnect)) {
		if (hw_deq_dma >= ring->first_seg->dma + 4 * sizeof(union xhci_trb)) {
			hw_deq -= 4 * sizeof(union xhci_trb);
			hw_deq_dma = (dma_addr_t)(hw_deq & ERST_PTR_MASK);
		} else if (hw_deq_dma >= ring->first_seg->dma + 2 * sizeof(union xhci_trb)) {
			hw_deq -= 2 * sizeof(union xhci_trb);
			hw_deq_dma = (dma_addr_t)(hw_deq & ERST_PTR_MASK);
		}
	}

	ring->cycle_state = readl(&uaudio_qdev->qsram->data[2]);
	uaudio_dbg("hw_deq=%llx dma=%pa\n", (unsigned long long)hw_deq, &hw_deq_dma);
	/* Search through all segments in the ring */
	seg = ring->first_seg;
	do {
		seg_dma = seg->dma;

		/* Check if DMA address falls within this segment */
		if (hw_deq_dma >= seg_dma &&
		    hw_deq_dma < (seg_dma + TRB_SEGMENT_SIZE)) {
			/* Calculate offset within segment */
			offset = hw_deq_dma - seg_dma;

			/* Convert to TRB index */
			trb_index = offset / sizeof(union xhci_trb);

			/* Validate TRB index is within segment bounds */
			if (trb_index >= TRBS_PER_SEGMENT) {
				xhci_err(xhci, "Invalid TRB index %u in segment\n",
					 trb_index);
				return -EINVAL;
			}

			/* Update ring pointers with virtual address */
			ring->deq_seg = seg;
			ring->dequeue = &seg->trbs[trb_index];
			/*EHB set to clear */
			hw_deq |= ERST_EHB;
			xhci_write_64(xhci, hw_deq, &ir->ir_set->erst_dequeue);
			uaudio_dbg("hw_deq=%llx\n", (unsigned long long)hw_deq);
			return 0;

		}

		seg = seg->next;
	} while (seg != ring->first_seg);

	/* DMA address not found in any segment */
	xhci_err(xhci, "Hardware dequeue pointer 0x%llx not in event ring, fallback\n",
		 (unsigned long long)hw_deq_dma);

	for (i = 0; i < 64; i++)
		writel(0x0, &uaudio_qdev->qsram->data[i]);/*out of sync, reinit all to 0*/
	return xhci_rebuild_erdp_cycle(xhci, ir);
}

/*
 * Validate that a TRB pointer is inside [base, end)
 * Prevents NULL / corrupted / out-of-range pointer dereference
 */
static inline bool uaudio_trb_ptr_valid(union xhci_trb *ptr,
					union xhci_trb *base,
					union xhci_trb *end)
{
	if (unlikely(!ptr || !base || !end))
		return false;

	if (unlikely(ptr < base || ptr >= end))
		return false;

	/* Optional: enforce 16-byte TRB alignment */
	if (unlikely(((unsigned long)ptr & 0xF) != 0))
		return false;

	return true;
}

/*
 * uaudio_should_skip_trb - Decide whether to skip (drop) a TRB event from
 *                           the ADSP SW event ring.
 *
 * Rules:
 *  1. Non-Transfer TRBs (Port Status Change, Command Completion, etc.) are
 *     normal AP-owned events and must NOT be skipped.
 *  2. Transfer Event TRBs whose EP is ISOC (ISOC_OUT_EP or ISOC_IN_EP) are
 *     owned by the ADSP offload path; AP must NOT process them -> skip.
 *  3. Transfer Event TRBs whose EP is disabled (device gone) -> skip.
 *  4. Transfer Event TRBs for the Control EP (CTRL_EP) are stale completions
 *     of USB control transfers (e.g. SET_INTERFACE, SET_CUR) that were already
 *     handled by the AP primary interrupter.  Re-processing them produces
 *     "no TDs queued" warnings.
 *  5. All other Transfer Event TRBs (Bulk, Interrupt) belong to AP.
 *     If the completion code is COMP_USB_TRANSACTION_ERROR it is rewritten
 *     to COMP_STOPPED before being passed to xhci_handle_event_trb() to
 *     prevent the HID class driver from issuing new transfers or resetting
 *     the endpoint while it is being torn down.
 *
 * Returns true if the event should be dropped, false if it should be handled.
 */
static bool uaudio_should_skip_trb(struct xhci_hcd *xhci, union xhci_trb *trb)
{
	u32 trb_type, slot_id, ep_index;
	struct xhci_ep_ctx *ep_ctx;
	u32 ep_state, ep_type;

	uaudio_dbg("TRB raw: %08x %08x %08x %08x\n",
		le32_to_cpu(trb->generic.field[0]),
		le32_to_cpu(trb->generic.field[1]),
		le32_to_cpu(trb->generic.field[2]),
		le32_to_cpu(trb->generic.field[3]));

	trb_type = TRB_FIELD_TO_TYPE(le32_to_cpu(trb->generic.field[3]));
	if (trb_type != TRB_TRANSFER) {
		uaudio_dbg("pass non-Transfer TRB: type=%u field3=0x%08x\n",
			   trb_type, le32_to_cpu(trb->generic.field[3]));
		return false;
	}

	/* Extract slot_id and ep_index from Transfer Event TRB */
	slot_id = TRB_TO_SLOT_ID(le32_to_cpu(trb->generic.field[3]));
	ep_index = TRB_TO_EP_INDEX(le32_to_cpu(trb->generic.field[3]));
	/* Rule 3: device gone, skip */
	if (!xhci->devs[slot_id]) {
		uaudio_dbg("skip Transfer TRB: device gone slot=%u ep_idx=%u\n",
			   slot_id, ep_index);
		return true;
	}

	ep_ctx = xhci_get_ep_ctx(xhci,
		 xhci->devs[slot_id]->out_ctx,
		 ep_index);
	ep_state = le32_to_cpu(ep_ctx->ep_info) & EP_STATE_MASK;

	/* Rule 3: EP disabled (stopped), skip */
	if (ep_state == EP_STATE_DISABLED) {
		uaudio_dbg("skip Transfer TRB: EP disabled slot=%u ep_idx=%u\n",
			   slot_id, ep_index);
		return true;
	}

	ep_type = CTX_TO_EP_TYPE(le32_to_cpu(ep_ctx->ep_info2));
	/* Rule 2: ISOC EP is owned by ADSP offload, AP must not process it */
	if (ep_type == ISOC_OUT_EP || ep_type == ISOC_IN_EP) {
		uaudio_dbg("skip ADSP ISOC transfer event: slot=%u ep_idx=%u ep_type=%u\n",
			   slot_id, ep_index, ep_type);
		return true;
	}

	/* Rule 4: Control EP completions are stale sideband copies of transfers
	 * already handled by the AP primary interrupter; re-processing them
	 * produces "no TDs queued" warnings with no benefit.
	 */
	if (ep_type == CTRL_EP) {
		uaudio_dbg("pass stale CTRL_EP transfer event: slot=%u ep_idx=%u\n",
			   slot_id, ep_index);
		return false;
	}

	/*
	 * Rule 5: non-ISOC, non-CTRL Transfer event belongs to AP.
	 *
	 * If the completion code is COMP_USB_TRANSACTION_ERROR (4), rewrite
	 * it in-place to COMP_STOPPED (26) before passing the TRB to
	 * xhci_handle_event_trb().
	 *
	 * COMP_USB_TRANSACTION_ERROR causes handle_tx_event() to call
	 * xhci_handle_halted_endpoint() -> EP soft-reset.  Once the reset
	 * completes the HID class driver resubmits transfers to the endpoint.
	 * During USB audio offload disconnect the endpoint is already being
	 * torn down; the resubmission races with teardown and can cause
	 * use-after-free or permanently hung URBs.
	 *
	 * COMP_STOPPED (26) makes finish_td() return 0 immediately without
	 * completing the URB and without issuing any reset or resubmit,
	 * which is the safe behaviour when the endpoint is being stopped.
	 */
	if (GET_COMP_CODE(le32_to_cpu(trb->generic.field[2])) ==
	    COMP_USB_TRANSACTION_ERROR) {
		u32 f2 = le32_to_cpu(trb->generic.field[2]);

		f2 = (f2 & ~COMP_CODE_MASK) | ((u32)COMP_STOPPED << 24);
		trb->generic.field[2] = cpu_to_le32(f2);
		uaudio_dbg("rewrote TRANSACTION_ERROR->STOPPED: slot=%u ep_idx=%u ep_type=%u\n",
			   slot_id, ep_index, ep_type);
	}
	uaudio_dbg("pass AP non-ISOC transfer event: slot=%u ep_idx=%u ep_type=%u\n",
		   slot_id, ep_index, ep_type);

	return false;
}

/**
 * uaudio_qsram_reset_all - Reinit all QSRAM data words to 0
 *
 * Called on error/out-of-sync paths in xhci_handle_offload() to bring
 * QSRAM back to a known state.
 */
static void uaudio_qsram_reset_all(void)
{
	int i;

	for (i = 0; i < 64; i++)
		writel(0x0, &uaudio_qdev->qsram->data[i]);
}

/**
 * uaudio_find_offload_data - Find the offload_data for this xhci
 * @xhci: pointer to xhci_hcd
 * @pending_events: true if QSRAM shows enqueue_idx != dequeue_idx
 *
 * Prefers an active offload stream for this xhci instance. If none is
 * active but @pending_events is set and the SW event ring has not been
 * freed, falls back to the first not-yet-freed ring for this xhci so
 * ADSP-queued pending events are not lost during final stream teardown.
 *
 * Returns the offload_data to use, or NULL if none is found.
 */
static struct audio_offload_data *uaudio_find_offload_data(struct xhci_hcd *xhci,
							     bool pending_events)
{
	struct audio_offload_data *pending_drain_data = NULL;
	int card_num;

	for (card_num = 0; card_num < SNDRV_CARDS; card_num++) {
		struct audio_offload_data *data = &uadev[card_num].offload_data;

		if (uadev[card_num].xhci != xhci)
			continue;

		if (!data->event_buffer || !data->sw_event_ring)
			continue;

		if (data->active)
			return data;

		if (pending_events && !uaudio_qdev->sw_evt_ring_freed &&
			!pending_drain_data)
			pending_drain_data = data;
	}

	return pending_drain_data;
}

/**
 * uaudio_offload_prepare_ring - Snapshot event buffer, compute and validate
 * SW event ring pointers
 * @offload_data: offload data for this stream
 * @dequeue_idx: SW ring dequeue index read from QSRAM
 * @enqueue_idx: SW ring enqueue index read from QSRAM
 * @sw_event_ring: set to the base of the SW event ring on success
 * @sw_event_ring_end: set to the end of the SW event ring on success
 *
 * Returns 0 on success, -ENODEV if the event buffer is NULL, or -EINVAL
 * if the computed ring pointers are invalid.
 */
static int uaudio_offload_prepare_ring(struct audio_offload_data *offload_data,
					u32 dequeue_idx, u32 enqueue_idx,
					union xhci_trb **sw_event_ring,
					union xhci_trb **sw_event_ring_end)
{
	union xhci_trb *buf;

	/* snapshot buffer pointer once to avoid race */
	buf = (union xhci_trb *)READ_ONCE(offload_data->event_buffer);
	if (unlikely(!buf)) {
		uaudio_err("offload active but event_buffer NULL\n");
		return -ENODEV;
	}

	*sw_event_ring = buf;
	*sw_event_ring_end = buf + SW_EVENT_RING_NUM_TRBS;

	offload_data->sw_dequeue = buf + dequeue_idx;
	offload_data->sw_enqueue = buf + enqueue_idx;

	/* validate computed pointers BEFORE loop */
	if (unlikely(!uaudio_trb_ptr_valid(offload_data->sw_dequeue,
					*sw_event_ring,
					*sw_event_ring_end) ||
					!uaudio_trb_ptr_valid(offload_data->sw_enqueue,
					*sw_event_ring,
					*sw_event_ring_end))) {
		uaudio_err("invalid ring ptr: base=%p deq=%p enq=%p\n",
					*sw_event_ring,
					offload_data->sw_dequeue,
					offload_data->sw_enqueue);
		return -EINVAL;
	}

	return 0;
}

/**
 * uaudio_offload_process_events - Drain pending events from the SW event ring
 * @xhci: pointer to xhci_hcd
 * @ir: xhci interrupter
 * @offload_data: offload data for this stream
 * @sw_event_ring: base of the SW event ring
 * @sw_event_ring_end: end of the SW event ring
 * @qsram: QSRAM pointer to write back the dequeue index
 * @dequeue_idx: current dequeue index, updated as events are processed
 * @max_events_to_process: stop after this many events (-1 == unbounded),
 * used to cap draining of a pending/inactive ring
 *
 * Returns the number of events processed.
 */
static int uaudio_offload_process_events(struct xhci_hcd *xhci,
					  struct xhci_interrupter *ir,
					  struct audio_offload_data *offload_data,
					  union xhci_trb *sw_event_ring,
					  union xhci_trb *sw_event_ring_end,
					  struct qsram_xhci __iomem *qsram,
					  u32 *dequeue_idx,
					  int max_events_to_process)
{
	int events_processed = 0;
	int err = 0;

	/* Process events from SW ring */
	while (offload_data->sw_enqueue != offload_data->sw_dequeue) {
		/*
		 * Inactive-stream drain path: only handle up to
		 * UAUDIO_QSRAM_POLL_MAX_PENDING_EVENTS events here; leave
		 * the rest for the normal interrupt-driven path to pick up
		 * once (or if) a stream becomes active again.
		 */
		if (max_events_to_process >= 0 &&
			events_processed >= max_events_to_process) {
			uaudio_dbg("inactive drain: max %d events reached, deq=%u\n",
				   max_events_to_process, *dequeue_idx);
			break;
		}
		/* critical guard for UAF/corruption */
		if (unlikely(!uaudio_trb_ptr_valid(offload_data->sw_dequeue,
				sw_event_ring, sw_event_ring_end))) {
			uaudio_err("sw_dequeue invalid: %p base=%p\n",
						offload_data->sw_dequeue,
						sw_event_ring);
			break;
		}
		/* Process one event using the passed function pointer.
		 * Skip ADSP-owned ISOC transfer events and stopped-EP events;
		 * pass through non-Transfer TRBs and AP-owned non-ISOC
		 * transfer events.
		 *
		 * Guard ir->event_ring->dequeue before calling xhci_handle_event_trb:
		 * that kernel function internally calls trace_xhci_handle_event(xhci,
		 * ir->event_ring->dequeue) regardless of the TRB pointer we pass.
		 * If the sideband interrupter's HW event ring has not been set up, or
		 * its dequeue pointer is NULL (e.g. ring advanced past end with no
		 * link TRB), the trace point will dereference NULL and crash.
		 * Skipping the call in that case is safe: the TRB has already been
		 * consumed from the SW ring and will not be replayed.
		 */
		if (!uaudio_should_skip_trb(xhci, offload_data->sw_dequeue)) {
			if (unlikely(!ir->event_ring || !ir->event_ring->dequeue)) {
				uaudio_err("ir->event_ring or dequeue is NULL, skip xhci_handle_event_trb\n");
			} else {
				err = xhci_handle_event_trb(xhci,
						ir, offload_data->sw_dequeue);
			}
		}
		events_processed++;

		/* Move to next TRB */
		offload_data->sw_dequeue++;
		(*dequeue_idx)++;

		/* Wrap around if at end */
		if (offload_data->sw_dequeue >= sw_event_ring_end) {
			offload_data->sw_dequeue = sw_event_ring;
			*dequeue_idx = 0;
		}

		/* Write back dequeue index to QSRAM */
		writel(*dequeue_idx, &qsram->data[1]);

		if (err)
			break;
	}

	return events_processed;
}

/**
 * xhci_handle_offload - Process audio offload events from QSRAM
 * @xhci: pointer to xhci_hcd
 * @offload: Indicates the called about the success/failure of
 * the function.
 *
 * This function is called from the xHCI interrupt handler when audio
 * offload is active. It reads the SW event ring state from QSRAM and
 * processes pending events.
 */
static void xhci_handle_offload(void *unused, struct xhci_hcd *xhci,
			       struct xhci_interrupter *ir,
			       bool *offload)
{
	union xhci_trb *sw_event_ring, *sw_event_ring_end;
	u32 dequeue_idx;
	u32 enqueue_idx;
	u32 cycle_state;
	bool pending_events;
	u64 temp;
	int events_processed;
	int max_events_to_process = -1; /* -1 == unbounded */
	int ret;
	struct audio_offload_data *offload_data;
	struct qsram_xhci __iomem *qsram;

	/* Get QSRAM pointer */
	qsram = uaudio_qdev->qsram;
	if (!qsram) {
		uaudio_err("QSRAM not available for audio offload\n");
		*offload = false;
		return;
	}

	/* Read SW event ring state from QSRAM */
	enqueue_idx = readl(&qsram->data[0]);
	dequeue_idx = readl(&qsram->data[1]);
	cycle_state = readl(&qsram->data[2]);
	pending_events = enqueue_idx != dequeue_idx;

	uaudio_dbg("QSRAM ring state: enqueue_idx=%u dequeue_idx=%u cycle_state=%u\n",
		enqueue_idx, dequeue_idx, cycle_state);

	/* Check if this is the primary interrupter */
	if (!ir || ir->intr_num != 0) {
		*offload = false;
		return;
	}

	/* clear the interrupter pending flag */
	if (!ir->ip_autoclear) {
		u32 irq_pending;

		irq_pending = readl(&ir->ir_set->irq_pending);
		irq_pending |= IMAN_IP;
		writel(irq_pending, &ir->ir_set->irq_pending);
	}

	if (unlikely(enqueue_idx >= SW_EVENT_RING_NUM_TRBS ||
		dequeue_idx >= SW_EVENT_RING_NUM_TRBS)) {
		uaudio_err("QSRAM idx invalid: enq=%u deq=%u\n",
			   enqueue_idx, dequeue_idx);
		*offload = false;
		uaudio_qsram_reset_all();
		return;
	}
	/* clear interrupter done */

	if (xhci->xhc_state & XHCI_STATE_DYING ||
	    xhci->xhc_state & XHCI_STATE_HALTED) {
		pr_err("xHCI dying, ignoring interrupt\n");

		/* Clear the event handler busy flag (RW1C) */
		temp = xhci_read_64(xhci, &ir->ir_set->erst_dequeue);
		xhci_write_64(xhci, temp | ERST_EHB,
					 &ir->ir_set->erst_dequeue);
		*offload = false;
		uaudio_qsram_reset_all();
		return;
	}

	/* Find the offload data for this xhci instance */
	offload_data = uaudio_find_offload_data(xhci, pending_events);

	/*
	 * If we are only draining a pending/inactive ring (no active
	 * offload stream), cap the number of events handled here to
	 * UAUDIO_QSRAM_POLL_MAX_PENDING_EVENTS. A large backlog while
	 * inactive is unexpected; rely on the normal interrupt-driven
	 * path once a stream becomes active again rather than draining
	 * an unbounded number of stale events here.
	 */
	if (offload_data && !offload_data->active)
		max_events_to_process = UAUDIO_QSRAM_POLL_MAX_PENDING_EVENTS;

	if (!offload_data) {
		if (pending_events)
			uaudio_err("no drainable ring: enq=%u deq=%u cycle=%u freed=%u\n",
				   enqueue_idx, dequeue_idx,
				   cycle_state, uaudio_qdev->sw_evt_ring_freed);
		*offload = false;
		return;
	}

	if (!offload_data->event_buffer || !offload_data->sw_event_ring) {
		uaudio_err("Invalid offload data: buffer=%p ring=%p\n",
			offload_data->event_buffer, offload_data->sw_event_ring);
		*offload = false;
		uaudio_qsram_reset_all();
		return;
	}

	ret = uaudio_offload_prepare_ring(offload_data, dequeue_idx, enqueue_idx,
					   &sw_event_ring, &sw_event_ring_end);
	if (ret == -ENODEV) {
		*offload = false;
		return;
	} else if (ret) {
		uaudio_qsram_reset_all();
		*offload = true;
		return;
	}

	events_processed = uaudio_offload_process_events(xhci, ir, offload_data,
							  sw_event_ring, sw_event_ring_end,
							  qsram, &dequeue_idx,
							  max_events_to_process);
	if (events_processed == 0) {
		if ((le32_to_cpu(ir->event_ring->dequeue->event_cmd.flags) & TRB_CYCLE) ==
				ir->event_ring->cycle_state) {
			uaudio_dbg("unhandled event trb, gen %x %x %x %x\n",
				le32_to_cpu(ir->event_ring->dequeue->generic.field[0]),
				le32_to_cpu(ir->event_ring->dequeue->generic.field[1]),
				le32_to_cpu(ir->event_ring->dequeue->generic.field[2]),
				le32_to_cpu(ir->event_ring->dequeue->generic.field[3]));
		}
	}
	temp = xhci_read_64(xhci, &ir->ir_set->erst_dequeue);
	uaudio_dbg("Audio offload: processed %d events, deq=%u enq=%u offload=%d xhc_state 0x%x, temp 0x%llx\n",
		events_processed, dequeue_idx, enqueue_idx, *offload, xhci->xhc_state, temp);

	*offload = true;
}

static unsigned long uaudio_get_iova(unsigned long *curr_iova,
	size_t *curr_iova_size, struct list_head *head, size_t size)
{
	struct iova_info *info, *new_info = NULL;
	struct list_head *curr_head;
	unsigned long va = 0;
	size_t tmp_size = size;
	bool found = false;

	if (size % PAGE_4K) {
		uaudio_err("size %zu is not page size multiple\n", size);
		goto done;
	}

	if (size > *curr_iova_size) {
		uaudio_err("size %zu > curr size %zu\n", size, *curr_iova_size);
		goto done;
	}
	if (*curr_iova_size == 0) {
		uaudio_err("iova mapping is full\n");
		goto done;
	}

	list_for_each_entry(info, head, list) {
		/* exact size iova_info */
		if (!info->in_use && info->size == size) {
			info->in_use = true;
			va = info->start_iova;
			*curr_iova_size -= size;
			found = true;
			uaudio_dbg("exact size: %zu found\n", size);
			goto done;
		} else if (!info->in_use && tmp_size >= info->size) {
			if (!new_info)
				new_info = info;
			uaudio_dbg("partial size: %zu found\n", info->size);
			tmp_size -= info->size;
			if (tmp_size)
				continue;

			va = new_info->start_iova;
			for (curr_head = &new_info->list; curr_head !=
			&info->list; curr_head = curr_head->next) {
				new_info = list_entry(curr_head, struct
						iova_info, list);
				new_info->in_use = true;
			}
			info->in_use = true;
			*curr_iova_size -= size;
			found = true;
			goto done;
		} else {
			/* iova region in use */
			new_info = NULL;
			tmp_size = size;
		}
	}

	info = kzalloc(sizeof(struct iova_info), GFP_KERNEL);
	if (!info) {
		va = 0;
		goto done;
	}

	va = info->start_iova = *curr_iova;
	info->size = size;
	info->in_use = true;
	*curr_iova += size;
	*curr_iova_size -= size;
	found = true;
	list_add_tail(&info->list, head);

done:
	if (!found)
		uaudio_err("unable to find %zu size iova\n", size);
	else
		uaudio_dbg("va:0x%08lx curr_iova:0x%08lx curr_iova_size:%zu\n",
				va, *curr_iova, *curr_iova_size);

	return va;
}

static unsigned long uaudio_iommu_map(enum mem_type mtype, bool dma_coherent,
		phys_addr_t pa, size_t size, struct sg_table *sgt)
{
	unsigned long va_sg, va = 0;
	bool map = true;
	int i, ret;
	size_t sg_len, total_len = 0;
	struct scatterlist *sg;
	phys_addr_t pa_sg;
	int prot = IOMMU_READ | IOMMU_WRITE;

	if (dma_coherent)
		prot |= IOMMU_CACHE;

	switch (mtype) {
	case MEM_EVENT_RING:
		va = IOVA_BASE;
		/* er already mapped */
		if (uaudio_qdev->er_mapped)
			map = false;
		break;
	case MEM_XFER_RING:
		va = uaudio_get_iova(&uaudio_qdev->curr_xfer_ring_iova,
		&uaudio_qdev->xfer_ring_iova_size, &uaudio_qdev->xfer_ring_list,
		size);
		break;
	case MEM_XFER_BUF:
		va = uaudio_get_iova(&uaudio_qdev->curr_xfer_buf_iova,
		&uaudio_qdev->xfer_buf_iova_size, &uaudio_qdev->xfer_buf_list,
		size);
		break;
	case MEM_PSEUDO_EVT_RING:
		/* Check if already mapped - if so, just return existing VA */
		if (uaudio_qdev->pseudo_evt_ring_mapped) {
			uaudio_dbg("pseudo event ring already mapped\n");
			va = uaudio_qdev->pseudo_evt_ring_va;
			map = false;
			break;
		}

		/* Not mapped yet, allocate IOVA */
		va = uaudio_get_iova(&uaudio_qdev->curr_pseudo_evt_ring_iova,
			&uaudio_qdev->pseudo_evt_ring_iova_size,
			&uaudio_qdev->pseudo_evt_ring_list,
			size);
		if (!va) {
			uaudio_err("failed to allocate pseudo evt ring iova\n");
			break;
		}

		/* Mark as mapped (will be set to true after successful iommu_map) */
		uaudio_qdev->pseudo_evt_ring_mapped = true;
		break;
	default:
		uaudio_err("unknown mem type %d\n", mtype);
	}

	if (!va || !map)
		goto done;

	if (!sgt)
		goto skip_sgt_map;

	va_sg = va;
	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		sg_len = PAGE_ALIGN(sg->offset + sg->length);
		pa_sg = page_to_phys(sg_page(sg));
		ret = iommu_map(uaudio_qdev->domain, va_sg, pa_sg, sg_len,
						prot, GFP_KERNEL);
		if (ret) {
			uaudio_err("mapping failed ret%d\n", ret);
			uaudio_err("type:%d, pa:%pa iova:0x%08lx sg_len:%zu\n",
				mtype, &pa_sg, va_sg, sg_len);
			uaudio_iommu_unmap(MEM_XFER_BUF, va, size, total_len);
			va = 0;
			goto done;
		}
		uaudio_dbg("type:%d map pa:%pa to iova:0x%08lx len:%zu offset:%u\n",
				mtype, &pa_sg, va_sg, sg_len, sg->offset);
		va_sg += sg_len;
		total_len += sg_len;
	}

	if (size != total_len) {
		uaudio_err("iova size %zu != mapped iova size %zu\n", size,
				total_len);
		uaudio_iommu_unmap(MEM_XFER_BUF, va, size, total_len);
		va = 0;
	} else {
		/* Store VA for pseudo event ring on success */
		if (mtype == MEM_PSEUDO_EVT_RING)
			uaudio_qdev->pseudo_evt_ring_va = va;
	}

	return va;

skip_sgt_map:
	uaudio_dbg("type:%d map pa:%pa to iova:0x%08lx size:%zu\n", mtype, &pa,
			va, size);

	ret = iommu_map(uaudio_qdev->domain, va, pa, size, prot, GFP_KERNEL);
	if (ret)
		uaudio_err("failed to map pa:%pa iova:0x%lx type:%d ret:%d\n",
				&pa, va, mtype, ret);
done:
	return va;
}

static void uaudio_put_iova(unsigned long va, size_t size, struct list_head
	*head, size_t *curr_iova_size)
{
	struct iova_info *info;
	size_t tmp_size = size;
	bool found = false;

	list_for_each_entry(info, head, list) {
		if (info->start_iova == va) {
			if (!info->in_use) {
				uaudio_err("va %lu is not in use\n", va);
				return;
			}
			found = true;
			info->in_use = false;
			if (info->size == size)
				goto done;
		}

		if (found && tmp_size >= info->size) {
			info->in_use = false;
			tmp_size -= info->size;
			if (!tmp_size)
				goto done;
		}
	}

	if (!found) {
		uaudio_err("unable to find the va %lu\n", va);
		return;
	}
done:
	*curr_iova_size += size;
	uaudio_dbg("curr_iova_size %zu\n", *curr_iova_size);
}

static void uaudio_iommu_unmap(enum mem_type mtype, unsigned long va,
	size_t iova_size, size_t mapped_iova_size)
{
	size_t umap_size;
	bool unmap = true;

	if (!va || !iova_size)
		return;

	switch (mtype) {
	case MEM_EVENT_RING:
		if (uaudio_qdev->er_mapped)
			uaudio_qdev->er_mapped = false;
		else
			unmap = false;
		break;

	case MEM_XFER_RING:
		uaudio_put_iova(va, iova_size, &uaudio_qdev->xfer_ring_list,
		&uaudio_qdev->xfer_ring_iova_size);
		break;
	case MEM_XFER_BUF:
		uaudio_put_iova(va, iova_size, &uaudio_qdev->xfer_buf_list,
		&uaudio_qdev->xfer_buf_iova_size);
		break;
	case MEM_PSEUDO_EVT_RING:
		if (uaudio_qdev->pseudo_evt_ring_mapped) {
			uaudio_put_iova(va, iova_size,
				&uaudio_qdev->pseudo_evt_ring_list,
				&uaudio_qdev->pseudo_evt_ring_iova_size);
			uaudio_qdev->pseudo_evt_ring_mapped = false;
		} else {
			unmap = false;
		}
		break;

	default:
		uaudio_err("unknown mem type %d\n", mtype);
		unmap = false;
	}

	if (!unmap || !mapped_iova_size)
		return;

	uaudio_dbg("type %d: unmap iova 0x%08lx size %zu\n", mtype, va,
			mapped_iova_size);

	umap_size = iommu_unmap(uaudio_qdev->domain, va, mapped_iova_size);
	if (umap_size != mapped_iova_size)
		uaudio_err("unmapped size %zu for iova 0x%08lx of mapped size %zu\n",
				umap_size, va, mapped_iova_size);
}

/* looks up alias, if any, for controller DT node and returns the index */
static int usb_get_controller_id(struct usb_device *udev)
{
	if (udev->bus->sysdev && udev->bus->sysdev->of_node)
		return of_alias_get_id(udev->bus->sysdev->of_node, "usb");

	return -ENODEV;
}

static void *find_csint_desc(unsigned char *descstart, int desclen, u8 dsubtype)
{
	u8 *p, *end, *next;

	p = descstart;
	end = p + desclen;
	while (p < end) {
		if (p[0] < 2)
			return NULL;
		next = p + p[0];
		if (next > end)
			return NULL;
		if (p[1] == USB_DT_CS_INTERFACE && p[2] == dsubtype)
			return p;
		p = next;
	}
	return NULL;
}


static int initialize_uadev_if_in_use(int card_num,
		struct snd_usb_substream *subs, struct snd_usb_audio *chip);
/**
 * uaudio_populate_uac_desc() - parse UAC parameters and populate QMI resp
 * @subs: usb substream
 * @resp: QMI response buffer
 *
 * Parses information specified within UAC descriptors which explain the
 * sample parameters that the device expects.  This information is populated
 * to the QMI response sent back to the audio DSP.
 *
 */
static int uaudio_populate_uac_desc(struct snd_usb_substream *subs,
				    struct qmi_uaudio_stream_resp_msg_v01 *resp,
				    int pcm_card_num)
{
	struct usb_host_interface *alts;
	struct usb_interface_descriptor *altsd;
	struct usb_interface *iface;
	struct usb_interface_assoc_descriptor *assoc;
	struct uac_format_type_i_continuous_descriptor *fmt;
	struct uac_format_type_i_discrete_descriptor *fmt_v1;
	struct uac_format_type_i_ext_descriptor *fmt_v2;
	struct uac1_as_header_descriptor *as;
	void *hdr_ptr;
	int protocol, card_num;

	iface = usb_ifnum_to_if(subs->dev, subs->cur_audiofmt->iface);
	if (!iface) {
		dev_err(&subs->dev->dev, "interface # %d does not exist\n",
			subs->cur_audiofmt->iface);
		return -ENODEV;
	}

	assoc = iface->intf_assoc;
	card_num = pcm_card_num;
	alts = &iface->altsetting[subs->cur_audiofmt->altset_idx];
	altsd = get_iface_desc(alts);
	protocol = altsd->bInterfaceProtocol;

	/* get format type */
	if (protocol != UAC_VERSION_3) {
		fmt = find_csint_desc(alts->extra, alts->extralen,
				UAC_FORMAT_TYPE);
		if (!fmt) {
			uaudio_err("%u:%d : no UAC_FORMAT_TYPE desc\n",
					subs->cur_audiofmt->iface,
					subs->cur_audiofmt->altset_idx);
			return -ENODEV;
		}
	}

	if (!uadev[card_num].ctrl_intf) {
		uaudio_err("audio ctrl intf info not cached\n");
		return -ENODEV;
	}

	if (protocol != UAC_VERSION_3) {
		hdr_ptr = find_csint_desc(uadev[card_num].ctrl_intf->extra,
				uadev[card_num].ctrl_intf->extralen,
				UAC_HEADER);
		if (!hdr_ptr) {
			uaudio_err("no UAC_HEADER desc\n");
			return -ENODEV;
		}
	}

	if (protocol == UAC_VERSION_1) {
		struct uac1_ac_header_descriptor *uac1_hdr = hdr_ptr;

		as = find_csint_desc(alts->extra, alts->extralen,
			UAC_AS_GENERAL);
		if (!as) {
			uaudio_err("%u:%d : no UAC_AS_GENERAL desc\n",
					subs->cur_audiofmt->iface,
					subs->cur_audiofmt->altset_idx);
			return -ENODEV;
		}
		resp->data_path_delay = as->bDelay;
		resp->data_path_delay_valid = 1;
		fmt_v1 = (struct uac_format_type_i_discrete_descriptor *)fmt;
		resp->usb_audio_subslot_size = fmt_v1->bSubframeSize;
		resp->usb_audio_subslot_size_valid = 1;

		resp->usb_audio_spec_revision = le16_to_cpu(uac1_hdr->bcdADC);
		resp->usb_audio_spec_revision_valid = 1;
	} else if (protocol == UAC_VERSION_2) {
		struct uac2_ac_header_descriptor *uac2_hdr = hdr_ptr;

		fmt_v2 = (struct uac_format_type_i_ext_descriptor *)fmt;
		resp->usb_audio_subslot_size = fmt_v2->bSubslotSize;
		resp->usb_audio_subslot_size_valid = 1;

		resp->usb_audio_spec_revision = le16_to_cpu(uac2_hdr->bcdADC);
		resp->usb_audio_spec_revision_valid = 1;
	} else if (protocol == UAC_VERSION_3) {
		if (assoc->bFunctionSubClass ==
					UAC3_FUNCTION_SUBCLASS_FULL_ADC_3_0) {
			uaudio_err("full adc is not supported\n");
			return -EINVAL;
		}

		switch (le16_to_cpu(get_endpoint(alts, 0)->wMaxPacketSize)) {
		case UAC3_BADD_EP_MAXPSIZE_SYNC_MONO_16:
		case UAC3_BADD_EP_MAXPSIZE_SYNC_STEREO_16:
		case UAC3_BADD_EP_MAXPSIZE_ASYNC_MONO_16:
		case UAC3_BADD_EP_MAXPSIZE_ASYNC_STEREO_16: {
			resp->usb_audio_subslot_size = 0x2;
			break;
		}

		case UAC3_BADD_EP_MAXPSIZE_SYNC_MONO_24:
		case UAC3_BADD_EP_MAXPSIZE_SYNC_STEREO_24:
		case UAC3_BADD_EP_MAXPSIZE_ASYNC_MONO_24:
		case UAC3_BADD_EP_MAXPSIZE_ASYNC_STEREO_24: {
			resp->usb_audio_subslot_size = 0x3;
			break;
		}

		default:
			uaudio_err("%d: %u: Invalid wMaxPacketSize\n",
					subs->cur_audiofmt->iface,
					subs->cur_audiofmt->altset_idx);
			return -EINVAL;
		}
		resp->usb_audio_subslot_size_valid = 1;
	} else {
		uaudio_err("unknown protocol version %x\n", protocol);
		return -ENODEV;
	}

	resp->slot_id = subs->dev->slot_id;
	resp->slot_id_valid = 1;

	memcpy(&resp->std_as_opr_intf_desc, &alts->desc, sizeof(alts->desc));
	resp->std_as_opr_intf_desc_valid = 1;

	return 0;
}

/**
 * xhci_sideband_init_sw_event_ring - Initialize SW event ring for offload
 * @sb: pointer to xhci_sideband
 * @offload_data: pointer to audio_offload_data
 */

static void xhci_sideband_init_sw_event_ring(struct xhci_sideband *sb,
					     struct audio_offload_data *offload_data)
{
	struct xhci_ring *ring;
	struct xhci_segment *seg;
	union xhci_trb *trbs;
	dma_addr_t dma;
	int i;

	if (!offload_data->event_buffer) {
		uaudio_err("Invalid event buffer\n");
		return;
	}
	if (!offload_data->active) {
		/* Allocate the ring structure */
		ring = kzalloc(sizeof(*ring), GFP_KERNEL);
		if (!ring)
			return;
		/* Allocate a single segment for the ring */
		seg = kzalloc(sizeof(*seg), GFP_KERNEL);
		if (!seg) {
			kfree(ring);
			return;
		}
		/* Cast event_buffer to TRB array */
		trbs = (union xhci_trb *)offload_data->event_buffer;
		dma = (dma_addr_t)offload_data->event_buffer_va;
		/* Initialize segment */
		seg->trbs = trbs;
		seg->dma = dma;
		seg->next = seg;  /* Single segment, points to itself */
		seg->num = 0;

		/* Initialize ring structure */
		ring->first_seg = seg;
		ring->last_seg = seg;
		ring->type = TYPE_EVENT;
		ring->num_segs = 1;
		ring->cycle_state = 1;

		/* Set enqueue and dequeue to start of TRB array */
		ring->enqueue = trbs;
		ring->enq_seg = seg;
		ring->dequeue = trbs;
		ring->deq_seg = seg;

		INIT_LIST_HEAD(&ring->td_list);
		/* Assign to offload data */
		offload_data->sw_event_ring = ring;
		offload_data->sw_enqueue = trbs;
		offload_data->sw_dequeue = trbs;
		uaudio_qdev->qsram = uaudio_get_qsram(uaudio_qdev->dev);
		if (IS_ERR(uaudio_qdev->qsram)) {
			dev_err(uaudio_qdev->dev, "Failed to get qsram\n");
			kfree(ring);
			kfree(seg);
			return;
		}
		for (i = 0; i < 64; i++)
			writel(0x0, &uaudio_qdev->qsram->data[i]);
		uaudio_qdev->adsp_offload_active = true;
	}
	/* Ensure QSRAM writes complete before ADSP accesses */
	mb();
}

#define UAUDIO_OFFLOAD_READY_POLL_INTERVAL_MS	4
#define UAUDIO_OFFLOAD_READY_POLL_MAX_ITER	1000
#define UAUDIO_QSRAM_POLL_INTERVAL_MS		200

static bool uaudio_has_active_offload(void)
{
	int card_num;

	for (card_num = 0; card_num < SNDRV_CARDS; card_num++) {
		if (uadev[card_num].offload_data.active)
			return true;
	}

	return false;
}

static void uaudio_offload_qsram_poll_work(struct work_struct *work)
{
	struct uaudio_qmi_dev *qdev;
	struct uaudio_dev *dev = NULL;
	unsigned long flags;
	bool offload = false;
	u32 enqueue_idx, dequeue_idx, pending_events;
	int card_num;

	qdev = container_of(to_delayed_work(work), struct uaudio_qmi_dev,
						offload_qsram_poll_work);

	if (!qdev->offload_qsram_poll_active || !qdev->qsram ||
		qdev->sw_evt_ring_freed || !uaudio_has_active_offload()) {
		qdev->offload_qsram_poll_active = false;
		return;
	}

	enqueue_idx = readl(&qdev->qsram->data[0]);
	dequeue_idx = readl(&qdev->qsram->data[1]);

	if (enqueue_idx >= dequeue_idx)
		pending_events = enqueue_idx - dequeue_idx;
	else
		pending_events = SW_EVENT_RING_NUM_TRBS - dequeue_idx + enqueue_idx;
	/*
	 * Only self-drain a small number of stray pending events here.
	 * If more than UAUDIO_QSRAM_POLL_MAX_PENDING_EVENTS entries are
	 * outstanding, an interrupt must be relied upon to trigger the
	 * real handling, so bail out without touching the ring/xhci.
	 */
	if (pending_events > UAUDIO_QSRAM_POLL_MAX_PENDING_EVENTS)  {
		uaudio_dbg("QSRAM poll: too many pending (%u > %u), enq=%u deq=%u\n",
			   pending_events, UAUDIO_QSRAM_POLL_MAX_PENDING_EVENTS,
			   enqueue_idx, dequeue_idx);
	} else if (enqueue_idx != dequeue_idx) {
		for (card_num = 0; card_num < SNDRV_CARDS; card_num++) {
			if (!uadev[card_num].offload_data.active)
				continue;
			if (!uadev[card_num].sb || !uadev[card_num].sb->xhci ||
				!uadev[card_num].sb->ir)
				continue;

			dev = &uadev[card_num];
			break;
		}

		if (dev) {
			uaudio_dbg("QSRAM poll drain: enq=%u deq=%u card=%d\n",
				enqueue_idx, dequeue_idx, card_num);
			spin_lock_irqsave(&dev->sb->xhci->lock, flags);
			trace_android_vh_xhci_handle_offload(dev->xhci,
							dev->sb->ir,
							&offload);
			spin_unlock_irqrestore(&dev->sb->xhci->lock, flags);
		} else {
			uaudio_dbg("QSRAM poll pending but no active xhci: enq=%u deq=%u\n",
				enqueue_idx, dequeue_idx);
		}
	}

	if (qdev->offload_qsram_poll_active && !qdev->sw_evt_ring_freed &&
		uaudio_has_active_offload())
		schedule_delayed_work(&qdev->offload_qsram_poll_work,
				msecs_to_jiffies(UAUDIO_QSRAM_POLL_INTERVAL_MS));
	else
		qdev->offload_qsram_poll_active = false;
}

static void uaudio_start_offload_qsram_poll(void)
{
	if (!uaudio_qdev || uaudio_qdev->offload_qsram_poll_active)
		return;

	uaudio_qdev->offload_qsram_poll_active = true;
	schedule_delayed_work(&uaudio_qdev->offload_qsram_poll_work,
			msecs_to_jiffies(UAUDIO_QSRAM_POLL_INTERVAL_MS));
}

static void uaudio_stop_offload_qsram_poll(void)
{
	if (!uaudio_qdev)
		return;

	uaudio_qdev->offload_qsram_poll_active = false;
	cancel_delayed_work_sync(&uaudio_qdev->offload_qsram_poll_work);
}

static void uaudio_offload_ready_work(struct work_struct *work)
{
	struct xhci_hcd *xhci = NULL;
	int card_num, count = 0;
	u32 val;

	for (card_num = 0; card_num < SNDRV_CARDS; card_num++) {
		if (uadev[card_num].sb && uadev[card_num].sb->xhci) {
			xhci = uadev[card_num].sb->xhci;
			break;
		}
	}

	if (!xhci || !uaudio_qdev->qsram) {
		pr_debug("%s: no valid xhci\n", __func__);
		uaudio_qdev->poll_active = false;
		return;
	}

	while (uaudio_qdev->poll_active && count < UAUDIO_OFFLOAD_READY_POLL_MAX_ITER) {
		if (uaudio_qdev->sw_evt_ring_freed) {
			uaudio_err("sw_evt_ring_freed iter=%d\n", count);
			return;
		}
		val = readl(&uaudio_qdev->qsram->data[QSRAM_ADSP_READY_REG]);
		if (val & QSRAM_ADSP_READY_BIT) {
			register_trace_android_vh_xhci_handle_offload(xhci_handle_offload, NULL);
			writel(1, &uaudio_qdev->qsram->data[QSRAM_OFFLOAD_ACK_REG]);
			/* Ensure registration and write complete.*/
			mb();
			uaudio_start_offload_qsram_poll();
			uaudio_dbg("offload work done iter=%d\n", count);
			return;
		}
		count++;
		msleep(UAUDIO_OFFLOAD_READY_POLL_INTERVAL_MS);
	}
	uaudio_dbg("offload ready work timed out after %d ms (poll_active=%d)\n",
		   count * UAUDIO_OFFLOAD_READY_POLL_INTERVAL_MS,
		   uaudio_qdev->poll_active);
	uaudio_qdev->poll_active = false;
}

static bool allocate_once = true;

static int setup_xhci_interrupter(int card_num,
						struct snd_usb_substream *subs,
						struct qmi_uaudio_stream_resp_msg_v01 *resp)
{
	int ret = 0;

	if (!uadev[card_num].xhci) {
		struct usb_hcd *hcd = bus_to_hcd(subs->dev->bus);

		uadev[card_num].xhci = hcd_to_xhci(hcd);
	}

	if (resp->interrupter_num) {
		ret = xhci_sideband_create_interrupter(uadev[card_num].sb, 1,
						false, 0, uaudio_qdev->intr_num);
		if (ret == -ENOMEM)
			ret = -ENODEV;
	} else {
		dev_dbg(uaudio_qdev->dev, "Switching to 1IR approach");
		uadev[card_num].sb->ir = uadev[card_num].sb->xhci->interrupters[0];
	}

	return ret;
}

static int setup_pseudo_event_ring(int card_num,
					struct snd_usb_substream *subs,
					bool dma_coherent,
					struct qmi_uaudio_stream_resp_msg_v01 *resp)
{
	u8 *pseudo_evt_ring;
	phys_addr_t pseudo_evt_ring_pa;
	struct sg_table pseudo_evt_ring_sgt;
	unsigned long va;
	int ret = 0;

	if (allocate_once && (!uaudio_qdev->intr_num)) {
		pseudo_evt_ring = usb_alloc_coherent(subs->dev,
				PAGE_4K, GFP_KERNEL, &pseudo_evt_ring_pa);

		if (!pseudo_evt_ring) {
			ret = -ENOMEM;
			return ret;
		}

		memset(pseudo_evt_ring, 0, PAGE_4K);

		dma_get_sgtable(subs->dev->bus->sysdev,
				&pseudo_evt_ring_sgt,
				pseudo_evt_ring,
				pseudo_evt_ring_pa,
				PAGE_4K);

		va = uaudio_iommu_map(MEM_PSEUDO_EVT_RING, dma_coherent,
				pseudo_evt_ring_pa, PAGE_4K,
				&pseudo_evt_ring_sgt);
		if (!va) {
			ret = -ENOMEM;
			return ret;
		}

		uaudio_qdev->pseudo_evt_ring_buffer = pseudo_evt_ring;
		uaudio_qdev->pseudo_evt_ring_pa = pseudo_evt_ring_pa;
		uaudio_qdev->pseudo_evt_ring_va = va;
		uaudio_qdev->pseudo_evt_ring_udev = subs->dev;
		uadev[card_num].offload_data.event_buffer =
				uaudio_qdev->pseudo_evt_ring_buffer;
		uadev[card_num].offload_data.event_buffer_va =
				uaudio_qdev->pseudo_evt_ring_va;
		allocate_once = false;

	}

	if (!uaudio_qdev->intr_num) {
		resp->xhci_mem_info.pseudo_evt_ring.pa =
							uaudio_qdev->pseudo_evt_ring_pa;
		resp->xhci_mem_info.pseudo_evt_ring.size = PAGE_4K;
		resp->xhci_mem_info.pseudo_evt_ring.va =
			PREPEND_SID_TO_IOVA(uaudio_qdev->pseudo_evt_ring_va,
						uaudio_qdev->sid);

		dev_err(uaudio_qdev->dev,
			"pseudo_evt_ring va=0x%llx pa=0x%llx size=%u\n",
			resp->xhci_mem_info.pseudo_evt_ring.va,
			resp->xhci_mem_info.pseudo_evt_ring.pa,
			resp->xhci_mem_info.pseudo_evt_ring.size);
	}

	return ret;
}

/**
 * uaudio_add_endpoint - Safely add endpoint to xHCI sideband
 * @card_num: PCM card number
 * @ep: USB host endpoint to add
 *
 * This wrapper safely retrieves the sideband pointer and adds the endpoint.
 * It handles the case where sideband might change or become invalid.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int uaudio_add_endpoint(int card_num,
					struct usb_host_endpoint *ep)
{
	struct xhci_sideband *sb;
	int ret;

	if (card_num >= SNDRV_CARDS) {
		uaudio_err("invalid card number %d\n", card_num);
		return -EINVAL;
	}

	sb = uadev[card_num].sb;
	if (!sb) {
		dev_err(uaudio_qdev->dev,
			"sideband not available for card %d\n", card_num);
		return -ENODEV;
	}

	ret = xhci_sideband_add_endpoint(sb, ep);
	if (ret < 0) {
		dev_err(uaudio_qdev->dev,
			"failed to add endpoint (ret=%d)\n", ret);
		return ret;
	}
	return 0;
}

/**
 * prepare_data_endpoint() - Add data EP to sideband and extract its
 *                           transfer ring PA.
 * @subs:        USB audio substream
 * @card_num:    PCM card number
 * @resp:        QMI response buffer
 * @data_ep_pipe: output - data endpoint pipe value
 * @tr_data_pa:  output - physical address of data transfer ring
 *
 * Returns 0 on success, negative error code on failure. On failure the
 * data EP has already been removed from the sideband if it was added;
 * the caller does not need to remove it again.
 */
static int prepare_data_endpoint(struct snd_usb_substream *subs,
				 int card_num,
				 struct qmi_uaudio_stream_resp_msg_v01 *resp,
				 unsigned int *data_ep_pipe,
				 phys_addr_t *tr_data_pa)
{
	struct usb_host_endpoint *ep;
	struct sg_table *sgt;
	struct page *pg;
	struct xhci_ep_ctx *ep_ctx;
	unsigned int ep_index;
	u32 ep_state;
	u32 slot_id;
	int poll;
	int ret;

	ep = usb_pipe_endpoint(subs->dev, subs->data_endpoint->pipe);
	if (!ep) {
		uaudio_err("data ep # %d context is null\n",
			   subs->data_endpoint->ep_num);
		return -ENODEV;
	}

	*data_ep_pipe = subs->data_endpoint->pipe;
	memcpy(&resp->std_as_data_ep_desc, &ep->desc, sizeof(ep->desc));
	resp->std_as_data_ep_desc_valid = 1;

	ret = uaudio_add_endpoint(card_num, ep);
	if (ret < 0) {
		dev_err(uaudio_qdev->dev, "failed to get data ep ring address\n");
		return ret;
	}

	sgt = xhci_sideband_get_endpoint_buffer(uadev[card_num].sb, ep);
	if (!sgt) {
		dev_err(uaudio_qdev->dev, "failed to get data ep ring address\n");
		xhci_sideband_remove_endpoint(uadev[card_num].sb, ep);
		return -ENODEV;
	}

	ep_index = xhci_get_endpoint_index(&ep->desc);
	slot_id = subs->dev->slot_id;
	poll = 0;

	/* Ensure writes complete */
	mb();
	do {
		ep_ctx = xhci_get_ep_ctx(uadev[card_num].sb->xhci,
			 uadev[card_num].sb->xhci->devs[slot_id]->out_ctx, ep_index);
		ep_state = le32_to_cpu(ep_ctx->ep_info) & EP_STATE_MASK;
		uaudio_dbg("no ready %d state 0x%x poll: %d\n", ep_index, ep_state, poll);
		if (ep_state != EP_STATE_RUNNING)
			udelay(10);
		poll++;
	} while (ep_state != EP_STATE_RUNNING && poll < 1000);

	if (ep_state != EP_STATE_RUNNING) {
		uaudio_err("FAILURE!!");
		sg_free_table(sgt);
		xhci_sideband_remove_endpoint(uadev[card_num].sb, ep);
		return -ENODEV;
	}
	uaudio_dbg("ready %d state 0x%x\n", ep_index, ep_state);

	pg = sg_page(sgt->sgl);
	*tr_data_pa = page_to_phys(pg);
	resp->xhci_mem_info.tr_data.pa = sg_dma_address(sgt->sgl);
	sg_free_table(sgt);
	return 0;
}

/**
 * prepare_sync_endpoint() - Optionally add sync EP to sideband and extract
 *                           its transfer ring PA.
 * @subs:        USB audio substream
 * @card_num:    PCM card number
 * @resp:        QMI response buffer
 * @sync_ep_pipe: output - sync endpoint pipe value (0 if no sync EP)
 * @tr_sync_pa:  output - physical address of sync transfer ring (0 if none)
 *
 * Returns 0 on success (including the implicit-feedback / no-sync-EP case),
 * negative error code on failure.
 */
static int prepare_sync_endpoint(struct snd_usb_substream *subs,
				 int card_num,
				 struct qmi_uaudio_stream_resp_msg_v01 *resp,
				 unsigned int *sync_ep_pipe,
				 phys_addr_t *tr_sync_pa)
{
	struct usb_host_endpoint *ep;
	struct sg_table *sgt;
	struct page *pg;
	int ret;

	*sync_ep_pipe = 0;
	*tr_sync_pa   = 0;

	if (!subs->sync_endpoint)
		return 0;

	ep = usb_pipe_endpoint(subs->dev, subs->sync_endpoint->pipe);
	if (!ep) {
		uaudio_dbg("implicit fb on data ep\n");
		return 0;
	}

	*sync_ep_pipe = subs->sync_endpoint->pipe;
	memcpy(&resp->std_as_sync_ep_desc, &ep->desc, sizeof(ep->desc));
	resp->std_as_sync_ep_desc_valid = 1;

	ret = uaudio_add_endpoint(card_num, ep);
	if (ret < 0) {
		dev_err(uaudio_qdev->dev, "failed to get sync ep ring address\n");
		return ret;
	}

	sgt = xhci_sideband_get_endpoint_buffer(uadev[card_num].sb, ep);
	if (!sgt) {
		dev_err(uaudio_qdev->dev, "failed to get sync ep ring address\n");
		xhci_sideband_remove_endpoint(uadev[card_num].sb, ep);
		return -ENODEV;
	}
	pg = sg_page(sgt->sgl);
	*tr_sync_pa = page_to_phys(pg);
	resp->xhci_mem_info.tr_sync.pa = sg_dma_address(sgt->sgl);
	sg_free_table(sgt);
	return 0;
}

/**
 * prepare_iommu_mappings() - Map event ring and transfer rings into IOMMU.
 * @subs:        USB audio substream
 * @card_num:    PCM card number
 * @resp:        QMI response buffer
 * @dma_coherent: whether the bus is DMA-coherent
 * @tr_data_pa:  physical address of data transfer ring
 * @tr_sync_pa:  physical address of sync transfer ring (0 if none)
 * @tr_data_va:  output - IOVA of data transfer ring
 * @tr_sync_va:  output - IOVA of sync transfer ring (0 if none)
 *
 * Returns 0 on success, negative error code on failure.
 * Cleans up any partial mappings on failure.
 */
static int prepare_iommu_mappings(struct snd_usb_substream *subs,
				  int card_num,
				  struct qmi_uaudio_stream_resp_msg_v01 *resp,
				  bool dma_coherent,
				  phys_addr_t tr_data_pa,
				  phys_addr_t tr_sync_pa,
				  unsigned long *tr_data_va,
				  unsigned long *tr_sync_va)
{
	struct sg_table *sgt;
	phys_addr_t xhci_pa;
	unsigned long va;
	int ret;

	*tr_data_va = 0;
	*tr_sync_va = 0;

	/* Setup XHCI interrupter */
	ret = setup_xhci_interrupter(card_num, subs, resp);
	if (ret == -ENODEV || ret == -ENOMEM)
		return ret;

	/* Map event ring */
	sgt = xhci_sideband_get_event_buffer(uadev[card_num].sb);
	if (!sgt) {
		dev_err(uaudio_qdev->dev, "failed to get event ring address\n");
		ret = -ENODEV;
		goto free_sec_ring;
	}

	xhci_pa = page_to_phys(sg_page(sgt->sgl));
	resp->xhci_mem_info.evt_ring.pa = sg_dma_address(sgt->sgl);
	sg_free_table(sgt);
	if (!xhci_pa) {
		dev_err(uaudio_qdev->dev,
			"failed to get sec event ring address\n");
		ret = -ENODEV;
		goto free_sec_ring;
	}

	va = uaudio_iommu_map(MEM_EVENT_RING, dma_coherent, xhci_pa,
			      PAGE_4K, NULL);
	if (!va) {
		ret = -ENOMEM;
		goto free_sec_ring;
	}

	resp->xhci_mem_info.evt_ring.va = PREPEND_SID_TO_IOVA(va, uaudio_qdev->sid);
	resp->xhci_mem_info.evt_ring.size = PAGE_4K;
	uaudio_qdev->er_mapped = true;

	resp->speed_info = get_speed_info(subs->dev->speed);
	if (resp->speed_info == USB_AUDIO_DEVICE_SPEED_INVALID_V01) {
		ret = -ENODEV;
		goto unmap_er;
	}
	resp->speed_info_valid = 1;

	/* data transfer ring */
	va = uaudio_iommu_map(MEM_XFER_RING, dma_coherent, tr_data_pa,
			PAGE_4K, NULL);
	if (!va) {
		ret = -ENOMEM;
		goto unmap_er;
	}
	*tr_data_va = va;
	resp->xhci_mem_info.tr_data.va =
		PREPEND_SID_TO_IOVA(va, uaudio_qdev->sid);
	resp->xhci_mem_info.tr_data.size = PAGE_4K;

	/* sync transfer ring */
	if (!resp->xhci_mem_info.tr_sync.pa)
		return 0;

	va = uaudio_iommu_map(MEM_XFER_RING, dma_coherent, tr_sync_pa,
			PAGE_4K, NULL);
	if (!va) {
		ret = -ENOMEM;
		goto unmap_data;
	}
	*tr_sync_va = va;
	resp->xhci_mem_info.tr_sync.va =
		PREPEND_SID_TO_IOVA(va, uaudio_qdev->sid);
	resp->xhci_mem_info.tr_sync.size = PAGE_4K;
	return 0;

unmap_data:
	uaudio_iommu_unmap(MEM_XFER_RING, *tr_data_va, PAGE_4K, PAGE_4K);
	*tr_data_va = 0;
unmap_er:
	uaudio_iommu_unmap(MEM_EVENT_RING, IOVA_BASE, PAGE_4K, PAGE_4K);
free_sec_ring:
	xhci_sideband_remove_interrupter(uadev[card_num].sb);
	return ret;
}

/**
 * prepare_xfer_buffer() - Allocate and IOMMU-map the DMA transfer buffer,
 *                         then cache all interface info.
 * @subs:        USB audio substream
 * @card_num:    PCM card number
 * @pcm_dev_num: PCM device number
 * @info_idx:    interface info index
 * @resp:        QMI response buffer
 * @dma_coherent: whether the bus is DMA-coherent
 * @xfer_buf_len: requested transfer buffer length from QMI request
 * @data_ep_pipe: data endpoint pipe value
 * @sync_ep_pipe: sync endpoint pipe value
 * @tr_data_va:  IOVA of data transfer ring
 * @tr_sync_va:  IOVA of sync transfer ring
 * @out_xfer_buf: output - allocated xfer buffer, for cleanup by caller if
 *                a later phase fails
 * @out_xfer_buf_pa: output - PA of the allocated xfer buffer
 * @out_xfer_buf_len: output - length of the allocated xfer buffer
 *
 * Returns 0 on success, negative error code on failure.
 */
static int prepare_xfer_buffer(struct snd_usb_substream *subs,
			       int card_num,
			       int pcm_dev_num,
			       int info_idx,
			       struct qmi_uaudio_stream_resp_msg_v01 *resp,
			       bool dma_coherent,
			       u32 xfer_buf_len,
			       unsigned int data_ep_pipe,
			       unsigned int sync_ep_pipe,
			       unsigned long tr_data_va,
			       unsigned long tr_sync_va,
			       u8 **out_xfer_buf,
			       phys_addr_t *out_xfer_buf_pa,
			       u32 *out_xfer_buf_len)
{
	struct sg_table xfer_buf_sgt;
	phys_addr_t xfer_buf_pa;
	unsigned long va;
	u8 *xfer_buf;
	u32 len, mult, remainder;
	int ret;
	struct snd_usb_audio *chip;

	if (!xfer_buf_len)
		xfer_buf_len = PAGE_4K;

	mult      = xfer_buf_len / PAGE_4K;
	remainder = xfer_buf_len % PAGE_4K;
	len       = mult * PAGE_4K;
	len      += remainder ? PAGE_4K : 0;

	if (len > MAX_XFER_BUFF_LEN) {
		uaudio_err("req buf len %d > max buf len %d, setting %d\n",
				len, MAX_XFER_BUFF_LEN, MAX_XFER_BUFF_LEN);
		len = MAX_XFER_BUFF_LEN;
	}

	xfer_buf = usb_alloc_coherent(subs->dev, len, GFP_KERNEL, &xfer_buf_pa);
	if (!xfer_buf)
		return -ENOMEM;

	dma_get_sgtable(subs->dev->bus->sysdev, &xfer_buf_sgt,
			xfer_buf, xfer_buf_pa, len);
	va = uaudio_iommu_map(MEM_XFER_BUF, dma_coherent, xfer_buf_pa,
			      len, &xfer_buf_sgt);
	sg_free_table(&xfer_buf_sgt);
	if (!va) {
		usb_free_coherent(subs->dev, len, xfer_buf, xfer_buf_pa);
		return -ENOMEM;
	}

	uadev[card_num].offload_data.sw_enqueue = NULL;
	uadev[card_num].offload_data.sw_dequeue = NULL;

	resp->xhci_mem_info.xfer_buff.pa   = xfer_buf_pa;
	resp->xhci_mem_info.xfer_buff.size  = len;
	resp->xhci_mem_info.xfer_buff.va    =
		PREPEND_SID_TO_IOVA(va, uaudio_qdev->sid);
	resp->xhci_mem_info_valid = 1;

	chip = uadev[card_num].chip;

	if (atomic_read(&uadev[card_num].in_use) == 1 && chip) {
		ret = initialize_uadev_if_in_use(card_num, subs, chip);
		if (ret < 0) {
			usb_free_coherent(subs->dev, len, xfer_buf, xfer_buf_pa);
			uaudio_iommu_unmap(MEM_XFER_BUF, va, len, len);
			return ret;
		}
	}

	uadev[card_num].card_num    = card_num;
	uadev[card_num].usb_core_id = resp->controller_num;

	/* Cache interface-specific info for cleanup on disable/disconnect */
	uadev[card_num].info[info_idx].data_xfer_ring_va   = tr_data_va;
	uadev[card_num].info[info_idx].data_xfer_ring_size = PAGE_4K;
	uadev[card_num].info[info_idx].sync_xfer_ring_va   = tr_sync_va;
	uadev[card_num].info[info_idx].sync_xfer_ring_size = PAGE_4K;
	uadev[card_num].info[info_idx].xfer_buf_va         = va;
	uadev[card_num].info[info_idx].xfer_buf_pa         = xfer_buf_pa;
	uadev[card_num].info[info_idx].xfer_buf_size       = len;
	uadev[card_num].info[info_idx].data_ep_pipe        = data_ep_pipe;
	uadev[card_num].info[info_idx].sync_ep_pipe        = sync_ep_pipe;
	uadev[card_num].info[info_idx].data_ep_idx         = subs->data_endpoint ?
							   subs->data_endpoint->ep_num : 0;
	uadev[card_num].info[info_idx].sync_ep_idx         = subs->sync_endpoint ?
							   subs->sync_endpoint->ep_num : 0;
	uadev[card_num].info[info_idx].xfer_buf            = xfer_buf;
	uadev[card_num].info[info_idx].pcm_card_num        = card_num;
	uadev[card_num].info[info_idx].pcm_dev_num         = pcm_dev_num;
	uadev[card_num].info[info_idx].direction           = subs->direction;
	uadev[card_num].info[info_idx].intf_num            = subs->cur_audiofmt->iface;
	uadev[card_num].info[info_idx].in_use              = true;

	*out_xfer_buf     = xfer_buf;
	*out_xfer_buf_pa  = xfer_buf_pa;
	*out_xfer_buf_len = len;

	return 0;
}

static int prepare_qmi_response(struct snd_usb_substream *subs,
		struct qmi_uaudio_stream_req_msg_v01 *req_msg,
		struct qmi_uaudio_stream_resp_msg_v01 *resp, int info_idx)
{
	struct usb_interface *iface;
	int ret;
	int card_num, pcm_dev_num;
	unsigned int data_ep_pipe = 0, sync_ep_pipe = 0;
	unsigned long tr_data_va = 0, tr_sync_va = 0;
	phys_addr_t tr_data_pa = 0, tr_sync_pa = 0;
	u8 *xfer_buf = NULL;
	phys_addr_t xfer_buf_pa = 0;
	u32 xfer_buf_len = 0;
	bool dma_coherent;

	iface = usb_ifnum_to_if(subs->dev, subs->cur_audiofmt->iface);
	if (!iface) {
		uaudio_err("interface # %d does not exist\n",
			   subs->cur_audiofmt->iface);
		return -ENODEV;
	}

	pcm_dev_num = (req_msg->usb_token & SND_PCM_DEV_NUM_MASK) >> 8;
	card_num    = (req_msg->usb_token & SND_PCM_CARD_NUM_MASK) >> 16;

	if (!subs->data_endpoint) {
		uaudio_err("data_endpoint is null\n");
		return -EINVAL;
	}

	ret = uaudio_populate_uac_desc(subs, resp, card_num);
	if (ret < 0)
		return ret;

	/* Phase 1: data endpoint */
	ret = prepare_data_endpoint(subs, card_num, resp,
				    &data_ep_pipe, &tr_data_pa);
	if (ret < 0)
		return ret;

	/* Phase 2: sync endpoint */
	ret = prepare_sync_endpoint(subs, card_num, resp,
				    &sync_ep_pipe, &tr_sync_pa);
	if (ret < 0)
		goto drop_data_ep;

	resp->interrupter_num = uaudio_qdev->intr_num;
	resp->interrupter_num_valid = 1;
	resp->controller_num_valid = 0;
	ret = usb_get_controller_id(subs->dev);
	if (ret >= 0) {
		resp->controller_num = ret;
		resp->controller_num_valid = 1;
	}

	dma_coherent = dev_is_dma_coherent(subs->dev->bus->sysdev);

	/* Phase 3: IOMMU mappings for event ring + transfer rings */
	ret = prepare_iommu_mappings(subs, card_num, resp, dma_coherent,
				     tr_data_pa, tr_sync_pa,
				     &tr_data_va, &tr_sync_va);
	if (ret < 0)
		goto drop_sync_ep;

	/* Phase 4: transfer buffer allocation + intf info caching */
	ret = prepare_xfer_buffer(subs, card_num, pcm_dev_num, info_idx,
				  resp, dma_coherent,
				  req_msg->xfer_buff_size,
				  data_ep_pipe, sync_ep_pipe,
				  tr_data_va, tr_sync_va,
				  &xfer_buf, &xfer_buf_pa, &xfer_buf_len);
	if (ret < 0)
		goto unmap_rings;

	/* Phase 5: pseudo event ring */
	ret = setup_pseudo_event_ring(card_num, subs, dma_coherent, resp);
	if (ret < 0)
		goto free_xfer_buf;

	set_bit(card_num, &uaudio_qdev->card_slot);
	return 0;

free_xfer_buf:
	usb_free_coherent(subs->dev, xfer_buf_len, xfer_buf, xfer_buf_pa);
unmap_rings:
	uaudio_iommu_unmap(MEM_XFER_RING, tr_sync_va, PAGE_4K, PAGE_4K);
	uaudio_iommu_unmap(MEM_XFER_RING, tr_data_va, PAGE_4K, PAGE_4K);
	uaudio_iommu_unmap(MEM_EVENT_RING, IOVA_BASE, PAGE_4K, PAGE_4K);
	xhci_sideband_remove_interrupter(uadev[card_num].sb);
drop_sync_ep:
	if (subs->sync_endpoint)
		xhci_sideband_remove_endpoint(uadev[card_num].sb,
			usb_pipe_endpoint(subs->dev, subs->sync_endpoint->pipe));
drop_data_ep:
	xhci_sideband_remove_endpoint(uadev[card_num].sb,
			usb_pipe_endpoint(subs->dev, subs->data_endpoint->pipe));
	return ret;
}


static int initialize_uadev_if_in_use(int card_num,
		struct snd_usb_substream *subs, struct snd_usb_audio *chip)
{
	uaudio_dbg("Device in use: card_num=%d, num_intf=%d",
		card_num, subs->dev->config->desc.bNumInterfaces);
	if (!uadev[card_num].disconnect_wq_init) {
		init_waitqueue_head(&uadev[card_num].disconnect_wq);
		uadev[card_num].disconnect_wq_init = true;
	}
	uadev[card_num].num_intf =
		subs->dev->config->desc.bNumInterfaces;
	uadev[card_num].info = kcalloc(uadev[card_num].num_intf,
		sizeof(struct intf_info), GFP_KERNEL);
	if (!uadev[card_num].info)
		return -ENOMEM;
	if (!chip)
		return -ENODEV;
	mutex_lock(&chip->mutex);
	uadev[card_num].udev = subs->dev;
	mutex_unlock(&chip->mutex);
	return 0;
}

static void uaudio_dev_intf_cleanup(struct usb_device *udev,
	struct intf_info *info)
{

	if (!info) {
		uaudio_err("info is NULL\n");
		return;
	}

	uaudio_iommu_unmap(MEM_XFER_RING, info->data_xfer_ring_va,
		info->data_xfer_ring_size, info->data_xfer_ring_size);
	info->data_xfer_ring_va = 0;
	info->data_xfer_ring_size = 0;

	uaudio_iommu_unmap(MEM_XFER_RING, info->sync_xfer_ring_va,
		info->sync_xfer_ring_size, info->sync_xfer_ring_size);
	info->sync_xfer_ring_va = 0;
	info->sync_xfer_ring_size = 0;

	uaudio_iommu_unmap(MEM_XFER_BUF, info->xfer_buf_va,
		info->xfer_buf_size, info->xfer_buf_size);
	info->xfer_buf_va = 0;

	usb_free_coherent(udev, info->xfer_buf_size,
		info->xfer_buf, info->xfer_buf_pa);
	info->xfer_buf_size = 0;
	info->xfer_buf = NULL;
	info->xfer_buf_pa = 0;

	info->in_use = false;

	uaudio_dbg("release resources: intf# %d card# %d\n",
			info->intf_num, info->pcm_card_num);
}

static void uaudio_event_ring_cleanup_free(struct uaudio_dev *dev)
{
	clear_bit(dev->card_num, &uaudio_qdev->card_slot);
	/* all audio devices are disconnected */
	if (!uaudio_qdev->card_slot) {
		uaudio_iommu_unmap(MEM_EVENT_RING, IOVA_BASE, PAGE_4K,
			PAGE_4K);
		if (dev->chip)
			xhci_sideband_remove_interrupter(uadev[dev->chip->card->number].sb);

		uaudio_dbg("all audio devices disconnected\n");
	}
}

static void uaudio_dev_cleanup(struct uaudio_dev *dev)
{
	int if_idx;

	if (!dev->udev) {
		uaudio_dbg("USB audio device memory is already freed.\n");
		return;
	}

	/* free xfer buffer and unmap xfer ring and buf per interface */
	for (if_idx = 0; if_idx < dev->num_intf; if_idx++) {
		if (!dev->info[if_idx].in_use)
			continue;
		uaudio_dev_intf_cleanup(dev->udev, &dev->info[if_idx]);
	}

	dev->num_intf = 0;

	/* free interface info */
	kfree(dev->info);
	dev->info = NULL;
	uaudio_event_ring_cleanup_free(dev);
	dev->udev = NULL;
}

static int uaudio_sb_notifier(struct usb_interface*,
				struct xhci_sideband_event*);
static void uaudio_connect(struct snd_usb_audio *chip)
{
	struct xhci_sideband *sb;
	struct usb_interface *intf;

	if (chip->card->number >= SNDRV_CARDS) {
		uaudio_err("Invalid card number\n");
		return;
	}

	atomic_set(&card_disconnecting[chip->card->number], 0);
	mutex_lock(&chip->mutex);
	intf = chip->intf[chip->num_interfaces - 1];
	sb = xhci_sideband_register(intf, XHCI_SIDEBAND_VENDOR,
						 uaudio_sb_notifier);
	if (!sb) {
		mutex_unlock(&chip->mutex);
		return;
	}
	uadev[chip->card->number].chip = chip;
	uadev[chip->card->number].sb = sb;
	uaudio_info("WA for audio headset!");
	uadev[chip->card->number].chip->quirk_flags |=
				QUIRK_FLAG_CTL_MSG_DELAY_5M | QUIRK_FLAG_GET_SAMPLE_RATE;
	mutex_unlock(&chip->mutex);
}

/*
 * Sends QMI disconnect indication message, assumes chip->mutex and qdev_mutex
 * lock held by caller.
 */
static int uaudio_send_disconnect_ind(struct snd_usb_audio *chip)
{
	struct qmi_uaudio_stream_ind_msg_v01 disconnect_ind = {0};
	struct uaudio_qmi_svc *svc = uaudio_svc;
	struct uaudio_dev *dev;
	int ret = 0, i = 0;

	dev = &uadev[chip->card->number];

	if (atomic_read(&dev->in_use)) {
		uaudio_dbg("sending qmi indication disconnect\n");
		uaudio_dbg("sq->sq_family:%x sq->sq_node:%x sq->sq_port:%x\n",
				svc->client_sq.sq_family,
				svc->client_sq.sq_node, svc->client_sq.sq_port);
		disconnect_ind.dev_event = USB_AUDIO_DEV_DISCONNECT_V01;
		disconnect_ind.slot_id = dev->udev->slot_id;
		disconnect_ind.controller_num = dev->usb_core_id;
		disconnect_ind.controller_num_valid = 1;
		mutex_unlock(&chip->mutex);
		ret = qmi_send_indication(svc->uaudio_svc_hdl, &svc->client_sq,
					  QMI_UAUDIO_STREAM_IND_V01,
					  QMI_UAUDIO_STREAM_IND_MSG_V01_MAX_MSG_LEN,
					  qmi_uaudio_stream_ind_msg_v01_ei,
					  &disconnect_ind);
		if (ret < 0)
			uaudio_err("qmi send failed with err: %d\n", ret);

		mutex_unlock(&uadev[chip->card->number].mutex);
		ret = wait_event_interruptible_timeout(dev->disconnect_wq,
				!atomic_read(&dev->in_use),
				msecs_to_jiffies(DEV_RELEASE_WAIT_TIMEOUT));
		if (!ret) {
			uaudio_err("timeout while waiting for dev_release\n");
			if (dev->offload_data.active)
				uaudio_err("offload_data.active = %u non-zero on in_use clear (timeout)\n",
					   dev->offload_data.active);
			dev->offload_data.active = 0;
			atomic_set(&dev->in_use, 0);
		} else if (ret < 0) {
			uaudio_err("failed with ret %d\n", ret);
			if (dev->offload_data.active)
				uaudio_err("offload_data.active = %u non-zero on in_use clear (wait err)\n",
					   dev->offload_data.active);
			dev->offload_data.active = 0;
			atomic_set(&dev->in_use, 0);
		}
		mutex_lock(&uadev[chip->card->number].mutex);
		mutex_lock(&chip->mutex);
	}
	for (i = 0; i < 64; i++)
		writel(0x0, &uaudio_qdev->qsram->data[i]);

	return ret;
}

static void xhci_sideband_cleanup_sw_event_ring
			(struct xhci_sideband *sb,
			struct audio_offload_data *offload_data);

static void uaudio_disconnect(struct snd_usb_audio *chip)
{
	struct uaudio_dev *dev;
	int card_num;

	if (!chip) {
		uaudio_err("chip is NULL\n");
		return;
	}

	card_num = chip->card->number;
	if (card_num >= SNDRV_CARDS) {
		uaudio_err("invalid card number\n");
		return;
	}
	atomic_set(&card_disconnecting[card_num], 1);
	mutex_lock(&uadev[card_num].mutex);
	mutex_lock(&chip->mutex);
	dev = &uadev[card_num];
	if (!dev->xhci) {
		uaudio_dbg("xhci null bail disconnect\n");
		goto done;
	}
	/* clean up */
	if (!dev->udev) {
		uaudio_dbg("no clean up required\n");
		goto done;
	}
	writel(0x1, &uaudio_qdev->qsram->data[12]);
	uaudio_dbg("qsram 12=%x\n", readl((&uaudio_qdev->qsram->data[12])));

	if (uaudio_qdev->poll_active) {
		flush_work(&uaudio_qdev->offload_ready_work);
		uaudio_qdev->poll_active = false;  /* signal worker to stop */
	}
	uaudio_qdev->in_disconnect = true;
	uaudio_send_disconnect_ind(chip);
	/* Make sure indication does first before dev.*/
	mb();
	uaudio_qdev->in_disconnect = false;
	uaudio_qdev->sw_evt_ring_freed = true;
	uaudio_dev_cleanup(dev);
done:
	uaudio_dbg("sb_sync\n");

	if (dev->sb)
		xhci_sideband_unregister(dev->sb);

	/* Free the shared pseudo event ring */
	if (uaudio_qdev->pseudo_evt_ring_mapped) {
		uaudio_iommu_unmap(MEM_PSEUDO_EVT_RING,
			uaudio_qdev->pseudo_evt_ring_va,
			PAGE_4K, PAGE_4K);

		if (uaudio_qdev->pseudo_evt_ring_buffer) {
			usb_free_coherent(uaudio_qdev->pseudo_evt_ring_udev, PAGE_4K,
				uaudio_qdev->pseudo_evt_ring_buffer,
				uaudio_qdev->pseudo_evt_ring_pa);
		}
		uaudio_qdev->pseudo_evt_ring_buffer = NULL;
	}
	/* Free the segment */
	if ((uadev[card_num].offload_data.sw_event_ring) &&
		(uadev[card_num].offload_data.sw_event_ring->first_seg)) {
		kfree(uadev[card_num].offload_data.sw_event_ring->first_seg);

		/* Free the ring structure */
		kfree(uadev[card_num].offload_data.sw_event_ring);

		uadev[card_num].offload_data.sw_event_ring = NULL;
		uadev[card_num].offload_data.sw_enqueue = NULL;
		uadev[card_num].offload_data.sw_dequeue = NULL;
		uaudio_dbg("pseudo event ring destroyed\n");
		uadev[card_num].xhci = NULL;
	}

	allocate_once = true;
	/*
	 * Cancel stop_ep_monitor_work before nulling sb (issue 19.8).
	 * The work item dereferences uadev[card_num].sb->xhci->lock;
	 * cancel_work_sync() blocks until any running instance finishes.
	 * We hold uadev.mutex here so this is safe.
	 */
	if (uaudio_qdev->stop_monitor_wq)
		cancel_work_sync(&uaudio_qdev->stop_monitor.work);

	uadev[card_num].sb = NULL;
	uadev[card_num].chip = NULL;
	mutex_unlock(&chip->mutex);
	mutex_unlock(&uadev[card_num].mutex);
}

static unsigned int uaudio_ep_index_to_address(unsigned int ep_index)
{
	unsigned int ep_num = (ep_index + 1) / 2;
	unsigned int direction = (ep_index % 2) ? USB_DIR_OUT : USB_DIR_IN;

	return direction | ep_num;
}

/**
 * uaudio_sb_notifier() - xHCI sideband event handler
 * @intf: USB interface handle
 * @evt: xHCI sideband event type
 *
 * This callback is executed when the xHCI sideband encounters a sequence
 * that requires the sideband clients to take action.  An example, is when
 * xHCI frees the transfer ring, so the client has to ensure that the
 * offload path is halted.
 *
 */
static int uaudio_sb_notifier(struct usb_interface *intf,
				struct xhci_sideband_event *evt)
{
	struct snd_usb_audio *chip;
	struct uaudio_dev *dev;
	int if_idx;

	if (!intf || !evt)
		return 0;

	chip = usb_get_intfdata(intf);

	mutex_lock(&uadev[chip->card->number].mutex);
	mutex_lock(&chip->mutex);

	dev = &uadev[chip->card->number];

	if (evt->type == XHCI_SIDEBAND_XFER_RING_FREE) {
		unsigned int *ep = (unsigned int *) evt->evt_data;
		unsigned int ep_addr = uaudio_ep_index_to_address(*ep);

		for (if_idx = 0; if_idx < dev->num_intf; if_idx++) {
			if ((dev->info[if_idx].data_ep_idx == ep_addr) ||
			    (dev->info[if_idx].sync_ep_idx == ep_addr)) {
				uaudio_qdev->in_disconnect = true;
				uaudio_send_disconnect_ind(chip);
				uaudio_qdev->in_disconnect = false;
			}
		}
	}
	mutex_unlock(&chip->mutex);
	mutex_unlock(&uadev[chip->card->number].mutex);

	return 0;
}

static void uaudio_dev_release(struct uaudio_dev *dev)
{
	uaudio_dbg("for dev %pK\n", dev);

	uaudio_event_ring_cleanup_free(dev);
	if (dev->offload_data.active)
		uaudio_err("offload_data.active=%u non-zero on in_use clear (dev_release)\n",
			   dev->offload_data.active);
	dev->offload_data.active = 0;
	atomic_set(&dev->in_use, 0);
	wake_up(&dev->disconnect_wq);
}

/* maps audio format received over QMI to asound.h based pcm format */
static snd_pcm_format_t map_pcm_format(enum usb_qmi_audio_format fmt_received)
{
	switch (fmt_received) {
	case USB_QMI_PCM_FORMAT_S8:
		return SNDRV_PCM_FORMAT_S8;
	case USB_QMI_PCM_FORMAT_U8:
		return SNDRV_PCM_FORMAT_U8;
	case USB_QMI_PCM_FORMAT_S16_LE:
		return SNDRV_PCM_FORMAT_S16_LE;
	case USB_QMI_PCM_FORMAT_S16_BE:
		return SNDRV_PCM_FORMAT_S16_BE;
	case USB_QMI_PCM_FORMAT_U16_LE:
		return SNDRV_PCM_FORMAT_U16_LE;
	case USB_QMI_PCM_FORMAT_U16_BE:
		return SNDRV_PCM_FORMAT_U16_BE;
	case USB_QMI_PCM_FORMAT_S24_LE:
		return SNDRV_PCM_FORMAT_S24_LE;
	case USB_QMI_PCM_FORMAT_S24_BE:
		return SNDRV_PCM_FORMAT_S24_BE;
	case USB_QMI_PCM_FORMAT_U24_LE:
		return SNDRV_PCM_FORMAT_U24_LE;
	case USB_QMI_PCM_FORMAT_U24_BE:
		return SNDRV_PCM_FORMAT_U24_BE;
	case USB_QMI_PCM_FORMAT_S24_3LE:
		return SNDRV_PCM_FORMAT_S24_3LE;
	case USB_QMI_PCM_FORMAT_S24_3BE:
		return SNDRV_PCM_FORMAT_S24_3BE;
	case USB_QMI_PCM_FORMAT_U24_3LE:
		return SNDRV_PCM_FORMAT_U24_3LE;
	case USB_QMI_PCM_FORMAT_U24_3BE:
		return SNDRV_PCM_FORMAT_U24_3BE;
	case USB_QMI_PCM_FORMAT_S32_LE:
		return SNDRV_PCM_FORMAT_S32_LE;
	case USB_QMI_PCM_FORMAT_S32_BE:
		return SNDRV_PCM_FORMAT_S32_BE;
	case USB_QMI_PCM_FORMAT_U32_LE:
		return SNDRV_PCM_FORMAT_U32_LE;
	case USB_QMI_PCM_FORMAT_U32_BE:
		return SNDRV_PCM_FORMAT_U32_BE;
	default:
		/*
		 * We expect the caller to do input validation so we should
		 * never hit this. But we do have to return a proper
		 * snd_pcm_format_t value due to the __bitwise attribute; so
		 * just return the equivalent of 0 in case of bad input.
		 */
		return SNDRV_PCM_FORMAT_S8;
	}
}

static int info_idx_from_ifnum(int card_num, int intf_num, bool enable)
{
	int i;

	/*
	 * default index 0 is used when info is allocated upon
	 * first enable audio stream req for a pcm device
	 */
	if (enable && !uadev[card_num].info)
		return 0;

	for (i = 0; i < uadev[card_num].num_intf; i++) {
		if (enable && !uadev[card_num].info[i].in_use)
			return i;
		else if (!enable &&
				uadev[card_num].info[i].intf_num == intf_num)
			return i;
	}

	return -EINVAL;
}

static int get_data_interval_from_si(struct snd_usb_substream *subs,
	u32 service_interval)
{
	unsigned int bus_intval, bus_intval_mult, binterval;

	if (subs->dev->speed >= USB_SPEED_HIGH)
		bus_intval = BUS_INTERVAL_HIGHSPEED_AND_ABOVE;
	else
		bus_intval = BUS_INTERVAL_FULL_SPEED;

	if (service_interval % bus_intval)
		return -EINVAL;

	bus_intval_mult = service_interval / bus_intval;
	binterval = ffs(bus_intval_mult);
	if (!binterval || binterval > MAX_BINTERVAL_ISOC_EP)
		return -EINVAL;

	/* check if another bit is set then bail out */
	bus_intval_mult = bus_intval_mult >> binterval;
	if (bus_intval_mult)
		return -EINVAL;

	return (binterval - 1);
}

static struct snd_usb_substream *find_substream(unsigned int card_num,
	unsigned int pcm_idx, unsigned int direction)
{
	struct snd_usb_stream *as;
	struct snd_usb_substream *subs = NULL;
	struct snd_usb_audio *chip;

	chip = uadev[card_num].chip;
	if (!chip || atomic_read(&chip->shutdown)) {
		pr_debug("%s: instance of usb card # %d does not exist\n",
			__func__, card_num);
		goto err;
	}

	if (pcm_idx >= chip->pcm_devs) {
		pr_err("%s: invalid pcm dev number %u > %d\n", __func__,
			pcm_idx, chip->pcm_devs);
		goto err;
	}

	if (direction > SNDRV_PCM_STREAM_CAPTURE) {
		pr_err("%s: invalid direction %u\n", __func__, direction);
		goto err;
	}

	list_for_each_entry(as, &chip->pcm_list, list) {
		if (as->pcm_index == pcm_idx) {
			subs = &as->substream[direction];
			goto done;
		}
	}

done:
err:
	if (!subs)
		pr_debug("%s: substream instance not found\n", __func__);
	return subs;
}

static void disable_audio_stream(struct snd_usb_substream *subs)
{
	struct snd_usb_audio *chip = subs->stream->chip;

	snd_usb_hw_free(subs);
	snd_usb_autosuspend(chip);
}

static void _usb_qmi_snd_pcm_hw_param_any(struct snd_pcm_hw_params *params,
				  snd_pcm_hw_param_t var)
{
	if (hw_is_mask(var)) {
		snd_mask_any(hw_param_mask(params, var));
		params->cmask |= 1 << var;
		params->rmask |= 1 << var;
		return;
	}
	if (hw_is_interval(var)) {
		snd_interval_any(hw_param_interval(params, var));
		params->cmask |= 1 << var;
		params->rmask |= 1 << var;
		return;
	}
	snd_BUG();
}

static void _usb_qmi_snd_pcm_hw_params_any(struct snd_pcm_hw_params *params)
{
	unsigned int k;

	memset(params, 0, sizeof(*params));
	for (k = SNDRV_PCM_HW_PARAM_FIRST_MASK; k <= SNDRV_PCM_HW_PARAM_LAST_MASK; k++)
		_usb_qmi_snd_pcm_hw_param_any(params, k);
	for (k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++)
		_usb_qmi_snd_pcm_hw_param_any(params, k);
	params->info = ~0U;
}

// AUDIO_CUST_NOTE
/*
 * find a matching audio format
 */
static struct audioformat *
_find_format(struct list_head *fmt_list_head, snd_pcm_format_t format,
	    unsigned int rate, unsigned int channels, bool strict_match,
	    struct snd_usb_substream *subs)
{
	struct audioformat *fp;
	struct audioformat *found = NULL;
	int cur_attr = 0, attr;

	list_for_each_entry(fp, fmt_list_head, list) {
		if (strict_match) {
			if (!(fp->formats & pcm_format_to_bits(format)))
				continue;
			if (fp->channels != channels)
				continue;
		}
		if (rate < fp->rate_min || rate > fp->rate_max)
			continue;
		if (!(fp->rates & SNDRV_PCM_RATE_CONTINUOUS)) {
			unsigned int i;

			for (i = 0; i < fp->nr_rates; i++)
				if (fp->rate_table[i] == rate)
					break;
			if (i >= fp->nr_rates)
				continue;
		}
		attr = fp->ep_attr & USB_ENDPOINT_SYNCTYPE;
		if (!found) {
			found = fp;
			cur_attr = attr;
			continue;
		}
		/* avoid async out and adaptive in if the other method
		 * supports the same format.
		 * this is a workaround for the case like
		 * M-audio audiophile USB.
		 */
		if (subs && attr != cur_attr) {
			if ((attr == USB_ENDPOINT_SYNC_ASYNC &&
				subs->direction == SNDRV_PCM_STREAM_PLAYBACK) ||
				(attr == USB_ENDPOINT_SYNC_ADAPTIVE &&
				subs->direction == SNDRV_PCM_STREAM_CAPTURE))
				continue;
			if ((cur_attr == USB_ENDPOINT_SYNC_ASYNC &&
				subs->direction == SNDRV_PCM_STREAM_PLAYBACK) ||
				(cur_attr == USB_ENDPOINT_SYNC_ADAPTIVE &&
				subs->direction == SNDRV_PCM_STREAM_CAPTURE)) {
				found = fp;
				cur_attr = attr;
				continue;
			}
		}
		/* find the format with the largest max. packet size */
		if (fp->maxpacksize > found->maxpacksize) {
			found = fp;
			cur_attr = attr;
		}
	}
	return found;
}

static struct audioformat *
_find_substream_format(struct snd_usb_substream *subs,
		      const struct snd_pcm_hw_params *params)
{
	return _find_format(&subs->fmt_list, params_format(params),
				params_rate(params), params_channels(params),
				true, subs);
}
// AUDIO_CUST_END

static int enable_audio_stream(struct snd_usb_substream *subs,
				snd_pcm_format_t pcm_format,
				unsigned int channels, unsigned int cur_rate,
				int datainterval)
{
	struct snd_usb_audio *chip = subs->stream->chip;
	struct snd_pcm_hw_params params;
	struct audioformat *fmt; // AUDIO_CUST_NOTE
	struct snd_mask *m;
	struct snd_interval *i;
	int ret;

	_usb_qmi_snd_pcm_hw_params_any(&params);

	m = hw_param_mask(&params, SNDRV_PCM_HW_PARAM_FORMAT);
	snd_mask_leave(m, pcm_format);

	i = hw_param_interval(&params, SNDRV_PCM_HW_PARAM_CHANNELS);
	snd_interval_setinteger(i);
	i->min = i->max = channels;

	i = hw_param_interval(&params, SNDRV_PCM_HW_PARAM_RATE);
	snd_interval_setinteger(i);
	i->min = i->max = cur_rate;

	if (!chip->intf[0])
		return -ENODEV;

	// AUDIO_CUST_NOTE Yamaha sound card skip sync_ep
	if (uadev[chip->card->number].chip != NULL &&
		(uadev[chip->card->number].chip->usb_id == 0x0499172f)) {
		fmt = _find_substream_format(subs, &params);
		if (!fmt) {
			uaudio_err("not find fmt\n");
			return -EINVAL;
		}
		uaudio_info("is yamaha sound card, implicit_fb: %d, sync_ep: 0x%x\n",
			fmt->implicit_fb, fmt->sync_ep);
		if (fmt->implicit_fb && fmt->sync_ep == 0x81) {
			fmt->implicit_fb = false;
			fmt->sync_ep = 0x0;
		}
	}
	// AUDIO_CUST_END

	/*
	 * Resume the device only after format validation above.
	 * Resuming before a format check that may fail causes an
	 * unnecessary suspend/resume cycle: the failure path calls
	 * snd_usb_autosuspend() which schedules autosuspend, and the
	 * next retry arrives while the device is still suspending,
	 * hitting -EHOSTUNREACH in usb_set_interface().
	 */

	pm_runtime_barrier(&chip->intf[0]->dev);
	ret = snd_usb_autoresume(chip);
	if (ret < 0) {
		uaudio_err("snd_usb_autoresume failed ret %d\n", ret);
		return ret;
	}

	ret = snd_usb_hw_params(subs, &params);
	if (ret < 0)
		goto put_suspend;

	if (atomic_read(&chip->shutdown)) {
		uaudio_err("chip already shutdown\n");
		ret = -ENODEV;
	} else {
		ret = snd_usb_lock_shutdown(chip);
		if (ret < 0)
			goto detach_ep;

		if (subs->sync_endpoint) {
			ret = snd_usb_endpoint_prepare(chip, subs->sync_endpoint);
			if (ret < 0)
				goto unlock;
		}

		ret = snd_usb_endpoint_prepare(chip, subs->data_endpoint);
		if (ret < 0)
			goto unlock;

		snd_usb_unlock_shutdown(chip);
		uaudio_info("selected %s iface:%d altsetting:%d datainterval:%dus\n",
			subs->direction ? "capture" : "playback",
			subs->cur_audiofmt->iface, subs->cur_audiofmt->altsetting,
			(1 << subs->cur_audiofmt->datainterval) *
			(subs->dev->speed >= USB_SPEED_HIGH ?
			BUS_INTERVAL_HIGHSPEED_AND_ABOVE :
			BUS_INTERVAL_FULL_SPEED));
		struct usb_host_endpoint *ep = usb_pipe_endpoint(subs->dev,
									subs->data_endpoint->pipe);
		struct xhci_hcd *xhci = hcd_to_xhci(bus_to_hcd(subs->dev->bus));
		unsigned int ep_index = xhci_get_endpoint_index(&ep->desc);
		struct xhci_ep_ctx *ep_ctx = xhci_get_ep_ctx(xhci,
						xhci->devs[subs->dev->slot_id]->out_ctx, ep_index);

		uaudio_info("data EP[%d] state=0x%x\n",
			ep_index, le32_to_cpu(ep_ctx->ep_info) & EP_STATE_MASK);
	}

	return 0;

unlock:
	snd_usb_unlock_shutdown(chip);

detach_ep:
	snd_usb_hw_free(subs);

put_suspend:
	snd_usb_autosuspend(chip);

	return ret;
}

static int check_valid_request(struct qmi_uaudio_stream_req_msg_v01 *req_msg,
					int *info_idx)
{
	struct snd_usb_substream *subs;
	struct snd_usb_audio *chip;
	u8 pcm_card_num = 1, pcm_dev_num = 0, direction;

	direction = req_msg->usb_token & SND_PCM_STREAM_DIRECTION;
	pcm_dev_num = (req_msg->usb_token & SND_PCM_DEV_NUM_MASK) >> 8;
	pcm_card_num = (req_msg->usb_token & SND_PCM_CARD_NUM_MASK) >> 16;

	uaudio_info("card#:%d dev#:%d dir:%d en:%d fmt:%d rate:%d #ch:%d\n",
			pcm_card_num, pcm_dev_num, direction, req_msg->enable,
			req_msg->audio_format, req_msg->bit_rate,
			req_msg->number_of_ch);

	if (pcm_card_num >= SNDRV_CARDS) {
		uaudio_err("invalid card # %u", pcm_card_num);
		return -EINVAL;
	}

	if (req_msg->audio_format > USB_QMI_PCM_FORMAT_U32_BE) {
		uaudio_err("unsupported pcm format received %d\n",
				req_msg->audio_format);
		return -EINVAL;
	}

	subs = find_substream(pcm_card_num, pcm_dev_num, direction);

	if (!subs) {
		uaudio_err("invalid substream\n");
		return -EFAULT;
	}

	chip = uadev[pcm_card_num].chip;
	if (!subs || !chip || atomic_read(&chip->shutdown)) {
		uaudio_err("can't find substream for card# %u, dev# %u dir%u\n",
				pcm_card_num, pcm_dev_num, direction);
		return -ENODEV;
	}

	*info_idx = info_idx_from_ifnum(pcm_card_num, subs->cur_audiofmt ?
			subs->cur_audiofmt->iface : -1, req_msg->enable);

	if (atomic_read(&chip->shutdown) || !subs->stream || !subs->stream->pcm
			|| !subs->stream->chip) {
		uaudio_err("chip or sub not available: shutdown:%d stream:%p\n",
				atomic_read(&chip->shutdown), subs->stream);

		if (subs->stream)
			uaudio_err("pcm:%p chip:%p\n", subs->stream->pcm, subs->stream->chip);

		return -ENODEV;
	}

	if (req_msg->enable && (*info_idx < 0)) {
		uaudio_err("interface# %d already in use card# %d\n",
			subs->cur_audiofmt ? subs->cur_audiofmt->iface : -1, pcm_card_num);
		return -EBUSY;
	}

	return 0;
}

static void xhci_sideband_cleanup_sw_event_ring
			(struct xhci_sideband *sb,
			struct audio_offload_data *offload_data)
{
	struct xhci_interrupter *ir = sb->ir;
	struct xhci_ring *sw_event_ring = offload_data->sw_event_ring;
	unsigned long flags = 0;
	int drained = 0, i = 0;

	if (!uaudio_qdev->adsp_offload_active) {
		uaudio_err("UGMI: offload never done skip cleanup");
		return;
	}

	if (!sb || !sw_event_ring || !sb->xhci) {
		uaudio_err("sb already unregistered, skip ERST sync\n");
		return;
	}

	spin_lock_irqsave(&sb->xhci->lock, flags);
	ir = sb->ir;
	if (!ir || !ir->event_ring || !ir->event_ring->first_seg) {
		spin_unlock_irqrestore(&sb->xhci->lock, flags);
		uaudio_err("sb->ir is NULL, skip ERST sync\n");
		return;
	}
	xhci_sync_erst_dequeue(sb->xhci, ir);
	/*
	 * Drain any TRBs that the ADSP left unconsumed in the sideband
	 * interrupter's event ring.  Only done during disconnect, where the
	 * ADSP has already relinquished ownership and AP must complete any
	 * pending transfers to avoid URB leaks.
	 *
	 * Capped at 5 TRBs to bound latency under the spinlock.
	 *
	 * The spinlock is still held here, matching the normal IRQ handler
	 * context that xhci_handle_event_trb() expects.
	 */
	if (uaudio_qdev->in_disconnect) {
		int max_drain = MAX_DRAIN_EVT_RING_NUM;

		while (max_drain-- > 0 &&
				ir->event_ring && ir->event_ring->dequeue &&
				((le32_to_cpu(ir->event_ring->dequeue->event_cmd.flags) &
				TRB_CYCLE) == ir->event_ring->cycle_state)) {
			xhci_handle_event_trb(sb->xhci, ir,
						ir->event_ring->dequeue);
			drained++;
		}
	}
	spin_unlock_irqrestore(&sb->xhci->lock, flags);
	if (drained)
		uaudio_err("drained %d pending TRB(s)\n", drained);
	/* Make sure qsram sync completes.*/
	mb();
	uaudio_qdev->sw_evt_ring_freed = true;
	uaudio_qdev->adsp_offload_active = false;
	uaudio_stop_offload_qsram_poll();
	for (i = 0; i < 64; i++)
		writel(0x0, &uaudio_qdev->qsram->data[i]);
	uaudio_dbg("SW event ring cleaned up\n");

}

static void stop_ep_monitor_work(struct work_struct *work)
{
	struct stop_ep_monitor *monitor = container_of(work,
							struct stop_ep_monitor, work);
	int card_num = monitor->card_num;
	u32 adsp_state;
	int poll_count = 0, i = 0;
	/* 3 s total: 2 ms * 1500 polls */
	const int max_polls = 1500;
	const int poll_interval_ms = 2;
	bool offload = false;
	unsigned long flags;

	uaudio_dbg("card#%d: monitor start\n", card_num);

	while (poll_count < max_polls) {
		/*
		 * Fast-exit: disable_audio_stream() finished in
		 * handle_uaudio_stream_req. nothing left to do.
		 */
		if (atomic_read(&monitor->disable_completed) &&
			atomic_read(&monitor->stop_completed)) {
			uaudio_err("func_stop_ep_monitor_work card#%d disable completed after %d polls\n",
				   card_num, poll_count);
			return;
		}
		/* Guard: uaudio_disconnect() may have freed sb while we
		 * were sleeping.  Bail before dereferencing sb (issue 19.8).
		 */
		if (!uadev[card_num].sb) {
			uaudio_dbg("card#%d: sb freed during poll, aborting\n", card_num);
			return;
		}
		adsp_state = readl(&uaudio_qdev->qsram->data[5]);
		if (!adsp_state) {
			uaudio_dbg("card#%d: DSP released, cleanup\n", card_num);
			spin_lock_irqsave(&uadev[card_num].sb->xhci->lock, flags);
			trace_android_vh_xhci_handle_offload(uadev[card_num].xhci,
								uadev[card_num].sb->ir,
								&offload);

			spin_unlock_irqrestore(&uadev[card_num].sb->xhci->lock, flags);
			unregister_trace_android_vh_xhci_handle_offload(
						xhci_handle_offload, NULL);
			uaudio_dbg("drained\n");

			if (!uaudio_qdev->sw_evt_ring_freed)
				xhci_sideband_cleanup_sw_event_ring(uadev[card_num].sb,
						&uadev[card_num].offload_data);
			for (i = 0; i < 64; i++)
				writel(0x0, &uaudio_qdev->qsram->data[i]);
			return;
		}

		msleep(poll_interval_ms);
		poll_count++;
	}

	uaudio_dbg("Card#%d: DSP released at timeout\n", card_num);

	/* Guard: sb may have been freed during the poll timeout (issue 19.8). */
	if (!uadev[card_num].sb) {
		uaudio_dbg("card#%d: sb freed at timeout, aborting\n", card_num);
		return;
	}
	/* Timeout: force interrupt control back to AP anyway */
	spin_lock_irqsave(&uadev[card_num].sb->xhci->lock, flags);
	trace_android_vh_xhci_handle_offload(uadev[card_num].xhci,
			uadev[card_num].sb->ir, &offload);
	spin_unlock_irqrestore(&uadev[card_num].sb->xhci->lock, flags);
	unregister_trace_android_vh_xhci_handle_offload(xhci_handle_offload, NULL);
	uaudio_dbg("card#%d: monitor exit\n", card_num);
}

/**
 * uaudio_disable_endpoints_and_stream() - Remove endpoints and disable stream
 * @subs:        USB audio substream
 * @chip:        USB audio chip instance
 * @pcm_card_num: PCM card number
 * @info_idx:    interface info index
 *
 * Removes data and sync endpoints from xHCI sideband and
 * triggers stream disable:
 *  - Issues mb() before data endpoint removal to ensure all
 *    prior QSRAM writes are visible before tearing down the
 *    transfer ring
 *  - Normal stop: queues deferred disable_stream_work
 *  - During disconnect: calls disable_audio_stream() directly
 *    with QUIRK_FLAG_IFACE_SKIP_CLOSE to prevent double close
 */
static int uaudio_disable_endpoints_and_stream(
				struct snd_usb_substream *subs,
				struct snd_usb_audio *chip,
				u8 pcm_card_num,
				int info_idx)
{
	struct intf_info *info;
	struct usb_host_endpoint *ep;

	/*
	 * Guard against duplicate en(0): if info_idx is invalid
	 * (e.g. subs->cur_audiofmt was already cleared by a previous
	 * disable that ran disable_audio_stream), skip EP/pipe cleanup
	 * and go to the response label.  active, in_use, and EP pipe
	 * are all protected by their own guards above; this only
	 * prevents the out-of-bounds info[info_idx] access when
	 * info_idx == -EINVAL.
	 */
	if (info_idx < 0) {
		uaudio_dbg("duplicate en(0): info_idx invalid (%d), skipping cleanup\n", info_idx);
			return -EINVAL;
	}

	info = &uadev[pcm_card_num].info[info_idx];

	if (info->data_ep_pipe) {
		ep = usb_pipe_endpoint(uadev[pcm_card_num].udev,
				       info->data_ep_pipe);
		if (!ep) {
			uaudio_dbg("no data ep\n");
		} else {
			/* Make sure write completes.*/
			mb();
			uaudio_dbg("rm ep");
			xhci_sideband_remove_endpoint(uadev[pcm_card_num].sb, ep);
			atomic_set(&uaudio_qdev->stop_monitor.stop_completed, 1);
			uaudio_dbg("rm ep done\n");
		}
		info->data_ep_pipe = 0;
	}

	if (info->sync_ep_pipe) {
		ep = usb_pipe_endpoint(uadev[pcm_card_num].udev,
				       info->sync_ep_pipe);
		if (!ep) {
			uaudio_dbg("no sync ep\n");
		} else {
			uaudio_dbg("sync ep rm\n");
			xhci_sideband_remove_endpoint(
					uadev[pcm_card_num].sb, ep);
			atomic_set(&uaudio_qdev->stop_monitor.stop_completed, 1);
			uaudio_dbg("sync ep done\n");
		}
		info->sync_ep_pipe = 0;
	}

	if (uadev[pcm_card_num].offload_data.active <= 1) {
		uaudio_dbg("ref_count=%d", uadev[pcm_card_num].offload_data.active);
		if (uaudio_qdev->in_disconnect)
			chip->quirk_flags |= QUIRK_FLAG_IFACE_SKIP_CLOSE;
		uaudio_dbg("disable_audio_stream dir=%u", subs->direction);
		disable_audio_stream(subs);
		uaudio_dbg("disabled subs dir=%u", subs->direction);
		chip->quirk_flags &= ~QUIRK_FLAG_IFACE_SKIP_CLOSE;
		/* Make sure write completes*/
		mb();
		/*
		 * Signal stop_ep_monitor_work that disable_audio_stream
		 * has completed so it can skip the adsp_state poll loop.
		 */
		atomic_set(&uaudio_qdev->stop_monitor.disable_completed, 1);
	}
	return 0;
}

/**
 * uaudio_stop_offload_stream() - Stop audio offload stream and cleanup SW ring
 * @pcm_card_num: PCM card number
 *
 * Handles the offload teardown sequence:
 *  - Cancels offload ready work and decrements active count
 *  - If ADSP idle: drains SW event ring, unregisters trace hook,
 *    cleans up SW ring and zeroes all QSRAM registers
 *  - If ADSP busy: queues stop monitor worker to wait for ADSP idle
 *
 * Returns none
 */
static void uaudio_stop_offload_stream(u8 pcm_card_num)
{
	u32 adsp_state = 1;
	unsigned long flags;
	int i;

	if (uadev[pcm_card_num].offload_data.active) {
		if (uaudio_qdev->poll_active) {
			flush_work(&uaudio_qdev->offload_ready_work);
			uaudio_qdev->poll_active = false;  /* signal worker to stop */
		}
		uadev[pcm_card_num].offload_data.active--;
	}

	if (uadev[pcm_card_num].offload_data.active == 0) {
		//hguan: cancel work if no data active.
		cancel_work_sync(&uaudio_qdev->offload_ready_work);
		uaudio_qdev->poll_active = false;
		if (!uaudio_has_active_offload())
			uaudio_stop_offload_qsram_poll();
	}

	adsp_state = readl(&uaudio_qdev->qsram->data[5]);

	/* ADSP idle — immediate SW ring cleanup */
	if (!adsp_state && !uaudio_qdev->intr_num && !uaudio_qdev->sw_evt_ring_freed) {
		/*hguan: should only call it once when ownership back to ap*/
		bool offload = false;

		uaudio_dbg("normal free\n");
		/* Cleanup SW event ring */
		spin_lock_irqsave(&uadev[pcm_card_num].sb->xhci->lock, flags);
		trace_android_vh_xhci_handle_offload(
					uadev[pcm_card_num].xhci,
					uadev[pcm_card_num].sb->ir,
					&offload);
		spin_unlock_irqrestore(&uadev[pcm_card_num].sb->xhci->lock, flags);

		unregister_trace_android_vh_xhci_handle_offload(
					xhci_handle_offload, NULL);

		if (!uaudio_qdev->sw_evt_ring_freed)
			xhci_sideband_cleanup_sw_event_ring(
					uadev[pcm_card_num].sb,
					&uadev[pcm_card_num].offload_data);

		for (i = 0; i < 64; i++)
			writel(0x0, &uaudio_qdev->qsram->data[i]);
	}

	/* ADSP busy — queue monitor worker to wait for ADSP idle */
	if (!uaudio_qdev->intr_num && adsp_state &&
	    uadev[pcm_card_num].offload_data.active) {
		uaudio_qdev->stop_monitor.card_num = pcm_card_num;
		atomic_set(&uaudio_qdev->stop_monitor.stop_completed, 0);
		atomic_set(&uaudio_qdev->stop_monitor.disable_completed, 0);
		uaudio_dbg("workqueue free IN_USE = %d", atomic_read(&uadev[pcm_card_num].in_use));
		queue_work(uaudio_qdev->stop_monitor_wq,
			   &uaudio_qdev->stop_monitor.work);
	}
}

/**
 * uaudio_process_enable_req() - Process QMI enable stream request
 * @subs:         USB audio substream to enable
 * @chip:         USB audio chip instance
 * @req_msg:      decoded QMI request message
 * @resp:         QMI response buffer to populate
 * @info_idx:     interface info index
 * @datainterval: calculated data interval
 *
 * Handles the complete QMI enable stream request lifecycle:
 *  - Flushes any pending disable work
 *  - Increments device in_use reference count
 *  - Configures USB audio hardware via enable_audio_stream()
 *  - Populates QMI response with DMA memory info
 *  - Initializes SW event ring for 1IR offload mode
 *  - Starts ADSP ready polling worker
 *
 * On failure, decrements the in_use reference count.
 *
 * Returns 0 on success, negative error code on failure.
 */
static int uaudio_process_enable_req(
		struct snd_usb_substream *subs,
		struct snd_usb_audio *chip,
		struct qmi_uaudio_stream_req_msg_v01 *req_msg,
		struct qmi_uaudio_stream_resp_msg_v01 *resp,
		int info_idx,
		int datainterval)
{
	int ret = 0;
	u8 pcm_card_num, direction;

	pcm_card_num = (req_msg->usb_token & SND_PCM_CARD_NUM_MASK) >> 16;
	direction = req_msg->usb_token & SND_PCM_STREAM_DIRECTION;

	if (atomic_read(&card_disconnecting[pcm_card_num])) {
		uaudio_err("card#%d disconnect pending, aborting enable\n",
				pcm_card_num);
		return -ENODEV;
	}

	/*
	 * Duplicate-enable detection: if this substream is already
	 * active (cur_audiofmt set by a prior hw_params, data_ep_pipe
	 * non-zero meaning prepare_qmi_response completed) AND the
	 * incoming parameters match what is already running, return
	 * success immediately without re-running the full enable path.
	 * This avoids the ~5s delay caused by flush_work +
	 * snd_usb_hw_params tearing down and re-building the stream.
	 */
	if (subs->cur_audiofmt && subs->data_endpoint &&
	    uadev[pcm_card_num].info &&
	    uadev[pcm_card_num].info[info_idx >= 0 ? info_idx : 0].in_use &&
	    subs->data_endpoint->cur_rate == req_msg->bit_rate &&
	    subs->data_endpoint->cur_channels == req_msg->number_of_ch &&
	    subs->data_endpoint->cur_format == map_pcm_format(req_msg->audio_format)) {
		uaudio_err("card#%d dir%d already enabled, skip\n", pcm_card_num, direction);
		return 0;
	}

	mutex_lock(&uadev[pcm_card_num].mutex);
	if (atomic_read(&card_disconnecting[pcm_card_num])) {
		uaudio_err("card#%d disconnect started while acquiring lock, aborting enable\n",
				pcm_card_num);
		mutex_unlock(&uadev[pcm_card_num].mutex);
		return -ENODEV;
	}
	if (uaudio_qdev->in_disconnect) {
		uaudio_err("lock ack disconnect in progress\n");
		mutex_unlock(&uadev[pcm_card_num].mutex);
		return -EINVAL;
	}
	chip = uadev[pcm_card_num].chip;
	if (!chip) {
		uaudio_err("invalid chip during enable\n");
		mutex_unlock(&uadev[pcm_card_num].mutex);
		return -EINVAL;
	}

	/*
	 * Re-check in_disconnect under uadev.mutex (issue 19.6).
	 * The check at line 2637 is a TOCTOU race: uaudio_disconnect()
	 * acquires uadev.mutex before setting in_disconnect=true, so
	 * this re-check under the same lock is race-free.
	 */
	if (uaudio_qdev->in_disconnect) {
		uaudio_err("cannot enable stream: disconnect in progress\n");
		mutex_unlock(&uadev[pcm_card_num].mutex);
		return -ENODEV;
	}
	mutex_lock(&chip->mutex);
	atomic_inc(&uadev[pcm_card_num].in_use);
	mutex_unlock(&chip->mutex);

	ret = enable_audio_stream(subs,
			map_pcm_format(req_msg->audio_format),
			req_msg->number_of_ch, req_msg->bit_rate,
			datainterval);
	if (ret) {
		uaudio_dbg("enable_audio_stream failed %d\n", ret);
		goto dec_in_use;
	}

	ret = prepare_qmi_response(subs, req_msg, resp, info_idx);
	if (ret)
		goto dec_in_use;

	if (!uaudio_qdev->intr_num) {
		xhci_sideband_init_sw_event_ring(
			uadev[pcm_card_num].sb,
			&uadev[pcm_card_num].offload_data);

		uaudio_qdev->sw_evt_ring_freed = false;
		if ((!uaudio_qdev->poll_active) && (!(uadev[pcm_card_num].offload_data.active))) {
			uaudio_qdev->poll_active = true;
			/*
			 * Use high priority wq so that ADSP handshake
			 * poll/ack is executed ASAP under load.
			 */
			if (uaudio_qdev->offload_ready_wq)
				queue_work(uaudio_qdev->offload_ready_wq,
					&uaudio_qdev->offload_ready_work);
			else
				schedule_work(&uaudio_qdev->offload_ready_work);
		}
		uadev[pcm_card_num].offload_data.active++;
	}
	mutex_unlock(&uadev[pcm_card_num].mutex);
	return 0;

dec_in_use:
	mutex_lock(&chip->mutex);
	uaudio_dbg("enable process failed %d\n", ret);
	if (atomic_dec_and_test(&uadev[pcm_card_num].in_use)) {
		if (uadev[pcm_card_num].offload_data.active)
			uaudio_err("offload_data.active=%u non-zero on in_use clear (enable fail)\n",
				uadev[pcm_card_num].offload_data.active);
		uadev[pcm_card_num].offload_data.active = 0;
	}
	mutex_unlock(&chip->mutex);
	mutex_unlock(&uadev[pcm_card_num].mutex);
	return ret;
}


/**
 * uaudio_validate_stream_req() - Validate incoming QMI stream request
 * @req_msg:      decoded QMI request message
 * @subs:         pointer to store found usb substream
 * @chip:         pointer to store found usb audio chip
 * @info_idx:     pointer to store interface info index
 * @datainterval: pointer to store calculated data interval
 *
 * Validates all mandatory fields in the QMI stream request,
 * locates the correct USB audio substream and chip instance,
 * and calculates the data interval from the service interval
 * if provided.
 *
 * Returns 0 on success, negative error code on failure.
 */
static int uaudio_validate_stream_req(
		struct qmi_uaudio_stream_req_msg_v01 *req_msg,
		struct snd_usb_substream **subs,
		struct snd_usb_audio **chip,
		int *info_idx,
		int *datainterval)
{
	u8 pcm_card_num = 1, pcm_dev_num = 0, direction;
	int ret = 0;

	if (!req_msg->audio_format_valid || !req_msg->bit_rate_valid ||
		!req_msg->number_of_ch_valid || !req_msg->xfer_buff_size_valid) {
		uaudio_err("invalid request msg\n");
		return -EINVAL;
	}

	direction    = req_msg->usb_token & SND_PCM_STREAM_DIRECTION;
	pcm_dev_num  = (req_msg->usb_token & SND_PCM_DEV_NUM_MASK) >> 8;
	pcm_card_num = (req_msg->usb_token & SND_PCM_CARD_NUM_MASK) >> 16;
	/* Ensure write completes */
	mb();

	if (pcm_card_num >= SNDRV_CARDS) {
		uaudio_err("invalid car num\n");
		return -EINVAL;
	}

	*subs = find_substream(pcm_card_num, pcm_dev_num, direction);
	if (!*subs) {
		uaudio_err("invalid substream\n");
		return -EINVAL;
	}

	*chip = uadev[pcm_card_num].chip;
	if (!*chip) {
		uaudio_err("invalid chip\n");
		return -EINVAL;
	}

	ret = check_valid_request(req_msg, info_idx);
	if (ret)
		return ret;

	if (req_msg->service_interval_valid) {
		ret = get_data_interval_from_si(*subs,
						req_msg->service_interval);
		if (ret == -EINVAL) {
			uaudio_err("invalid service interval %u\n",
			req_msg->service_interval);
			return ret;
		}
		*datainterval = ret;
		uaudio_dbg("data interval %u\n", ret);
	}

	uadev[pcm_card_num].ctrl_intf = (*chip)->ctrl_intf;

	return 0;
}

static void handle_uaudio_stream_req(struct qmi_handle *handle,
			struct sockaddr_qrtr *sq,
			struct qmi_txn *txn,
			const void *decoded_msg)
{
	struct qmi_uaudio_stream_req_msg_v01 *req_msg;
	struct qmi_uaudio_stream_resp_msg_v01 resp = {{0}, 0};
	struct snd_usb_substream *subs = NULL;
	struct uaudio_qmi_svc *svc = uaudio_svc;
	struct intf_info *info;

	ktime_t t_request_recvd = ktime_get();
	struct snd_usb_audio *chip = NULL;

	u8 pcm_card_num = 1;
	int info_idx = -EINVAL, datainterval = -EINVAL, ret = 0;

	uaudio_dbg("sq_node:%x sq_port:%x sq_family:%x\n", sq->sq_node,
			sq->sq_port, sq->sq_family);
	if (!svc->client_connected) {
		svc->client_sq = *sq;
		svc->client_connected = true;
	}

	req_msg = (struct qmi_uaudio_stream_req_msg_v01 *)decoded_msg;
	ret = uaudio_validate_stream_req(req_msg, &subs, &chip,
									&info_idx, &datainterval);
	if (ret)
		goto response;
	pcm_card_num = (req_msg->usb_token & SND_PCM_CARD_NUM_MASK) >> 16;

	if ((uaudio_qdev->in_disconnect) && (req_msg->enable)) {
		uaudio_err("cannot en stream while disconnect in progress\n");
		ret = -EINVAL;
		goto response;
	}
	cancel_work_sync(&uaudio_qdev->stop_monitor.work);
	if (req_msg->enable) {
		ret = uaudio_process_enable_req(subs, chip, req_msg,
									&resp, info_idx,
									datainterval);
		if (ret)
			goto response;
	} else {
		uaudio_stop_offload_stream(pcm_card_num);
		ret = uaudio_disable_endpoints_and_stream(subs, chip,
										pcm_card_num,
										info_idx);
		if (ret)
			goto response;
	}

response:
	if (!req_msg->enable && ret != -EINVAL && ret != -ENODEV) {
		mutex_lock(&chip->mutex);
		if (info_idx >= 0 && uadev[pcm_card_num].info) {
			info = &uadev[pcm_card_num].info[info_idx];
			if (uadev[pcm_card_num].udev) {
				uaudio_dev_intf_cleanup(
					uadev[pcm_card_num].udev, info);
			} else {
				uaudio_dbg("udev already freed, skip intf cleanup for card %u\n",
					pcm_card_num);
			}
		}
		// uaudio_disable_stream_work could run already and rel dev already,
		// no need to do it again.
		if (atomic_read(&uadev[pcm_card_num].in_use) > 0) {
			if (atomic_dec_and_test(&uadev[pcm_card_num].in_use)) {
				wake_up(&uadev[pcm_card_num].disconnect_wq);
				if (!uaudio_qdev->sw_evt_ring_freed) {
					if (uadev[pcm_card_num].offload_data.active)
						uaudio_err("active=%u, in_use clear (disable)\n",
							uadev[pcm_card_num].offload_data.active);
					uadev[pcm_card_num].offload_data.active = 0;
					uaudio_dev_release(&uadev[pcm_card_num]);
				}
			}
		}
		mutex_unlock(&chip->mutex);
	}

	resp.usb_token = req_msg->usb_token;
	resp.usb_token_valid = 1;
	resp.internal_status = ret;
	resp.internal_status_valid = 1;
	resp.status = ret ? USB_AUDIO_STREAM_REQ_FAILURE_V01 : ret;
	resp.status_valid = 1;
	ret = qmi_send_response(svc->uaudio_svc_hdl, sq, txn,
			QMI_UAUDIO_STREAM_RESP_V01,
			QMI_UAUDIO_STREAM_RESP_MSG_V01_MAX_MSG_LEN,
			qmi_uaudio_stream_resp_msg_v01_ei, &resp);

	uaudio_dbg("ret %d: qmi response latency %lld ms\n", ret,
		ktime_to_ms(ktime_sub(ktime_get(), t_request_recvd)));
}

static void uaudio_qmi_disconnect(void)
{
	struct intf_info *info;
	int idx, if_idx, i = 0;
	struct snd_usb_substream *subs;
	struct snd_usb_audio *chip;
	struct usb_host_endpoint *ep;
	int pcm_card_num;

	/* find all active intf for set alt 0 and cleanup usb audio dev */
	for (idx = 0; idx < SNDRV_CARDS; idx++) {
		if (!atomic_read(&uadev[idx].in_use))
			continue;

		for (if_idx = 0; if_idx < uadev[idx].num_intf; if_idx++) {
			if (!uadev[idx].info || !uadev[idx].info[if_idx].in_use)
				continue;
			info = &uadev[idx].info[if_idx];
			pcm_card_num = info->pcm_card_num;
			subs = find_substream(info->pcm_card_num,
						info->pcm_dev_num,
						info->direction);
			chip = uadev[idx].chip;
			if (!subs || !chip || atomic_read(&chip->shutdown)) {
				uaudio_dbg("no subs for c#%u, dev#%u dir%u\n",
						info->pcm_card_num,
						info->pcm_dev_num,
						info->direction);
				continue;
			}

			/* Release XHCI endpoints */
			if (info->data_ep_pipe) {
				ep = usb_pipe_endpoint(uadev[pcm_card_num].udev,
						info->data_ep_pipe);
				xhci_sideband_remove_endpoint(uadev[pcm_card_num].sb,
								ep);
			}

			if (info->sync_ep_pipe) {
				ep = usb_pipe_endpoint(uadev[pcm_card_num].udev,
						info->sync_ep_pipe);
				xhci_sideband_remove_endpoint(uadev[pcm_card_num].sb,
								ep);
			}

			disable_audio_stream(subs);
		}
		if (uadev[idx].offload_data.active)
			uaudio_err("offload_data.active=%u non-zero on in_use clear (disconnect)\n",
				   uadev[idx].offload_data.active);
		uadev[idx].offload_data.active = 0;
		atomic_set(&uadev[idx].in_use, 0);
		uaudio_dev_cleanup(&uadev[idx]);
	}
	for (i = 0; i < 64; i++)
		writel(0x0, &uaudio_qdev->qsram->data[i]);
}

static void cleanup_pseudo_event_ring(void)
{
	int idx = 0;
	struct audio_offload_data *offload;

	uaudio_dbg("Starting pseudo event ring and SW ring cleanup\n");

	if (uaudio_qdev->pseudo_evt_ring_mapped) {
		uaudio_dbg("Unmapping and freeing shared pseudo event ring\n");

		uaudio_iommu_unmap(MEM_PSEUDO_EVT_RING,
			uaudio_qdev->pseudo_evt_ring_va,
			PAGE_4K, PAGE_4K);
		if (uaudio_qdev->pseudo_evt_ring_buffer) {
			usb_free_coherent(
				uaudio_qdev->pseudo_evt_ring_udev,
				PAGE_4K,
				uaudio_qdev->pseudo_evt_ring_buffer,
				uaudio_qdev->pseudo_evt_ring_pa);
		}

		uaudio_qdev->pseudo_evt_ring_buffer = NULL;

		for (idx = 0; idx < SNDRV_CARDS; idx++) {
			offload = &uadev[idx].offload_data;
			if (offload->sw_event_ring && offload->sw_event_ring->first_seg) {
				uaudio_dbg("Freeing SW ring for card %d\n", idx);
				kfree(offload->sw_event_ring->first_seg);
				kfree(offload->sw_event_ring);
				offload->sw_event_ring = NULL;
				offload->sw_enqueue = NULL;
				offload->sw_dequeue = NULL;
				uadev[idx].xhci = NULL;
			}
		}
	}
	if (uaudio_qdev->poll_active) {
		flush_work(&uaudio_qdev->offload_ready_work);
		uaudio_qdev->poll_active = false;
	}
	allocate_once = true;
	uaudio_dbg("Pseudo event ring and SW ring cleanup completed\n");
}

static void uaudio_handle_bye(void)
{
	int idx = 0;
	bool offload = false;
	unsigned long flags;

	for (idx = 0; idx < SNDRV_CARDS; idx++) {
		if (!atomic_read(&uadev[idx].in_use))
			continue;

		spin_lock_irqsave(&uadev[idx].sb->xhci->lock, flags);
		trace_android_vh_xhci_handle_offload(uadev[idx].xhci,
					uadev[idx].sb->ir,
						&offload);
		spin_unlock_irqrestore(&uadev[idx].sb->xhci->lock, flags);

		unregister_trace_android_vh_xhci_handle_offload
			(xhci_handle_offload, NULL);
		if (!uaudio_qdev->sw_evt_ring_freed)
			xhci_sideband_cleanup_sw_event_ring(uadev[idx].sb,
				&uadev[idx].offload_data);
	}
}

static void uaudio_qmi_bye_cb(struct qmi_handle *handle, unsigned int node)
{
	struct uaudio_qmi_svc *svc = uaudio_svc;
	int idx = 0;

	if (svc->uaudio_svc_hdl != handle) {
		uaudio_err("handle mismatch\n");
		return;
	}

	if (svc->client_connected && svc->client_sq.sq_node == node) {
		uaudio_dbg("node: %d\n", node);
		for (idx = 0; idx < SNDRV_CARDS; idx++) {
			if (atomic_read(&uadev[idx].in_use)) {
				uaudio_handle_bye();
				cleanup_pseudo_event_ring();
				uaudio_qmi_disconnect();
			}
		}
		uaudio_err("bye cb critical sec done");
		svc->client_sq.sq_node = 0;
		svc->client_sq.sq_port = 0;
		svc->client_sq.sq_family = 0;
		svc->client_connected = false;
	}
}

static void uaudio_qmi_svc_disconnect_cb(struct qmi_handle *handle,
				  unsigned int node, unsigned int port)
{
	struct uaudio_qmi_svc *svc;
	int idx = 0;

	if (uaudio_svc == NULL)
		return;

	svc = uaudio_svc;
	if (svc->uaudio_svc_hdl != handle) {
		uaudio_err("handle mismatch\n");
		return;
	}

	if (svc->client_connected && svc->client_sq.sq_node == node &&
			svc->client_sq.sq_port == port) {
		uaudio_dbg("client node:%x port:%x\n", node, port);
		for (idx = 0; idx < SNDRV_CARDS; idx++) {
			if (atomic_read(&uadev[idx].in_use)) {
				uaudio_handle_bye();
				cleanup_pseudo_event_ring();
				uaudio_qmi_disconnect();
			}
		}
		uaudio_dbg("svc cb critical sec done");
		svc->client_sq.sq_node = 0;
		svc->client_sq.sq_port = 0;
		svc->client_sq.sq_family = 0;
		svc->client_connected = false;
	}
}

static struct qmi_ops uaudio_svc_ops_options = {
	.bye = uaudio_qmi_bye_cb,
	.del_client = uaudio_qmi_svc_disconnect_cb,
};

static int uaudio_qmi_svc_init(void);

static struct snd_usb_platform_ops offload_ops = {
	.connect_cb = uaudio_connect,
	.disconnect_cb = uaudio_disconnect,
};

static struct qsram_xhci __iomem *uaudio_get_qsram(struct device *dev)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct qsram_xhci __iomem *qsram;

	/* Find the dwc3-msm device node */
	np = of_find_compatible_node(NULL, NULL, "qcom,dwc-usb3-msm");
	if (!np) {
		np = of_find_compatible_node(NULL, NULL, "qcom,dwc3-msm-fw-managed");
		if (!np) {
			dev_err(dev, "dwc3-msm device node not found\n");
			return ERR_PTR(-ENODEV);
		}
	}

	/* Get the platform device */
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev) {
		dev_err(dev, "dwc3-msm platform device not found\n");
		return ERR_PTR(-ENODEV);
	}

	qsram = dwc3_msm_get_qsram(&pdev->dev);
	put_device(&pdev->dev);

	if (!qsram) {
		dev_err(dev, "dwc3-msm qsram not initialized\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	return qsram;
}

static int uaudio_qmi_plat_probe(struct platform_device *pdev)
{
	int ret;
	int i = 0;
	struct device_node *node = pdev->dev.of_node;
	struct qsram_xhci __iomem *qsram;

	if (!uaudio_svc) {
		ret = uaudio_qmi_svc_init();
		if (ret)
			return ret;
	}

	uaudio_qdev = devm_kzalloc(&pdev->dev, sizeof(struct uaudio_qmi_dev),
		GFP_KERNEL);
	if (!uaudio_qdev)
		return -ENOMEM;

	uaudio_qdev->dev = &pdev->dev;
	qsram = uaudio_get_qsram(&pdev->dev);

	if (IS_ERR(qsram)) {
		ret = PTR_ERR(qsram);
		if (ret == -EPROBE_DEFER) {
			dev_info(&pdev->dev, "Deferring probe, waiting for dwc3-msm\n");
			return ret;  // Kernel will retry probe later
		}
		dev_err(&pdev->dev, "Failed to get qsram: %d\n", ret);
		return ret;
	}

	uaudio_qdev->qsram = qsram;
	uaudio_qdev->adsp_offload_active = false;
	ret = of_property_read_u32(node, "qcom,usb-audio-stream-id",
				&uaudio_qdev->sid);
	if (ret) {
		dev_err(&pdev->dev, "failed to read sid.\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(node, "qcom,usb-audio-intr-num",
				&uaudio_qdev->intr_num);
	if (ret) {
		dev_err(&pdev->dev, "failed to read intr num.\n");
		return -ENODEV;
	}

	uaudio_qdev->domain = iommu_domain_alloc(pdev->dev.bus);
	if (!uaudio_qdev->domain) {
		dev_err(&pdev->dev, "failed to allocate iommu domain\n");
		return -ENODEV;
	}

	/* attach to external processor iommu */
	ret = iommu_attach_device(uaudio_qdev->domain, &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to attach device ret = %d\n", ret);
		goto free_domain;
	}

	/* initialize xfer ring and xfer buf iova list */
	INIT_LIST_HEAD(&uaudio_qdev->xfer_ring_list);
	uaudio_qdev->curr_xfer_ring_iova = IOVA_XFER_RING_BASE;
	uaudio_qdev->xfer_ring_iova_size =
			IOVA_XFER_RING_MAX - IOVA_XFER_RING_BASE;

	INIT_LIST_HEAD(&uaudio_qdev->xfer_buf_list);
	uaudio_qdev->curr_xfer_buf_iova = IOVA_XFER_BUF_START;
	uaudio_qdev->xfer_buf_iova_size =
		IOVA_XFER_BUF_MAX - IOVA_XFER_BUF_START;

	INIT_LIST_HEAD(&uaudio_qdev->pseudo_evt_ring_list);
	uaudio_qdev->curr_pseudo_evt_ring_iova = IOVA_PSEUDO_EVT_RING_BASE;
	uaudio_qdev->pseudo_evt_ring_iova_size = PAGE_4K;
	uaudio_qdev->pseudo_evt_ring_mapped = false;

	INIT_WORK(&uaudio_qdev->offload_ready_work, uaudio_offload_ready_work);
	INIT_DELAYED_WORK(&uaudio_qdev->offload_qsram_poll_work,
				uaudio_offload_qsram_poll_work);
	uaudio_qdev->poll_active = false;
	uaudio_qdev->offload_qsram_poll_active = false;

	/*
	 * Create a dedicated high priority workqueue for offload_ready_work.
	 */
	uaudio_qdev->offload_ready_wq = alloc_workqueue("uaudio_offload_ready",
							WQ_HIGHPRI | WQ_UNBOUND | WQ_MEM_RECLAIM,
							0);
	if (!uaudio_qdev->offload_ready_wq)
		return -ENOMEM;

	uaudio_qdev->stop_monitor_wq = alloc_ordered_workqueue("uaudio_stop_mon", 0);
	if (!uaudio_qdev->stop_monitor_wq)
		return -ENOMEM;

	INIT_WORK(&uaudio_qdev->stop_monitor.work, stop_ep_monitor_work);
	atomic_set(&uaudio_qdev->stop_monitor.stop_completed, 0);
	atomic_set(&uaudio_qdev->stop_monitor.disable_completed, 0);

	ret = snd_usb_register_platform_ops(&offload_ops);
	if (ret < 0)
		goto detach_device;

	for (i = 0; i < SNDRV_CARDS; i++)
		mutex_init(&uadev[i].mutex);

	return 0;

detach_device:
	iommu_detach_device(uaudio_qdev->domain, &pdev->dev);
free_domain:
	iommu_domain_free(uaudio_qdev->domain);
	return ret;
}

static void uaudio_qmi_plat_remove(struct platform_device *pdev)
{
	snd_usb_unregister_platform_ops();
	iommu_detach_device(uaudio_qdev->domain, &pdev->dev);
	iommu_domain_free(uaudio_qdev->domain);
	uaudio_qdev->domain = NULL;

	if (uaudio_qdev->stop_monitor_wq) {
		cancel_work_sync(&uaudio_qdev->stop_monitor.work);
		destroy_workqueue(uaudio_qdev->stop_monitor_wq);
	}
	uaudio_stop_offload_qsram_poll();

	if (uaudio_qdev->offload_ready_wq) {
		/*
		 * Ensure the worker is not running before destroying the wq
		 */
		cancel_work_sync(&uaudio_qdev->offload_ready_work);
		destroy_workqueue(uaudio_qdev->offload_ready_wq);
		uaudio_qdev->offload_ready_wq = NULL;
	}

	/* Free shared pseudo event ring if all	cated */
	if (uaudio_qdev->pseudo_evt_ring_mapped) {
		uaudio_iommu_unmap(MEM_PSEUDO_EVT_RING,
			uaudio_qdev->pseudo_evt_ring_va,
			PAGE_4K, PAGE_4K);

		if (uaudio_qdev->pseudo_evt_ring_buffer) {
			/* Need a valid USB device to free coherent memory */
			usb_free_coherent(uaudio_qdev->pseudo_evt_ring_udev, PAGE_4K,
			uaudio_qdev->pseudo_evt_ring_buffer,
			uaudio_qdev->pseudo_evt_ring_pa);
		}
		uaudio_qdev->pseudo_evt_ring_buffer = NULL;
	}
}

static const struct of_device_id of_uaudio_matach[] = {
	{
		.compatible = "qcom,usb-audio-qmi-1ir-dev",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, of_uaudio_matach);

static struct platform_driver uaudio_qmi_driver = {
	.probe		= uaudio_qmi_plat_probe,
	.remove		= uaudio_qmi_plat_remove,
	.driver		= {
		.name	= "uaudio-qmi-1ir",
		.of_match_table	= of_uaudio_matach,
	},
};

static int uaudio_qmi_svc_init(void)
{
	int ret;
	struct uaudio_qmi_svc *svc;

	svc = kzalloc(sizeof(struct uaudio_qmi_svc), GFP_KERNEL);
	if (!svc)
		return -ENOMEM;

	svc->uaudio_svc_hdl = kzalloc(sizeof(struct qmi_handle), GFP_KERNEL);
	if (!svc->uaudio_svc_hdl) {
		ret = -ENOMEM;
		goto free_svc;
	}

	ret = qmi_handle_init(svc->uaudio_svc_hdl,
				QMI_UAUDIO_STREAM_REQ_MSG_V01_MAX_MSG_LEN,
				&uaudio_svc_ops_options,
				&uaudio_stream_req_handlers);
	if (ret < 0) {
		pr_err("%s:Error registering uaudio svc %d\n", __func__, ret);
		goto free_svc_hdl;
	}

	uaudio_svc = svc;
	ret = qmi_add_server(svc->uaudio_svc_hdl, UAUDIO_STREAM_SERVICE_ID_V01,
					UAUDIO_STREAM_SERVICE_VERS_V01, 0);
	if (ret < 0) {
		pr_err("%s: failed to add uaudio svc server :%d\n",
							__func__, ret);
		goto release_uaudio_svs_hdl;
	}

	svc->uaudio_ipc_log = ipc_log_context_create(NUM_LOG_PAGES * 10, "usb_audio",
			0);

	return 0;

release_uaudio_svs_hdl:
	qmi_handle_release(svc->uaudio_svc_hdl);
	uaudio_svc = NULL;
free_svc_hdl:
	kfree(svc->uaudio_svc_hdl);
free_svc:
	kfree(svc);
	return ret;
}

static void uaudio_qmi_svc_exit(void)
{
	struct uaudio_qmi_svc *svc = uaudio_svc;

	qmi_handle_release(svc->uaudio_svc_hdl);
	kfree(svc->uaudio_svc_hdl);
	ipc_log_context_destroy(svc->uaudio_ipc_log);
	kfree(svc);
	uaudio_svc = NULL;
}

static int __init uaudio_qmi_plat_init(void)
{
	return platform_driver_register(&uaudio_qmi_driver);
}

static void __exit uaudio_qmi_plat_exit(void)
{
	uaudio_qmi_svc_exit();
	platform_driver_unregister(&uaudio_qmi_driver);
}

module_init(uaudio_qmi_plat_init);
module_exit(uaudio_qmi_plat_exit);

MODULE_DESCRIPTION("USB AUDIO QMI One Interrupter Service Driver");
MODULE_LICENSE("GPL");
