// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <nvhe/iommu.h>
#include <nvhe/trace.h>
#include "smmuv2_nesting.h"
#include "arm-smmuv2-defs.h"
#include "qcom_smmu_dispatcher.h"

static struct smmu_vendor_data data;
unsigned long smmu_v2_nested_count;
struct smmu_v2_nested *smmu_v2_nested_base;

#define for_each_smmu(smmu) \
	for ((smmu) = smmu_v2_nested_base; \
	     (smmu) != &smmu_v2_nested_base[smmu_v2_nested_count]; \
	     (smmu)++)

#define SMMU_V2_DEBUG 0

static void trace___hyp_printk(u8 fmt_id, u64 a, u64 b, u64 c, u64 d)
{
	data.vops->tracing_mod_hyp_printk(fmt_id, a, b, c, d);
}

#define smmu_v2_debug_print(fmt, ...) \
	do { \
		if (SMMU_V2_DEBUG) \
			trace_hyp_printk(fmt, ##__VA_ARGS__); \
	} while (0)

static struct smmu_v2_nested *to_smmu_v2(struct kvm_hyp_iommu *iommu)
{
	return container_of(iommu, struct smmu_v2_nested, iommu);
}

/* Helper functions for register access */
static void smmuv2_cbar_read(struct smmu_v2_nested *smmu, u32 offset, u64 *buf)
{
	int i;

	/* Validate CBAR offset and convert to index */
	if (!ARM_SMMU_GR1_CBAR_VALID(offset, smmu->num_cbar)) {
		*buf = 0;
		return;
	}
	i = ARM_SMMU_GR1_CBAR_INDEX(offset);

	/* Read current hardware value */
	*buf = arm_smmu_gr1_read(smmu, offset);

	/* Extract current type from hardware */
	u32 current_type = FIELD_GET(ARM_SMMU_CBAR_TYPE, *buf);

	/* If hardware has S1_TRANS_S2_TRANS, convert back to S1_TRANS_S2_BYPASS for EL1 */
	if (current_type == CBAR_TYPE_S1_TRANS_S2_TRANS) {
		/* Clear TYPE, VMID, and CBNDX fields (bits [8:15]) */
		*buf &= ~(ARM_SMMU_CBAR_TYPE | ARM_SMMU_CBAR_VMID | ARM_SMMU_CBAR_CBNDX);

		/* Set BYPASS type */
		*buf |= FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S1_TRANS_S2_BYPASS);

		/* Restore original VMID that EL1 wrote */
		u32 original_vmid = smmu->cbar_pool[i] & ARM_SMMU_CBAR_VMID;
		*buf |= original_vmid;
	}

	smmu_v2_debug_print("cbar_read: idx: %d, regoffset: %llx, HW_val: 0x%x, EL1_val: 0x%llx\n",
			    i, smmu->base_pa + offset, (u32)arm_smmu_gr1_read(smmu, offset), *buf);
}

/* Takes absolute hw idx, instead of offset. Checks conflicts against ALL hw SMRs */
static int smmuv2_smr_program(struct smmu_v2_nested *smmu, u32 idx, u32 val)
{
	int j;
	bool old_valid, new_valid;
	u32 new_sid, new_mask;
	u32 other_smr, other_sid, other_mask;

	if (idx >= smmu->total_num_smr) {
		smmu_v2_debug_print("smr_program: idx %u out of range (total_num_smr=%u)\n",
				     idx, smmu->total_num_smr);
		return -EINVAL;
	}

	/* Extract VALID bits from new and old values */
	new_valid = !!(val & ARM_SMMU_SMR_VALID);
	old_valid = !!(smmu->smr_pool[idx] & ARM_SMMU_SMR_VALID);

	if (!new_valid && old_valid) {
		smmu_v2_debug_print("smmu_v2_smr_write: Detaching SMR[%d] (VALID 1->0)\n", idx);
		smmu->smr_pool[idx] = val;
		arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_SMR(idx), val);
		smmu_v2_debug_print("smmu_v2_smr_write: SMR[%d] detached, val: 0x%x\n", idx, val);
		return 0;
	}

	/* If setting VALID=1, perform ARM SMMUv2 spec compliance checks */
	if (new_valid) {
		/* Validate StreamID mask for conflicts
		 */
		new_sid = val & ARM_SMMU_SMR_ID;
		new_mask = (val & ARM_SMMU_SMR_MASK) >> 16;

		for (j = 0; j < smmu->total_num_smr; j++) {
			if (j == idx)
				continue;

			other_smr = smmu->smr_pool[j];
			if (!(other_smr & ARM_SMMU_SMR_VALID))
				continue;

			other_sid = other_smr & ARM_SMMU_SMR_ID;
			other_mask = (other_smr & ARM_SMMU_SMR_MASK) >> 16;

			/* Check for overlapping stream matching
			 * Two SMRs conflict if they can match the same StreamID
			 */
			if (((new_sid & ~new_mask) == (other_sid & ~other_mask)) &&
			    ((new_mask & other_mask) != 0)) {
				smmu_v2_debug_print("smr_write: SMR[%d] (SID=0x%x, MASK=0x%x) ",
						    idx, new_sid, new_mask);
				smmu_v2_debug_print("conflicts with SMR[%d] (SID=0x%x,MASK=0x%x)\n",
						    j, other_sid, other_mask);
				return -EINVAL;
			}
		}
	}

	/* Atomic write with proper ordering per ARM SMMUv2 spec:
	 * Update software pool first, then hardware register
	 */
	smmu->smr_pool[idx] = val;
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_SMR(idx), val);

	smmu_v2_debug_print("smr_write: SMR[%d] written: 0x%x\n",
			    idx, val);
	smmu_v2_debug_print("smr_write: (VALID=%d, SID=0x%x, MASK=0x%x)\n",
			    new_valid, (val & ARM_SMMU_SMR_ID), ((val & ARM_SMMU_SMR_MASK) >> 16));

	return 0;
}

/* Host trap path - validates against host-visible range */
static int smmuv2_smr_write(struct smmu_v2_nested *smmu, u32 offset, u32 val)
{
	if (!ARM_SMMU_GR0_SMR_VALID(offset, smmu->num_smr)) {
		smmu_v2_debug_print("smmu_v2_smr_write: invalid offset: %llx\n", offset);
		return -EINVAL;
	}

	return smmuv2_smr_program(smmu, ARM_SMMU_GR0_SMR_INDEX(offset), val);
}

static int smmuv2_s2cr_program(struct smmu_v2_nested *smmu, u32 idx, u32 s2_cb_idx, u32 val)
{
	u32 hw_val = val;	/* Value to write to hardware */
	u32 current_type;

	/*
	 * Save the ORIGINAL value for later read operations.
	 * This is what EL1 will see on reads.
	 */
	smmu->s2cr_pool[idx] = val;

	/* Extract current S2CR type from the original value */
	current_type = (val & ARM_SMMU_S2CR_TYPE) >> 16;

	/* If type is BYPASS, modify hardware value to TRANS with host/guest CB */
	if (current_type == S2CR_TYPE_BYPASS) {
		/* Clear the type and CBNDX fields in hardware value */
		hw_val &= ~(ARM_SMMU_S2CR_TYPE | ARM_SMMU_S2CR_CBNDX);

		/* Set type to TRANS and CBNDX to host/guest s2_cb_idx */
		hw_val |= FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_TRANS);
		hw_val |= FIELD_PREP(ARM_SMMU_S2CR_CBNDX, s2_cb_idx);
	}

	/* Write the (possibly modified) value to hardware */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_S2CR(idx), hw_val);

	smmu_v2_debug_print("s2cr_write: idx: %d, EL1_val: 0x%x, HW_val: 0x%x, stored: 0x%x\n",
			    idx, val, hw_val, smmu->s2cr_pool[idx]);

	return 0;
}

static int smmuv2_s2cr_write(struct smmu_v2_nested *smmu, u32 offset, u32 val)
{
	u32 current_type;

	if (!ARM_SMMU_GR0_S2CR_VALID(offset, smmu->num_s2cr)) {
		smmu_v2_debug_print("smmu_v2_s2cr_write: invalid offset: %llx\n", offset);
		return -EINVAL;
	}

	current_type = FIELD_GET(ARM_SMMU_S2CR_TYPE, val);
	if (current_type == S2CR_TYPE_TRANS) {
		u32 cbndx = FIELD_GET(ARM_SMMU_S2CR_CBNDX, val);

		if (cbndx >= smmu->num_cbar) {
			smmu_v2_debug_print("smmu_v2_s2cr_write: invalid cbndx: %u (max: %u)\n",
					    cbndx, smmu->num_cbar);
			return -EINVAL;
		}
	} else if (current_type != S2CR_TYPE_BYPASS &&
		   current_type != S2CR_TYPE_FAULT) {
		smmu_v2_debug_print("smmu_v2_s2cr_write: invalid type: %u\n", current_type);
		return -EINVAL;
	}

	return smmuv2_s2cr_program(smmu, ARM_SMMU_GR0_S2CR_INDEX(offset),
				    smmu->host_s2_cb_idx, val);
}

static int smmuv2_cbar_program(struct smmu_v2_nested *smmu, u32 idx, u32 s2_vmid, u32 val)
{
	u32 hw_val;
	u32 current_type;

	/*
	 * Save the ORIGINAL value for later read operations.
	 * This is what EL1 will see on reads.
	 */
	smmu->cbar_pool[idx] = val;

	/* Extract current CBAR type from the original value */
	current_type = FIELD_GET(ARM_SMMU_CBAR_TYPE, val);

	if (current_type == CBAR_TYPE_S2_TRANS ||
		current_type == CBAR_TYPE_S1_TRANS_S2_BYPASS ||
		current_type == CBAR_TYPE_S1_TRANS_S2_TRANS) {
		/* Keep only the interrupt index from the caller-controlled value. */
		hw_val = val & ARM_SMMU_CBAR_IRPTNDX;

		if (s2_vmid == HOST_S2_VMID) {
			hw_val |= FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S1_TRANS_S2_TRANS);
			hw_val |= FIELD_PREP(ARM_SMMU_CBAR_CBNDX, smmu->host_s2_cb_idx);
		} else {
			hw_val |= FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S2_TRANS);
		}

		hw_val |= FIELD_PREP(ARM_SMMU_CBAR_VMID, s2_vmid);
	} else {
		hw_val = (val & ARM_SMMU_CBAR_IRPTNDX) |
			 FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S1_TRANS_S2_FAULT);
	}

	arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBAR(idx), hw_val);

	smmu_v2_debug_print("cbar_write: idx: %d, EL1_val: 0x%x, HW_val: 0x%x, stored: 0x%x\n",
				idx, val, hw_val, smmu->cbar_pool[idx]);

	return 0;
}

static int smmuv2_cbar_write(struct smmu_v2_nested *smmu, u32 offset, u32 val)
{
	/* Validate CBAR offset and convert to index */
	if (!ARM_SMMU_GR1_CBAR_VALID(offset, smmu->num_cbar)) {
		smmu_v2_debug_print("smmu_v2_cbar_write: invalid offset: %llx\n", offset);
		return -EINVAL;
	}

	return smmuv2_cbar_program(smmu, ARM_SMMU_GR1_CBAR_INDEX(offset), HOST_S2_VMID, val);
}

/*
 * Programs TTBR0, TCR, CBA2R, SCTLR, ACTLR, CONTEXTIDR for a
 * stage-2 context bank. Used by both host (idmap) and guest pVM CBs.
 */
static void smmu_program_stage_2(struct smmu_v2_nested *smmu, u32 cb_idx,
				 struct io_pgtable *pgtable)
{
	struct io_pgtable_cfg *pt_cfg = &pgtable->cfg;
	u32 vtcr, sctlr_val, cba2r_val;
	u64 vttbr;

	/* Extract stage-2 configuration from domain */
	vttbr = pt_cfg->arm_lpae_s2_cfg.vttbr;

	/* Build VTCR from page table configuration */
	vtcr = ARM_SMMU_VTCR_RES1 |
	       FIELD_PREP(ARM_SMMU_VTCR_PS, pt_cfg->arm_lpae_s2_cfg.vtcr.ps) |
	       FIELD_PREP(ARM_SMMU_VTCR_TG0, pt_cfg->arm_lpae_s2_cfg.vtcr.tg) |
	       FIELD_PREP(ARM_SMMU_VTCR_SH0, pt_cfg->arm_lpae_s2_cfg.vtcr.sh) |
	       FIELD_PREP(ARM_SMMU_VTCR_ORGN0, pt_cfg->arm_lpae_s2_cfg.vtcr.orgn) |
	       FIELD_PREP(ARM_SMMU_VTCR_IRGN0, pt_cfg->arm_lpae_s2_cfg.vtcr.irgn) |
	       FIELD_PREP(ARM_SMMU_VTCR_SL0, pt_cfg->arm_lpae_s2_cfg.vtcr.sl) |
	       FIELD_PREP(ARM_SMMU_VTCR_T0SZ, pt_cfg->arm_lpae_s2_cfg.vtcr.tsz);

	/* Configure the stage-2 translation registers */
	arm_smmu_cb_writeq(smmu, cb_idx, ARM_SMMU_CB_TTBR0, vttbr);
	arm_smmu_cb_write(smmu, cb_idx, ARM_SMMU_CB_TCR, vtcr);

	/* Configure CBA2R - set VA64 bit for 64-bit virtual addressing */
	cba2r_val = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBA2R(cb_idx));

	cba2r_val |= ARM_SMMU_CBA2R_VA64;
	arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBA2R(cb_idx), cba2r_val);

	/* Configure SCTLR for the host/guest S2 context bank (0xCF0061) */
	sctlr_val = ARM_SMMU_SCTLR_M |
		    ARM_SMMU_SCTLR_CFRE |
		    ARM_SMMU_SCTLR_CFIE |
		    (0xF << 16) |   /* Bits 16-19 */
		    (0x3 << 22);    /* Bits 22-23 */
	arm_smmu_cb_write(smmu, cb_idx, ARM_SMMU_CB_SCTLR, sctlr_val);

	/* Configure ACTLR - set CPRE and CMTLB bits
	 * CPRE  - Enable context caching in the prefetch buffer.
	 * CMTLB - Enable context caching in the macro-TLB.
	 */
	arm_smmu_cb_write(smmu, cb_idx, ARM_SMMU_CB_ACTLR, 0x3);

	/* Configure CONTEXTIDR */
	arm_smmu_cb_write(smmu, cb_idx, ARM_SMMU_CB_CONTEXTIDR, 0x0);

	smmu_v2_debug_print("Configured S2, CB[%d] VTCR=0x%x, VTTBR=0x%llx, CBA2R=0x%x\n",
			     cb_idx, vtcr, vttbr, cba2r_val);
	smmu_v2_debug_print("Configured S2, CB[%d] SCTLR=0x%x, ACTLR=0x3, CONTEXTIDR=0x0\n",
			     cb_idx, sctlr_val);
}

/*
 * Programs the host S2 context bank across all SMMUs using the
 * idmapped vdomain page table. Called during post-boot init.
 */
static int smmu_attach_stage_2(void)
{
	struct smmu_v2_nested *smmu;

	if (!data.vdomain)
		return -EINVAL;

	for_each_smmu(smmu)
		smmu_program_stage_2(smmu, smmu->host_s2_cb_idx,
				     data.vdomain->pgtable);

	return 0;
}

/* Core register access functions */
static int smmuv2_read_global_region_0(struct smmu_v2_nested *smmu, u64 offset, u32 len, u64 *buf)
{
	/* Read the register value first, then handle specific cases */
	*buf = arm_smmu_gr0_read(smmu, offset);
	/* Use specific log messages for special registers */
	if (offset == ARM_SMMU_GR0_ID0) {
		*buf = (*buf & ~ARM_SMMU_ID0_NUMSMRG) | (smmu->num_smr & ARM_SMMU_ID0_NUMSMRG);
		*buf = (*buf & ~ARM_SMMU_ID0_S2TS);
		*buf = (*buf & ~ARM_SMMU_ID0_NTS);
		*buf = (*buf & ~ARM_SMMU_ID0_EXIDS);
		smmu_v2_debug_print("smmu_v2_id0_read, addr: %llx, buf: %llx\n",
				    smmu->base_pa + offset, *buf);
	} else if (offset == ARM_SMMU_GR0_ID1) {
		*buf = (*buf & ~ARM_SMMU_ID1_NUMCB) | (smmu->num_cb & ARM_SMMU_ID1_NUMCB);
		*buf = (*buf & ~ARM_SMMU_ID1_NUMS2CB);
		smmu_v2_debug_print("smmu_v2_id1_read, addr: %llx, buf: %llx\n",
				    smmu->base_pa + offset, *buf);
	} else if (offset == ARM_SMMU_GR0_ID2) {
		*buf &= ~ARM_SMMU_ID2_VMID16;
		smmu_v2_debug_print("smmu_v2_id2_read, addr: %llx, buf: %llx\n",
				    smmu->base_pa + offset, *buf);
	} else if (offset >= ARM_SMMU_GR0_SMR(0) &&
		   offset <= ARM_SMMU_GR0_SMR(ARM_SMMU_MAX_SMRS - 1)) {
		/* SMR register range */
		smmu_v2_debug_print("smmu_v2_smr_read, addr: %llx, buf: %llx\n",
				    smmu->base_pa + offset, *buf);
	} else if (offset >= ARM_SMMU_GR0_S2CR(0) &&
		   offset <= ARM_SMMU_GR0_S2CR(ARM_SMMU_MAX_S2CRS - 1)) {
		/* S2CR register range - read from hardware and virtualize type + CBNDX */
		int i;

		/* Validate S2CR offset and convert to index */
		if (!ARM_SMMU_GR0_S2CR_VALID(offset, smmu->num_s2cr))
			return -EINVAL;  /* Invalid S2CR offset */
		i = ARM_SMMU_GR0_S2CR_INDEX(offset);

		/* Extract current type */
		u32 current_type = (*buf & ARM_SMMU_S2CR_TYPE) >> 16;

		/* If hardware has TRANS pointing at host_s2_cb_idx, convert back to BYPASS for EL1 */
		if (current_type == S2CR_TYPE_TRANS &&
		    FIELD_GET(ARM_SMMU_S2CR_CBNDX, *buf) == smmu->host_s2_cb_idx) {
			/* Clear type and CBNDX fields */
			*buf &= ~(ARM_SMMU_S2CR_TYPE | ARM_SMMU_S2CR_CBNDX);

			/* Set BYPASS type */
			*buf |= FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_BYPASS);

			/* Restore original CBNDX that EL1 wrote */
			u32 original_cbndx = smmu->s2cr_pool[i] & ARM_SMMU_S2CR_CBNDX;
			*buf |= original_cbndx;
		}

		smmu_v2_debug_print("smmu_v2_s2cr_read, addr: %llx, buf: %llx\n",
				    smmu->base_pa + offset, *buf);
	} else {
		/* Default case for all other registers */
		smmu_v2_debug_print("smmu_v2_read_global_region_0, addr: %llx, buf: %llx\n",
				    smmu->base_pa + offset, *buf);
	}
	return 0;
}

static int smmuv2_read_global_region_1(struct smmu_v2_nested *smmu, u64 offset, u32 len, u64 *buf)
{
	if (offset >= ARM_SMMU_GR1_CBAR(0) &&
	    offset <= ARM_SMMU_GR1_CBAR(ARM_SMMU_MAX_CBARS - 1)) {
		smmuv2_cbar_read(smmu, offset, buf);
	} else if (offset >= ARM_SMMU_GR1_CBA2R(0) &&
		   offset <= ARM_SMMU_GR1_CBA2R(ARM_SMMU_MAX_CBARS - 1)) {
		if (!ARM_SMMU_GR1_CBA2R_VALID(offset, smmu->num_cbar)) {
			*buf = 0;
			return 0;
		}
		*buf = arm_smmu_gr1_read(smmu, offset);
	} else {
		*buf = arm_smmu_gr1_read(smmu, offset);
		smmu_v2_debug_print("smmu_v2_read_global_region_1, addr: %llx, buf: %llx\n",
				    smmu->base_pa + offset, *buf);
	}
	return 0;
}

static int smmuv2_write_global_region_0(struct smmu_v2_nested *smmu,
					u32 offset,
					unsigned int len,
					u64 val)
{
	if (len != sizeof(u32) && len != sizeof(u64))
		return -EINVAL;

	/* Handle SMR and S2CR register ranges */
	if (offset >= ARM_SMMU_GR0_SMR(0) && offset <= ARM_SMMU_GR0_SMR(ARM_SMMU_MAX_SMRS - 1))
		return smmuv2_smr_write(smmu, offset, (u32)val);
	if (offset >= ARM_SMMU_GR0_S2CR(0) && offset <= ARM_SMMU_GR0_S2CR(ARM_SMMU_MAX_S2CRS - 1))
		return smmuv2_s2cr_write(smmu, offset, (u32)val);

	/* Explicit allowlist for Global Region 0 registers (default-deny) */
	switch (offset) {
	case ARM_SMMU_GR0_sCR0: {
		u32 hw_val = (u32)val;

		/*
		 * Enforce SMMU enabled (CLIENTPD=0), unidentified stream fault
		 * (USFCFG=1), stream match conflict fault (SMCFCFG=1), and
		 * disallow extended stream IDs (EXIDENABLE=0) and 16-bit VMIDs
		 * (VMID16EN=0).
		 */
		hw_val &= ~(ARM_SMMU_sCR0_CLIENTPD | ARM_SMMU_sCR0_EXIDENABLE |
			    ARM_SMMU_sCR0_VMID16EN);
		hw_val |= ARM_SMMU_sCR0_USFCFG | ARM_SMMU_sCR0_SMCFCFG;
		arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sCR0, hw_val);
		return 0;
	}
	case ARM_SMMU_GR0_sACR:
	case ARM_SMMU_GR0_sGFSR:
	case ARM_SMMU_GR0_TLBIVMID:
	case ARM_SMMU_GR0_TLBIALLNSNH:
	case ARM_SMMU_GR0_TLBIALLH:
	case ARM_SMMU_GR0_sTLBGSYNC:
		arm_smmu_gr0_write(smmu, offset, (u32)val);
		return 0;
	default:
		return -EPERM;
	}
}

static int smmuv2_write_global_region_1(struct smmu_v2_nested *smmu,
					u64 offset,
					unsigned int len,
					u64 val)
{
	if (len != sizeof(u32) && len != sizeof(u64))
		return -EINVAL;

	/* Handle CBAR registers */
	if (offset >= ARM_SMMU_GR1_CBAR(0) &&
	    offset <= ARM_SMMU_GR1_CBAR(ARM_SMMU_MAX_CBARS - 1))
		return smmuv2_cbar_write(smmu, offset, (u32)val);

	/*
	 * Handle CBA2R registers: only allow writes to host-owned stage-1
	 * context banks (< num_cbar) so the host cannot clear VA64 on
	 * CBA2R(host_s2_cb_idx) or firmware context banks. Also clear VMID16
	 * to prevent upper VMID bits from diverging from HOST_S2_VMID.
	 */
	if (offset >= ARM_SMMU_GR1_CBA2R(0) &&
	    offset <= ARM_SMMU_GR1_CBA2R(ARM_SMMU_MAX_CBARS - 1)) {
		if (!ARM_SMMU_GR1_CBA2R_VALID(offset, smmu->num_cbar))
			return -EINVAL;

		arm_smmu_gr1_write(smmu, offset, (u32)val & ~ARM_SMMU_CBA2R_VMID16);
		return 0;
	}

	/* Default-deny all other Global Region 1 writes */
	return -EPERM;
}

/* Device access and fault handling */
static bool smmuv2_nesting_dabt_device(struct smmu_v2_nested *smmu,
				      struct user_pt_regs *regs,
				      u64 esr, u32 addr)
{
	bool is_write = esr & ESR_ELx_WNR;
	unsigned int len = BIT((esr & ESR_ELx_SAS) >> ESR_ELx_SAS_SHIFT);
	int rd = (esr & ESR_ELx_SRT_MASK) >> ESR_ELx_SRT_SHIFT;
	u64 val = regs->regs[rd];
	u32 offset;
	int ret = 0;
	u32 host_s2_cb_offset = ARM_SMMU_CB(smmu, smmu->host_s2_cb_idx) << smmu->pgshift;
	u32 cb_size = 1U << smmu->pgshift;

	smmu_v2_debug_print("addr: 0x%llx, val: 0x%llx, esr: 0x%llx\n",
			    addr, regs->regs[rd], esr);
	smmu_v2_debug_print("size: %d, rd: %d, is_write: %d\n",
			    len, rd, is_write);

	kvm_iommu_lock(&smmu->iommu);

	offset = (u32)(addr & SMMU_V2_GLB_ADDR_OFFSET_MASK);

	if (addr >= host_s2_cb_offset && addr < host_s2_cb_offset + cb_size) {
		if (!is_write) {
			void *reg_addr = (void *)((u64)smmu->base_va + addr);

			regs->regs[rd] = readl_relaxed(reg_addr);
		} else if ((addr - host_s2_cb_offset) == ARM_SMMU_CB_FSR) {
			void *reg_addr = (void *)((u64)smmu->base_va + addr);

			writel_relaxed((u32)val, reg_addr);
		} else {
			ret = -EPERM;
		}
	} else if (offset < ARM_SMMU_GLOBAL_REGION1_OFFSET) {
		if (is_write) {
			ret = smmuv2_write_global_region_0(smmu, offset, len, val);
		} else {
			ret = smmuv2_read_global_region_0(smmu, offset, len, &val);
			regs->regs[rd] = val;
		}
	} else if (offset < ARM_SMMU_GLOBAL_REGION2_OFFSET) {
		offset -= ARM_SMMU_GLOBAL_REGION1_OFFSET;
		if (is_write) {
			ret = smmuv2_write_global_region_1(smmu, offset, len, val);
		} else {
			ret = smmuv2_read_global_region_1(smmu, offset, len, &val);
			regs->regs[rd] = val;
		}
	} else {
		ret = -EPERM;
	}
	kvm_iommu_unlock(&smmu->iommu);
	return ret;
}

static bool smmuv2_nesting_dabt_handler(struct user_pt_regs *regs, u64 esr, u64 addr)
{
	struct smmu_v2_nested *smmu;
	u32 size = ARM_SMMU_GLOBAL_REGION2_OFFSET;
	int ret;

	for_each_smmu(smmu) {
		u32 host_s2_cb_offset = ARM_SMMU_CB(smmu, smmu->host_s2_cb_idx) << smmu->pgshift;
		u32 cb_size = 1U << smmu->pgshift;

		/* Check if address is within this SMMU's range */
		if (((addr >= smmu->base_pa) && (addr < (smmu->base_pa + size))) ||
		    ((addr >= smmu->base_pa + host_s2_cb_offset) &&
			 (addr < smmu->base_pa + host_s2_cb_offset + cb_size))) {
			ret = smmuv2_nesting_dabt_device(smmu, regs, esr, addr - smmu->base_pa);
			return (ret != 0) ? false : true;
		}
	}
	return false;
}

/* True if @iommu is one of this driver's SMMU instances. */
static bool smmuv2_owns_iommu(struct kvm_hyp_iommu *iommu)
{
	struct smmu_v2_nested *smmu;

	for_each_smmu(smmu)
		if (&smmu->iommu == iommu)
			return true;

	return false;
}

/* TLB maintenance */
/* Wait for any pending TLB invalidations to complete */
static int __arm_smmu_tlb_sync(struct smmu_v2_nested *smmu, int cb_idx,
				int sync, int status)
{
	unsigned int inc, delay;
	u32 reg;

	arm_smmu_cb_write(smmu, cb_idx, sync, QCOM_DUMMY_VAL);

	for (delay = 1, inc = 1; delay < TLB_LOOP_TIMEOUT; delay += inc) {
		reg = arm_smmu_cb_read(smmu, cb_idx, status);
		if (!(reg & ARM_SMMU_sTLBGSTATUS_GSACTIVE))
			return 0;

		pkvm_udelay(inc);

		if (inc < TLB_LOOP_INC_MAX)
			inc *= 2;
	}

	return -EINVAL;
}

static void arm_smmu_tlb_sync_context(struct smmu_v2_nested *smmu, int cb_idx)
{
	if (__arm_smmu_tlb_sync(smmu, cb_idx,
				ARM_SMMU_CB_TLBSYNC,
				ARM_SMMU_CB_TLBSTATUS)) {
		/* Fatal assertion in case of TLB sync failure*/
		BUG_ON(1);
	}
}

static void arm_smmu_tlb_sync_global(struct smmu_v2_nested *smmu)
{
	unsigned int inc, delay;
	u32 reg;

	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sTLBGSYNC, QCOM_DUMMY_VAL);

	for (delay = 1, inc = 1; delay < TLB_LOOP_TIMEOUT; delay += inc) {
		reg = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sTLBGSTATUS);
		if (!(reg & ARM_SMMU_sTLBGSTATUS_GSACTIVE))
			return;

		pkvm_udelay(inc);

		if (inc < TLB_LOOP_INC_MAX)
			inc *= 2;
	}

	/* Fatal assertion in case of TLB sync failure */
	BUG_ON(1);
}

/*
 * Invalidate all S2 TLB entries tagged with @vmid on @smmu and wait for
 * the sync. Called when a guest CB is retired so a future domain reusing
 * the CB/VMID cannot hit stale translations.
 */
static void arm_smmu_tlb_inv_vmid(struct smmu_v2_nested *smmu, u32 vmid)
{
	/* Issue invalidate */
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIVMID, vmid);
	/* Request sync and wait for completion */
	arm_smmu_tlb_sync_global(smmu);
}

static void arm_smmu_tlb_inv_range_s2(struct smmu_v2_nested *smmu, int cb_idx,
				      unsigned long iova, size_t size,
				      size_t granule, int reg)
{
	iova >>= PAGE_SHIFT;

	do {
		arm_smmu_cb_writeq(smmu, cb_idx, reg, iova);
		iova += granule >> PAGE_SHIFT;
	} while (size > granule && (size -= granule));
}

/*
 * Lock, invalidate an IPA range in @cb_idx's S2 TLB, sync, invalidate
 * combined S1+S2 entries for @vmid, and unlock.
 */
static void arm_smmu_tlb_inv_sync_s2(struct smmu_v2_nested *smmu, int cb_idx,
				     u32 vmid, unsigned long iova, size_t size,
				     size_t granule, int reg)
{
	kvm_iommu_lock(&smmu->iommu);
	arm_smmu_tlb_inv_range_s2(smmu, cb_idx, iova, size, granule, reg);
	arm_smmu_tlb_sync_context(smmu, cb_idx);
	arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_TLBIVMIDS1, vmid);
	arm_smmu_tlb_sync_global(smmu);
	kvm_iommu_unlock(&smmu->iommu);
}

/*
 * Host fully-virtualized path: invalidate the reserved host S2 context
 * bank on every SMMU instance.
 */
static void arm_smmu_tlb_add_page_s2(struct iommu_iotlb_gather *gather,
				     unsigned long iova, size_t granule,
				     void *cookie)
{
	struct smmu_v2_nested *smmu;

	for_each_smmu(smmu)
		arm_smmu_tlb_inv_sync_s2(smmu, smmu->host_s2_cb_idx,
					 HOST_S2_VMID, iova, granule, granule,
					 ARM_SMMU_CB_S2_TLBIIPAS2L);
}

static void arm_smmu_tlb_inv_walk_s2(unsigned long iova, size_t size,
				     size_t granule, void *cookie)
{
	struct smmu_v2_nested *smmu;

	for_each_smmu(smmu)
		arm_smmu_tlb_inv_sync_s2(smmu, smmu->host_s2_cb_idx,
					 HOST_S2_VMID, iova, size, granule,
					 ARM_SMMU_CB_S2_TLBIIPAS2);
}

static const struct iommu_flush_ops arm_smmu_s2_tlb_ops_v2 = {
	.tlb_flush_walk	= arm_smmu_tlb_inv_walk_s2,
	.tlb_add_page	= arm_smmu_tlb_add_page_s2,
};

/*
 * Guest para-virtualized path: the io-pgtable cookie is the
 * kvm_hyp_iommu_domain, whose priv (smmu_nested_guest_domain) resolves to
 * the guest domain's own SMMU and CB (both set at attach_dev). Invalidate
 * only that CB on that SMMU.
 *
 * v1 assumption: a guest domain spans a single SMMU instance
 * (smmu_nested_guest_domain.iommu). Multi-SMMU domains would require
 * walking a per-domain SMMU list.
 */
static void arm_smmu_guest_tlb_inv_s2(void *cookie, unsigned long iova,
				      size_t size, size_t granule, int reg)
{
	struct kvm_hyp_iommu_domain *domain = cookie;
	struct smmu_nested_guest_domain *guest_domain = domain->priv;

	if (!guest_domain || !guest_domain->iommu || guest_domain->cb_idx < 0)
		return;

	arm_smmu_tlb_inv_sync_s2(to_smmu_v2(guest_domain->iommu),
				 guest_domain->cb_idx, domain->domain_id,
				 iova, size, granule, reg);
}

static void arm_smmu_guest_tlb_add_page_s2(struct iommu_iotlb_gather *gather,
					   unsigned long iova, size_t granule,
					   void *cookie)
{
	arm_smmu_guest_tlb_inv_s2(cookie, iova, granule, granule,
				  ARM_SMMU_CB_S2_TLBIIPAS2L);
}

static void arm_smmu_guest_tlb_inv_walk_s2(unsigned long iova, size_t size,
					   size_t granule, void *cookie)
{
	arm_smmu_guest_tlb_inv_s2(cookie, iova, size, granule,
				  ARM_SMMU_CB_S2_TLBIIPAS2);
}

static const struct iommu_flush_ops arm_smmu_guest_s2_tlb_ops_v2 = {
	.tlb_flush_walk	= arm_smmu_guest_tlb_inv_walk_s2,
	.tlb_add_page	= arm_smmu_guest_tlb_add_page_s2,
};

/* Device attach/detach */
static int smmuv2_attach_dev(struct kvm_hyp_iommu *iommu,
			     struct kvm_hyp_iommu_domain *domain,
			     u32 sid, u32 pasid, u32 pasid_bits,
			     unsigned long flags)
{
	struct smmu_v2_nested *smmu = to_smmu_v2(iommu);
	struct smmu_nested_guest_domain *guest_domain = domain->priv;
	int i, ret = 0;
	int guest_smr, guest_cb;
	u32 smr_val = 0, s2cr_val = 0, cbar_val = 0;
	pkvm_handle_t domain_id = domain->domain_id;
	bool new_cb = false;

	/* Not one of our SMMUs - let the dispatcher try the next driver. */
	if (!smmuv2_owns_iommu(iommu))
		return -ENODEV;

	/*
	 * A domain binds to one SMMU. cb_idx and the CB/SMR pools are per
	 * instance, so a CB index from one SMMU is meaningless on another.
	 */
	if (guest_domain->iommu && guest_domain->iommu != iommu)
		return -EINVAL;

	/*
	 * Allocate the guest S2 page table on first attach (deferred from
	 * alloc_domain since the SMMU version is only known here).
	 */
	if (!guest_domain->pgtable) {
		struct io_pgtable_cfg cfg = (struct io_pgtable_cfg) {
			.fmt = ARM_64_LPAE_S2,
			.pgsize_bitmap = SZ_4K | SZ_2M | SZ_1G,
			.ias = 48,
			.oas = 48,
			.coherent_walk = true,
			.tlb = &arm_smmu_guest_s2_tlb_ops_v2,
		};

		/* Double-checked: prevents a concurrent attach from allocating twice. */
		hyp_spin_lock(&guest_domain->pgt_lock);
		if (!guest_domain->pgtable)
			guest_domain->pgtable = kvm_arm_io_pgtable_alloc(&cfg, domain,
									 &ret, false);
		hyp_spin_unlock(&guest_domain->pgt_lock);
		if (ret)
			return ret;
	}

	kvm_iommu_lock(iommu);

	/* Allocate a CB for this domain, if not yet assigned */
	if (guest_domain->cb_idx < 0) {
		for (i = 0; i < smmu->num_guest_cb; i++) {
			/* Assumption here - domain_id couldn't be 0 */
			if (smmu->guest_cb_pool[i] == ARM_SMMU_GUEST_CB_FREE) {
				smmu->guest_cb_pool[i] = (u32)domain_id;
				guest_cb = i + smmu->guest_cb_base;
				guest_domain->cb_idx = guest_cb;

				/* Bind domain to this SMMU (v1: single SMMU per domain). */
				guest_domain->iommu = iommu;
				break;
			}
		}

		if (guest_domain->cb_idx < 0) {
			kvm_iommu_unlock(iommu);
			return -ENOSPC;
		}

		new_cb = true;

		/* Program CBAR - S2 transl with domain_id as VMID */
		cbar_val |= FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S2_TRANS);
		cbar_val |= FIELD_PREP(ARM_SMMU_CBAR_VMID, domain->domain_id);

		smmuv2_cbar_program(smmu, guest_cb, domain->domain_id, cbar_val);

		/* Program the guest S2 CB (TTBR/SCTLR) from the domain page table */
		smmu_program_stage_2(smmu, guest_cb, guest_domain->pgtable);
	}

	/* Get the Guest S2 CB idx */
	guest_cb = guest_domain->cb_idx;

	/* Allocate SMR from guest pool */
	guest_smr = -1;
	for (i = 0; i < smmu->num_guest_smr; i++) {
		if (smmu->guest_smr_pool[i] == ARM_SMMU_GUEST_SMR_FREE) {
			smmu->guest_smr_pool[i] = guest_cb;
			guest_smr = i + smmu->guest_smr_base;
			break;
		}
	}

	if (guest_smr < 0) {
		/*
		 * Only free a CB this call allocated. One inherited from an earlier
		 * attach still backs a live S2CR, and clearing cb_idx would leave
		 * that SID with no way to detach.
		 */
		if (new_cb) {
			smmu->guest_cb_pool[guest_cb - smmu->guest_cb_base] =
				ARM_SMMU_GUEST_CB_FREE;
			guest_domain->cb_idx = -1;
			guest_domain->iommu = NULL;
		}
		kvm_iommu_unlock(iommu);
		return -ENOSPC;
	}

	/* Program SMR */
	smr_val = sid | ARM_SMMU_SMR_VALID;
	smmuv2_smr_program(smmu, guest_smr, smr_val);

	/* Program S2CR */
	s2cr_val |= FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_TRANS);
	s2cr_val |= FIELD_PREP(ARM_SMMU_S2CR_CBNDX, guest_cb);
	smmuv2_s2cr_program(smmu, guest_smr, guest_cb, s2cr_val);

	kvm_iommu_unlock(iommu);

	return 0;
}

static int smmuv2_detach_dev(struct kvm_hyp_iommu *iommu,
			     struct kvm_hyp_iommu_domain *domain,
			     u32 sid, u32 pasid)
{
	int i;
	struct smmu_v2_nested *smmu = to_smmu_v2(iommu);
	struct smmu_nested_guest_domain *guest_domain = domain->priv;
	int guest_cb = guest_domain->cb_idx;
	u32 guest_smr = 0;
	bool sid_found = false, cb_in_use = false;

	/* Not one of our SMMUs - let the dispatcher try the next driver. */
	if (!smmuv2_owns_iommu(iommu))
		return -ENODEV;

	/*
	 * Same one-SMMU binding as attach. Detaching on the wrong instance
	 * would clear another domain's SMR/S2CR and free its CB.
	 */
	if (guest_domain->iommu && guest_domain->iommu != iommu)
		return -EINVAL;

	if (guest_cb < 0)
		return 0;

	kvm_iommu_lock(iommu);

	for (i = 0; i < smmu->num_guest_smr; i++) {
		if (smmu->guest_smr_pool[i] != guest_cb)
			continue;

		guest_smr = smmu->guest_smr_base + i;

		if ((smmu->smr_pool[guest_smr] & ARM_SMMU_SMR_ID)
			== (sid & ARM_SMMU_SMR_ID)) {
			sid_found = true;

			smmuv2_smr_program(smmu, guest_smr, 0);
			smmuv2_s2cr_program(smmu, guest_smr, guest_cb, 0);

			smmu->guest_smr_pool[i] = ARM_SMMU_GUEST_SMR_FREE;
		} else {
			cb_in_use = true;
		}
	}

	if (!cb_in_use) {
		/* Flush the CB's stale S2 entries before returning it to the pool. */
		arm_smmu_tlb_inv_vmid(smmu, domain->domain_id);
		smmu->guest_cb_pool[guest_cb - smmu->guest_cb_base] = ARM_SMMU_GUEST_CB_FREE;
		guest_domain->cb_idx = -1;
		guest_domain->iommu = NULL;
	}

	if (!sid_found)
		smmu_v2_debug_print("detach_dev: SID 0x%x not found in guest SMR pool\n", sid);

	kvm_iommu_unlock(iommu);

	return 0;
}

/* Initialization functions */
static int take_over_smmus(void)
{
	struct smmu_v2_nested *smmu;
	int ret;

	for_each_smmu(smmu) {
		ret = ___pkvm_host_donate_hyp(smmu->base_pa >> PAGE_SHIFT,
					      ARM_SMMU_GLOBAL_REGION2_OFFSET >> PAGE_SHIFT,
					      true);
		if (ret)
			return ret;

		smmu->base_va = (u64)hyp_phys_to_virt(smmu->base_pa);
		smmu_v2_debug_print("donate to hyp: base_pa: %llx, base_va: %llx, size: %llx\n",
				    smmu->base_pa, smmu->base_va, ARM_SMMU_GLOBAL_REGION2_OFFSET);
	}
	return 0;
}

static bool is_handoff_smr(struct smmu_v2_nested *smmu, u32 smr_val)
{
	u32 k;
	u32 smr_sid = smr_val & ARM_SMMU_SMR_ID;

	for (k = 0; k < smmu->num_handoff_smrs; k++) {
		if (smr_sid == (smmu->handoff_smrs[k] & ARM_SMMU_SMR_ID))
			return true;
	}
	return false;
}

static int update_s2cr_profile(struct smmu_v2_nested *smmu)
{
	int i, j;
	u32 s2cr_val;
	u32 smr_val;
	u32 s2cr_reset_val = 0;
	u32 s2cr_handoff_val = 0;

	s2cr_reset_val = FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_FAULT);

	/* Set PRIVCFG to PRIV, type to TRANS and CBNDX to host_s2_cb_idx */
	s2cr_handoff_val |= FIELD_PREP(ARM_SMMU_S2CR_PRIVCFG, S2CR_PRIVCFG_PRIV);
	s2cr_handoff_val |= FIELD_PREP(ARM_SMMU_S2CR_TYPE, S2CR_TYPE_TRANS);
	s2cr_handoff_val |= FIELD_PREP(ARM_SMMU_S2CR_CBNDX, smmu->host_s2_cb_idx);

	for (i = smmu->num_s2cr - 1; i >= 0; i--) {
		s2cr_val = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_S2CR(i));
		smr_val  = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(i));
		if (!(smr_val & ARM_SMMU_SMR_VALID))
			break;
		smmu_v2_debug_print("S2CR[%d]: smr_val:%llx s2cr_val: %llx\n",
				    i, smr_val, s2cr_val);
	}
	/* Reset remaining SMRs and S2CRs */
	for (j = i; j >= 0; j--) {
		smr_val  = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_SMR(j));

		if (is_handoff_smr(smmu, smr_val)) {
			s2cr_val = s2cr_handoff_val;
		} else {
			smr_val  = 0x0;
			s2cr_val = s2cr_reset_val;
		}

		arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_S2CR(j), s2cr_val);
		arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_SMR(j), smr_val);
	}
	smmu->num_s2cr = i + 1;
	smmu->num_smr = smmu->num_s2cr; /* smr == s2cr */
	WARN_ON(smmu->num_s2cr == 0);
	return 0;
}

static int update_cbar_profile(struct smmu_v2_nested *smmu)
{
	int i, j;
	u32 fault_val = FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S1_TRANS_S2_FAULT);

	/* Get available number of context banks from cbar register */
	for (i = smmu->num_cbar - 1; i >= 0; i--) {
		u32 cbar_val = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBAR(i));

		if (((cbar_val & ARM_SMMU_CBAR_TYPE) >> 16) ==
		    CBAR_TYPE_S1_TRANS_S2_FAULT) {
			break; /* Skip invalid entries */
		}
		smmu_v2_debug_print("CBAR[%d]: val: %llx\n",
				    i, cbar_val);
	}
	smmu->num_cbar = i + 1;
	smmu->num_cb = smmu->num_cbar; /* cbar == cb */

	/* Sanitize all admitted context banks to fault until configured by host */
	for (j = 0; j < smmu->num_cbar; j++) {
		smmu->cbar_pool[j] = fault_val;
		arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBAR(j), fault_val);
	}

	return 0;
}

static int reserve_host_s2_context_bank(struct smmu_v2_nested *smmu)
{
	/* Use the last available context bank for host S2 */
	smmu->host_s2_cb_idx = --smmu->num_cb;
	smmu->num_cbar = smmu->num_cb; /* Keep num_cbar in sync with num_cb */

	/* Read current CBAR value and update it for S2 translation */
	u32 cbar_val = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBAR(smmu->host_s2_cb_idx));
	/* Clear TYPE and VMID fields, then set new values */
	cbar_val &= ~(ARM_SMMU_CBAR_TYPE | ARM_SMMU_CBAR_VMID);
	cbar_val |= FIELD_PREP(ARM_SMMU_CBAR_TYPE, CBAR_TYPE_S2_TRANS) |
		    FIELD_PREP(ARM_SMMU_CBAR_VMID, HOST_S2_VMID);

	/* Write the modified CBAR value back to hardware */
	arm_smmu_gr1_write(smmu, ARM_SMMU_GR1_CBAR(smmu->host_s2_cb_idx), cbar_val);

	smmu_v2_debug_print("Reserved CB[%d] for host S2: CBAR: 0x%x Total CB: %d\n",
			    smmu->host_s2_cb_idx, cbar_val, smmu->num_cb);
	return 0;
}

static int reserve_guest_resources(struct smmu_v2_nested *smmu)
{
	if (smmu->num_cb < smmu->num_guest_cb ||
		smmu->num_smr < smmu->num_guest_smr) {
		smmu_v2_debug_print("Insufficient resources for guest: num_cb=%u num_smr=%u\n",
					smmu->num_cb, smmu->num_smr);
		return -ENODEV;
	}

	/*
	 * Carve out the tail of the host CB/SMR pools for guest pVM use
	 * Shrink host-visible counts accordingly so guest entries are
	 * never touched by the host allocation paths.
	 */
	smmu->guest_cb_base = smmu->num_cb - smmu->num_guest_cb;
	smmu->num_cb = smmu->guest_cb_base;
	smmu->num_cbar = smmu->num_cb;

	smmu->guest_smr_base = smmu->num_smr - smmu->num_guest_smr;
	smmu->num_smr = smmu->guest_smr_base;
	smmu->num_s2cr = smmu->num_smr;

	smmu_v2_debug_print("Guest resources reserved: %u CBs, %u SMRs\n",
			     smmu->num_guest_cb, smmu->num_guest_smr);

	return 0;
}

static int hw_profile_init(void)
{
	struct smmu_v2_nested *smmu;
	int ret;

	for_each_smmu(smmu) {
		u32 id0 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_ID0);
		u32 id1 = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_ID1);
		u32 scr_val = arm_smmu_gr0_read(smmu, ARM_SMMU_GR0_sCR0);

		smmu->total_num_smr = id0 & ARM_SMMU_ID0_NUMSMRG;
		smmu->num_smr = smmu->total_num_smr;
		smmu->num_s2cr = smmu->num_smr; /* smr == s2cr */
		smmu->num_cb = id1 & ARM_SMMU_ID1_NUMCB;
		smmu->num_cbar = smmu->num_cb; /* cbar == cb */
		/* ID1 */
		smmu->pgshift = (id1 & ARM_SMMU_ID1_PAGESIZE) ? 16 : 12;
		smmu->numpage = 1 << (FIELD_GET(ARM_SMMU_ID1_NUMPAGENDXB, id1) + 1);

		smmu_v2_debug_print("SMMU ID0: %llx, ID1: %llx, num_smr: %d\n",
				    id0, id1, smmu->num_smr);
		smmu_v2_debug_print("SMMU num_cb: %d, pgshift: %d, numpage: %d\n",
				    smmu->num_cb, smmu->pgshift, smmu->numpage);

		/*
		 * update_cbar_profile() and reserve_host_s2_context_bank() must
		 * run before update_s2cr_profile() so that host_s2_cb_idx is
		 * valid when handoff SMR S2CRs are converted from bypass to
		 * S2 translation mode.
		 */
		ret = update_cbar_profile(smmu);
		if (ret) {
			smmu_v2_debug_print("Failed to update CBAR profile!\n");
			return ret;
		}

		/* Reserve the bottom available context bank for host S2 */
		ret = reserve_host_s2_context_bank(smmu);
		if (ret) {
			smmu_v2_debug_print("Failed to reserve host S2 context bank: %d\n", ret);
			return ret;
		}

		ret = update_s2cr_profile(smmu);
		if (ret) {
			smmu_v2_debug_print("Failed to update S2CR profile!\n");
			return ret;
		}

		/* Reserve guest resources out of the profiled CB/SMR tails */
		ret = reserve_guest_resources(smmu);
		if (ret) {
			smmu_v2_debug_print("Failed to reserve guest resources: %d\n", ret);
			return ret;
		}

		smmu_v2_debug_print("num_smr: %d, num_cb: %d\n",
				    smmu->num_smr, smmu->num_cb);

		smmu_v2_debug_print("After reservation, available guests' CBs: %d & CBARs: %d\n",
				    smmu->num_cb, smmu->num_cbar);

		/*
		 * Enable SMMU by default, enable unidentified stream and
		 * stream match conflicts by default, and disallow EXIDENABLE
		 * and VMID16EN.
		 */
		scr_val &= ~(ARM_SMMU_sCR0_CLIENTPD | ARM_SMMU_sCR0_EXIDENABLE |
			     ARM_SMMU_sCR0_VMID16EN);
		scr_val |= FIELD_PREP(ARM_SMMU_sCR0_USFCFG, 1) |
			   FIELD_PREP(ARM_SMMU_sCR0_SMCFCFG, 1);
		arm_smmu_gr0_write(smmu, ARM_SMMU_GR0_sCR0, scr_val);
	}

	return 0;
}

const struct smmu_vendor_callbacks v2callbacks = {
	.tlb_ops = &arm_smmu_s2_tlb_ops_v2,
	.get_cfg = NULL, /* fix me */
	.post_init = smmu_attach_stage_2,
	.dabt_hdl = smmuv2_nesting_dabt_handler,
	.attach_dev = smmuv2_attach_dev,
	.detach_dev = smmuv2_detach_dev,
};

static struct smmu_vendor_driver smmuv2_driver = {
	.name = "smmuv2_nesting",
	.callbacks = &v2callbacks,
	.vdata = &data,
};

int smmuv2_hyp_nesting_init(void)
{
	int smmu_arr_size = PAGE_ALIGN(sizeof(*smmu_v2_nested_base) * smmu_v2_nested_count);
	struct smmu_v2_nested *smmu;
	int ret;

	/* Register this driver with the common vendor module */
	ret = smmu_vendor_register_driver(&smmuv2_driver);
	if (ret)
		return ret;

	smmu_v2_nested_base = kern_hyp_va(smmu_v2_nested_base);

	u32 page_count = smmu_arr_size >> PAGE_SHIFT;

	ret = ___pkvm_host_donate_hyp((hyp_virt_to_phys(smmu_v2_nested_base) >> PAGE_SHIFT),
				      page_count, false);
	if (ret)
		return ret;

	ret = take_over_smmus();
	if (ret)
		return ret;

	ret = hw_profile_init();
	if (ret)
		return ret;

	for_each_smmu(smmu) {
		u64 page_pa;

		page_pa = (u64)arm_smmu_page_pa(smmu, smmu->host_s2_cb_idx);

		/* Donate the S2 context bank page to hypervisor */
		ret = ___pkvm_host_donate_hyp(page_pa >> PAGE_SHIFT, 1, true);
		if (ret) {
			smmu_v2_debug_print("Failed to donate CB page: %d\n", ret);
			return ret;
		}
	}

	return ret;
}
