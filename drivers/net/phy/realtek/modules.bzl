def register_modules(registry):
    registry.register(
        name = "drivers/net/phy/realtek/realtek",
        out = "realtek.ko",
        config = "CONFIG_REALTEK_PHY",
        srcs = [
            # do not sort
            "drivers/net/phy/realtek.c",
        ],
    )
