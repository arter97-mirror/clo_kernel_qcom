def register_modules(registry):
    registry.register(
        name = "drivers/virtio/virtio_mem",
        out = "virtio_mem.ko",
        config = "CONFIG_VIRTIO_MEM",
        srcs = [
            # do not sort
            "drivers/virtio/virtio_mem.c",
            "drivers/virtio/qti_virtio_mem.c",
            "drivers/virtio/qti_virtio_mem.h",
        ],
        deps = [
            # do not sort
            "drivers/soc/qcom/mem_buf/mem_buf",
            "drivers/soc/qcom/mem_buf/mem_buf_dev",
            "drivers/virt/gunyah/gh_rm_drv",
        ],
    )

    registry.register(
        name = "drivers/virtio/virtio_mmio",
        out = "virtio_mmio.ko",
        config = "CONFIG_VIRTIO_MMIO",
        srcs = [
            # do not sort
            "drivers/virtio/virtio_mmio.c",
        ],
        deps = [
            # do not sort
            "drivers/virtio/virtio",
            "drivers/virtio/virtio_ring",
        ],
    )

    registry.register(
        name = "drivers/virtio/virtio_input",
        out = "virtio_input.ko",
        config = "CONFIG_VIRTIO_INPUT",
        srcs = [
            # do not sort
            "drivers/virtio/virtio_input.c",
        ],
        deps = [
            # do not sort
            "drivers/virtio/virtio_mmio",
        ],
    )

    registry.register(
        name = "drivers/virtio/virtio_dma_buf",
        out = "virtio_dma_buf.ko",
        config = "CONFIG_VIRTIO_DMA_SHARED_BUFFER",
        srcs = [
            # do not sort
            "drivers/virtio/virtio_dma_buf.c",
        ],
    )
