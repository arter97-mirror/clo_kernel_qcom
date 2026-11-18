// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include "smc-filter-hyp.h"
#include <asm/kvm_pkvm_module.h>
#include <asm/kvm_arm.h>

/****************************************
 * QCOM pKVM SMC filter module - EL2
 */

static const struct pkvm_module_ops *m_ops;

/*
 * Handling extended arguments buffer:
 *  - Check that it is within the host's address space;
 *  - Share the host's pages with the hypervisor;
 *  - Pin the pages into the host's memory;
 *  - Return error if any of above actions has failed;
 *  - Forward the SMC to TZ manually;
 *  - Unwind the sharing and pinning actions above.
 */
static inline bool pin_shared_mem(u64 pfn)
{
	return m_ops->pin_shared_mem(m_ops->hyp_va(__pfn_to_phys(pfn)),
				m_ops->hyp_va(__pfn_to_phys(pfn + 1)));
}

static inline void unpin_shared_mem(u64 pfn)
{
	return m_ops->unpin_shared_mem(m_ops->hyp_va(__pfn_to_phys(pfn)),
				m_ops->hyp_va(__pfn_to_phys(pfn + 1)));
}

static bool forward_smc_ext_args(phys_addr_t buf, struct user_pt_regs *regs)
{
	const u64 pfn_start = __phys_to_pfn(buf);
	const u64 pfn_end = __phys_to_pfn(buf + SCM_EXT_ARG_BUF_SIZE - 1);
	enum pfn_err_type error = PFN_NO_ERR;
	u64 pfn;

	/* Check that the buffer spans across at most two pages */
	static_assert(SCM_EXT_ARG_BUF_SIZE <= PAGE_SIZE,
		      "Extended arguments buffer too long");
	if (pfn_start > pfn_end)
		return true;	/* address space wrap-around - error */

	for (pfn = pfn_start; pfn <= pfn_end; pfn++) {
		/* Share the host page with the hypervisor */
		if (m_ops->host_share_hyp(pfn))
			error = PFN_SHARE_ERR;

		/* Pin the host page */
		if (!error && pin_shared_mem(pfn))
			error = PFN_PIN_ERR;

		/* Unwind if any failure */
		if (error) {
			if (pfn != pfn_start) {
				/* unwind for the previous pfn */
				unpin_shared_mem(pfn_start);
				WARN_ON(m_ops->host_unshare_hyp(pfn_start));
			}
			if (error == PFN_PIN_ERR) {
				/* unwind for this pfn */
				WARN_ON(m_ops->host_unshare_hyp(pfn));
			}
			return true;  /* error */
		}
	}

	/* Forward the SMC to TZ */
	/* TODO: use arm_smccc_1_2_smc() module API once available upstream */
	__forward_smc(regs);

	/* Undo the memory sharing and pinning */
	for (pfn = pfn_start; pfn <= pfn_end; pfn++) {
		unpin_shared_mem(pfn);
		WARN_ON(m_ops->host_unshare_hyp(pfn));
	}

	return false;
}

/*
 * Helper function to handle extended ABI processing for all owners.
 * Returns true if the call was handled (either successfully or with error),
 *  false if the call should be forwarded to TZ via standard path.
 */
static bool handle_scm_extended_abi(struct user_pt_regs *regs)
{
	const int arglen =
		regs->regs[SCM_SMC_ARG_INFO_REG_IDX] & SCM_SMC_ARG_LEN_MASK;

	if (arglen > SCM_SMC_N_REG_ARGS) {
		/*
		 * Using a parameter buffer (extended ABI) - perform the
		 * necessary checks and if those pass, manually forward the SMC
		 * to the TZ, as unwind actions are needed after the SMC return.
		 */
		const phys_addr_t ext_arg_buf = regs->regs[SCM_SMC_LAST_REG_IDX];

		if (forward_smc_ext_args(ext_arg_buf, regs)) {
			m_ops->puts("[pKVM EL2] ERROR: qcom SMC filter Extended ABI, ID:");
			m_ops->putx64(regs->regs[SMCCC_FUNC_ID_REG_IDX]);
			regs->regs[SMCCC_EC_REG_IDX] = SMCCC_RET_INVALID_PARAMETER;
		}
		return true;  /* handled */
	}

	return false;  /* not using extended ABI */
}

/*
 * Handler for ARM_SMCCC_OWNER_SIP calls.
 * If this returns false, call may be forwarded to TZ.
 */
static bool smc_filter_host_sip_handler(u32 func_id)
{
	const u16 svc_cmd = ARM_SMCCC_FUNC_NUM(func_id);
	bool blocked = true;

	/*
	 * Check which SIP calls are permitted from the host.
	 * Only allow specific svc/cmd combinations.
	 */
	switch (svc_cmd) {
	case SMC_SIP_CONFIG_HW_FOR_RAM_DUMP_ID:
	case SMC_SIP_SUBSYS_SET_STATE_ID:
	case SMC_SIP_INFO_IS_CALL_AVAIL:
	case SMC_SIP_INFO_GET_FEAT_VERSION:
	case SMC_SIP_INFO_GET_SECURE_STATE:
	case SMC_SIP_IO_READ:
	case SMC_SIP_IO_WRITE:
	case SMC_SIP_GIC_SET_CPUCLASS:
	case SMC_SIP_PWR_IO_DISABLE_PMIC_ARBITER:
		blocked = false;
		break;
	default:
		break;
	}

	return blocked;
}

/*
 * Handler for ARM_SMCCC_OWNER_OEM calls.
 * If this returns false, call may be forwarded to TZ.
 */
static bool smc_filter_host_oem_handler(u32 func_id)
{
	const u16 svc_cmd = ARM_SMCCC_FUNC_NUM(func_id);
	bool blocked = true;

	/*
	 * Check which OEM calls are permitted from the host.
	 * Only allow specific svc/cmd combinations.
	 */
	switch (svc_cmd) {
	case SMC_OEM_HW_POWER_REBOOT:
		blocked = false;
		break;
	default:
		break;
	}

	return blocked;
}

/*
 * Handler for ARM_SMCCC_OWNER_TRUSTED_OS calls.
 * If this returns false, call may be forwarded to TZ.
 */
static bool smc_filter_host_trusted_os_handler(u32 func_id)
{
	const u16 svc_cmd = ARM_SMCCC_FUNC_NUM(func_id);
	const bool is_fast = ARM_SMCCC_IS_FAST_CALL(func_id);
	const bool is_64 = ARM_SMCCC_IS_64(func_id);
	bool blocked = true;

	/*
	 * Check which Trusted OS calls are permitted from the host.
	 * Only allow specific svc/cmd combinations.
	 */
	switch (svc_cmd) {
	case SMC_TOS_QSEELOG_REGISTER:
	case SMC_TOS_REQUEST_ENCR_LOG:
	case SMC_TOS_QUERY_LOG_STATUS:
	case SMC_TOS_QUERY_TZ_TIME:
		blocked = false;
		break;
	case SMC_TOS_SMCINVOKE_INVOKE_FFA:
	case SMC_TOS_SMCINVOKE_CB_RSP_FFA:
		blocked = is_fast || !is_64;
		break;
	default:
		break;
	}

	return blocked;
}

/*
 * SMC call interception.
 * If this returns false, call may be forwarded to TZ.
 */
bool smc_filter_host_handler(struct user_pt_regs *regs)
{
	const u32 func_id = (u32)regs->regs[SMCCC_FUNC_ID_REG_IDX];
	const u32 owner = ARM_SMCCC_OWNER_NUM(func_id);
	bool blocked = true;
	bool is_scm = false;

	/* MBZ bits must be zero */
	if (ARM_SMCCC_MBZ(func_id) != 0U)
		goto terminate_smc;

	switch (owner) {
	case ARM_SMCCC_OWNER_ARCH:
		blocked = false;
		break;
	case ARM_SMCCC_OWNER_SIP:
		blocked = smc_filter_host_sip_handler(func_id);
		is_scm = true;
		break;
	case ARM_SMCCC_OWNER_OEM:
		blocked = smc_filter_host_oem_handler(func_id);
		is_scm = true;
		break;
	case ARM_SMCCC_OWNER_STANDARD:
		blocked = !ARM_SMCCC_IS_FAST_CALL(func_id);
		break;
	case ARM_SMCCC_OWNER_TRUSTED_OS:
		blocked = smc_filter_host_trusted_os_handler(func_id);
		is_scm = true;
		break;
	default:
		/* Unhandled Owner ID */
		break;
	}

	if (blocked)
		goto terminate_smc;

	if (is_scm)
		return handle_scm_extended_abi(regs);

	return false;

terminate_smc:
	m_ops->puts("[pKVM EL2] WARNING: qcom SMC filter blocking Function ID:");
	m_ops->putx64(func_id);
	regs->regs[SMCCC_EC_REG_IDX] = SMCCC_RET_NOT_SUPPORTED;
	return true;
}

/*
 * Module init
 */
int smc_filter_hyp_init(const struct pkvm_module_ops *ops)
{
	int ret;

	m_ops = ops;

	ret = ops->register_host_smc_handler(smc_filter_host_handler);
	if (ret) {
		ops->puts("[pKVM EL2] ERROR: qcom SMC filter registration failed");
		return ret;
	}

	ops->puts("[pKVM EL2] qcom SMC filter module loaded");
	return 0;
}
