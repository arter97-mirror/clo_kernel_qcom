// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include "hab.h"
#include "hab_grantable.h"
#include "hab_trace_os.h"

static void hab_msg_drop(struct physical_channel *pchan, size_t sizebytes);

static int hab_rx_queue_empty(struct virtual_channel *vchan)
{
	int ret;
	const int disabled_irqs = irqs_disabled();

	hab_spin_lock(&vchan->rx_lock, disabled_irqs);
	ret = list_empty(&vchan->rx_list);
	hab_spin_unlock(&vchan->rx_lock, disabled_irqs);

	return ret;
}

static struct hab_message*
hab_scatter_msg_alloc(struct physical_channel *pchan, size_t sizebytes)
{
	struct hab_message *message;
	int i;
	int allocated = 0;
	bool failed = false;
	void **scatter_buf;
	uint32_t total_num, page_num;

	/* The scatter routine is only for the message larger than one page size */
	if (sizebytes <= PAGE_SIZE)
		return NULL;

	page_num = (uint32_t)(sizebytes >> PAGE_SHIFT);
	total_num = ((sizebytes % PAGE_SIZE) == 0U) ? page_num : (page_num + 1U);
	message = kzalloc(sizeof(struct hab_message)
		+ (total_num * sizeof(void *)), GFP_ATOMIC);
	if (!message)
		return NULL;
	message->scatter = true;
	scatter_buf = (void **)message->data;

	/*
	 * All recv buffers need to be prepared before actual recv.
	 * If instant recving is performed when each page is allocated,
	 * we cannot ensure the success of the next allocation.
	 * Part of the message will stuck in the channel if allocation
	 * failed half way.
	 */

	for (i = 0; i < (int)page_num; i++) {
		scatter_buf[i] = kzalloc(PAGE_SIZE, GFP_ATOMIC);
		if (scatter_buf[i] == NULL) {
			failed = true;
			allocated = i;
			break;
		}
	}
	if ((!failed) && ((sizebytes % PAGE_SIZE) != 0U)) {
		scatter_buf[i] = kzalloc(sizebytes % PAGE_SIZE, GFP_ATOMIC);
		if (scatter_buf[i] == NULL) {
			failed = true;
			allocated = i;
		}
	}

	if (!failed) {
		for (i = 0; i < (int)(sizebytes / PAGE_SIZE); i++) {
			message->sizebytes += physical_channel_read(pchan,
				scatter_buf[i], PAGE_SIZE);
		}
		if ((sizebytes % PAGE_SIZE) != 0U) {
			message->sizebytes += physical_channel_read(pchan,
				scatter_buf[i], sizebytes % PAGE_SIZE);
		}
		message->sequence_rx = pchan->sequence_rx;
	} else {
		for (i = 0; i < allocated; i++)
			kfree(scatter_buf[i]);
		kfree(message);
		message = NULL;
	}

	return message;
}

static struct hab_message*
hab_msg_alloc(struct virtual_channel *vchan, size_t sizebytes)
{
	struct physical_channel *pchan = vchan->pchan;
	struct hab_message *message;

	if (sizebytes > HAB_HEADER_SIZE_MAX) {
		pr_err("pchan %s send size too large %zd\n",
			pchan->name, sizebytes);
		return NULL;
	}

	message = kzalloc(sizeof(*message) + sizebytes, GFP_ATOMIC);
	if (!message) {
		/*
		 * big buffer allocation may fail when memory fragment.
		 * Instead of one big consecutive kmem, try alloc one page at a time
		 */
		message = hab_scatter_msg_alloc(pchan, sizebytes);
		if (!message) {
			/*
			 * All allocation attempts failed. Drain the channel data
			 * so the channel remains consistent for subsequent messages.
			 * Mark otherend_closed so the client gets -ENODEV (-19) on next
			 * recv, since the dropped message leaves the vchan in an unreliable
			 * state.
			 */
			pr_err("%s failed to alloc recv buf %zu bytes, forced to drop msg\n",
				pchan->name, sizebytes);
			hab_msg_drop(pchan, sizebytes);
			pr_err("%s stopping vchan %x due to msg alloc failure\n",
				pchan->name, vchan->id);
			hab_vchan_stop_notify(vchan);
		}
	} else {
		message->sizebytes =
			physical_channel_read(pchan, message->data, sizebytes);

		message->sequence_rx = pchan->sequence_rx;
	}

	return message;
}

void hab_msg_free(struct hab_message *message)
{
	int i;
	uint32_t page_num;
	void **scatter_buf;

	if (unlikely(message->scatter)) {
		scatter_buf = (void **)message->data;
		page_num = (uint32_t)(message->sizebytes >> PAGE_SHIFT);

		if ((message->sizebytes % PAGE_SIZE) != 0U)
			page_num++;

		for (i = 0; i < (int)page_num; i++)
			kfree(scatter_buf[i]);
	}

	kfree(message);
}

int
hab_msg_dequeue(struct virtual_channel *vchan, struct hab_message **msg,
		int *rsize, unsigned int timeout, unsigned int flags)
{
	struct hab_message *message = NULL;
	/*
	 * 1. When the user sets the Non-blocking flag and the rx_list is empty,
	 *    or hab_rx_queue_empty is not empty, but due to the competition relationship,
	 *    the rx_list is empty after the lock is obtained,
	 *    and the value of ret in both cases is the default value.
	 * 2. When the function calls API wait_event_*, wait_event_* returns due to timeout
	 *    and the condition is not met, the value of ret is set to 0.
	 * If the default value of ret is 0, we would have a hard time distinguishing
	 * between the above two cases (or with more redundant code).
	 * So we set the default value of ret to be -EAGAIN.
	 * In this way, we can easily distinguish the above two cases.
	 * This is what we expected to see.
	 */
	int ret = -EAGAIN;
	const int wait = !((int)(flags & HABMM_SOCKET_RECV_FLAGS_NON_BLOCKING));
	const int interruptible = !((int)(flags & HABMM_SOCKET_RECV_FLAGS_UNINTERRUPTIBLE));
	const int timeout_flag = (int)(flags & HABMM_SOCKET_RECV_FLAGS_TIMEOUT);
	const int disabled_irqs = irqs_disabled();

	if (wait != 0) {
		/* we will wait forever if timeout_flag not set */
		if (timeout_flag == 0)
			timeout = UINT_MAX;

		if (hab_rx_queue_empty(vchan) != 0) {
			if (interruptible != 0) {
				ret = wait_event_interruptible_timeout(vchan->rx_queue,
					(hab_rx_queue_empty(vchan) == 0) ||
					vchan->otherend_closed,
					msecs_to_jiffies(timeout));
			} else {
				ret = wait_event_timeout(vchan->rx_queue,
					(hab_rx_queue_empty(vchan) == 0) ||
					vchan->otherend_closed,
					msecs_to_jiffies(timeout));
			}
		}
	}

	/*
	 * return all the received messages before the remote close,
	 * and need empty check again in case the list is empty now due to
	 * dequeue by other threads
	 */
	hab_spin_lock(&vchan->rx_lock, disabled_irqs);

	if (!list_empty(&vchan->rx_list)) {
		message = list_first_entry(&vchan->rx_list,
				struct hab_message, node);
		if (message) {
			if (*rsize >= (int)message->sizebytes) {
				/* msg can be safely retrieved in full */
				list_del(&message->node);
				vchan->rx_pending_cnt--;
				vchan->rx_pending_sz -= message->sizebytes;
				atomic_sub((int)message->sizebytes, &vchan->pchan->rx_pending_sz);
				atomic_dec(&vchan->pchan->rx_pending_cnt);
				*rsize = (int)message->sizebytes;
				ret = 0;
			} else {
				pr_err("vcid %x rcv buf too small %d < %zd\n",
					   vchan->id, *rsize,
					   message->sizebytes);
				/*
				 * Here we return the actual message size in RxQ instead of 0,
				 * so that the hab client can re-receive the message with the
				 * correct message size.
				 */
				*rsize = (int)message->sizebytes;
				message = NULL;
				ret = -EOVERFLOW; /* come back again */
			}
		}
	} else {
		/* no message received */
		*rsize = 0;

		if (vchan->otherend_closed) {
			ret = -ENODEV;
		} else if (ret == -ERESTARTSYS) {
			ret = -EINTR;
			dump_hab_pending_signals();
		} else if (ret == 0) {
			pr_debug("timeout! vcid: %x\n", vchan->id);
			ret = -ETIMEDOUT;
		} else {
			pr_debug("EAGAIN: ret = %d, flags = %x\n", ret, flags);
			ret = -EAGAIN;
		}
	}

	hab_spin_unlock(&vchan->rx_lock, disabled_irqs);

	*msg = message;
	return ret;
}

static void hab_msg_queue(struct virtual_channel *vchan,
					struct hab_message *message)
{
	const int disabled_irqs = irqs_disabled();
	int size;
	bool need_stop = false;

	hab_spin_lock(&vchan->rx_lock, disabled_irqs);
	list_add_tail(&message->node, &vchan->rx_list);

	/* msg accounting for vchan & pchan */
	vchan->rx_pending_cnt++;
	if (vchan->rx_pending_cnt > vchan->rx_pending_cnt_peak)
		vchan->rx_pending_cnt_peak = vchan->rx_pending_cnt;
	vchan->rx_pending_sz += message->sizebytes;
	atomic_inc(&vchan->pchan->rx_pending_cnt);
	size = atomic_add_return((int)message->sizebytes, &vchan->pchan->rx_pending_sz);

	/* disconnect current vchan if hold too many msgs */
	if (vchan->rx_pending_cnt > hab_driver.vchan_rx_pending_cnt_max)
		need_stop = true;
	hab_spin_unlock(&vchan->rx_lock, disabled_irqs);

	if (size > vchan->pchan->rx_pending_sz_peak)
		vchan->pchan->rx_pending_sz_peak = size;

	trace_hab_pchan_recv_wakeup(vchan->pchan);
	wake_up(&vchan->rx_queue);

	if (need_stop) {
		pr_err("Stop HAB channel %x due to more than %d msgs not received by client!\n",
			vchan->id, hab_driver.vchan_rx_pending_cnt_max);
		hab_vchan_stop_notify(vchan);
	}

	if (atomic_read(&vchan->pchan->rx_pending_sz) > vchan->pchan->rx_pending_sz_max) {
		/*
		 * Wake up hab oom killer to cleanup inactive pending messages.
		 * Avoid spamming the serial console: printing warnings here may overflow
		 * the log buffer when messages arrive too quickly. Detailed warnings are
		 * handled asynchronously in the OOM workqueue instead.
		 */
		pr_debug("%d bytes msg not recv by client on pchan %s, triggered on vc %x\n",
			atomic_read(&vchan->pchan->rx_pending_sz), vchan->pchan->name, vchan->id);
		schedule_work(&vchan->pchan->oom_work);
	}
}

static int hab_export_enqueue(struct virtual_channel *vchan,
		struct export_desc *export)
{
	struct uhab_context *ctx = vchan->ctx;
	struct export_desc_super *exp_super = container_of(export, struct export_desc_super, exp);
	const int disabled_irqs = irqs_disabled();
	struct export_desc_super *ret;

	hab_spin_lock(&ctx->imp_lock, disabled_irqs);
	ret = hab_rb_exp_insert(&ctx->imp_whse, exp_super);
	if (ret != NULL) {
		pr_err("expid %u already exists on vc %x, size %lu\n",
			export->export_id, vchan->id, PAGE_SIZE * export->payload_count);
	} else {
		ctx->import_total++;
	}
	hab_spin_unlock(&ctx->imp_lock, disabled_irqs);

	return (ret == NULL) ? 0 : -EINVAL;
}

/*
 * Called when received an invalid import request from importer.
 * If not doing this, importer will hang forever awaiting import ack msg.
 */
static int hab_send_import_ack_fail(struct virtual_channel *vchan,
				uint32_t exp_id)
{
	int ret;
	uint32_t export_id = exp_id;
	struct hab_header header = HAB_HEADER_INITIALIZER;

	HAB_HEADER_SET_SIZE(header, sizeof(uint32_t));
	HAB_HEADER_SET_TYPE(header, HAB_PAYLOAD_TYPE_IMPORT_ACK_FAIL);
	HAB_HEADER_SET_ID(header, vchan->otherend_id);
	HAB_HEADER_SET_SESSION_ID(header, vchan->session_id);
	ret = physical_channel_send(vchan->pchan, &header, &export_id,
			HABMM_SOCKET_SEND_FLAGS_NON_BLOCKING);
	if (ret != 0) {
		pr_err("failed to send imp ack fail msg %d, exp_id %d, vcid %x\n",
			ret,
			export_id,
			vchan->id);
	}

	return ret;
}

static int hab_send_import_ack(struct virtual_channel *vchan,
				struct export_desc *exp)
{
	int ret;
	struct export_desc_super *exp_super = container_of(exp, struct export_desc_super, exp);
	enum hab_payload_type type;

	if (unlikely(exp_super->is_loopback))
		type = HAB_PAYLOAD_TYPE_IMPORT_LOOPBACK_ACK;
	else
		type = HAB_PAYLOAD_TYPE_IMPORT_ACK;
	/*
	 * Add 1 byte to the export desc size to avoid mismatch in size
	 * at PVM side, as to resolve error for payload[1]
	 * in 6.12 kernel, as it is converted to payload[]
	 */
	const uint32_t sizebytes = (uint32_t)(sizeof(*exp) + 1U + exp_super->payload_size);
	struct hab_header header = HAB_HEADER_INITIALIZER;

	HAB_HEADER_SET_SIZE(header, sizebytes);
	HAB_HEADER_SET_TYPE(header, type);
	HAB_HEADER_SET_ID(header, vchan->otherend_id);
	HAB_HEADER_SET_SESSION_ID(header, vchan->session_id);

	/*
	 * Local pointers should not be leaked to remote from security perspective.
	 * Relevant lock should be held like other places of modifying exp node
	 * when cleaning local pointers. It is protected by exp_lock for now inside invoker.
	 */
	exp->pchan = NULL;
	exp->vchan = NULL;
	exp->ctx = NULL;
	ret = physical_channel_send(vchan->pchan, &header, exp,
			HABMM_SOCKET_SEND_FLAGS_NON_BLOCKING);
	if (ret != 0) {
		pr_err("failed to send imp ack msg %d, vcid %x\n",
			ret, vchan->id);
	}

	exp->pchan = vchan->pchan;
	exp->vchan = vchan;
	exp->ctx = vchan->ctx;

	return ret;
}

/* Called when facing issue during handling import ack msg to wake up local importer */
static void hab_create_invalid_ack(struct virtual_channel *vchan, uint32_t export_id)
{
	const int disabled_irqs = irqs_disabled();
	struct hab_import_ack_recvd *ack_recvd = kzalloc(sizeof(*ack_recvd), GFP_ATOMIC);

	if (!ack_recvd)
		return;

	ack_recvd->ack.export_id = export_id;
	ack_recvd->ack.vcid_local = vchan->id;
	ack_recvd->ack.vcid_remote = vchan->otherend_id;
	ack_recvd->ack.imp_whse_added = 0;

	hab_spin_lock(&vchan->ctx->impq_lock, disabled_irqs);
	list_add_tail(&ack_recvd->node, &vchan->ctx->imp_rxq);
	hab_spin_unlock(&vchan->ctx->impq_lock, disabled_irqs);
}

static int hab_receive_import_ack_fail(struct physical_channel *pchan,
					struct virtual_channel *vchan)
{
	struct hab_import_ack_recvd *ack_recvd;
	const int disabled_irqs = irqs_disabled();
	uint32_t exp_id = 0U;

	physical_channel_read(pchan, &exp_id, sizeof(uint32_t));

	ack_recvd = kzalloc(sizeof(*ack_recvd), GFP_ATOMIC);
	if (!ack_recvd)
		return -ENOMEM;

	ack_recvd->ack.export_id = exp_id;
	ack_recvd->ack.vcid_local = vchan->id;
	ack_recvd->ack.vcid_remote = vchan->otherend_id;
	ack_recvd->ack.imp_whse_added = 0;

	hab_spin_lock(&vchan->ctx->impq_lock, disabled_irqs);
	list_add_tail(&ack_recvd->node, &vchan->ctx->imp_rxq);
	hab_spin_unlock(&vchan->ctx->impq_lock, disabled_irqs);

	return 0;
}

static int hab_send_export_ack(struct virtual_channel *vchan,
				struct physical_channel *pchan,
				struct export_desc *exp)
{
	int ret;

	struct hab_export_ack exp_ack = {
		.export_id = exp->export_id,
		.vcid_local = exp->vcid_local,
		.vcid_remote = exp->vcid_remote
	};
	struct hab_header header = HAB_HEADER_INITIALIZER;

	HAB_HEADER_SET_SIZE(header, sizeof(exp_ack));
	HAB_HEADER_SET_TYPE(header, HAB_PAYLOAD_TYPE_EXPORT_ACK);
	HAB_HEADER_SET_ID(header, exp->vcid_local);
	HAB_HEADER_SET_SESSION_ID(header, vchan->session_id);
	ret = physical_channel_send(pchan, &header, &exp_ack,
			HABMM_SOCKET_SEND_FLAGS_NON_BLOCKING);
	if (ret != 0) {
		pr_err("failed to send exp ack msg %d, vcid %x\n",
			ret, vchan->id);
	}

	return ret;
}

static int hab_receive_create_export_ack(struct physical_channel *pchan,
		struct uhab_context *ctx, size_t sizebytes)
{
	struct hab_export_ack_recvd *ack_recvd =
		kzalloc(sizeof(*ack_recvd), GFP_ATOMIC);
	const int disabled_irqs = irqs_disabled();

	if (!ack_recvd)
		return -ENOMEM;

	if (sizeof(ack_recvd->ack) != sizebytes) {
		pr_err("%s exp ack size %zu is not as arrived %zu\n",
			   pchan->name, sizeof(ack_recvd->ack), sizebytes);
	}

	if (sizebytes > sizeof(ack_recvd->ack)) {
		pr_err("pchan %s read size too large %zd %zd\n",
			pchan->name, sizebytes, sizeof(ack_recvd->ack));
		kfree(ack_recvd);
		return -EINVAL;
	}

	/*
	 * If the hab version on remote side is different with local side,
	 * the size of the ack structure may differ. Under this circumstance,
	 * the sizebytes is still trusted. Thus, we need to read it out and
	 * drop the mismatched ack message from channel.
	 * Dropping such message could avoid the [payload][header][payload]
	 * data layout which will make the whole channel unusable.
	 * But for security reason, we cannot perform it when sizebytes is
	 * larger than expected.
	 */
	if (physical_channel_read(pchan,
		&ack_recvd->ack,
		sizebytes) != sizebytes) {
		kfree(ack_recvd);
		return -EIO;
	}

	/* add ack_recvd node into rx queue only if the sizebytes is expected */
	if (sizeof(ack_recvd->ack) == sizebytes) {
		hab_spin_lock(&ctx->expq_lock, disabled_irqs);
		list_add_tail(&ack_recvd->node, &ctx->exp_rxq);
		hab_spin_unlock(&ctx->expq_lock, disabled_irqs);
	} else {
		kfree(ack_recvd);
		return -EINVAL;
	}

	return 0;
}

static int compressed_pfns_sanity_check(struct virtual_channel *vchan,
	struct export_desc *exp_desc, size_t sizebytes)
{
	struct compressed_pfns *pfn_table;
	size_t exp_desc_size_expected;
	int ret = 0;

	/*
	 * Add 1 byte to the export desc size to avoid mismatch in size
	 * coming from PVM/Host side, as to resolve error for payload[1]
	 * in 6.12 kernel, as it is converted to payload[]
	 */
	exp_desc_size_expected = sizeof(struct export_desc) + 1U
		+ sizeof(struct compressed_pfns);

	/*
	 * We should do all the checks here.
	 * But in order to improve performance, we put the
	 * checks related to exp->payload_count and pfn_table->region[i].size
	 * into function pages_list_create. So any potential usage of such data
	 * from the remote side after the checks here and before the checks in
	 * pages_list_create needs to add some more checks if necessary.
	 */
	pfn_table = (struct compressed_pfns *)exp_desc->payload;
	if (pfn_table->nregions <= 0 ||
	   (pfn_table->nregions > SIZE_MAX / sizeof(struct region)) ||
	   (SIZE_MAX - exp_desc_size_expected <
	   pfn_table->nregions * sizeof(struct region))) {
		pr_err("%x nregions is too large or negative, nregions:%d!\n",
				vchan->id, pfn_table->nregions);
		ret = -EINVAL;
		goto sanity_fail;
	}

	if (pfn_table->nregions > exp_desc->payload_count) {
		pr_err("%x nregions %d greater than payload_count %d\n",
			vchan->id, pfn_table->nregions, exp_desc->payload_count);
		ret = -EINVAL;
		goto sanity_fail;
	}

	if (exp_desc->payload_count > MAX_EXP_PAYLOAD_COUNT) {
		pr_err("payload_count out of range: %d size overflow\n",
			exp_desc->payload_count);
		ret = -EINVAL;
		goto sanity_fail;
	}

	exp_desc_size_expected += pfn_table->nregions * sizeof(struct region);
	if (sizebytes != exp_desc_size_expected) {
		pr_err("%x exp size not equal %zu expect %zu\n",
			vchan->id, sizebytes, exp_desc_size_expected);
		ret = -EINVAL;
	}

sanity_fail:
	return ret;
}

static int hab_receive_export_desc(struct physical_channel *pchan,
					struct virtual_channel *vchan,
					size_t sizebytes,
					struct export_desc **exp_desc)
{
	size_t exp_desc_size_minimum;
	struct export_desc *export;
	struct export_desc_super *exp_desc_super;
	/*
	 * Add 1 byte to the export desc size to avoid mismatch in size
	 * coming from PVM/Host side, as to resolve error for payload[1]
	 * in 6.12 kernel, as it is converted to payload[]
	 */
	exp_desc_size_minimum = sizeof(struct export_desc) + 1U
							+ sizeof(struct lb_mem_info);
	if ((sizebytes > (size_t)(HAB_HEADER_SIZE_MAX)) ||
			(sizebytes < exp_desc_size_minimum)) {
		pr_err("%s exp size too large/small %zu header %zu %zu\n",
				pchan->name, sizebytes, sizeof(struct export_desc),
				exp_desc_size_minimum);
		return -EINVAL;
	}

	pr_debug("%s exp payload %zu bytes\n", pchan->name, sizebytes);

	exp_desc_super = kzalloc(sizebytes + sizeof(struct export_desc_super)
							- sizeof(struct export_desc), GFP_ATOMIC);
	if (!exp_desc_super)
		return -ENOMEM;

	export = &exp_desc_super->exp;

	if (physical_channel_read(pchan, export, sizebytes) != sizebytes) {
		pr_err("%s corrupted exp expect %zd bytes vcid %X remote %X open %d!\n",
			pchan->name, sizebytes, vchan->id,
			vchan->otherend_id, vchan->session_id);
		kfree(exp_desc_super);
		return -EIO;
	}

	if ((pchan->vmid_local != export->domid_remote) ||
	    (pchan->vmid_remote != export->domid_local)) {
		pr_err("corrupted vmid %d != %d %d != %d\n",
			pchan->vmid_local, export->domid_remote,
			pchan->vmid_remote, export->domid_local);
	}
	export->domid_remote = pchan->vmid_remote;
	export->domid_local = pchan->vmid_local;
	/*
	 * re-init pchan, vchan to local pointers for local usage.
	 * exp->ctx is left un-initialized due to no local usage.
	 */
	export->pchan = pchan;
	export->vchan = vchan;
	*exp_desc = &exp_desc_super->exp;

	return 0;
}

static int hab_recv_and_enqueue_export_desc(struct physical_channel *pchan,
					struct virtual_channel *vchan,
					size_t sizebytes,
					bool is_lb)
{
	struct hab_import_ack_recvd *ack_recvd = NULL;
	struct export_desc *exp_desc = NULL;
	struct export_desc_super *exp_desc_super = NULL;
	const int disabled_irqs = irqs_disabled();
	int ret;

	ret = hab_receive_export_desc(pchan, vchan, sizebytes, &exp_desc);
	if (ret != 0) {
		return ret;
	}

	/* sanity for loopback payload (mmid & expid) will be checked later */
	if (likely(!is_lb)) {
		/* vcid_remote is from remote POV, expect same with local vcid */
		if (vchan->id != exp_desc->vcid_remote) {
			pr_err("exp_desc received on vc %x, not expected vc %x\n",
				vchan->id, exp_desc->vcid_remote);
			goto err_imp;
		}
		ret = compressed_pfns_sanity_check(vchan, exp_desc, sizebytes);
		if (ret != 0)
			goto err_imp;
	}
	exp_desc_super = container_of(exp_desc, struct export_desc_super, exp);
	exp_desc_super->is_loopback = is_lb;

	if (pchan->mem_proto == 1) {
		/* switch vcid local and remote before assigning to ack */
		exp_desc->vcid_remote = exp_desc->vcid_local;
		exp_desc->vcid_local = vchan->id;

		ack_recvd = kzalloc(sizeof(*ack_recvd), GFP_ATOMIC);
		if (!ack_recvd) {
			ret = -ENOMEM;
			goto err_imp;
		}

		ack_recvd->ack.export_id = exp_desc->export_id;
		ack_recvd->ack.vcid_local = exp_desc->vcid_local;
		ack_recvd->ack.vcid_remote = exp_desc->vcid_remote;
	}

	ret = hab_export_enqueue(vchan, exp_desc);

	if (pchan->mem_proto == 1) {
		ack_recvd->ack.imp_whse_added = (uint8_t)(ret ? 0 : 1);
		hab_spin_lock(&vchan->ctx->impq_lock, disabled_irqs);
		list_add_tail(&ack_recvd->node, &vchan->ctx->imp_rxq);
		hab_spin_unlock(&vchan->ctx->impq_lock, disabled_irqs);
	} else {
		(void)hab_send_export_ack(vchan, pchan, exp_desc);
	}

	if (ret)
		kfree(exp_desc_super);

	return ret;

err_imp:
	if (pchan->mem_proto == 1) {
		hab_create_invalid_ack(vchan, exp_desc->export_id);
		hab_send_unimport_msg(vchan, exp_desc->export_id);
	}
	kfree(exp_desc_super);

	return ret;
}

static void hab_recv_imp_req(struct physical_channel *pchan,
					struct virtual_channel *vchan)
{
	struct export_desc *exp;
	struct export_desc_super *exp_desc_super = NULL;
	int found;
	struct hab_import_data imp_data = {0};
	const int disabled_irqs = irqs_disabled();

	if (physical_channel_read(pchan, &imp_data, sizeof(struct hab_import_data)) !=
		sizeof(struct hab_import_data)) {
		pr_err("corrupted import request, id %u page %u vcid %X\n",
				imp_data.exp_id, imp_data.page_cnt, vchan->id);
		return;
	}

	/* expid lock is hold to ensure the availability of exp node */
	hab_spin_lock(&pchan->expid_lock, disabled_irqs);
	exp = idr_find(&pchan->expid_idr, imp_data.exp_id);
	if ((exp != NULL)
	    && (imp_data.page_cnt == exp->payload_count)
	    && (vchan->id == exp->vcid_local)) {
		found = 1;
		exp_desc_super = container_of(exp, struct export_desc_super, exp);
	} else {
		found = 0;
		if (exp != NULL) {
			pr_err("expected vcid %x pcnt %d, actual %x %u\n",
				exp->vcid_local, exp->payload_count,
				vchan->id, imp_data.page_cnt);
		}
	}

	if ((found == 1) && (exp_desc_super->exp_state == HAB_EXP_SUCCESS)) {
		exp_desc_super->remote_imported = 1;
		/* might sleep in Vhost & VirtIO HAB, need non-blocking send or RT Linux */
		hab_send_import_ack(vchan, exp);
		pr_debug("remote imported exp id %d on vcid %x\n",
			exp->export_id, vchan->id);
	} else {
		pr_err("requested exp id %u not found %d on %x\n",
			imp_data.exp_id, found, vchan->id);
		/* might sleep in Vhost & VirtIO HAB, need non-blocking send or RT Linux */
		hab_send_import_ack_fail(vchan, imp_data.exp_id);
	}
	hab_spin_unlock(&pchan->expid_lock, disabled_irqs);
}

static void hab_msg_drop(struct physical_channel *pchan, size_t sizebytes)
{
	uint8_t chunk[HAB_DROP_CHUNK_SIZE];
	size_t remaining = sizebytes;

	if (sizebytes > HAB_HEADER_SIZE_MAX) {
		pr_err("%s read size too large %zd\n", pchan->name, sizebytes);
		return;
	}

	while (remaining > 0) {
		size_t to_read = min(remaining, sizeof(chunk));
		int ret;

		ret = physical_channel_read(pchan, chunk, to_read);
		if (ret <= 0) {
			pr_err("%s drop read failed ret=%d, %zu bytes remained in channel\n",
				pchan->name, ret, remaining);
			break;
		}
		remaining -= ret;
	}
}

static void hab_recv_unimport_msg(struct physical_channel *pchan, int vchan_exist)
{
	uint32_t exp_id = 0U;
	struct export_desc *exp;
	struct export_desc_super *exp_super;
	const int disabled_irqs = irqs_disabled();

	physical_channel_read(pchan, &exp_id, sizeof(uint32_t));

	if (vchan_exist == 0) {
		pr_debug("unimp msg recv after vchan closed on %s, exp id %u\n",
			pchan->name, exp_id);
	}

	/*
	 * expid_lock must be hold long enough to ensure the accessibility of exp_super
	 * before it is freed in habmem_export_destroy where the expid_lock is hold during
	 * idr_remove.
	 */
	hab_spin_lock(&pchan->expid_lock, disabled_irqs);
	exp = idr_find(&pchan->expid_idr, exp_id);

	if ((exp != NULL) && (exp_id == exp->export_id) && (exp->pchan == pchan)) {
		exp_super = container_of(exp, struct export_desc_super, exp);
		if (exp_super->remote_imported != 0) {
			exp_super->remote_imported = 0;
		} else {
			pr_warn("invalid unimp msg recv on pchan %s, exp id %u\n",
				pchan->name, exp_id);
		}
	} else {
		pr_err("invalid unimp msg recv on %s, exp id %u\n", pchan->name, exp_id);
	}
	hab_spin_unlock(&pchan->expid_lock, disabled_irqs);

	if (vchan_exist == 0) {
		/* exp node is not in the reclaim list when vchan still exists */
		schedule_work(&hab_driver.reclaim_work);
	}
}

static int hab_try_get_vchan(struct physical_channel *pchan,
				struct hab_header *header,
				struct virtual_channel **vchan_out)
{
	struct virtual_channel *vchan = NULL;
	const size_t sizebytes = HAB_HEADER_GET_SIZE(*header);
	const uint32_t payload_type = HAB_HEADER_GET_TYPE(*header);
	const uint32_t vchan_id = HAB_HEADER_GET_ID(*header);
	const uint32_t session_id = HAB_HEADER_GET_SESSION_ID(*header);

	/* get the local virtual channel if it isn't an open message */
	if ((payload_type != (uint32_t)HAB_PAYLOAD_TYPE_INIT) &&
		(payload_type != (uint32_t)HAB_PAYLOAD_TYPE_INIT_ACK) &&
		(payload_type != (uint32_t)HAB_PAYLOAD_TYPE_INIT_DONE) &&
		(payload_type != (uint32_t)HAB_PAYLOAD_TYPE_INIT_CANCEL)) {

		/* sanity check the received message */
		if ((payload_type >= (uint32_t)HAB_PAYLOAD_TYPE_MAX) ||
			(vchan_id > (HAB_HEADER_ID_MASK >> HAB_HEADER_ID_SHIFT))
			|| (vchan_id == 0U) || (session_id == 0U)) {
			pr_err("%s Invalid msg type %d vcid %x bytes %zx sn %d\n",
				pchan->name, payload_type,
				vchan_id, sizebytes, session_id);
			dump_hab_wq(pchan);
		}

		/*
		 * need both vcid and session_id to be accurate.
		 * this is from pchan instead of ctx
		 */
		vchan = hab_vchan_get(pchan, header);
		if (!vchan) {
			pr_debug("vchan not found type %d vcid %x sz %zx sesn %d\n",
				payload_type, vchan_id, sizebytes, session_id);

			if (payload_type == (uint32_t)HAB_PAYLOAD_TYPE_UNIMPORT) {
				hab_recv_unimport_msg(pchan, 0);
				return 0;
			}

			if (sizebytes != 0U) {
				hab_msg_drop(pchan, sizebytes);
				pr_err("%s msg dropped type %d size %lu vcid %X session id %d\n",
				pchan->name, payload_type,
				sizebytes, vchan_id,
				session_id);
			}
			return -EINVAL;
		} else if (vchan->otherend_closed) {
			hab_vchan_put(vchan);
			pr_info("vchan remote closed type %d, vchan id %x, sizebytes %zx, session %d\n",
				payload_type, vchan_id,
				sizebytes, session_id);
			if (sizebytes != 0U) {
				hab_msg_drop(pchan, sizebytes);
				pr_err("%s message %d dropped remote close, session id %d\n",
				pchan->name, payload_type,
				session_id);
			}
			return -ENODEV;
		} else {
			/* vchan found and not closed, proceed */
		}
	} else {
		if (sizebytes != sizeof(struct hab_open_send_data)) {
			pr_err("%s Invalid open req type %d vcid %x bytes %zx session %d\n",
				pchan->name, payload_type, vchan_id,
				sizebytes, session_id);
			if (sizebytes != 0U) {
				hab_msg_drop(pchan, sizebytes);
				pr_err("%s msg %d dropped unknown reason session id %d\n",
					pchan->name,
					payload_type,
					session_id);
				dump_hab_wq(pchan);
			}
			return -ENODEV;
		}
	}
	*vchan_out = vchan;

	return 0;
}

static void hab_handle_profile_msg(struct virtual_channel *vchan, size_t sizebytes)
{
	struct timespec64 ts = {0};
	struct physical_channel *pchan = vchan->pchan;
	struct hab_message *message;

	ktime_get_ts64(&ts);

	if (sizebytes < sizeof(struct habmm_xing_vm_stat)) {
		pr_err("%s expected size greater than %zd at least %zd\n",
			pchan->name, sizebytes, sizeof(struct habmm_xing_vm_stat));
		return;
	}

	/* pull down the incoming data */
	message = hab_msg_alloc(vchan, sizebytes);
	if (!message) {
		pr_debug("%s failed to allocate msg Arrived msg will be lost\n",
				pchan->name);
	} else {
		struct habmm_xing_vm_stat *pstat =
			(struct habmm_xing_vm_stat *)message->data;
		pstat->rx_sec = ts.tv_sec;
		pstat->rx_usec = (uint32_t)((unsigned long)ts.tv_nsec / NSEC_PER_USEC);
		hab_msg_queue(vchan, message);
	}
}

static void hab_handle_sched_msg(struct virtual_channel *vchan, size_t sizebytes)
{
	struct hab_message *message;
	unsigned long long rx_mpm_tv;

	if (sizebytes < sizeof(unsigned long long)) {
		pr_err("%s expected size greater than %zd at least %zd\n",
			vchan->pchan->name, sizebytes, sizeof(unsigned long long));
		return;
	}

	rx_mpm_tv = msm_timer_get_sclk_ticks();
	/* pull down the incoming data */
	message = hab_msg_alloc(vchan, sizebytes);
	if (!message) {
		pr_debug("%s failed to allocate msg Arrived msg will be lost\n",
			vchan->pchan->name);
	} else {
		((unsigned long long *)message->data)[0] = rx_mpm_tv;
		hab_msg_queue(vchan, message);
	}
}

int hab_msg_recv(struct physical_channel *pchan,
		struct hab_header *header)
{
	int ret = 0;
	struct hab_message *message;
	struct hab_device *dev = pchan->habdev;
	const size_t sizebytes = HAB_HEADER_GET_SIZE(*header);
	const uint32_t payload_type = HAB_HEADER_GET_TYPE(*header);
	const uint32_t vchan_id = HAB_HEADER_GET_ID(*header);
	const uint32_t session_id = HAB_HEADER_GET_SESSION_ID(*header);
	struct virtual_channel *vchan = NULL;

	ret = hab_try_get_vchan(pchan, header, &vchan);
	if ((ret != 0) ||
		((vchan == NULL) && (payload_type == (uint32_t)HAB_PAYLOAD_TYPE_UNIMPORT))) {
		return ret;
	}

	switch (payload_type) {
	case HAB_PAYLOAD_TYPE_MSG:
	case HAB_PAYLOAD_TYPE_SCHE_RESULT_REQ:
	case HAB_PAYLOAD_TYPE_SCHE_RESULT_RSP:
		message = hab_msg_alloc(vchan, sizebytes);
		if (!message) {
			ret = -ENOMEM;
			break;
		}

		hab_msg_queue(vchan, message);
		break;

	case HAB_PAYLOAD_TYPE_INIT:
	case HAB_PAYLOAD_TYPE_INIT_ACK:
	case HAB_PAYLOAD_TYPE_INIT_DONE:
		ret = hab_open_request_add(pchan, sizebytes, payload_type);
		if (ret) {
			pr_err("%s open request add failed, ret %d, payload type %d, sizebytes %zx\n",
				pchan->name, ret, payload_type, sizebytes);
			break;
		}
		wake_up(&dev->openq);
		break;

	case HAB_PAYLOAD_TYPE_INIT_CANCEL:
		pr_info("remote open cancel header vcid %X session %d local %d remote %d\n",
			vchan_id, session_id, pchan->vmid_local,
			pchan->vmid_remote);
		ret = hab_open_receive_cancel(pchan, sizebytes);
		if (ret) {
			pr_err("%s open cancel handling failed ret %d vcid %X session %d\n",
				pchan->name, ret, vchan_id, session_id);
		}
		break;

	case HAB_PAYLOAD_TYPE_EXPORT:
		ret = hab_recv_and_enqueue_export_desc(pchan, vchan, sizebytes, false);
		if (ret) {
			pr_err("failed to handle exp msg on vcid %x, ret %d\n",
				vchan->id, ret);
		}
		break;

	case HAB_PAYLOAD_TYPE_EXPORT_ACK:
		ret = hab_receive_create_export_ack(pchan, vchan->ctx,
				sizebytes);
		if (ret) {
			pr_err("%s failed to handled export ack %d\n",
				pchan->name, ret);
			break;
		}
		wake_up_interruptible(&vchan->ctx->exp_wq);
		break;

	case HAB_PAYLOAD_TYPE_CLOSE: {
		/* remote request close */
		int refcnt = get_refcnt(vchan->refcount);

		pr_debug("remote close vcid %pK %X other id %X session %d refcnt %d\n",
			vchan, vchan->id, vchan->otherend_id,
			session_id, refcnt);
		hab_vchan_stop(vchan);
		break;
	}

	case HAB_PAYLOAD_TYPE_PROFILE:
		hab_handle_profile_msg(vchan, sizebytes);
		break;

	case HAB_PAYLOAD_TYPE_SCHE_MSG:
	case HAB_PAYLOAD_TYPE_SCHE_MSG_ACK:
		hab_handle_sched_msg(vchan, sizebytes);
		break;

	case HAB_PAYLOAD_TYPE_IMPORT:
		hab_recv_imp_req(pchan, vchan);
		break;

	case HAB_PAYLOAD_TYPE_IMPORT_LOOPBACK_ACK:
		ret = hab_recv_and_enqueue_export_desc(pchan, vchan, sizebytes, true);
		if (ret != 0)
			pr_err("%x failed to handle import loopback ack %d\n", vchan->id, ret);

		/* always try to wake up importer when any failure happens */
		wake_up_interruptible(&vchan->ctx->imp_wq);
		break;

	case HAB_PAYLOAD_TYPE_IMPORT_ACK:
		ret = hab_recv_and_enqueue_export_desc(pchan, vchan, sizebytes, false);
		if (ret != 0)
			pr_err("%x failed to handle import ack %d\n", vchan->id, ret);

		/* always try to wake up importer when any failure happens */
		wake_up_interruptible(&vchan->ctx->imp_wq);
		break;

	case HAB_PAYLOAD_TYPE_IMPORT_ACK_FAIL:
		ret = hab_receive_import_ack_fail(pchan, vchan);
		if (ret)
			pr_err("%s failed to handle import ack fail msg %d\n", pchan->name, ret);

		/* always try to wake up importer when any failure happens */
		wake_up_interruptible(&vchan->ctx->imp_wq);
		break;

	case HAB_PAYLOAD_TYPE_UNIMPORT:
		hab_recv_unimport_msg(pchan, 1);
		break;

	default:
		pr_err("%s unknown msg received, payload type %d, vchan id %x, sizebytes %zx, session %d\n",
			pchan->name, payload_type, vchan_id,
			sizebytes, session_id);
		break;
	}
	hab_vchan_put(vchan);

	return ret;
}
