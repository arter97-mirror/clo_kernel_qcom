def register_modules(registry):
    registry.register(
        name = "drivers/i2c/i2c-mux",
        out = "i2c-mux.ko",
        config = "CONFIG_I2C_MUX",
        srcs = [
            "drivers/i2c/i2c-mux.c",
        ],
        deps = [],
    )

    registry.register(
        name = "drivers/i2c/muxes/i2c-mux-gpio",
        out = "i2c-mux-gpio.ko",
        config = "CONFIG_I2C_MUX_GPIO",
        srcs = [
            "drivers/i2c/muxes/i2c-mux-gpio.c",
        ],
        deps = [
            "drivers/i2c/i2c-mux",
        ],
    )
