// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include "hab.h"

/*
 * HAB OOM killer, executed in workqueue
 *
 * Steps:
 * 1) Scan all vchans on this pchan to find the one with the largest
 *    pending message size, while summing the total pending size.
 * 2) If the accumulated size still exceeds the limit and vchan_max is alive,
 *    holds a reference and stops the channel, then notify the other end.
 * 3) Discard and free all pending messages.
 * 4) Drop the reference.
 */
static void hab_pchan_oom_killer(struct work_struct *work)
{
	struct physical_channel *pchan = container_of(work, struct physical_channel, oom_work);
	struct virtual_channel *vchan, *vchan_max = NULL;
	struct hab_message *message, *msg_tmp;
	int sz, max_sz = 0, total_sz = 0;
	const int disabled_irqs = irqs_disabled();
	int found = 0;

	/* find the vc who has the biggest pending msg sz */
	read_lock(&pchan->vchans_lock);
	list_for_each_entry(vchan, &pchan->vchannels, pnode) {
		sz = vchan->rx_pending_sz;
		total_sz += sz;
		if (sz > max_sz) {
			vchan_max = vchan;
			max_sz = sz;
		}
	}
	if ((vchan_max != NULL) &&
	    (total_sz > pchan->rx_pending_sz_max)) {
		if (kref_get_unless_zero(&vchan_max->refcount) != 0) {
			hab_vchan_stop_notify(vchan_max);
			pr_warn("discard %u bytes msg on vc %x\n",
				vchan_max->rx_pending_sz, vchan_max->id);
			found = 1;
		}
	}
	read_unlock(&pchan->vchans_lock);

	if (found == 1) {
		hab_spin_lock(&vchan_max->rx_lock, disabled_irqs);
		/*
		 * Normally, pending messages are discarded when the client closes the
		 * channel. However, in this scenario the client does not perform any
		 * close/cleanup action, so the HAB driver must explicitly clean all
		 * remaining messages on this vchan.
		 */
		list_for_each_entry_safe(message, msg_tmp, &vchan_max->rx_list, node) {
			list_del(&message->node);
			hab_msg_free(message);
		}
		atomic_sub(vchan_max->rx_pending_sz, &pchan->rx_pending_sz);
		atomic_sub(vchan_max->rx_pending_cnt, &pchan->rx_pending_cnt);
		vchan_max->rx_pending_cnt = 0;
		vchan_max->rx_pending_sz = 0;
		hab_spin_unlock(&vchan_max->rx_lock, disabled_irqs);

		pr_info("after cleanup, %s remaining rx_p sz %d\n",
			pchan->name, atomic_read(&pchan->rx_pending_sz));
		hab_vchan_put(vchan_max);
	}
}

struct physical_channel *
hab_pchan_alloc(struct hab_device *habdev, int otherend_id)
{
	struct physical_channel *pchan = kzalloc(sizeof(*pchan), GFP_KERNEL);

	if (!pchan)
		return NULL;

	idr_init(&pchan->vchan_idr);
	spin_lock_init(&pchan->vid_lock);
	idr_init(&pchan->expid_idr);
	spin_lock_init(&pchan->expid_lock);
	kref_init(&pchan->refcount);

	pchan->habdev = habdev;
	pchan->dom_id = otherend_id;
	pchan->closed = 1;
	pchan->hyp_data = NULL;

	pchan->rx_pending_sz_max = hab_driver.pchan_rx_pending_sz_max;
	pchan->rx_pending_sz_peak = 0;
	atomic_set(&pchan->rx_pending_sz, 0);
	atomic_set(&pchan->rx_pending_cnt, 0);
	INIT_WORK(&pchan->oom_work, hab_pchan_oom_killer);

	INIT_LIST_HEAD(&pchan->vchannels);
	rwlock_init(&pchan->vchans_lock);
	spin_lock_init(&pchan->rxbuf_lock);

	write_lock_bh(&habdev->pchan_lock);
	list_add_tail(&pchan->node, &habdev->pchannels);
	habdev->pchan_cnt++;
	write_unlock_bh(&habdev->pchan_lock);

	return pchan;
}

static void hab_pchan_free(struct kref *ref)
{
	struct physical_channel *pchan =
		container_of(ref, struct physical_channel, refcount);
	struct virtual_channel *vchan;

	pr_debug("pchan %s refcnt %d\n", pchan->name,
			get_refcnt(pchan->refcount));

	write_lock_bh(&pchan->habdev->pchan_lock);
	list_del(&pchan->node);
	pchan->habdev->pchan_cnt--;
	write_unlock_bh(&pchan->habdev->pchan_lock);

	/* check vchan leaking */
	read_lock_bh(&pchan->vchans_lock);
	list_for_each_entry(vchan, &pchan->vchannels, pnode) {
		/* no logging on the owner. it might have been gone */
		pr_warn("leaking vchan id %X remote %X refcnt %d\n",
				vchan->id, vchan->otherend_id,
				get_refcnt(vchan->refcount));
	}
	read_unlock_bh(&pchan->vchans_lock);

	kfree(pchan);
}

struct physical_channel *
hab_pchan_find_domid(struct hab_device *dev, int dom_id)
{
	struct physical_channel *pchan = NULL;
	struct physical_channel *tmp;

	read_lock_bh(&dev->pchan_lock);
	list_for_each_entry(tmp, &dev->pchannels, node) {
		if ((tmp->dom_id == dom_id) || (dom_id == HABCFG_VMID_DONT_CARE)) {
			pchan = tmp;
			break;
		}
	}

	if (pchan && (pchan->dom_id != dom_id) && (dom_id != HABCFG_VMID_DONT_CARE)) {
		pr_err("dom_id mismatch requested %d, existing %d\n",
			dom_id, pchan->dom_id);
		pchan = NULL;
	}

	if (pchan && !kref_get_unless_zero(&pchan->refcount))
		pchan = NULL;

	read_unlock_bh(&dev->pchan_lock);

	return pchan;
}

void hab_pchan_get(struct physical_channel *pchan)
{
	if (pchan)
		kref_get(&pchan->refcount);
}

void hab_pchan_put(struct physical_channel *pchan)
{
	if (pchan)
		kref_put(&pchan->refcount, hab_pchan_free);
}
