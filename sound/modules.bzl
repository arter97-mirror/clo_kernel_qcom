load(":sound/soc/codecs/modules.bzl", register_soc_codecs = "register_modules")
load(":sound/usb/modules.bzl", register_usb = "register_modules")
load(":sound/virtio/modules.bzl", register_virtio = "register_modules")

def register_modules(registry):
    register_soc_codecs(registry)
    register_usb(registry)
    register_virtio(registry)
