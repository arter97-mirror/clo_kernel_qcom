/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <asm/kvm_pkvm_module.h>

/**********************************
 * QCOM pKVM SMC filtering module
 */

/* From qcom_scm.h and qcom_scm-smc.c */
#define MAX_QCOM_SCM_ARGS	10
#define SCM_SMC_N_REG_ARGS	4
#define SCM_SMC_N_EXT_ARGS	(MAX_QCOM_SCM_ARGS - SCM_SMC_N_REG_ARGS + 1)
#define SCM_SMC_FIRST_REG_IDX	2
#define SCM_SMC_LAST_REG_IDX	(SCM_SMC_FIRST_REG_IDX + SCM_SMC_N_REG_ARGS - 1)

/* Input registers */
#define SMCCC_FUNC_ID_REG_IDX      0  /* Function ID is being passed in x0 */
#define SCM_SMC_ARG_INFO_REG_IDX   1  /* Argument info is being passed in x1 */

#define SCM_SMC_ARG_LEN_MASK	0xF  /* Argument count mask (x1 LSB-s) */

/* SCM extended arguments buffer fixed size, see __scm_smc_call() */
#define SCM_EXT_ARG_BUF_SIZE  ((u64)SCM_SMC_N_EXT_ARGS * sizeof(u64))

#define ARM_SMCCC_MBZ_MASK	0x7F
#define ARM_SMCCC_MBZ_SHIFT	17

#define ARM_SMCCC_MBZ(smc_val) \
	(((smc_val) >> ARM_SMCCC_MBZ_SHIFT) & ARM_SMCCC_MBZ_MASK)

/* Result registers */
#define SMCCC_EC_REG_IDX  0  /* Error code is being returned in x0 */

/* SCM Service and Command definitions */
/* SiP Service Calls */
#define QCOM_SCM_SVC_BOOT			0x01
#define QCOM_SCM_CONFIG_HW_FOR_RAM_DUMP_ID	0x09
#define QCOM_SCM_BOOT_SET_REMOTE_STATE		0x0A

#define QCOM_SCM_SVC_INFO			0x06
#define QCOM_SCM_INFO_IS_CALL_AVAIL		0x01
#define QCOM_SCM_INFO_GET_FEAT_VERSION_CMD	0x03
#define QCOM_SCM_INFO_GET_SECURE_STATE		0x04

#define QCOM_SCM_SVC_IO				0x05
#define QCOM_SCM_IO_READ			0x01
#define QCOM_SCM_IO_WRITE			0x02

#define QCOM_SCM_SVC_GIC			0x1D
#define QCOM_SCM_GIC_SET_CPUCLASS		0x02

#define QCOM_SCM_SVC_PWR			0x09
#define QCOM_SCM_PWR_IO_DISABLE_PMIC_ARBITER	0x01

/* OEM Services and Function IDs */
#define QCOM_SCM_SVC_OEM_POWER			0x09
#define QCOM_SCM_OEM_POWER_REBOOT		0x22

/* Trusted OS calls */
#define QCOM_SCM_SVC_QSEELOG			0x01
#define QCOM_SCM_QSEELOG_REGISTER		0x06
#define QCOM_SCM_REQUEST_ENCR_LOG_ID		0x0C
#define QCOM_SCM_QUERY_LOG_STATUS		0x0F
#define QCOM_SCM_QUERY_TZ_TIME_ID		0x11

#define QCOM_SCM_SVC_SMCINVOKE			0x06
#define QCOM_SCM_SMCINVOKE_INVOKE_FFA		0x08
#define QCOM_SCM_SMCINVOKE_CB_RSP_FFA		0x09

/* Helper macro to create function ID from service and command */
#define SCM_FNID(s, c)	((((s) & 0xFF) << 8) | ((c) & 0xFF))

/* Whitelisted SIP Function ID-s (svc/cmd combinations) */
#define SMC_SIP_CONFIG_HW_FOR_RAM_DUMP_ID \
	SCM_FNID(QCOM_SCM_SVC_BOOT, QCOM_SCM_CONFIG_HW_FOR_RAM_DUMP_ID)
#define SMC_SIP_SUBSYS_SET_STATE_ID \
	SCM_FNID(QCOM_SCM_SVC_BOOT, QCOM_SCM_BOOT_SET_REMOTE_STATE)
#define SMC_SIP_INFO_IS_CALL_AVAIL \
	SCM_FNID(QCOM_SCM_SVC_INFO, QCOM_SCM_INFO_IS_CALL_AVAIL)
#define SMC_SIP_INFO_GET_FEAT_VERSION \
	SCM_FNID(QCOM_SCM_SVC_INFO, QCOM_SCM_INFO_GET_FEAT_VERSION_CMD)
#define SMC_SIP_INFO_GET_SECURE_STATE \
	SCM_FNID(QCOM_SCM_SVC_INFO, QCOM_SCM_INFO_GET_SECURE_STATE)
#define SMC_SIP_IO_READ \
	SCM_FNID(QCOM_SCM_SVC_IO, QCOM_SCM_IO_READ)
#define SMC_SIP_IO_WRITE \
	SCM_FNID(QCOM_SCM_SVC_IO, QCOM_SCM_IO_WRITE)
#define SMC_SIP_GIC_SET_CPUCLASS \
	SCM_FNID(QCOM_SCM_SVC_GIC, QCOM_SCM_GIC_SET_CPUCLASS)
#define SMC_SIP_PWR_IO_DISABLE_PMIC_ARBITER \
	SCM_FNID(QCOM_SCM_SVC_PWR, QCOM_SCM_PWR_IO_DISABLE_PMIC_ARBITER)

/* Whitelisted OEM Function ID-s (svc/cmd combinations) */
#define SMC_OEM_HW_POWER_REBOOT \
	SCM_FNID(QCOM_SCM_SVC_OEM_POWER, QCOM_SCM_OEM_POWER_REBOOT)

/* Whitelisted Trusted OS Function ID-s (svc/cmd combinations) */
#define SMC_TOS_QSEELOG_REGISTER \
	SCM_FNID(QCOM_SCM_SVC_QSEELOG, QCOM_SCM_QSEELOG_REGISTER)
#define SMC_TOS_REQUEST_ENCR_LOG \
	SCM_FNID(QCOM_SCM_SVC_QSEELOG, QCOM_SCM_REQUEST_ENCR_LOG_ID)
#define SMC_TOS_QUERY_LOG_STATUS \
	SCM_FNID(QCOM_SCM_SVC_QSEELOG, QCOM_SCM_QUERY_LOG_STATUS)
#define SMC_TOS_QUERY_TZ_TIME \
	SCM_FNID(QCOM_SCM_SVC_QSEELOG, QCOM_SCM_QUERY_TZ_TIME_ID)
#define SMC_TOS_SMCINVOKE_INVOKE_FFA \
	SCM_FNID(QCOM_SCM_SVC_SMCINVOKE, QCOM_SCM_SMCINVOKE_INVOKE_FFA)
#define SMC_TOS_SMCINVOKE_CB_RSP_FFA \
	SCM_FNID(QCOM_SCM_SVC_SMCINVOKE, QCOM_SCM_SMCINVOKE_CB_RSP_FFA)

enum pfn_err_type {
	PFN_NO_ERR = 0,
	PFN_SHARE_ERR,
	PFN_PIN_ERR
};

void __forward_smc(struct user_pt_regs *regs);
