def register_modules(registry):
    registry.register(
        name = "drivers/misc/fl7112",
        out = "fl7112.ko",
        config = "CONFIG_PARADA_PD_FL7112",
        srcs = [
            "drivers/misc/fl7112/fl7112.c",
            "drivers/misc/fl7112/fl7112.h",
        ],
    )
