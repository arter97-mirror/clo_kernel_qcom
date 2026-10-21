def register_modules(registry):
    registry.register(
        name = "drivers/iio/common/scmi_sensors/scmi_iio",
        out = "scmi_iio.ko",
        config = "CONFIG_IIO_SCMI",
        srcs = [
            # do not sort
            "drivers/iio/common/scmi_sensors/scmi_iio.c",
        ],
        deps = [
            # do not sort
            "drivers/iio/buffer/kfifo_buf",
        ],
    )
