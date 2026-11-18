def register_modules(registry):
    registry.register(
        name = "drivers/gpio/gpio-virtio",
        out = "gpio-virtio.ko",
        config = "CONFIG_GPIO_VIRTIO",
        srcs = [
            # do not sort
            "drivers/gpio/gpio-virtio.c",
        ],
    )
    registry.register(
        name = "drivers/gpio/gpio-pca953x",
        out = "gpio-pca953x.ko",
        config = "CONFIG_GPIO_PCA953X",
        srcs = [
            # do not sort
            "drivers/gpio/gpio-pca953x.c",
        ],
    )
