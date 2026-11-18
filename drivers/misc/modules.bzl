load(":drivers/misc/fl7112/modules.bzl", register_fl7112 = "register_modules")
load(":drivers/misc/isl97900_led/modules.bzl", register_isl97900_led = "register_modules")
load(":drivers/misc/lkdtm/modules.bzl", register_lkdtm = "register_modules")

def register_modules(registry):
    register_fl7112(registry)
    register_isl97900_led(registry)
    register_lkdtm(registry)

    registry.register(
        name = "drivers/misc/qseecom_proxy",
        out = "qseecom_proxy.ko",
        config = "CONFIG_QSEECOM_PROXY",
        srcs = [
            # do not sort
            "drivers/misc/qseecom_proxy.c",
        ],
    )

    registry.register(
        name = "drivers/misc/bootmarker_proxy",
        out = "bootmarker_proxy.ko",
        config = "CONFIG_BOOTMARKER_PROXY",
        srcs = [
            # do not sort
            "drivers/misc/bootmarker_proxy.c",
        ],
    )

    registry.register(
        name = "drivers/misc/frpc-adsprpc",
        out = "frpc-adsprpc.ko",
        config = "CONFIG_QTI_FASTRPC",
        srcs = [
            # do not sort
            "drivers/misc/fastrpc.c",
        ],
        deps = [
            # do not sort
            "drivers/firmware/qcom/qcom-scm",
        ],
    )

    registry.register(
        name = "drivers/misc/tps6286",
        out = "tps6286.ko",
        config = "CONFIG_TPS6286_STEP_DOWN_CONV",
        srcs = [
            # do not sort
            "drivers/misc/tps6286.c",
        ],
    )

    registry.register(
        name = "drivers/misc/ina234b",
        out = "ina234b.ko",
        config = "CONFIG_INA234_CURRENT_MONITOR",
        srcs = [
            # do not sort
            "drivers/misc/ina234b.c",
        ],
    )

    registry.register(
        name = "drivers/misc/slg4dc",
        out = "slg4dc.ko",
        config = "CONFIG_SLG4DC_SIGNAL_IC",
        srcs = [
            # do not sort
            "drivers/misc/slg4dc.c",
        ],
    )
