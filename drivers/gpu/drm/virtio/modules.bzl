def register_modules(registry):
    registry.register(
        name = "drivers/gpu/drm/virtio-gpu",
        out = "virtio-gpu.ko",
        config = "CONFIG_DRM_VIRTIO_GPU",
        srcs = [
            # do not sort
            "drivers/gpu/drm/virtio/virtgpu_debugfs.c",
            "drivers/gpu/drm/virtio/virtgpu_display.c",
            "drivers/gpu/drm/virtio/virtgpu_drv.c",
            "drivers/gpu/drm/virtio/virtgpu_drv.h",
            "drivers/gpu/drm/virtio/virtgpu_fence.c",
            "drivers/gpu/drm/virtio/virtgpu_gem.c",
            "drivers/gpu/drm/virtio/virtgpu_ioctl.c",
            "drivers/gpu/drm/virtio/virtgpu_kms.c",
            "drivers/gpu/drm/virtio/virtgpu_object.c",
            "drivers/gpu/drm/virtio/virtgpu_plane.c",
            "drivers/gpu/drm/virtio/virtgpu_prime.c",
            "drivers/gpu/drm/virtio/virtgpu_submit.c",
            "drivers/gpu/drm/virtio/virtgpu_trace_points.c",
            "drivers/gpu/drm/virtio/virtgpu_trace.h",
            "drivers/gpu/drm/virtio/virtgpu_vq.c",
            "drivers/gpu/drm/virtio/virtgpu_vram.c",
        ],
        deps = [
            # do not sort
            "drivers/virtio/virtio_dma_buf",
        ],
    )
