def register_modules(registry):
    registry.register(
        name = "drivers/bus/mhi/devices/mhi_dev_satellite",
        out = "mhi_dev_satellite.ko",
        config = "CONFIG_MHI_SATELLITE",
        srcs = [
            # do not sort
            "drivers/bus/mhi/devices/mhi_satellite.c",
        ],
        deps = [
            # do not sort
            "drivers/bus/mhi/host/mhi",
            "drivers/pci/controller/pci-msm-drv",
            "drivers/remoteproc/rproc_qcom_common",
            "drivers/rpmsg/qcom_smd",
            "drivers/rpmsg/qcom_glink_smem",
            "drivers/rpmsg/qcom_glink",
            "drivers/soc/qcom/pcie-pdc",
            "drivers/pinctrl/qcom/pinctrl-msm",
            "drivers/clk/qcom/clk-qcom",
            "drivers/clk/qcom/gdsc-regulator",
            "drivers/regulator/debug-regulator",
            "drivers/regulator/proxy-consumer",
            "drivers/soc/qcom/crm-v2",
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

    registry.register(
        name = "drivers/bus/mhi/devices/mhi_dev_uci",
        out = "mhi_dev_uci.ko",
        config = "CONFIG_MHI_UCI",
        srcs = [
            # do not sort
            "drivers/bus/mhi/devices/mhi_uci.c",
        ],
        deps = [
            # do not sort
            "drivers/bus/mhi/host/mhi",
            "drivers/pci/controller/pci-msm-drv",
            "drivers/soc/qcom/pcie-pdc",
            "drivers/pinctrl/qcom/pinctrl-msm",
            "drivers/clk/qcom/clk-qcom",
            "drivers/clk/qcom/gdsc-regulator",
            "drivers/regulator/debug-regulator",
            "drivers/regulator/proxy-consumer",
            "drivers/soc/qcom/crm-v2",
            "drivers/firmware/qcom/qcom-scm",
            "drivers/virt/gunyah/gh_rm_drv",
            "drivers/virt/gunyah/gh_msgq",
            "drivers/virt/gunyah/gh_dbl",
            "arch/arm64/gunyah/gh_arm_drv",
            "kernel/trace/qcom_ipc_logging",
        ],
    )

    registry.register(
        name = "drivers/bus/mhi/devices/mhi_netdev",
        out = "mhi_netdev.ko",
        config = "CONFIG_MHI_NETDEV",
        srcs = [
            # do not sort
            "drivers/bus/mhi/devices/mhi_netdev.c",
        ],
        deps = [
            # do not sort
            "drivers/bus/mhi/host/mhi",
            "kernel/trace/qcom_ipc_logging",
        ],
    )
