load(":configs/shikra_tiny_consolidate.bzl", "shikra_tiny_consolidate_config")
load(":configs/shikra_tiny_perf.bzl", "shikra_tiny_perf_config")
load(":kleaf-scripts/android_build.bzl", "define_typical_android_build")
load(":kleaf-scripts/consolidate.bzl", "define_consolidated_kernel")
load(":kleaf-scripts/image_opts.bzl", "boot_image_opts")
load(":target_variants.bzl", "la_variants")

target_name = "shikra_tiny"

_SHIKRA_TINY_EXCLUDED_GKI_MODULES = [
    "drivers/bluetooth/btbcm.ko",
    "drivers/bluetooth/btqca.ko",
    "drivers/bluetooth/btsdio.ko",
    "drivers/bluetooth/hci_uart.ko",
    "net/bluetooth/bluetooth.ko",
    "net/bluetooth/hidp/hidp.ko",
    "net/bluetooth/rfcomm/rfcomm.ko",
    "net/nfc/nfc.ko",
]

def define_shikra_tiny():
    define_consolidated_kernel(
        name = "shikra_tiny_kernel_aarch64_perf",
        defconfig = ":arch/arm64/configs/shikra_tiny_defconfig",
        pre_defconfig_fragments = [],
        exclude_modules = _SHIKRA_TINY_EXCLUDED_GKI_MODULES,
    )
    native.filegroup(
        name = "shikra_tiny_kernel_aarch64_perf_download_configs",
        srcs = [],
    )
    native.filegroup(
        name = "shikra_tiny_kernel_aarch64_perf_filegroup_declaration",
        srcs = [],
    )

    define_consolidated_kernel(
        name = "shikra_tiny_kernel_aarch64_consolidate",
        defconfig = ":arch/arm64/configs/shikra_tiny_defconfig",
        exclude_modules = _SHIKRA_TINY_EXCLUDED_GKI_MODULES,
    )
    native.filegroup(
        name = "shikra_tiny_kernel_aarch64_consolidate_download_configs",
        srcs = [],
    )
    native.filegroup(
        name = "shikra_tiny_kernel_aarch64_consolidate_filegroup_declaration",
        srcs = [],
    )

    for variant in la_variants:
        board_kernel_cmdline_extras = []
        board_bootconfig_extras = []
        kernel_vendor_cmdline_extras = ["bootconfig"]

        if variant == "consolidate":
            board_bootconfig_extras += ["androidboot.serialconsole=1"]
            board_kernel_cmdline_extras += [
                # do not sort
                "console=ttyMSM0,115200n8",
                "qcom_geni_serial.con_enabled=1",
                "earlycon",
            ]
            kernel_vendor_cmdline_extras += [
                # do not sort
                "console=ttyMSM0,115200n8",
                "qcom_geni_serial.con_enabled=1",
                "earlycon",
            ]

            consolidate_build_img_opts = boot_image_opts(
                earlycon_addr = "qcom_geni,0x04a90000",
                kernel_vendor_cmdline_extras = kernel_vendor_cmdline_extras,
                board_kernel_cmdline_extras = board_kernel_cmdline_extras,
                board_bootconfig_extras = board_bootconfig_extras,
            )

        else:
            board_kernel_cmdline_extras += ["nosoftlockup console=ttynull qcom_geni_serial.con_enabled=0"]
            kernel_vendor_cmdline_extras += ["nosoftlockup console=ttynull qcom_geni_serial.con_enabled=0"]
            board_bootconfig_extras += ["androidboot.serialconsole=0"]

            perf_build_img_opts = boot_image_opts(
                earlycon_addr = "qcom_geni,0x04a90000",
                kernel_vendor_cmdline_extras = kernel_vendor_cmdline_extras,
                board_kernel_cmdline_extras = board_kernel_cmdline_extras,
                board_bootconfig_extras = board_bootconfig_extras,
            )

    define_typical_android_build(
        name = "shikra_tiny",
        module_lists_name = "shikra",
        perf_config = shikra_tiny_perf_config,
        consolidate_config = shikra_tiny_consolidate_config,
        perf_build_img_opts = perf_build_img_opts,
        consolidate_build_img_opts = consolidate_build_img_opts,
        perf_kwargs = {
            "base_kernel": ":shikra_tiny_kernel_aarch64_perf",
            "config_path": "configs/shikra_tiny_perf.bzl",
        },
        consolidate_kwargs = {
            "base_kernel": ":shikra_tiny_kernel_aarch64_consolidate",
            "config_path": "configs/shikra_tiny_consolidate.bzl",
        },
    )
