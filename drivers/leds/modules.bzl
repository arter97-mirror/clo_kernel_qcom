load(":drivers/leds/flash/modules.bzl", register_flash = "register_modules")
load(":drivers/leds/rgb/modules.bzl", register_rgb = "register_modules")

def register_modules(registry):
    register_flash(registry)
    register_rgb(registry)

    registry.register(
        name = "drivers/leds/leds-qti-flash",
        out = "leds-qti-flash.ko",
        config = "CONFIG_LEDS_QTI_FLASH",
        srcs = [
            # do not sort
            "drivers/leds/leds-qti-flash.c",
        ],
        deps = [
            # do not sort
            "drivers/power/supply/qti_battery_charger",
            "drivers/soc/qcom/panel_event_notifier",
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

    registry.register(
        name = "drivers/leds/trigger/ledtrig-pattern",
        out = "ledtrig-pattern.ko",
        config = "CONFIG_LEDS_TRIGGER_PATTERN",
        srcs = [
            # do not sort
            "drivers/leds/trigger/ledtrig-pattern.c",
        ],
    )

    registry.register(
        name = "drivers/leds/trigger/ledtrig-netdev",
        out = "ledtrig-netdev.ko",
        config = "CONFIG_LEDS_TRIGGER_NETDEV",
        srcs = [
            # do not sort
            "drivers/leds/trigger/ledtrig-netdev_fixed.c",
            "drivers/leds/leds.h",
        ],
    )

    registry.register(
        name = "drivers/leds/leds-qpnp-vibrator-ldo",
        out = "leds-qpnp-vibrator-ldo.ko",
        config = "CONFIG_LEDS_QPNP_VIBRATOR_LDO",
        srcs = [
            # do not sort
            "drivers/leds/leds-qpnp-vibrator-ldo.c",
        ],
        deps = [
            # do not sort
        ],
    )

    registry.register(
        name = "drivers/leds/leds-aw2016",
        out = "leds-aw2016.ko",
        config = "CONFIG_LEDS_AW2016",
        srcs = [
            # do not sort
            "drivers/leds/leds-aw2016.c",
            "drivers/leds/leds-aw2016.h",
        ],
        deps = [
            # do not sort
        ],
    )

    registry.register(
        name = "drivers/leds/leds-user-aw21104",
        out = "leds-user-aw21104.ko",
        config = "CONFIG_LEDS_USER_AW21104",
        srcs = [
            # do not sort
            "drivers/leds/leds-user-aw21104.c",
        ],
        deps = [
            # do not sort
        ],
    )

    registry.register(
        name = "drivers/leds/leds-bys-aw21104",
        out = "leds-bys-aw21104.ko",
        config = "CONFIG_LEDS_BYS_AW21104",
        srcs = [
            # do not sort
            "drivers/leds/leds-bys-aw21104.c",
        ],
        deps = [
            # do not sort
        ],
    )

    registry.register(
        name = "drivers/leds/leds-user-lp5814",
        out = "leds-user-lp5814.ko",
        config = "CONFIG_LEDS_USER_LP5814",
        srcs = [
            # do not sort
            "drivers/leds/leds-user-lp5814.c",
        ],
        deps = [
            # do not sort
        ],
    )

    registry.register(
        name = "drivers/leds/leds-etirled-lm3645",
        out = "leds-etirled-lm3645.ko",
        config = "CONFIG_LEDS_ETIRLED_LM3645",
        srcs = [
            # do not sort
            "drivers/leds/leds-etirled-lm3645.c",
        ],
        deps = [
            # do not sort
        ],
    )

    registry.register(
        name = "drivers/leds/leds-aw200xx",
        out = "leds-aw200xx.ko",
        config = "CONFIG_LEDS_AW200XX",
        srcs = [
            # do not sort
            "drivers/leds/leds-aw200xx.c",
        ],
        deps = [
            # do not sort
        ],
    )
