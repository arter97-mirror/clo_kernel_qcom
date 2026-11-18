def register_modules(registry):
    registry.register(
        name = "drivers/usb/extcon/extcon-ktm5030",
        out = "extcon-ktm5030.ko",
        config = "CONFIG_KTM5000B0T",
        srcs = [
            "drivers/usb/extcon/extcon-ktm5030.c",
        ],
        deps = [
            # do not sort
            "drivers/usb/dwc3/dwc3-msm",
        ],
    )
