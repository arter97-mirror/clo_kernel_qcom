def register_modules(registry):
    registry.register(
        name = "drivers/net/ethernet/realtek/r8169",
        out = "r8169.ko",
        config = "CONFIG_R8169",
        srcs = [
            # do not sort
            "drivers/net/ethernet/realtek/r8169_main.c",
            "drivers/net/ethernet/realtek/r8169_firmware.c",
            "drivers/net/ethernet/realtek/r8169_phy_config.c",
            "drivers/net/ethernet/realtek/r8169_leds.c",
            "drivers/net/ethernet/realtek/r8169.h",
            "drivers/net/ethernet/realtek/r8169_firmware.h",
        ],
    )
