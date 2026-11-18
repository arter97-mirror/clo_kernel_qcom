load("//build/bazel_common_rules/dist:dist.bzl", "copy_to_dist_dir")
load("//build/kernel/kleaf:constants.bzl", "aarch64_outs")
load(
    "//build/kernel/kleaf:kernel.bzl",
    "gki_artifacts",
    "kernel_build",
    "kernel_compile_commands",
    "kernel_images",
    "kernel_modules_install",
    "kernel_unstripped_modules_archive",
)
load("//common:modules.bzl", "get_gki_modules_list", "get_kunit_modules_list")

def define_consolidated_kernel(
        name = "kernel_aarch64_consolidate",
        defconfig = "//common:arch/arm64/configs/gki_defconfig",
        pre_defconfig_fragments = None,
        exclude_modules = []):
    if pre_defconfig_fragments == None:
        pre_defconfig_fragments = [":arch/arm64/configs/consolidate.fragment"]

    _gki_modules_list = [m for m in get_gki_modules_list("arm64") if m not in exclude_modules]
    _module_implicit_outs = _gki_modules_list + get_kunit_modules_list("arm64")

    # mostly copied from build/kleaf/common_kernels.bzl:_define_common_kernel().
    # Kept what I could, dropped what I couldn't keep or doesn't make sense,
    # specifically ABI monitoring.
    kernel_build(
        name = name,
        srcs = ["//common:kernel_aarch64_sources"],
        outs = aarch64_outs,
        arch = "arm64",
        make_goals = [
            "Image",
            "Image.lz4",
            "Image.gz",
            "modules",
        ],
        implicit_outs = [
            "scripts/sign-file",
            "certs/signing_key.pem",
            "certs/signing_key.x509",
        ],
        build_config = ":build.config.qcom.dtb",
        defconfig = defconfig,
        pre_defconfig_fragments = pre_defconfig_fragments,
        # TODO: make this True to align with gki kernel
        strip_modules = False,
        module_implicit_outs = _module_implicit_outs,
        collect_unstripped_modules = True,
        keep_module_symvers = True,
        pack_module_env = True,
        trim_nonlisted_kmi = False,
        kmi_symbol_list_strict_mode = False,
        kmi_symbol_list = "//common:gki/aarch64/symbols/qcom",
        ddk_module_defconfig_fragments = [
            Label("//build/kernel/kleaf/impl/defconfig:signing_modules_disabled"),
        ],
        visibility = ["//visibility:public"],
    )

    kernel_modules_install(
        name = name + "_modules_install",
        # The GKI target does not have external modules. GKI modules goes
        # into the in-tree kernel module list, aka kernel_build.module_implicit_outs.
        # Hence, this is empty.
        kernel_modules = [],
        kernel_build = name,
    )

    kernel_unstripped_modules_archive(
        name = name + "_unstripped_modules_archive",
        kernel_build = name,
    )

    kernel_images(
        name = name + "_images",
        kernel_build = name,
        kernel_modules_install = name + "_modules_install",
        # Sync with CI_TARGET_MAPPING.*.download_configs.images
        build_system_dlkm = True,
        build_system_dlkm_flatten = True,
        system_dlkm_fs_types = ["erofs", "ext4"],
        # Keep in sync with build.config.gki* MODULES_LIST
        modules_list = "//common:gki_system_dlkm_modules_arm64",
    )

    gki_artifacts(
        name = name + "_gki_artifacts",
        kernel_build = name,
        boot_img_sizes = {
            # Assume BUILD_GKI_BOOT_IMG_SIZE is the following
            "": "83886080",
            # Assume BUILD_GKI_BOOT_IMG_LZ4_SIZE is the following
            "lz4": "53477376",
            # Assume BUILD_GKI_BOOT_IMG_GZ_SIZE is the following
            "gz": "47185920",
        },
        arch = "arm64",
    )

    # modules_staging_archive from <name>
    native.filegroup(
        name = name + "_modules_staging_archive",
        srcs = [name],
        output_group = "modules_staging_archive",
    )

    # All GKI modules
    native.filegroup(
        name = name + "_modules",
        srcs = [
            "{}/{}".format(name, module)
            for module in (_gki_modules_list or [])
        ],
    )

    # The purpose of this target is to allow device kernel build to include reasonable
    # defaults of artifacts from GKI. Hence, this target includes everything in name + "_dist",
    # excluding the following:
    # - UAPI headers, because device-specific external kernel modules may install different
    #   headers.
    # - DDK; see _ddk_artifacts below.
    native.filegroup(
        name = name + "_additional_artifacts",
        srcs = [
            # Sync with additional_artifacts_items
            name + "_headers",
            name + "_images",
            name + "_kmi_symbol_list",
            name + "_raw_kmi_symbol_list",
        ],
    )

    # Everything in name + "_dist" for the DDK.
    # These are necessary for driver development. Hence they are also added to
    # kernel_*_dist so they can be downloaded.
    ddk_artifacts = [
        name + "_download_configs",
        name + "_filegroup_declaration",
        name + "_unstripped_modules_archive",
    ]
    native.filegroup(
        name = name + "_ddk_artifacts",
        srcs = ddk_artifacts,
    )

    dist_targets = [
        name,
        name + "_uapi_headers",
        name + "_additional_artifacts",
        name + "_ddk_artifacts",
        name + "_modules",
        name + "_modules_install",
        # BUILD_GKI_CERTIFICATION_TOOLS=1 for all kernel_build defined here.
        Label("//build/kernel:gki_certification_tools"),
        "build.config.constants",
        Label("//build/kernel:init_ddk_zip"),
    ]

    copy_to_dist_dir(
        name = name + "_dist",
        data = dist_targets,
        flat = True,
        dist_dir = "out/{name}/dist".format(name = name),
        log = "info",
    )

    kernel_compile_commands(
        name = name + "_compile_commands",
        deps = [name],
    )
