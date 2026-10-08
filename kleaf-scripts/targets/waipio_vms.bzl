load("//build:msm_kernel_extensions.bzl", "define_combined_vm_image", "define_extras", "get_dtb_list")
load("//build/bazel_common_rules/dist:dist.bzl", "copy_to_dist_dir")
load(":kleaf-scripts/image_opts.bzl", "vm_image_opts")
load(":kleaf-scripts/msm_common.bzl", "get_out_dir")
load(":kleaf-scripts/msm_dtc.bzl", "define_dtc_dist")
load(":target_variants.bzl", "vm_types", "vm_variants")

target_name = "waipio-vms"

def define_waipio_vms(vm_image_opts = vm_image_opts()):
    vm_types = ["tuivm"]
    base_target = "waipio-tuivm"
    for variant in vm_variants:
        base_tv = "{}_{}".format(base_target, variant)

        dtb_list = get_dtb_list(base_target)
        compiled_dtbs = ["//soc-repo:waipio-{}_{}_dtb_build/{}".format(vt, variant, t) for vt in vm_types for t in dtb_list]

        if variant == "debug-defconfig":
            base_kernel = "kernel_aarch64_qtvm_debug"
        else:
            base_kernel = "kernel_aarch64_qtvm"

        out_dtb_list = [":waipio-{}_{}_vm_dtb_img".format(vt, variant) for vt in vm_types]
        dist_targets = (
            ["waipio-{}_{}_vm_dist".format(vt, variant) for vt in vm_types] +
            ["waipio-{}_{}_dist".format(vt, variant) for vt in vm_types] +
            [":waipio-{}_{}_modules_install".format(vt, variant) for vt in vm_types] +
            [":waipio-{}_{}_signed_modules".format(vt, variant) for vt in vm_types] +
            ["waipio-{}_{}_merge_msm_uapi_headers".format(vt, variant) for vt in vm_types] +
            [base_kernel] +
            [":signing_key"] +
            [":verity_key"] +
            [":waipio-{}_{}_dtb_build".format(vt, variant) for vt in vm_types]
        ) + out_dtb_list

        copy_to_dist_dir(
            name = "{}_{}_dist".format(target_name, variant),
            data = dist_targets + compiled_dtbs,
            dist_dir = "{}/dist".format(get_out_dir(target_name, variant)),
            flat = True,
            wipe_dist_dir = True,
            allow_duplicate_filenames = True,
            mode_overrides = {
                "**/vmlinux": "755",
                "**/Image": "755",
                "**/*.dtb*": "755",
                "**/gen_init_cpio": "755",
                "**/sign-file": "755",
                "**/*": "644",
            },
        )

        copy_to_dist_dir(
            name = "{}_{}_host_dist".format(target_name, variant),
            data = [
                ":gen-headers_install.sh",
                ":unifdef",
            ],
            dist_dir = "{}/host".format(get_out_dir(target_name, variant)),
            flat = True,
            log = "info",
        )

        archive_targets = ["waipio-{}_{}_unstripped_modules_tar".format(vt, variant) for vt in vm_types]
        copy_to_dist_dir(
            name = "{}_{}_um_dist".format(target_name, variant),
            archives = archive_targets,
            dist_dir = "{}/dist".format(get_out_dir(target_name, variant)),
            flat = True,
            wipe_dist_dir = False,
            allow_duplicate_filenames = True,
            log = "info",
        )

        define_dtc_dist("{}_{}".format(target_name, variant), target_name, variant)
        define_extras(base_tv, kbuild_config = base_kernel, alias = "{}_{}".format(target_name, variant))
        define_combined_vm_image(target_name, variant, vm_image_opts.vm_size_ext4)
