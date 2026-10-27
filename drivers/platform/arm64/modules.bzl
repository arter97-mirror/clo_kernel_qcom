def register_modules(registry):
    registry.register(
        name = "drivers/platform/arm64/qcom-glymur-ec",
        out = "qcom-glymur-ec.ko",
        config = "CONFIG_EC_QCOM_GLYMUR",
        srcs = [
            # do not sort
            "drivers/platform/arm64/qcom-glymur-ec.c",
        ],
    )
