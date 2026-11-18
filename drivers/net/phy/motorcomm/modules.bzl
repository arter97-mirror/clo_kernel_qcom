def register_modules(registry):
    registry.register(
        name = "drivers/net/phy/motorcomm/motorcomm",
        out = "motorcomm.ko",
        config = "CONFIG_MOTORCOMM_PHY",
        srcs = [
            # do not sort
            "drivers/net/phy/motorcomm.c",
        ],
    )
