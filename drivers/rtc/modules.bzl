def register_modules(registry):
    registry.register(
        name = "drivers/rtc/rtc-pm8xxx",
        out = "rtc-pm8xxx.ko",
        config = "CONFIG_RTC_DRV_PM8XXX",
        srcs = [
            # do not sort
            "drivers/rtc/rtc-pm8xxx.c",
        ],
    )

    registry.register(
        name = "drivers/rtc/rtc_qti_glink",
        out = "rtc_qti_glink.ko",
        config = "CONFIG_RTC_QTI_GLINK",
        srcs = [
            # do not sort
            "drivers/rtc/rtc_qti_glink.c",
        ],
        deps = [
            # do not sort
            "drivers/soc/qcom/qti_pmic_glink",
            "drivers/soc/qcom/pdr_interface",
            "drivers/soc/qcom/qmi_helpers",
            "drivers/remoteproc/rproc_qcom_common",
            "drivers/rpmsg/qcom_smd",
            "drivers/rpmsg/qcom_glink_smem",
            "drivers/rpmsg/qcom_glink",
            "kernel/trace/qcom_ipc_logging",
            "drivers/soc/qcom/minidump",
            "drivers/soc/qcom/smem",
            "drivers/soc/qcom/debug_symbol",
            "drivers/dma-buf/heaps/qcom_dma_heaps",
            "drivers/iommu/msm_dma_iommu_mapping",
            "drivers/soc/qcom/mem_buf/mem_buf_dev",
            "drivers/soc/qcom/secure_buffer",
            "drivers/firmware/qcom/qcom-scm",
            "drivers/virt/gunyah/gh_rm_drv",
            "drivers/virt/gunyah/gh_msgq",
            "drivers/virt/gunyah/gh_dbl",
            "arch/arm64/gunyah/gh_arm_drv",
        ],
    )
