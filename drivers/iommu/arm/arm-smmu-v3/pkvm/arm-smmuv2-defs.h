/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * IOMMU API for ARM architected SMMU implementations.
 */

#ifndef _ARM_SMMUV2_DEFS_H
#define _ARM_SMMUV2_DEFS_H

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/mutex.h>
#include <linux/mutex.h>
#include <linux/types.h>
/* Global region offsets */
#define ARM_SMMU_GLOBAL_REGION0_OFFSET 0x0U // global 0
#define ARM_SMMU_GLOBAL_REGION1_OFFSET 0x1000U // global 1
#define ARM_SMMU_GLOBAL_REGION2_OFFSET 0x2000U // impl def 0
#define ARM_SMMU_GLOBAL_REGION3_OFFSET 0x3000U // perf monitor
#define ARM_SMMU_GLOBAL_REGION4_OFFSET 0x4000U // ssd
#define ARM_SMMU_GLOBAL_REGION5_OFFSET 0x6000U // impl def 1
#define ARM_SMMU_GLOBAL_REGION6_OFFSET 0x7000U // impl def 2

/* Configuration registers */
#define ARM_SMMU_GR0_sCR0 0x0
#define ARM_SMMU_sCR0_VMID16EN BIT(31)
#define ARM_SMMU_sCR0_SMCFCFG BIT(21)
#define ARM_SMMU_sCR0_BSU GENMASK(15, 14)
#define ARM_SMMU_sCR0_FB BIT(13)
#define ARM_SMMU_sCR0_PTM BIT(12)
#define ARM_SMMU_sCR0_VMIDPNE BIT(11)
#define ARM_SMMU_sCR0_USFCFG BIT(10)
#define ARM_SMMU_sCR0_GCFGFIE BIT(5)
#define ARM_SMMU_sCR0_GCFGFRE BIT(4)
#define ARM_SMMU_sCR0_EXIDENABLE BIT(3)
#define ARM_SMMU_sCR0_GFIE BIT(2)
#define ARM_SMMU_sCR0_GFRE BIT(1)
#define ARM_SMMU_sCR0_CLIENTPD BIT(0)

/* Auxiliary Configuration register */
#define ARM_SMMU_GR0_sACR 0x10

/* Identification registers */
#define ARM_SMMU_GR0_ID0 0x20
#define ARM_SMMU_ID0_S1TS BIT(30)
#define ARM_SMMU_ID0_S2TS BIT(29)
#define ARM_SMMU_ID0_NTS BIT(28)
#define ARM_SMMU_ID0_SMS BIT(27)
#define ARM_SMMU_ID0_ATOSNS BIT(26)
#define ARM_SMMU_ID0_PTFS_NO_AARCH32 BIT(25)
#define ARM_SMMU_ID0_PTFS_NO_AARCH32S BIT(24)
#define ARM_SMMU_ID0_NUMIRPT GENMASK(23, 16)
#define ARM_SMMU_ID0_CTTW BIT(14)
#define ARM_SMMU_ID0_NUMSIDB GENMASK(12, 9)
#define ARM_SMMU_ID0_EXIDS BIT(8)
#define ARM_SMMU_ID0_NUMSMRG GENMASK(7, 0)

#define ARM_SMMU_GR0_ID1 0x24
#define ARM_SMMU_ID1_PAGESIZE BIT(31)
#define ARM_SMMU_ID1_NUMPAGENDXB GENMASK(30, 28)
#define ARM_SMMU_ID1_NUMS2CB GENMASK(23, 16)
#define ARM_SMMU_ID1_NUMCB GENMASK(7, 0)

#define ARM_SMMU_GR0_ID2 0x28
#define ARM_SMMU_ID2_VMID16 BIT(15)
#define ARM_SMMU_ID2_PTFS_64K BIT(14)
#define ARM_SMMU_ID2_PTFS_16K BIT(13)
#define ARM_SMMU_ID2_PTFS_4K BIT(12)
#define ARM_SMMU_ID2_UBS GENMASK(11, 8)
#define ARM_SMMU_ID2_OAS GENMASK(7, 4)
#define ARM_SMMU_ID2_IAS GENMASK(3, 0)

#define ARM_SMMU_GR0_ID3 0x2c
#define ARM_SMMU_GR0_ID4 0x30
#define ARM_SMMU_GR0_ID5 0x34
#define ARM_SMMU_GR0_ID6 0x38

#define ARM_SMMU_GR0_ID7 0x3c
#define ARM_SMMU_ID7_MAJOR GENMASK(7, 4)
#define ARM_SMMU_ID7_MINOR GENMASK(3, 0)
#define ARM_SMMU_GFAR0 0x40
#define ARM_SMMU_GFSR 0x44
#define ARM_SMMU_GR0_sGFSR 0x48
#define ARM_SMMU_sGFSR_USF BIT(1)

#define ARM_SMMU_GR0_sGFSYNR0 0x50
#define ARM_SMMU_GR0_sGFSYNR1 0x54
#define ARM_SMMU_GR0_sGFSYNR2 0x58

/* Global TLB invalidation */
#define ARM_SMMU_GR0_TLBIVMID 0x64
#define ARM_SMMU_GR0_TLBIALLNSNH 0x68
#define ARM_SMMU_GR0_TLBIALLH 0x6c
#define ARM_SMMU_GR0_sTLBGSYNC 0x70

#define ARM_SMMU_GR0_sTLBGSTATUS 0x74
#define ARM_SMMU_sTLBGSTATUS_GSACTIVE BIT(0)
#define ARM_SMMU_GR0_TLBIVMIDS1 0xb8

#define ARM_SMMU_TRANSBUF_READSW 0x80U
#define ARM_SMMU_TRANSBUF_DR2 0x9CU

#define ARM_SMMU_GPAR0 0x180U
#define ARM_SMMU_GPAR1 0x184U
#define ARM_SMMU_NSGFSYNR0 0x450U
#define ARM_SMMU_NSGFSYNR1 0x454
#define ARM_SMMU_NSGFSYNR2 0x458

/* Stream mapping registers */
#define ARM_SMMU_GR0_SMR(n) (0x800 + ((n) << 2))
#define ARM_SMMU_SMR_VALID BIT(31)
#define ARM_SMMU_SMR_MASK GENMASK(31, 16)
#define ARM_SMMU_SMR_ID GENMASK(15, 0)

/* Inverse helpers: get SMR index from a GR0 offset */
#define ARM_SMMU_GR0_SMR_FIRST        ARM_SMMU_GR0_SMR(0)
#define ARM_SMMU_GR0_SMR_INDEX(off)   (((off) - ARM_SMMU_GR0_SMR_FIRST) >> 2)
/* Validate an SMR offset given runtime max (num_smr) */
#define ARM_SMMU_GR0_SMR_VALID(off, max) \
	((off) >= ARM_SMMU_GR0_SMR_FIRST && \
	 ((off) - ARM_SMMU_GR0_SMR_FIRST) < ((max) << 2) && \
	 !((off) & 0x3))

#define ARM_SMMU_GR0_S2CR(n) (0xc00 + ((n) << 2))
#define ARM_SMMU_S2CR_PRIVCFG GENMASK(25, 24)

#define QCOM_DUMMY_VAL	-1

enum arm_smmu_s2cr_privcfg {
	S2CR_PRIVCFG_DEFAULT,
	S2CR_PRIVCFG_DIPAN,
	S2CR_PRIVCFG_UNPRIV,
	S2CR_PRIVCFG_PRIV,
};

#define ARM_SMMU_S2CR_TYPE GENMASK(17, 16)

enum arm_smmu_s2cr_type {
	S2CR_TYPE_TRANS,
	S2CR_TYPE_BYPASS,
	S2CR_TYPE_FAULT,
};

#define ARM_SMMU_S2CR_EXIDVALID BIT(10)
#define ARM_SMMU_S2CR_CBNDX GENMASK(7, 0)

/* Inverse helpers: get S2CR index from a GR0 offset */
#define ARM_SMMU_GR0_S2CR_FIRST        ARM_SMMU_GR0_S2CR(0)
#define ARM_SMMU_GR0_S2CR_INDEX(off)   (((off) - ARM_SMMU_GR0_S2CR_FIRST) >> 2)
/* Validate an S2CR offset given runtime max (num_s2cr) */
#define ARM_SMMU_GR0_S2CR_VALID(off, max) \
	((off) >= ARM_SMMU_GR0_S2CR_FIRST && \
	 ((off) - ARM_SMMU_GR0_S2CR_FIRST) < ((max) << 2) && \
	 !((off) & 0x3))

/* Context bank attribute registers */
#define ARM_SMMU_GR1_CBAR(n) (0x0 + ((n) << 2))
#define ARM_SMMU_CBAR_IRPTNDX GENMASK(31, 24)
#define ARM_SMMU_CBAR_TYPE GENMASK(17, 16)

enum arm_smmu_cbar_type {
	CBAR_TYPE_S2_TRANS,
	CBAR_TYPE_S1_TRANS_S2_BYPASS,
	CBAR_TYPE_S1_TRANS_S2_FAULT,
	CBAR_TYPE_S1_TRANS_S2_TRANS,
};

#define ARM_SMMU_CBAR_CBNDX GENMASK(15, 8)
#define ARM_SMMU_CBAR_S1_MEMATTR GENMASK(15, 12)
#define ARM_SMMU_CBAR_S1_MEMATTR_WB 0xf
#define ARM_SMMU_CBAR_S1_BPSHCFG GENMASK(9, 8)
#define ARM_SMMU_CBAR_S1_BPSHCFG_NSH 3
#define ARM_SMMU_CBAR_VMID GENMASK(7, 0)

/* Inverse helpers: get CBAR index from a GR1 offset */
#define ARM_SMMU_GR1_CBAR_FIRST        ARM_SMMU_GR1_CBAR(0)
#define ARM_SMMU_GR1_CBAR_INDEX(off)   (((off) - ARM_SMMU_GR1_CBAR_FIRST) >> 2)
/* Validate a CBAR offset given runtime max (num_cbar) */
#define ARM_SMMU_GR1_CBAR_VALID(off, max) \
	((off) >= ARM_SMMU_GR1_CBAR_FIRST && \
	 ((off) - ARM_SMMU_GR1_CBAR_FIRST) < ((max) << 2) && \
	 !((off) & 0x3))

#define ARM_SMMU_GR1_CBFRSYNRA(n) (0x400 + ((n) << 2))
#define ARM_SMMU_CBFRSYNRA_SID GENMASK(15, 0)

#define ARM_SMMU_GR1_CBA2R(n) (0x800 + ((n) << 2))
#define ARM_SMMU_CBA2R_VMID16 GENMASK(31, 16)
#define ARM_SMMU_CBA2R_VA64 BIT(0)

/* Inverse helpers: get CBA2R index from a GR1 offset */
#define ARM_SMMU_GR1_CBA2R_FIRST       ARM_SMMU_GR1_CBA2R(0)
#define ARM_SMMU_GR1_CBA2R_INDEX(off)  (((off) - ARM_SMMU_GR1_CBA2R_FIRST) >> 2)
/* Validate a CBA2R offset given runtime max (num_cbar) */
#define ARM_SMMU_GR1_CBA2R_VALID(off, max) \
	((off) >= ARM_SMMU_GR1_CBA2R_FIRST && \
	 ((off) - ARM_SMMU_GR1_CBA2R_FIRST) < ((max) << 2) && \
	 !((off) & 0x3))

#define ARM_SMMU_CB_SCTLR 0x0
#define ARM_SMMU_SCTLR_S1_ASIDPNE BIT(12)
#define ARM_SMMU_SCTLR_CFCFG BIT(7)
#define ARM_SMMU_SCTLR_HUPCF BIT(8)
#define ARM_SMMU_SCTLR_CFIE BIT(6)
#define ARM_SMMU_SCTLR_CFRE BIT(5)
#define ARM_SMMU_SCTLR_E BIT(4)
#define ARM_SMMU_SCTLR_AFE BIT(2)
#define ARM_SMMU_SCTLR_TRE BIT(1)
#define ARM_SMMU_SCTLR_M BIT(0)

#define ARM_SMMU_CB_ACTLR 0x4

#define ARM_SMMU_CB_RESUME 0x8
#define ARM_SMMU_RESUME_TERMINATE BIT(0)

#define ARM_SMMU_CB_TCR2 0x10
#define ARM_SMMU_TCR2_SEP GENMASK(17, 15)
#define ARM_SMMU_TCR2_SEP_UPSTREAM 0x7
#define ARM_SMMU_TCR2_AS BIT(4)
#define ARM_SMMU_TCR2_PASIZE GENMASK(3, 0)

#define ARM_SMMU_CB_TTBR0 0x20
#define ARM_SMMU_CB_TTBR1 0x28
#define ARM_SMMU_TTBRn_ASID GENMASK_ULL(63, 48)

#define ARM_SMMU_CB_TCR 0x30
#define ARM_SMMU_TCR_EAE BIT(31)
#define ARM_SMMU_TCR_EPD1 BIT(23)
#define ARM_SMMU_TCR_A1 BIT(22)
#define ARM_SMMU_TCR_TG0 GENMASK(15, 14)
#define ARM_SMMU_TCR_SH0 GENMASK(13, 12)
#define ARM_SMMU_TCR_ORGN0 GENMASK(11, 10)
#define ARM_SMMU_TCR_IRGN0 GENMASK(9, 8)
#define ARM_SMMU_TCR_EPD0 BIT(7)
#define ARM_SMMU_TCR_T0SZ GENMASK(5, 0)

#define ARM_SMMU_VTCR_RES1 BIT(31)
#define ARM_SMMU_VTCR_PS GENMASK(18, 16)
#define ARM_SMMU_VTCR_TG0 ARM_SMMU_TCR_TG0
#define ARM_SMMU_VTCR_SH0 ARM_SMMU_TCR_SH0
#define ARM_SMMU_VTCR_ORGN0 ARM_SMMU_TCR_ORGN0
#define ARM_SMMU_VTCR_IRGN0 ARM_SMMU_TCR_IRGN0
#define ARM_SMMU_VTCR_SL0 GENMASK(7, 6)
#define ARM_SMMU_VTCR_T0SZ ARM_SMMU_TCR_T0SZ

#define ARM_SMMU_CB_CONTEXTIDR 0x34
#define ARM_SMMU_CB_S1_MAIR0 0x38
#define ARM_SMMU_CB_S1_MAIR1 0x3c

#define ARM_SMMU_CB_PAR 0x50
#define ARM_SMMU_CB_PAR_F BIT(0)

#define ARM_SMMU_CB_FSR 0x58
#define ARM_SMMU_CB_FSR_MULTI BIT(31)
#define ARM_SMMU_CB_FSR_SS BIT(30)
#define ARM_SMMU_CB_FSR_FORMAT GENMASK(10, 9)
#define ARM_SMMU_CB_FSR_UUT BIT(8)
#define ARM_SMMU_CB_FSR_ASF BIT(7)
#define ARM_SMMU_CB_FSR_TLBLKF BIT(6)
#define ARM_SMMU_CB_FSR_TLBMCF BIT(5)
#define ARM_SMMU_CB_FSR_EF BIT(4)
#define ARM_SMMU_CB_FSR_PF BIT(3)
#define ARM_SMMU_CB_FSR_AFF BIT(2)
#define ARM_SMMU_CB_FSR_TF BIT(1)

#define ARM_SMMU_CB_FSR_IGN                                                   \
	(ARM_SMMU_CB_FSR_AFF | ARM_SMMU_CB_FSR_ASF | ARM_SMMU_CB_FSR_TLBMCF | \
	 ARM_SMMU_CB_FSR_TLBLKF)

#define ARM_SMMU_CB_FSR_FAULT                                               \
	(ARM_SMMU_CB_FSR_MULTI | ARM_SMMU_CB_FSR_SS | ARM_SMMU_CB_FSR_UUT | \
	 ARM_SMMU_CB_FSR_EF | ARM_SMMU_CB_FSR_PF | ARM_SMMU_CB_FSR_TF |     \
	 ARM_SMMU_CB_FSR_IGN)

#define ARM_SMMU_CB_FAR 0x60

#define ARM_SMMU_CB_IPAFAR 0x70

#define ARM_SMMU_CB_FSYNR0 0x68
#define ARM_SMMU_CB_FSYNR0_PLVL GENMASK(1, 0)
#define ARM_SMMU_CB_FSYNR0_WNR BIT(4)
#define ARM_SMMU_CB_FSYNR0_PNU BIT(5)
#define ARM_SMMU_CB_FSYNR0_IND BIT(6)
#define ARM_SMMU_CB_FSYNR0_NSATTR BIT(8)
#define ARM_SMMU_CB_FSYNR0_PTWF BIT(10)
#define ARM_SMMU_CB_FSYNR0_AFR BIT(11)
#define ARM_SMMU_CB_FSYNR0_S1CBNDX GENMASK(23, 16)

#define ARM_SMMU_CB_FSYNR1 0x6c

#define ARM_SMMU_CB_S1_TLBIVA 0x600
#define ARM_SMMU_CB_S1_TLBIASID 0x610
#define ARM_SMMU_CB_S1_TLBIVAL 0x620
#define ARM_SMMU_CB_S2_TLBIIPAS2 0x630
#define ARM_SMMU_CB_S2_TLBIIPAS2L 0x638
#define ARM_SMMU_CB_TLBSYNC 0x7f0
#define ARM_SMMU_CB_TLBSTATUS 0x7f4
#define ARM_SMMU_CB_ATS1PR 0x800

#define ARM_SMMU_CB_ATSR 0x8f0
#define ARM_SMMU_CB_ATSR_ACTIVE BIT(0)

#define ARM_SMMU_RESUME_TERMINATE BIT(0)

#define TLB_LOOP_TIMEOUT 1000000 /* 1s! */
#define TLB_SPIN_COUNT   10
#define TLB_LOOP_INC_MAX 1000    /* 1ms */

/* Helper Function definitions */
struct smmu_v2_nested; /* forward */
#define ARM_SMMU_CB(s, n) ((s)->numpage + (n))

static inline void __iomem *arm_smmu_page_pa(struct smmu_v2_nested *s, int n)
{
	return (void __iomem *)(s->base_pa + (ARM_SMMU_CB(s, n) << s->pgshift));
}

static inline void __iomem *arm_smmu_page(struct smmu_v2_nested *s, int n)
{
	return (void __iomem *)s->base_va + (n << s->pgshift);
}

static inline u32 arm_smmu_readl(struct smmu_v2_nested *s, int page, int offset)
{
	return readl_relaxed(arm_smmu_page(s, page) + offset);
}

static inline void arm_smmu_writel(struct smmu_v2_nested *s, int page, int offset, u32 val)
{
	writel_relaxed(val, arm_smmu_page(s, page) + offset);
}

static inline u64 arm_smmu_readq(struct smmu_v2_nested *s, int page, int offset)
{
	return readq_relaxed(arm_smmu_page(s, page) + offset);
}

static inline void arm_smmu_writeq(struct smmu_v2_nested *s, int page, int offset, u64 val)
{
	writeq_relaxed(val, arm_smmu_page(s, page) + offset);
}

#define ARM_SMMU_GR0 0
#define ARM_SMMU_GR1 1

#define arm_smmu_gr0_read(s, o) arm_smmu_readl((s), ARM_SMMU_GR0, (o))
#define arm_smmu_gr0_write(s, o, v) arm_smmu_writel((s), ARM_SMMU_GR0, (o), (v))

#define arm_smmu_gr1_read(s, o) arm_smmu_readl((s), ARM_SMMU_GR1, (o))
#define arm_smmu_gr1_write(s, o, v) arm_smmu_writel((s), ARM_SMMU_GR1, (o), (v))

#define arm_smmu_cb_read(s, n, o) \
	arm_smmu_readl((s), ARM_SMMU_CB((s), (n)), (o))
#define arm_smmu_cb_write(s, n, o, v) \
	arm_smmu_writel((s), ARM_SMMU_CB((s), (n)), (o), (v))
#define arm_smmu_cb_readq(s, n, o) \
	arm_smmu_readq((s), ARM_SMMU_CB((s), (n)), (o))
#define arm_smmu_cb_writeq(s, n, o, v) \
	arm_smmu_writeq((s), ARM_SMMU_CB((s), (n)), (o), (v))

struct arm_smmu_device {};

/*
 ** Describes resources required for on/off power operation.
 ** Separate reference count is provided for atomic/nonatomic
 ** operations.
 ** gdscs - on kernel 6.6, power domains are used instead. This
 ** field can be removed once no legacy targets using it remain.
 **/
struct arm_smmu_power_resources {
	struct device	*dev;
	struct clk	**clocks;
	int		num_clocks;
	struct regulator_bulk_data	*gdscs;
	int				num_gdscs;
	struct icc_path			*icc_path;
	/* Protects power_count */
	struct mutex			power_lock;
	int				power_count;
	int (*resume)(struct arm_smmu_power_resources *pwr);
	void (*suspend)(struct arm_smmu_power_resources *pwr);
};

int arm_smmu_power_on(struct arm_smmu_power_resources *pwr);
void arm_smmu_power_off(struct arm_smmu_device *smmu,
			struct arm_smmu_power_resources *pwr);
struct arm_smmu_power_resources *arm_smmu_init_power_resources(struct device *dev);
#endif /* _ARM_SMMUV2_DEFS_H */
