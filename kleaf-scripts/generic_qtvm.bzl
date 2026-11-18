load("@bazel_skylib//rules:copy_file.bzl", "copy_file")
load("@bazel_skylib//rules:write_file.bzl", "write_file")
load("//build/kernel/kleaf:hermetic_tools.bzl", "hermetic_genrule")
load(
    "//build/kernel/kleaf:kernel.bzl",
    "kernel_build",
    "kernel_build_config",
)
load(":kleaf-scripts/image_opts.bzl", "vm_image_opts")

def define_qc_core_header_config(name):
    hermetic_genrule(
        name = name,
        outs = ["build.config.qc_core.header"],
        cmd = "echo \"KERNEL_DIR=common\" > $@",
    )
    hermetic_genrule(
        name = "signing_key",
        srcs = ["//soc-repo:certs/qcom_x509.genkey"],
        outs = ["signing_key.pem"],
        tools = ["//prebuilts/build-tools:openssl"],
        cmd = """
          $(location //prebuilts/build-tools:openssl) req -new -nodes -utf8 -sha256 -days 36500 \
            -batch -x509 -config $(location //soc-repo:certs/qcom_x509.genkey) \
            -outform PEM -out "$@" -keyout "$@"
        """,
        visibility = ["//visibility:public"],
    )

    hermetic_genrule(
        name = "verity_key",
        srcs = ["//soc-repo:certs/qcom_x509.genkey"],
        outs = ["verity_cert.pem", "verity_key.pem"],
        tools = ["//prebuilts/build-tools:openssl"],
        cmd = """
          $(location //prebuilts/build-tools:openssl) req -new -nodes -utf8 -newkey rsa:1024 -days 36500 \
            -batch -x509 -config $(location //soc-repo:certs/qcom_x509.genkey) \
            -outform PEM -out $(location verity_cert.pem) -keyout $(location verity_key.pem)
        """,
    )

def _gen_qc_core_build_config(name):
    """Generates a generic VM build config."""
    vm_opts = vm_image_opts()
    write_file(
        name = name,
        out = name + ".generated",
        content = [
            "KERNEL_DIR=\"common\"",
            "IN_KERNEL_MODULES=",
            "DTC_FLAGS=\"-@\"",
            "LLVM=1",
            "export CLANG_PREBUILT_BIN=prebuilts/clang/host/linux-x86/clang-r522817/bin",
            "export BUILDTOOLS_PREBUILT_BIN=build/kernel/build-tools/path/linux-x86",
            "VARIANTS=(debug-defconfig defconfig)",
            "VARIANT=defconfig",
            "ARCH=arm64",
            "PREFERRED_USERSPACE={}".format(vm_opts.preferred_usespace),
            "VM_DTB_IMG_CREATE={}".format(vm_opts.vm_dtb_img_create),
            "KERNEL_OFFSET={}".format(vm_opts.kernel_offset),
            "DTB_OFFSET={}".format(vm_opts.dtb_offset),
            "BASE_DTB_OFFSET={}".format(vm_opts.base_dtb_offset),
            "RAMDISK_OFFSET={}".format(vm_opts.ramdisk_offset),
            "CMDLINE_CPIO_OFFSET={}".format(vm_opts.cmdline_cpio_offset),
            "METADATA_OFFSET={}".format(vm_opts.metadata_offset),
            "METADATA_SIZE={}".format(vm_opts.metadata_size),
            "VM_SIZE_EXT4={}".format(vm_opts.vm_size_ext4),
            "DUMMY_IMG_SIZE={}".format(vm_opts.dummy_img_size),
            "",
        ],
    )

def define_qc_core_kernel(name, defconfig, defconfig_fragments = None):
    _gen_qc_core_build_config("build.config.{}".format(name))
    kernel_build_config(
        name = name + "_build_config",
        srcs = [
            # do not sort
            ":qc_core_kernel_header_config",
            "//common:build.config.common",
            "//common:build.config.aarch64",
            "build.config.{}".format(name),
        ],
    )

    kernel_build(
        name = name,
        srcs = ["//common:kernel_aarch64_sources"],
        outs = [
            # keep sorted
            "Image",
            "Module.symvers",
            "System.map",
            "certs/signing_key.x509",
            "gen_init_cpio",
            "modules.builtin",
            "modules.builtin.modinfo",
            "scripts/sign-file",
            "vmlinux",
            "vmlinux.symvers",
        ],
        build_config = name + "_build_config",
        defconfig = defconfig,
        keep_module_symvers = True,
        module_signing_key = ":signing_key",
        strip_modules = True,
        collect_unstripped_modules = True,
        post_defconfig_fragments = defconfig_fragments,
        make_goals = [
            "Image",
            "modules",
        ],
        system_trusted_key = ":verity_cert.pem",
        kconfig_ext = ":kconfig.qtvm.generated",
        visibility = ["//visibility:public"],
    )

def define_qtvm():
    # create a copy of Kconfig.qtvm for qtvm to use because :kconfig.msm.generated gave errors if
    # included twice, as would be the case when used by ddk_module.
    copy_file(
        name = "kconfig.qtvm.generated",
        src = "Kconfig.qtvm",
        out = "kconfig.qtvm/Kconfig.ext",
    )
    define_qc_core_header_config("qc_core_kernel_header_config")
    define_qc_core_kernel(
        "kernel_aarch64_qtvm",
        "arch/arm64/configs/generic_vm_defconfig",
        defconfig_fragments = ["arch/arm64/configs/generic_vm_cmdline.fragment"],
    )
    define_qc_core_kernel(
        "kernel_aarch64_qtvm_debug",
        "arch/arm64/configs/generic_vm_defconfig",
        defconfig_fragments = [
            "arch/arm64/configs/generic_vm_cmdline.fragment",
            "arch/arm64/configs/generic_vm_debug.fragment",
        ],
    )

    define_qc_core_kernel(
        "kernel_aarch64_autogvmlv",
        "arch/arm64/configs/generic_auto_defconfig",
    )
    define_qc_core_kernel(
        "kernel_aarch64_autogvmlv_debug",
        "arch/arm64/configs/generic_auto_defconfig",
        defconfig_fragments = ["arch/arm64/configs/generic_auto_debug.fragment"],
    )
