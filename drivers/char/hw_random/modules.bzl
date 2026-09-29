def register_modules(registry):
    registry.register(
        name = "drivers/char/hw_random/virtio-rng",
        out = "virtio-rng.ko",
        config = "CONFIG_HW_RANDOM_VIRTIO",
        srcs = [
            # do not sort
            "drivers/char/hw_random/virtio-rng.c",
        ],
    )
