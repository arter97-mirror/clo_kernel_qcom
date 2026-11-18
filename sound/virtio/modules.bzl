def register_modules(registry):
    registry.register(
        name = "sound/virtio/virtio_snd",
        out = "virtio_snd.ko",
        config = "CONFIG_SND_VIRTIO",
        srcs = [
            # do not sort
            "sound/virtio/virtio_card.c",
            "sound/virtio/virtio_card.h",
            "sound/virtio/virtio_chmap.c",
            "sound/virtio/virtio_ctl_msg.c",
            "sound/virtio/virtio_ctl_msg.h",
            "sound/virtio/virtio_jack.c",
            "sound/virtio/virtio_kctl.c",
            "sound/virtio/virtio_pcm_msg.c",
            "sound/virtio/virtio_pcm_ops.c",
            "sound/virtio/virtio_pcm.c",
            "sound/virtio/virtio_pcm.h",
        ],
    )
