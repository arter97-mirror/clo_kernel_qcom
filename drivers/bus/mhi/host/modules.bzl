def register_modules(registry):
    registry.register(
        name = "drivers/bus/mhi/host/mhi",
        out = "mhi.ko",
        config = "CONFIG_MHI_BUS",
        srcs = [
            # do not sort
            "drivers/bus/mhi/common.h",
            "drivers/bus/mhi/host/internal.h",
            "drivers/bus/mhi/host/boot.c",
            "drivers/bus/mhi/host/init.c",
            "drivers/bus/mhi/host/main.c",
            "drivers/bus/mhi/host/misc.h",
            "drivers/bus/mhi/host/pm.c",
            "drivers/bus/mhi/host/trace.h",
        ],
        conditional_srcs = {
            "CONFIG_MHI_BUS_MISC": {
                True: [
                    # do not sort
                    "drivers/bus/mhi/host/misc.c",
                ],
            },
            "CONFIG_MHI_BUS_DEBUG": {
                True: [
                    # do not sort
                    "drivers/bus/mhi/host/debugfs.c",
                ],
            },
        },
        deps = [
            # do not sort
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
        name = "drivers/bus/mhi/host/mhi_pci_generic",
        out = "mhi_pci_generic.ko",
        config = "CONFIG_MHI_BUS_PCI_GENERIC",
        srcs = [
            # do not sort
            "drivers/bus/mhi/host/pci_generic.c",
        ],
        deps = [
            # do not sort
            "drivers/bus/mhi/host/mhi",
        ],
    )
