# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)

# Subdirectories
obj-y += dcvs/
obj-y += hab/
obj-y += mem_buf/
obj-y += memshare/
obj-y += mpam/
obj-y += qpace/
obj-y += sps/

obj-$(CONFIG_MEM_PROT) += mem-prot.o

obj-$(CONFIG_QCOM_CX_IPEAK) += cx-ipeak.o
cx-ipeak-objs += cx_ipeak.o

obj-$(CONFIG_QCOM_ADSP_SLEEPMON) += adsp_sleepmon.o

obj-$(CONFIG_QTI_ALTMODE_GLINK) += altmode-glink.o

obj-$(CONFIG_MSM_BOOT_STATS) += boot_stats.o

obj-$(CONFIG_QCOM_CDSP_RM) += cdsprm.o

obj-$(CONFIG_QTI_CHARGER_ULOG_GLINK) += charger-ulog-glink.o

obj-$(CONFIG_QCOM_COMMAND_DB) += cmd-db.o

obj-$(CONFIG_QTI_CPUCP_LOG) += cpucp_log.o

obj-$(CONFIG_QCOM_CPUSS_TELEMETRY) += cpuss_telemetry.o

obj-$(CONFIG_QCOM_CPU_PHYS_LOG_MAP) += cpu_phys_log_map.o

obj-$(CONFIG_QCOM_CRM_V2) += crm-v2.o

obj-$(CONFIG_QCOM_DCC_V2) += dcc_v2.o

obj-$(CONFIG_QCOM_DEBUG_SYMBOL) += debug_symbol.o

obj-$(CONFIG_QCOM_VM_DMESG_DUMPER) += dmesg_dumper.o
ifdef CONFIG_QCOM_VM_ALIVE_LOG_ENCRYPT
dmesg_dumper-y += dmesg_dumper_crypto.o
endif

obj-$(CONFIG_QCOM_EUD) += eud.o

obj-$(CONFIG_QCOM_FDCVS) += qcom_fdcvs.o

obj-$(CONFIG_GH_TLMM_VM_MEM_ACCESS) += gh_tlmm_vm_mem_access.o

obj-$(CONFIG_GIC_INTERRUPT_ROUTING) += gic_intr_routing.o

obj-$(CONFIG_QCOM_GLINK) += glink_probe.o

obj-$(CONFIG_QCOM_HUNG_TASK_ENH) += hung_task_enh.o

obj-$(CONFIG_QTI_HW_KEY_MANAGER_V1) += hwkm.o

obj-$(CONFIG_QCOM_LLCC_PERFMON) += llcc_perfmon.o

obj-$(CONFIG_QCOM_LLCC_HEURISTICS) += llcc_heuristics.o

obj-$(CONFIG_QCOM_LLCC) += llcc-qcom.o

obj-$(CONFIG_QCOM_LLCC_DEBUG) += llcc_perf_debug.o

obj-$(CONFIG_QCOM_MDT_LOADER) += mdt_loader.o

obj-$(CONFIG_QCOM_MEM_OFFLINE) += mem-offline.o

obj-$(CONFIG_QCOM_MEM_HOOKS) += mem-hooks.o

obj-$(CONFIG_QCOM_MEMORY_DUMP_V2) += memory_dump_v2.o

obj-$(CONFIG_QCOM_MICRODUMP) += microdump_collector.o

obj-$(CONFIG_QCOM_MINIDUMP) += minidump.o
minidump-objs += minidump_log.o msm_minidump.o
minidump-$(CONFIG_QCOM_MINIDUMP_PANIC_MEMORY_INFO) += minidump_memory.o
minidump-$(CONFIG_QCOM_MINIDUMP_SMEM) += minidump_smem.o
minidump-$(CONFIG_QCOM_MINIDUMP_RM) += minidump_rm.o

obj-$(CONFIG_SHOW_SUSPEND_EPOCH) += msm_show_epoch.o

obj-$(CONFIG_QCOM_SDEI) += qcom_sdei.o

obj-$(CONFIG_MSM_PERFORMANCE) += msm_performance.o

obj-$(CONFIG_QCOM_PANEL_EVENT_NOTIFIER) += panel_event_notifier.o

obj-$(CONFIG_QCOM_PCIE_PDC) += pcie-pdc.o

obj-$(CONFIG_MSM_POWER_STATE) += power_state.o

obj-$(CONFIG_QCOM_POWER_MODE) += power_mode.o

obj-$(CONFIG_QCOM_PD_MAPPER) += qcom_pd_mapper.o

obj-$(CONFIG_QCOM_PDR_MSG) += qcom_pdr_msg.o

obj-$(CONFIG_QCOM_PDR_HELPERS) += pdr_interface.o

obj-$(CONFIG_QTI_PMIC_GLINK_DEBUG) += pmic-glink-debug.o

obj-$(CONFIG_QTI_PMIC_PON_LOG) += pmic-pon-log.o

obj-$(CONFIG_QCOM_AOSS_QMP) += qcom_aoss.o

obj-$(CONFIG_QCOM_CACHE_ALLOCATION) += qcom_cache_allocation.o

obj-$(CONFIG_QCOM_CPUCP) += qcom_cpucp.o

obj-$(CONFIG_QCOM_CPUCP_FAST) += cpucp_fast.o

obj-$(CONFIG_QCOM_CPUSS_SLEEP_STATS_V4) += qcom_cpuss_sleep_stats_v4.o

obj-$(CONFIG_QCOM_CPUSS_SLEEP_STATS) += qcom_cpuss_sleep_stats.o

obj-$(CONFIG_QCOM_CPU_VENDOR_HOOKS) += qcom_cpu_vendor_hooks.o

obj-$(CONFIG_QCOM_DYNAMIC_RAMOOPS) += qcom_dynamic_ramoops.o

obj-$(CONFIG_QCOM_INLINE_CRYPTO_ENGINE) += qcom_ice.o
qcom_ice-objs += ice.o

obj-$(CONFIG_QCOM_LOGBUF_BOOTLOG) += qcom_logbuf_boot_log.o

obj-$(CONFIG_QCOM_LOGBUF_VENDOR_HOOKS) += qcom_logbuf_vendor_hooks.o

obj-$(CONFIG_QCOM_LPM_MONITOR) += qcom_lpm_monitor.o
qcom_lpm_monitor-objs += qcom_lpm_monitor.o qcom_lpm_monitor_service_v01.o

obj-$(CONFIG_QCOM_RAMDUMP) += qcom_ramdump.o

obj-$(CONFIG_QCOM_RPMH) += qcom_rpmh.o
qcom_rpmh-objs += rpmh-rsc.o rpmh.o

obj-$(CONFIG_MSM_RPM_SMD_DEBUG) += rpm-smd-debug.o

obj-$(CONFIG_QCOM_SOC_WATCHDOG) += qcom_soc_wdt.o

obj-$(CONFIG_QCOM_STATS) += qcom_stats.o

obj-$(CONFIG_QCOM_TLMM_VM_IRQCHIP) += qcom_tlmm_vm_irqchip.o

obj-$(CONFIG_QCOM_VA_MINIDUMP) += qcom_va_minidump.o

obj-$(CONFIG_QCOM_VADJ) += qcom_voltage_adj.o

obj-$(CONFIG_QCOM_WDT_CORE) += qcom_wdt_core.o

obj-$(CONFIG_QCOM_QFPROM_SYS) += qfprom-sys.o

obj-$(CONFIG_QCOM_QMI_HELPERS) += qmi_helpers.o
qmi_helpers-objs += qmi_encdec.o qmi_interface.o

obj-$(CONFIG_QSEE_IPC_IRQ_BRIDGE) += qsee_ipc_irq_bridge.o

obj-$(CONFIG_QTI_BATTERY_GLINK_DEBUG) += qti_battery_debug.o

obj-$(CONFIG_QTI_DMOF_SCMI) += qti_dmof_scmi.o

obj-$(CONFIG_QTI_PMIC_GLINK) += qti_pmic_glink.o

obj-$(CONFIG_QCOM_SC_TCM_MEM) += sc_tcm.o

obj-$(CONFIG_QCOM_SECURE_BUFFER) += secure_buffer.o

obj-$(CONFIG_QCOM_SLC_PERFMON) += slc_perfmon.o

obj-$(CONFIG_QCOM_SMEM) += smem.o

obj-$(CONFIG_QCOM_DBL) += qcom_dbl.o

obj-$(CONFIG_QCOM_SMP2P) += smp2p.o

obj-$(CONFIG_QCOM_SMSM) += smsm.o

obj-$(CONFIG_QCOM_SMP2P_SLEEPSTATE) += smp2p_sleepstate.o

obj-$(CONFIG_QCOM_SOCINFO) += socinfo.o

obj-$(CONFIG_QCOM_SYSMON_SUBSYSTEM_STATS) += sysmon_subsystem_stats.o

obj-$(CONFIG_QTI_SYS_PM_VX) += sys_pm_vx.o

obj-$(CONFIG_QTI_SYS_PM_VX_V2) += sys_pm_vx_v2.o

obj-$(CONFIG_QCOM_WCD_USBSS_I2C) += wcd_usbss_i2c.o
wcd_usbss_i2c-objs += wcd-usbss-regmap.o wcd-usbss-tables.o wcd-usbss-utils.o wcd939x-i2c.o

obj-$(CONFIG_QCOM_FSA4480_I2C) += fsa4480_i2c.o
fsa4480_i2c-objs += fsa4480-i2c.o

obj-$(CONFIG_RENAME_DEVICES) += rename_devices.o

obj-$(CONFIG_QTI_PMIC_LPM) += qti-pmic-lpm.o

obj-$(CONFIG_QCOM_PMIC_ECID) += qcom-pmic-ecid.o

obj-$(CONFIG_QCOM_SPMI_PMIC_ERR_DBG) += spmi-pmic-err-debug.o

obj-$(CONFIG_QCOM_RUN_QUEUE_STATS) += rq_stats.o

obj-$(CONFIG_QCOM_GENI_SE_MSM) += qcom-geni-se-msm.o

obj-$(CONFIG_QTI_CRYPTO_VIRTUALIZATION) += crypto-qti-virt.o

obj-$(CONFIG_QCOM_S2D_VENDOR_HOOK) += qcom_hib.o

obj-$(CONFIG_QCOM_SECURE_HIBERNATION) += qcom_secure_hibernation.o

obj-$(CONFIG_QCOM_MINIDUMP_VIRTIO) += minidump_virtio.o

obj-$(CONFIG_QCOM_DHMS_RPMSG) += dhms_rpmsg.o

obj-$(CONFIG_MSM_CORE_HANG_DETECT) += core_hang_detect.o

obj-$(CONFIG_MSM_SLATECOM_EVENT) += slatecom_event.o

obj-$(CONFIG_MSM_SEB) += slate_events_bridge.o

obj-$(CONFIG_MSM_SEB_RPMSG) += slate_events_bridge_rpmsg.o

obj-$(CONFIG_MSM_SLATERSB) += slate_rsb.o

obj-$(CONFIG_MSM_SLATERSB_RPMSG) += slatersb_rpmsg.o

obj-$(CONFIG_MSM_SLATECOM) += slatecom_spi.o

obj-$(CONFIG_MSM_SLATECOM_INTERFACE) += slatecom_interface.o

obj-$(CONFIG_MSM_SLATECOM_RPMSG) += slatecom_rpmsg.o

obj-$(CONFIG_QCOM_PBS) += qcom-pbs.o

obj-$(CONFIG_MSM_BAM_DMUX) += bam_dmux.o

obj-$(CONFIG_QCOM_POWER_MODE) += power_mode.o

obj-$(CONFIG_QCOM_WEAR_CONFIG) += wear_config.o

