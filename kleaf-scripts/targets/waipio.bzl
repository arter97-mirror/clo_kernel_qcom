load("//build/kernel/kleaf:kernel.bzl", "kernel_abi", "kernel_module_group")
load(":configs/waipio_consolidate.bzl", "waipio_consolidate_config")
load(":configs/waipio_perf.bzl", "waipio_perf_config")
load(":configs/waipio_tuivm.bzl", "waipio_tuivm_config")
load(":configs/waipio_tuivm_debug.bzl", "waipio_tuivm_debug_config")
load(":kleaf-scripts/android_build.bzl", "define_typical_android_build")
load(":kleaf-scripts/generic_qtvm.bzl", "define_qc_core_kernel")
load(":kleaf-scripts/image_opts.bzl", "boot_image_opts", "vm_image_opts")
load(":kleaf-scripts/vm_build.bzl", "define_typical_vm_build")
load(":target_variants.bzl", "la_variants")

target_name = "waipio"

def define_waipio():
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
                "ufshcd_core.uic_cmd_timeout=2000",
            ]
            kernel_vendor_cmdline_extras += [
                # do not sort
                "console=ttyMSM0,115200n8",
                "qcom_geni_serial.con_enabled=1",
                "earlycon",
            ]

            consolidate_build_img_opts = boot_image_opts(
                earlycon_addr = "qcom_geni,0x0099c000",
                kernel_vendor_cmdline_extras = kernel_vendor_cmdline_extras,
                board_kernel_cmdline_extras = board_kernel_cmdline_extras,
                board_bootconfig_extras = board_bootconfig_extras,
            )

        else:
            board_kernel_cmdline_extras += ["nosoftlockup console=ttynull qcom_geni_serial.con_enabled=0"]
            kernel_vendor_cmdline_extras += ["nosoftlockup console=ttynull qcom_geni_serial.con_enabled=0"]
            board_bootconfig_extras += ["androidboot.serialconsole=0"]

            perf_build_img_opts = boot_image_opts(
                earlycon_addr = "qcom_geni,0x0099c000",
                kernel_vendor_cmdline_extras = kernel_vendor_cmdline_extras,
                board_kernel_cmdline_extras = board_kernel_cmdline_extras,
                board_bootconfig_extras = board_bootconfig_extras,
            )

    define_typical_android_build(
        name = "waipio",
        consolidate_config = waipio_consolidate_config,
        perf_config = waipio_perf_config,
        consolidate_build_img_opts = consolidate_build_img_opts,
        perf_build_img_opts = perf_build_img_opts,
        consolidate_kwargs = {
            "config_path": "configs/waipio_consolidate.bzl",
        },
        perf_kwargs = {
            "config_path": "configs/waipio_perf.bzl",
        },
    )

    kernel_abi(
        name = "waipio_perf_abi",
        kernel_build = "//common:kernel_aarch64",
        kernel_modules = [
            ":waipio_perf_all_modules",
        ],
    )

def define_waipio_tuivm():
    waipio_vm_opts = vm_image_opts(
        kernel_offset = 0xE0C00000,
        dtb_offset = 0xE2C00000,
        ramdisk_offset = 0xE2F00000,
        dummy_img_offset = 0xE55F0000,
    )
    define_qc_core_kernel(
        "kernel_aarch64_qtvm_waipio",
        "arch/arm64/configs/generic_vm_defconfig",
        defconfig_fragments = ["arch/arm64/configs/generic_vm_cmdline.fragment"],
        vm_opts = waipio_vm_opts,
    )
    define_qc_core_kernel(
        "kernel_aarch64_qtvm_waipio_debug",
        "arch/arm64/configs/generic_vm_defconfig",
        defconfig_fragments = [
            "arch/arm64/configs/generic_vm_cmdline.fragment",
            "arch/arm64/configs/generic_vm_debug.fragment",
        ],
        vm_opts = waipio_vm_opts,
    )
    define_typical_vm_build(
        name = "waipio-tuivm",
        config = waipio_tuivm_config,
        debug_config = waipio_tuivm_debug_config,
        dtb_target = "waipio-tuivm",
        base_kernel = "kernel_aarch64_qtvm_waipio",
        debug_kwargs = {
            "config_path": "configs/waipio_tuivm_debug.bzl",
        },
        config_kwargs = {
            "config_path": "configs/waipio_tuivm.bzl",
        },
    )
