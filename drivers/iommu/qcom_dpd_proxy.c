// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#define pr_fmt(fmt) "%s: " fmt,  __func__

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/qcom_dpd_proxy.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/si_core_xts.h>
#include <linux/of.h>
#include <soc/qcom/secure_buffer.h>
#include <asm/kvm_pkvm.h>
#include "qcom_dpd_proxy_tee.h"

struct dpd_proxy {
	struct device *dev;
	struct si_object *env;
	struct si_object *service;
	/* Legacy ID of the current linux instance */
	int vmid;
	/* Mapping from PA to dpd_scatterlist */
	struct maple_tree mt;
	struct notifier_block qcom_scm_nb;
	struct notifier_block hyp_assign_nb;
};

static struct dpd_proxy *__dpd_priv;

/*
 * TEE Service Calls
 * -----------------
 */
int dpd_svc_unmap(struct dpd_scatterlist *dpd_sg, u32 domain_id, u64 iova)
{
	struct si_arg args[3] = { 0 };
	int result, ret;
	struct si_object_invoke_ctx oic;
	struct imm_unmap_cmd cmd = {0};

	cmd.eVM = domain_id;
	cmd.iova = iova;
	cmd.size = dpd_sg->size;

	args[0].b = (struct si_buffer) { {&cmd}, sizeof(cmd) };
	args[0].type = SI_AT_IB;

	/* si_object_do_invoke takes away 1 refcount */
	get_si_object(dpd_sg->shm);
	args[1].o = dpd_sg->shm;
	args[1].type = SI_AT_IO;
	args[2].type = SI_AT_END;

	ret = si_object_do_invoke(&oic, __dpd_priv->service, IMM_SVC_UNMAP, args, &result);
	if (ret) {
		pr_err("Invoke failed with %d\n", ret);
		return ret;
	}
	if (result) {
		pr_err("Unmap service call failed with %d for domain:%d\n",
			result, domain_id);
		return -EINVAL;
	}

	/* warn if already unmapped */
	WARN(atomic_dec_return(&dpd_sg->mapcount) < 0, "Mapcount underflow");
	return 0;
}
EXPORT_SYMBOL_GPL(dpd_svc_unmap);

int dpd_svc_map(struct dpd_scatterlist *dpd_sg, u32 domain_id, u32 flags, u64 iova)
{
	struct si_arg args[3] = { 0 };
	int result, ret;
	struct si_object_invoke_ctx oic;
	struct imm_map_cmd cmd = {0};

	if (dpd_sg->perms & QCOM_SCM_PERM_EXEC)
		flags |= IMM_F_EXEC;

	if (flags & ~IMM_VALID_FLAGS)
		return -EINVAL;

	cmd.eVM = domain_id;
	cmd.flags = flags;
	cmd.iova = iova;
	cmd.size = dpd_sg->size;
	cmd.offset = 0;

	args[0].b = (struct si_buffer) { {&cmd}, sizeof(cmd) };
	args[0].type = SI_AT_IB;

	/* si_object_do_invoke takes away 1 refcount */
	get_si_object(dpd_sg->shm);
	args[1].o = dpd_sg->shm;
	args[1].type = SI_AT_IO;
	args[2].type = SI_AT_END;

	ret = si_object_do_invoke(&oic, __dpd_priv->service, IMM_SVC_MAP, args, &result);
	if (ret) {
		pr_err("Invoke failed with %d\n", ret);
		return ret;
	}
	if (result) {
		pr_err("Map service call failed with %d for domain %d\n",
			result, domain_id);
		/*
		 * Return -ENOMEM to client directly when the limit of
		 * number of FFA SPM handles in pKVM hypervisor is getting
		 * exhausted and ENOMEM is returned in result of SMCInvoke.
		 */
		if (result == ENOMEM)
			return -ENOMEM;
		return -EINVAL;
	}

	atomic_inc(&dpd_sg->mapcount);
	return 0;
}
EXPORT_SYMBOL_GPL(dpd_svc_map);

int dpd_svc_register_cbo(struct si_object *si)
{
	struct si_arg args[2] = { 0 };
	int result, ret;
	struct si_object_invoke_ctx oic;

	/* si_object_do_invoke takes away 1 refcount */
	get_si_object(si);
	args[0].o = si;
	args[0].type = SI_AT_IO;
	args[1].type = SI_AT_END;

	ret = si_object_do_invoke(&oic, __dpd_priv->service, IMM_SVC_REGISTER_CBO, args, &result);
	if (ret) {
		pr_err("Invoke failed with %d\n", ret);
		return ret;
	}
	if (result) {
		pr_err("Register cbo service call failed with %d\n", result);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(dpd_svc_register_cbo);

/*
 * This driver is intended to be backwards compatible with the APIs for
 * previous secure memory backends - specifically hyp_assign(),
 * qcom_scm_assign_mem(), and any existing smcinvoke call interfaces.
 *
 * Exceptions:
 * Compatibility with smcinvoke call interfaces requires a one-to-one
 * conversion between dma-buf and smcinvoke memory objects, which in
 * turn prevents use of the secure-system heap memory pools.
 *
 * Any use of hyp-assign/qcom_scm_assign_mem which operates on a
 * subset of a region which was previously hyp-assign'd or
 * qcom_scm_assign_mem'd is disallowed (FFA rule). No known usecases.
 *
 * hyp-assign calls from HLOS to HLOS.
 *
 * SMCINVOKE is weird!
 * init_si_mem_object_sg() does not call FFA_LEND. Instead, if
 * in the future the memory object is passed to TEE by si_object_do_invoke()
 * TZ will then send a request back to HLOS asking it to do FFA_LEND.
 */

#define FMT_QCOM_SCM (0)
#define FMT_HYP_ASSIGN (1)

#define OP_INVALID (0)
#define OP_RECLAIM (1)
#define OP_LEND (2)
#define OP_SHARE (3)
struct dpd_mem_ops_args {
	struct hyp_assign_notifier_data *hyp;
	struct qcom_scm_assign_mem_notifier_data *scm;

	u32 type;
	u32 op;
	bool is_cpz;
	bool iommu_bypass;
};

#define RECLAIM_TIMEOUT_MS (10000)

static int dpd_iommu_bypass_map(struct dpd_mem_ops_args *args, struct dpd_scatterlist *dpd_sg);
static void dpd_iommu_bypass_unmap(struct dpd_mem_ops_args *args, struct dpd_scatterlist *dpd_sg,
	int nr);

static u32 dpd_args_src_len(struct dpd_mem_ops_args *args)
{
	if (args->type == FMT_QCOM_SCM)
		return hweight64(*(args->scm->srcvm));
	else
		return args->hyp->source_nelems;
}

static u32 dpd_args_dst_len(struct dpd_mem_ops_args *args)
{
	if (args->type == FMT_QCOM_SCM)
		return args->scm->dest_cnt;
	else
		return args->hyp->dest_nelems;
}

static struct qcom_scm_vmperm dpd_args_src_vmperm(struct dpd_mem_ops_args *args, int index)
{
	struct qcom_scm_vmperm vmperm = {0};

	if (args->type == FMT_QCOM_SCM) {
		const unsigned long *tmp = (unsigned long *)args->scm->srcvm;
		u32 nr_bits = sizeof(*args->scm->srcvm) * BITS_PER_BYTE;
		int bit;

		/* Caller expected to pass valid index */
		bit = find_nth_bit(tmp, nr_bits, index);
		WARN(bit >= nr_bits, "Bad vm bitmap index");

		vmperm.vmid = bit;
		/* qcom_scm_assign_mem doesn't save src permission */
		vmperm.perm = 0;
		return vmperm;
	} else if (args->type == FMT_HYP_ASSIGN) {
		vmperm.vmid = args->hyp->source_vm_list[index];
		/* hyp-assign doesn't save src permission */
		vmperm.perm = 0;
		return vmperm;
	}

	return vmperm;
}

static struct qcom_scm_vmperm dpd_args_dst_vmperm(struct dpd_mem_ops_args *args, int index)
{
	struct qcom_scm_vmperm vmperm = {0};

	if (args->type == FMT_QCOM_SCM) {
		return args->scm->newvm[index];
	} else if (args->type == FMT_HYP_ASSIGN) {
		vmperm.vmid = args->hyp->dest_vmids[index];
		vmperm.perm = args->hyp->dest_perms[index];
		return vmperm;
	}

	return vmperm;
}

static void dpd_args_show(struct dpd_mem_ops_args *args, bool stack, char *fmt, ...)
{
	int i;
	char *op;
	struct qcom_scm_vmperm vmperm;
	char reason[128];
	va_list va_args;

	va_start(va_args, fmt);
	vscnprintf(reason, sizeof(reason), fmt, va_args);
	va_end(va_args);

	switch (args->op) {
	case OP_RECLAIM:
		op = "Reclaim";
		break;
	case OP_LEND:
		op = "Lend";
		break;
	case OP_SHARE:
		op = "Share";
		break;
	default:
		op = "Invalid Op";
	}

	pr_err("%s failed: %s\n", op, reason);
	for (i = 0; i < dpd_args_src_len(args); i++) {
		vmperm = dpd_args_src_vmperm(args, i);
		pr_err("Source VMID: %x: Perm: %x\n", vmperm.vmid, vmperm.perm);
	}
	for (i = 0; i < dpd_args_dst_len(args); i++) {
		vmperm = dpd_args_dst_vmperm(args, i);
		pr_err("Destination VMID: %x: Perm: %x\n", vmperm.vmid, vmperm.perm);
	}
	if (stack)
		dump_stack();
}

static u32 dpd_args_op(struct dpd_mem_ops_args *args)
{
	struct qcom_scm_vmperm vmperm;
	int perm = QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE | QCOM_SCM_PERM_EXEC;
	int i;

	vmperm = dpd_args_dst_vmperm(args, 0);
	if (dpd_args_dst_len(args) == 1 && vmperm.vmid == __dpd_priv->vmid) {
		if (vmperm.perm == perm)
			return OP_RECLAIM;
		/*
		 * HLOS -> HLOS transactions are not supported because:
		 * SMCinvoke currently always sets the destination to TZ.
		 * Linux FFA framework does not provide a way to adjust the permissions
		 * of the current VM (other than No-Access) because ffa_get_id()
		 * is not exported.
		 */
		args->op = OP_INVALID;
		dpd_args_show(args, true, "Sharing from HLOS -> HLOS is not permitted\n");
		return args->op;
	}

	for (i = 0; i < dpd_args_dst_len(args); i++) {
		vmperm = dpd_args_dst_vmperm(args, i);
		if (vmperm.vmid == __dpd_priv->vmid)
			return OP_SHARE;
	}

	return OP_LEND;
}

/*
 * For lend/share transactions, the source is known to be the current vmid. Similar for
 * the destination for reclaim transactions.
 *
 * Thus, the useful information for identifying usecases is the destination for
 * lend/share and the source for reclaim.
 */
static u32 dpd_args_len(struct dpd_mem_ops_args *args)
{
	if (args->op == OP_RECLAIM)
		return dpd_args_src_len(args);
	else
		return dpd_args_dst_len(args);
}

static struct qcom_scm_vmperm dpd_args_vmperm(struct dpd_mem_ops_args *args, int index)
{
	if (args->op == OP_RECLAIM)
		return dpd_args_src_vmperm(args, index);
	else
		return dpd_args_dst_vmperm(args, index);
}

static int dpd_args_validate(struct dpd_mem_ops_args *args, struct sg_table *sgt)
{
	int i;
	struct qcom_scm_vmperm vmperm;
	bool is_cpz = false;
	bool iommu_bypass = false;

	args->op = dpd_args_op(args);
	if (args->op == OP_INVALID) {
		dpd_args_show(args, false, "Failed to detect Reclaim/Lend/Share Op\n");
		return -EINVAL;
	}

	/* CPZ usecases */
	for (i = 0; i < dpd_args_len(args); i++) {
		vmperm = dpd_args_vmperm(args, i);
		switch (vmperm.vmid) {
		case QCOM_SCM_VMID_CP_PIXEL:
		case QCOM_SCM_VMID_CP_NON_PIXEL:
		case QCOM_SCM_VMID_CP_BITSTREAM:
			is_cpz = true;
			break;
		case QCOM_SCM_VMID_HLOS:
			break;
		default:
			is_cpz = false;
			goto out_cpz;
		}
	}
	if (is_cpz) {
		args->is_cpz = true;
		return 0;
	}
out_cpz:

	/* Dynamic Pil usecases */
	for (i = 0; i < dpd_args_len(args); i++) {
		vmperm = dpd_args_vmperm(args, i);
		switch (vmperm.vmid) {
		case QCOM_SCM_VMID_LPASS:
		case QCOM_SCM_VMID_ADSP_HEAP:
			iommu_bypass = true;
			break;
		case QCOM_SCM_VMID_HLOS:
			break;
		default:
			iommu_bypass = false;
			goto out_iommu_bypass;
		}
	}

	if (iommu_bypass) {
		if (sgt->orig_nents != 1) {
			dpd_args_show(args, false, "iommu bypass requires contiguous memory\n");
			return -EINVAL;
		}
		args->iommu_bypass = true;
		return 0;
	}
out_iommu_bypass:

	dpd_args_show(args, false, "Illegal usecase\n");
	return -EINVAL;
}

static void dpd_sg_rcu_release(struct rcu_head *rcu)
{
	struct dpd_scatterlist *dpd_sg = container_of(rcu, struct dpd_scatterlist, rcu);

	sg_free_table(&dpd_sg->sgt);
	kfree(dpd_sg);
}

/*
 * Used for qcom_scm_assign_mem/hyp_assign case.
 *
 * These callers will synchronously wait for memory release via
 * wait_for_completion(). Additionally, the callers track and release
 * the underlying memory regions separately from us, so free only the
 * sgtable & leave the underlying memory alone.
 */
static void dpd_si_release(void *priv)
{
	struct dpd_scatterlist *dpd_sg = priv;
	struct scatterlist *sg;
	int i;

	for_each_sg(dpd_sg->sgt.sgl, sg, dpd_sg->nents_in_mt, i)
		mtree_erase(&__dpd_priv->mt, PHYS_PFN(sg_phys(sg)));

	complete(&dpd_sg->done);
}

static int put_dpd_si_sync(struct dpd_mem_ops_args *args, struct dpd_scatterlist *dpd_sg)
{
	put_si_object(dpd_sg->shm);
	if (!wait_for_completion_timeout(&dpd_sg->done,
			msecs_to_jiffies(RECLAIM_TIMEOUT_MS))) {
		WARN(true, "Timed out waiting si_object release\n");
		return -EBUSY;
	}
	call_rcu(&dpd_sg->rcu, dpd_sg_rcu_release);
	return 0;
}

/*
 * Acquires an extra refcount which can be released with put_si_object.
 */
struct dpd_scatterlist *dpd_mtree_lookup(unsigned long pfn)
{
	struct dpd_proxy *dpd = __dpd_priv;
	struct dpd_scatterlist *dpd_sg;

	rcu_read_lock();
	dpd_sg = mtree_load(&dpd->mt, pfn);
	if (!dpd_sg)
		goto out;

	/* uses kref_get_unless_zero; returns 0 on failure */
	if (!get_si_object(dpd_sg->shm))
		dpd_sg = NULL;
out:
	rcu_read_unlock();
	return dpd_sg;
}
EXPORT_SYMBOL_GPL(dpd_mtree_lookup);

static int dpd_memory_reclaim(struct dpd_mem_ops_args *args, struct sg_table *sgt)
{
	struct dpd_scatterlist *dpd_sg;
	struct scatterlist *pos, *sg;
	unsigned long pfn;
	int i, val;

	pfn = PHYS_PFN(sg_phys(sgt->sgl));
	dpd_sg = dpd_mtree_lookup(pfn);
	if (!dpd_sg) {
		dpd_args_show(args, true, "Not secure memory\n");
		return -EPERM;
	}
	/*
	 * Clear refcount acquired by dpd_mtree_lookup.
	 * double-free will be detected by refcount_t.
	 */
	put_si_object(dpd_sg->shm);

	val = atomic_read(&dpd_sg->mapcount);
	if (!args->iommu_bypass && val) {
		dpd_args_show(args, true, "Mapped by service\n");
		return -EINVAL;
	} else if (args->iommu_bypass && val != dpd_args_len(args)) {
		dpd_args_show(args, true, "Unexpected mapcount %ld for iommu bypass\n", val);
		return -EINVAL;
	}

	sg = sgt->sgl;
	for_each_sgtable_sg(&dpd_sg->sgt, pos, i) {
		if (!sg || sg_phys(sg) != sg_phys(pos))
			break;
		sg = sg_next(sg);
	}

	if (pos || sg) {
		dpd_args_show(args, true,
			"Reclaim scatterlist mismatch at nent %d/%d\n",
			i, dpd_sg->sgt.orig_nents);
		return -EINVAL;
	}

	dpd_iommu_bypass_unmap(args, dpd_sg, dpd_args_len(args));

	return put_dpd_si_sync(args, dpd_sg);
}

static int dpd_mtree_insert(struct dpd_scatterlist *dpd_sg)
{
	struct scatterlist *sg;
	int i, ret;

	for_each_sgtable_sg(&dpd_sg->sgt, sg, i) {
		/* limits are inclusive */
		unsigned long pfn = PHYS_PFN(sg_phys(sg));
		unsigned long pfn_end = PFN_UP(sg_phys(sg) + sg->length) - 1;

		ret = mtree_insert_range(&__dpd_priv->mt, pfn, pfn_end, dpd_sg, GFP_KERNEL);
		if (WARN(ret, "%s: pfn: %lx failed with %d\n",
			 __func__, pfn, ret))
			return ret;

		dpd_sg->nents_in_mt++;
	}

	return 0;
}

static int scm_to_ffa_perm(int scm_perm)
{
	int ffa_perm = 0;

	if (scm_perm & QCOM_SCM_PERM_READ) {
		if (scm_perm & QCOM_SCM_PERM_WRITE)
			ffa_perm |= FFA_MEM_RW;
		else
			ffa_perm |= FFA_MEM_RO;
	}
	if (scm_perm & QCOM_SCM_PERM_EXEC)
		ffa_perm |= FFA_MEM_EXEC;

	return ffa_perm;
}

/*
 * On success, takes ownership of the scatterlist in sgt and sgt is zero'd.
 * This may occur for certain failure cases as well.
 */
static int dpd_memory_lend_share(struct dpd_mem_ops_args *args, struct sg_table *sgt)
{
	int ret, i;
	struct dpd_scatterlist *dpd_sg;
	uint32_t flags = 0;
	struct scatterlist *sg;
	u32 op = args->op;
	struct qcom_scm_vmperm vmperm;

	dpd_sg = kzalloc(sizeof(*dpd_sg), GFP_KERNEL);
	if (!dpd_sg)
		return -ENOMEM;

	init_completion(&dpd_sg->done);
	atomic_set(&dpd_sg->mapcount, 0);

	/* Take ownership of *sgt */
	dpd_sg->sgt = *sgt;
	*sgt = (struct sg_table){0};

	for_each_sgtable_sg(&dpd_sg->sgt, sg, i)
		dpd_sg->size += sg->length;

	if (op == OP_SHARE)
		flags |= SI_CORE_MEM_OBJ_SHARE;
	else
		flags |= SI_CORE_MEM_OBJ_LEND;

	for (i = 0; i < dpd_args_dst_len(args); i++) {
		vmperm = dpd_args_dst_vmperm(args, i);
		if (vmperm.vmid != VMID_HLOS) {
			dpd_sg->perms = vmperm.perm;
			flags |= SI_CORE_MEM_OBJ_FFA_PERM(scm_to_ffa_perm(dpd_sg->perms));
			break;
		}
	}

	dpd_sg->shm = init_si_mem_object_sg(&dpd_sg->sgt, 0, flags, dpd_si_release, dpd_sg);
	if (!dpd_sg->shm) {
		dpd_args_show(args, false, "init_si_mem_object_sg");
		sg_free_table(&dpd_sg->sgt);
		kfree(dpd_sg);
		return -ENOMEM;
	}

	ret = dpd_mtree_insert(dpd_sg);
	if (ret) {
		put_dpd_si_sync(args, dpd_sg);
		return ret;
	}

	ret = dpd_iommu_bypass_map(args, dpd_sg);
	if (ret)
		put_dpd_si_sync(args, dpd_sg);
	return ret;
}

static int dpd_qcom_scm_assign_mem_notifier(struct notifier_block *nb,
		unsigned long action, void *_data)
{
	struct qcom_scm_assign_mem_notifier_data *data = _data;
	struct sg_table sgt;
	struct dpd_mem_ops_args args = {0};
	struct qcom_scm_vmperm vmperm;
	int ret, i;
	u64 next_vm = 0;

	args.type = FMT_QCOM_SCM;
	args.scm = data;

	ret = sg_alloc_table(&sgt, 1, GFP_KERNEL);
	if (ret)
		return notifier_from_errno(ret);
	sg_set_page(sgt.sgl, phys_to_page(args.scm->mem_addr),
		args.scm->mem_sz, 0);

	ret = dpd_args_validate(&args, &sgt);
	if (ret) {
		sg_free_table(&sgt);
		return notifier_from_errno(ret);
	}

	if (args.op == OP_RECLAIM)
		ret = dpd_memory_reclaim(&args, &sgt);
	else
		ret = dpd_memory_lend_share(&args, &sgt);

	for (i = 0; i < dpd_args_len(&args); i++) {
		vmperm = dpd_args_vmperm(&args, i);
		next_vm |= BIT(vmperm.vmid);
	}
	if (!ret)
		*(args.scm->srcvm) = next_vm;

	sg_free_table(&sgt);
	return ret ? notifier_from_errno(ret) : NOTIFY_STOP;
}

static int dpd_hyp_assign_notifier(struct notifier_block *nb,
		unsigned long action, void *_data)
{
	struct hyp_assign_notifier_data *data = _data;
	struct sg_table sgt;
	struct dpd_mem_ops_args args = {0};
	int ret, i;
	struct scatterlist *sg, *sg_orig;

	args.type = FMT_HYP_ASSIGN;
	args.hyp = data;

	/* Duplicate the sg-table. No contract with caller to preserve it. */
	ret = sg_alloc_table(&sgt, data->table->orig_nents, GFP_KERNEL);
	if (ret)
		return notifier_from_errno(ret);
	sg_orig = data->table->sgl;
	for_each_sgtable_sg(&sgt, sg, i) {
		sg_set_page(sg, sg_page(sg_orig), sg_orig->length, sg_orig->offset);
		sg_orig = sg_next(sg_orig);
	}

	ret = dpd_args_validate(&args, &sgt);
	if (ret) {
		sg_free_table(&sgt);
		return notifier_from_errno(ret);
	}

	if (args.op == OP_RECLAIM)
		ret = dpd_memory_reclaim(&args, &sgt);
	else
		ret = dpd_memory_lend_share(&args, &sgt);

	sg_free_table(&sgt);
	return ret ? notifier_from_errno(ret) : NOTIFY_STOP;
}

/* Special handling for iommu bypass usecases */
static u32 vmid_to_domain_id(int vmid)
{
	switch (vmid) {
	case QCOM_SCM_VMID_LPASS:
		return QCOM_SCM_VMID_LPASS;
	case QCOM_SCM_VMID_ADSP_HEAP:
		return QCOM_SCM_VMID_ADSP_HEAP;
	default:
		return 0;
	}
}

static u32 perm_to_flags(int perm)
{
	u32 flags = 0;

	if (perm & QCOM_SCM_PERM_READ)
		flags |= IMM_F_READ;
	if (perm & QCOM_SCM_PERM_WRITE)
		flags |= IMM_F_WRITE;
	return flags;
}

/*
 * Ideally these would be done using an identity domain through the iommu api.
 * However, legacy clients did not use the dma/iommu apis; only qcom_scm_assign_mem().
 * On x1e8100, we are bound by a requirement to minimize client changes.
 */
static void dpd_iommu_bypass_unmap(struct dpd_mem_ops_args *args,
					struct dpd_scatterlist *dpd_sg, int nr)
{
	int domain, i;
	struct qcom_scm_vmperm vmperm;
	unsigned long iova;

	if (!args->iommu_bypass)
		return;

	for (i = 0; i < nr; i++) {
		vmperm = dpd_args_vmperm(args, i);

		domain = vmid_to_domain_id(vmperm.vmid);
		if (!domain)
			continue;

		iova = sg_phys(dpd_sg->sgt.sgl);

		WARN(dpd_svc_unmap(dpd_sg, domain, iova), "Iommu bypass unmap failed.");
	}
}

static int dpd_iommu_bypass_map(struct dpd_mem_ops_args *args, struct dpd_scatterlist *dpd_sg)
{
	int domain, flags, i, ret;
	struct qcom_scm_vmperm vmperm;
	unsigned long iova;

	if (!args->iommu_bypass)
		return 0;

	for (i = 0; i < dpd_args_len(args); i++) {
		vmperm = dpd_args_vmperm(args, i);

		domain = vmid_to_domain_id(vmperm.vmid);
		if (!domain)
			continue;

		flags = perm_to_flags(vmperm.perm);
		iova = sg_phys(dpd_sg->sgt.sgl);

		ret = dpd_svc_map(dpd_sg, domain, flags, iova);
		if (ret) {
			dpd_iommu_bypass_unmap(args, dpd_sg, i);
			return ret;
		}
	}

	return 0;
}

int dpd_proxy_available(void)
{
	if (!__dpd_priv)
		return -EPROBE_DEFER;
	return 0;
}
EXPORT_SYMBOL_GPL(dpd_proxy_available);

static int dpd_proxy_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct dpd_proxy *dpd;
	struct si_object_invoke_ctx oic;

	dpd = devm_kzalloc(dev, sizeof(*dpd), GFP_KERNEL);
	if (!dpd)
		return -ENOMEM;

	/* If TUIVM support is required, this can be added to DT */
	dpd->vmid = VMID_HLOS;
	dpd->dev = dev;
	mt_init(&dpd->mt);

	ret = si_core_get_client_env(&oic, &dpd->env);
	if (ret) {
		dev_err_probe(dev, ret, "si_core_get_client_env\n");
		return ret;
	}

	ret = si_core_client_env_open(&oic, dpd->env, CSecureMemoryManager_UID, &dpd->service);
	if (ret) {
		dev_err_probe(dev, ret, "si_core_client_env_open\n");
		goto err_get_service;
	}

	dev_set_drvdata(dev, dpd);
	__dpd_priv = dpd;

	dpd->qcom_scm_nb.notifier_call = dpd_qcom_scm_assign_mem_notifier;
	qcom_scm_assign_mem_notifier_register(&dpd->qcom_scm_nb);
	dpd->hyp_assign_nb.notifier_call = dpd_hyp_assign_notifier;
	hyp_assign_notifier_register(&dpd->hyp_assign_nb);
	return 0;

err_get_service:
	put_si_object(dpd->env);
	return ret;
}

static void dpd_proxy_remove(struct platform_device *pdev)
{
	struct dpd_proxy *dpd = platform_get_drvdata(pdev);

	hyp_assign_notifier_unregister(&dpd->hyp_assign_nb);
	qcom_scm_assign_mem_notifier_unregister(&dpd->qcom_scm_nb);

	put_si_object(dpd->service);
	put_si_object(dpd->env);

	__dpd_priv = NULL;
}

static const struct of_device_id dpd_proxy_of_match[] = {
	{ .compatible = "qcom,dpd-proxy" },
	{}
};

static struct platform_driver dpd_proxy_driver = {
	.driver	= {
		.name			= "dpd-proxy",
		.of_match_table		= dpd_proxy_of_match,
	},
	.probe	= dpd_proxy_probe,
	.remove = dpd_proxy_remove,
};

module_platform_driver(dpd_proxy_driver);
MODULE_DESCRIPTION("QTI DPD Proxy Driver");
MODULE_LICENSE("GPL");
