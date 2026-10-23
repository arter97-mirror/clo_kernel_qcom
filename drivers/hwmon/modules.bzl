def register_modules(registry):
    registry.register(
        name = "drivers/hwmon/hwmon",
        out = "hwmon.ko",
        config = "CONFIG_HWMON",
        srcs = [
            # do not sort
            "drivers/hwmon/hwmon.c",
        ],
    )

    registry.register(
        name = "drivers/hwmon/qti_amoled_ecm",
        out = "qti_amoled_ecm.ko",
        config = "CONFIG_SENSORS_QTI_AMOLED_ECM",
        srcs = [
            # do not sort
            "drivers/hwmon/qti_amoled_ecm.c",
        ],
        deps = [
            # do not sort
            "drivers/hwmon/hwmon",
            "drivers/soc/qcom/panel_event_notifier",
        ],
    )

    registry.register(
        name = "drivers/hwmon/qti_btl",
        out = "qti_btl.ko",
        config = "CONFIG_SENSORS_QTI_BTL",
        srcs = [
            # do not sort
            "drivers/hwmon/qti_btl.c",
        ],
        deps = [
            # do not sort
            "drivers/hwmon/hwmon",
        ],
    )

    registry.register(
        name = "drivers/hwmon/qti_batt_telemetry",
        out = "qti_batt_telemetry.ko",
        config = "CONFIG_QTI_BATT_TELEMETRY",
        srcs = [
            # do not sort
            "drivers/hwmon/qti_batt_telemetry.c",
        ],
        deps = [
            # do not sort
            "drivers/soc/qcom/qti_pmic_glink",
            "drivers/hwmon/hwmon",
        ],
    )
