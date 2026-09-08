# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

"""Kleaf build rule for android-desktop image generation."""

load("//build/kernel/kleaf:hermetic_tools.bzl", "hermetic_genrule")

def define_android_desktop_build(name, variant, base_kernel, dtbo_target_name = None):
    """Define the genrule that generates android-desktop_image.bin.

    Returns the label of the genrule so the caller can add it to the
    target's standard {name}_{variant}_dist data list. This way the
    android-desktop_image.bin is included in the same copy_to_dist_dir
    as all other build artifacts — no separate dist rule, no ordering
    concerns, no changes to build_with_bazel.py.

    Args:
        name:             MSM target name (e.g. "glymur")
        variant:          Build variant (e.g. "consolidate")
        base_kernel:      Label string of the base GKI kernel build
                          (e.g. "//soc-repo:kernel_aarch64_consolidate").
                          The gki_artifacts for this kernel expose boot-lz4.img
                          via the "boot_lz4" output group.
        dtbo_target_name: Name of the custom dtbo image entry in
                          custom_dtbo_img_list (defaults to `name`).

    Returns:
        Label string of the genrule to add to dist_data.
    """
    stem = "{}_{}".format(name, variant)
    dtbo_name = dtbo_target_name or name

    # Extract boot-lz4.img from the gki_artifacts output group.
    native.filegroup(
        name = "{}_desktop_boot_lz4".format(stem),
        srcs = ["{}_gki_artifacts".format(base_kernel)],
        output_group = "boot_lz4",
    )

    # hermetic_genrule: extract the android_desktop tarball and invoke the shell script.
    # avbtool and mkbootimg are declared as tools so Bazel includes them and
    # their runfiles in the sandbox.
    #
    # HOST_DIR is set to the tarball's host/linux-x86/bin/ directory so that
    # pack_image finds all its sibling scripts (partition-common-sgdisk.sh,
    # partition-common.sh, partition-script.sh, sgdisk, etc.) via $SCRIPT_DIR.
    # avbtool and mkbootimg are overwritten in-place with thin wrapper scripts
    # that exec the Bazel-managed binaries from their original sandbox paths,
    # keeping $ORIGIN-relative lib resolution intact — no "no-sandbox" needed.
    #
    # init_boot.img and vendor_boot.img live in the DefaultInfo of {stem}_images
    # but are not exposed via a named output_group. Include the whole images
    # target in srcs and locate each file by basename in the cmd shell loop.
    hermetic_genrule(
        name = "{}_android_desktop_image".format(stem),
        srcs = [
            "//prebuilts/qcom_boot_artifacts:{}_android_desktop.tar.gz".format(name),
            "//prebuilts/qcom_boot_artifacts:init_boot.img",
            ":{}_desktop_boot_lz4".format(stem),
            ":{stem}_{dtbo_name}_dtbo_image".format(stem = stem, dtbo_name = dtbo_name),
            ":{}_unsparsed_image".format(stem),
            ":{}_images".format(stem),
            "//build/kernel:android/generate_android_desktop_bin_noavb.sh",
        ],
        tools = [
            "//prebuilts/kernel-build-tools:avbtool",
            "//tools/mkbootimg:mkbootimg.py",
        ],
        outs = ["{}/android-desktop_image.bin".format(stem)],
        cmd = """
            set -euo pipefail
            WORK=$$(dirname $@)
            DIST=$$WORK/dist
            mkdir -p $$WORK/tmp
            export TMPDIR=$$WORK/tmp

            tar -xzf $(location //prebuilts/qcom_boot_artifacts:{name}_android_desktop.tar.gz) \\
                -C $$WORK
            PREBUILTS_DIR=$$WORK/android_desktop/prebuilt_bins

            # Use the tarball's bin dir as HOST_DIR so pack_image finds all its
            # sibling scripts via $$SCRIPT_DIR. Overwrite only avbtool and
            # mkbootimg with wrapper scripts that exec the Bazel-managed
            # binaries from their original sandbox paths.
            # 'file' is not in the hermetic toolchain; it is only used for
            # diagnostic output in the script summary. Shim it so the build
            # does not fail in sandboxed environments.
            SHIMS=$$WORK/shims
            mkdir -p $$SHIMS
            printf '#!/bin/sh\necho "(unavailable in hermetic build)"\n' > "$$SHIMS/file"
            chmod +x "$$SHIMS/file"
            export PATH="$$SHIMS:$$PATH"

            HOST_DIR=$$WORK/android_desktop/host/linux-x86/bin
            AVBTOOL_PATH=$$(realpath "$(location //prebuilts/kernel-build-tools:avbtool)")
            MKBOOTIMG_PATH=$$(realpath "$(location //tools/mkbootimg:mkbootimg.py)")
            printf '#!/bin/bash\\nexec "%s" "$$@"\\n' "$$AVBTOOL_PATH"  > "$$HOST_DIR/avbtool"
            printf '#!/bin/bash\\nexec python3 "%s" "$$@"\\n' "$$MKBOOTIMG_PATH" > "$$HOST_DIR/mkbootimg"
            chmod +x "$$HOST_DIR/avbtool" "$$HOST_DIR/mkbootimg"

            mkdir -p $$DIST
            cp $(location :{stem}_desktop_boot_lz4)       $$DIST/boot-lz4.img
            cp $(location :{stem}_{dtbo_name}_dtbo_image) $$DIST/{dtbo_name}_dtbo.img
            cp $(location :{stem}_unsparsed_image)         $$DIST/super_unsparsed.img
            cp $(location //prebuilts/qcom_boot_artifacts:init_boot.img) $$DIST/init_boot.img

            for f in $(locations :{stem}_images); do
                case $$(basename $$f) in
                    vendor_boot.img) cp $$f $$DIST/vendor_boot.img ;;
                esac
            done

            bash $(location //build/kernel:android/generate_android_desktop_bin_noavb.sh) \\
                --dist      $$DIST \\
                --dtbo      {dtbo_name}_dtbo.img \\
                --host      $$HOST_DIR \\
                --prebuilts $$PREBUILTS_DIR \\
                --stage     $$WORK
        """.format(
            name = name,
            stem = stem,
            dtbo_name = dtbo_name,
        ),
    )

    return ":{}_android_desktop_image".format(stem)
