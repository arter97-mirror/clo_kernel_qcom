load("//build/kernel/kleaf:kernel.bzl", "kernel_abi", "kernel_module_group")
load(":configs/hamoa_consolidate.bzl", "hamoa_consolidate_config")
load(":configs/hamoa_perf.bzl", "hamoa_perf_config")
load(":kleaf-scripts/android_build.bzl", "define_typical_android_build")
load(":kleaf-scripts/image_opts.bzl", "boot_image_opts")
load(":kleaf-scripts/vm_build.bzl", "define_typical_vm_build")
load(":target_variants.bzl", "la_variants")

target_name = "hamoa"

def define_hamoa():
    for variant in la_variants:
        board_kernel_cmdline_extras = []
        board_bootconfig_extras = []
        kernel_vendor_cmdline_extras = [
            "bootconfig",
            "androidboot.bootdevice=1bf8000.pcie",
            "androidboot.boot_devices=soc/1bf8000.pcie",
            "androidboot.load_modules_parallel=true",
            "android_arch_task_struct_size=512",
            "kvm-arm.mode=protected",
            "arm64_sw.hvhe=0",
            "id_aa64mmfr1.vh=0",
            "kvm-arm.protected_modules=rpmh-regulator.ko,clk-rpmh.ko,gcc-x1e80100.ko,gpucc-x1e80100.ko,qnoc-x1e80100.ko,pkvm-geni,pkvm-smc-filter,qcom_smmu_v2_v3_dispatcher",
            "cpufreq.default_governor=performance",
            "log_buf_len=512K",
            "irqaffinity=0-3",
            "firmware_class.path=/vendor/firmware_mnt/image",
        ]

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
                earlycon_addr = "qcom_geni,0x894000",
                kernel_vendor_cmdline_extras = kernel_vendor_cmdline_extras,
                board_kernel_cmdline_extras = board_kernel_cmdline_extras,
                board_bootconfig_extras = board_bootconfig_extras,
            )

        else:
            board_kernel_cmdline_extras += ["nosoftlockup console=ttynull qcom_geni_serial.con_enabled=0"]
            kernel_vendor_cmdline_extras += ["nosoftlockup console=ttynull qcom_geni_serial.con_enabled=0"]
            board_bootconfig_extras += ["androidboot.serialconsole=0"]

            perf_build_img_opts = boot_image_opts(
                earlycon_addr = "qcom_geni,0x894000",
                kernel_vendor_cmdline_extras = kernel_vendor_cmdline_extras,
                board_kernel_cmdline_extras = board_kernel_cmdline_extras,
                board_bootconfig_extras = board_bootconfig_extras,
            )

    define_typical_android_build(
        name = "hamoa",
        consolidate_config = hamoa_consolidate_config,
        perf_config = hamoa_perf_config,
        consolidate_build_img_opts = consolidate_build_img_opts,
        perf_build_img_opts = perf_build_img_opts,
        consolidate_kwargs = {
            "config_path": "configs/hamoa_consolidate.bzl",
        },
        perf_kwargs = {
            "config_path": "configs/hamoa_perf.bzl",
        },
    )

    kernel_abi(
        name = "hamoa_perf_abi",
        kernel_build = "//common:kernel_aarch64",
        kernel_modules = [
            ":hamoa_perf_all_modules",
        ],
    )

    native.exports_files(["modules-lists/modules.list.msm.hamoa"])
