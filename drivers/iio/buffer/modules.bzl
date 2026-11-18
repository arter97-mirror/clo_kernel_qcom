def register_modules(registry):
    registry.register(
        name = "drivers/iio/buffer/kfifo_buf",
        out = "kfifo_buf.ko",
        config = "CONFIG_IIO_KFIFO_BUF",
        srcs = [
            # do not sort
            "drivers/iio/buffer/kfifo_buf.c",
        ],
    )
