load("//build/kernel/kleaf:kernel.bzl", "kernel_abi", "kernel_module_group")
load(":configs/glymur_consolidate.bzl", "glymur_consolidate_config")
load(":configs/glymur_perf.bzl", "glymur_perf_config")
load(":kleaf-scripts/android_build.bzl", "define_typical_android_build")
load(":kleaf-scripts/image_opts.bzl", "boot_image_opts")
load(":kleaf-scripts/vm_build.bzl", "define_typical_vm_build")
load(":target_variants.bzl", "la_variants")

target_name = "glymur"

def define_glymur():
    for variant in la_variants:
        board_kernel_cmdline_extras = []
        board_bootconfig_extras = []
        kernel_vendor_cmdline_extras = [
            "bootconfig",
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
                "clk_ignore_unused",
                "pd_ignore_unused",
                "efi=noruntime",
                "efi=novamap",
                "earlycon",
                "console=ttyMSM0,115200",
                "androidboot.hardware.platform=android-desktop",
                "androidboot.hardware=android-desktop",
                "androidboot.load_modules_parallel=true",
                "android_arch_task_struct_size=512",
                "id_aa64mmfr1.vh=0",
                "arm64_sw.hvhe=0",
                "kvm-arm.mode=none",
                "arm64.nosme",
                "arm64.nosve",
                "androidboot.selinux=permissive",
                "irqaffinity=0-3",
                "pcie_ports=compat",
                "cpufreq.default_governor=performance",
                "cpuidle.off=1",
                "firmware_class.path=/vendor/firmware,/vendor/firmware_mnt/image",
                "console=null",
                "8250.nr_uarts=4",
                "console_msg_format=syslog",
                "log_buf_len=4M",
                "cgroup_disable=pressure",
                "root=/dev/ram0",
                "firmware_class.path=/vendor/firmware",
                "reserve_mem=20M:2M:trace",
                "trace_instance=boot_mapped^traceoff@trace",
                "drm.trace=0x106",
                "module_blacklist=cros_ec_debugfs",
                "proc_mem.force_override=ptrace",
                "binder.impl=rust",
                "no_console_suspend",
                "cpufreq.default_governor=performance",
                "trace_instance=backup=boot_mapped",
                "cpuidle.governor=teo",
                "kvm-arm.protected_modules=pkvm-geni.ko,pkvm-smc-filter.ko,qcom_smmu_v2_v3_dispatcher.ko",
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
        name = "glymur",
        consolidate_config = glymur_consolidate_config,
        perf_config = glymur_perf_config,
        consolidate_build_img_opts = consolidate_build_img_opts,
        perf_build_img_opts = perf_build_img_opts,
        consolidate_kwargs = {
            "config_path": "configs/glymur_consolidate.bzl",
        },
        perf_kwargs = {
            "config_path": "configs/glymur_perf.bzl",
        },
    )

    kernel_abi(
        name = "glymur_perf_abi",
        kernel_build = "//common:kernel_aarch64",
        kernel_modules = [
            ":glymur_perf_all_modules",
        ],
    )

    native.exports_files(["modules-lists/modules.list.msm.glymur"])
