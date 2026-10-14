#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

import argparse
import concurrent.futures
import errno
import glob
import hashlib
import json
import logging
import os
import re
import sys
import subprocess
import shutil
import threading

HOST_TARGETS = ["dtc", "host"]
PHONY_TARGETS = ["16k"]
DEFAULT_SKIP_LIST = []
MSM_EXTENSIONS = "build/msm_kernel_extensions.bzl"
ABL_EXTENSIONS = "build/abl_extensions.bzl"
DEFAULT_MSM_EXTENSIONS_SRC = "../soc-repo/kleaf-scripts/msm_kernel_extensions.bzl"
DEFAULT_ABL_EXTENSIONS_SRC = "../bootable/bootloader/edk2/abl_extensions.bzl"
DEFAULT_OUT_DIR = "{workspace}/out/msm-kernel-{target}-{variant}"
GH_VARIANTS =["perf", "consolidate"]

CURR_DIR = os.getcwd()
DEFAULT_CACHE_DIR = os.path.join(CURR_DIR, 'bazel-cache')
if not os.path.exists(DEFAULT_CACHE_DIR):
   os.makedirs(DEFAULT_CACHE_DIR)

os.environ['TEST_TMPDIR'] = DEFAULT_CACHE_DIR

# Version token - bump whenever the query cache format changes
_QUERY_CACHE_VERSION = 1

# Max parallel workers for the dist phase.  Dist scripts just copy files from
# the Bazel output tree, so this is I/O-bound; 8 is a safe default that keeps
# throughput high without saturating the disk or /tmp.
_MAX_DIST_WORKERS = 8

class Target:
    def __init__(self, workspace, target, variant, bazel_label, out_dir=None):
        self.workspace = workspace
        self.target = target
        self.variant = variant
        self.bazel_label = bazel_label
        self.out_dir = out_dir

    def __lt__(self, other):
        return len(self.bazel_label) < len(other.bazel_label)

    def get_out_dir(self, suffix=None):
        if self.out_dir:
            out_dir = self.out_dir
        else:
            # Mirror the logic in msm_common.bzl:get_out_dir()
            if "allyes" in self.target:
                target_norm = self.target.replace("_", "-")
            else:
                target_norm = self.target.replace("-", "_")

            variant_norm = self.variant.replace("-", "_")

            out_dir = DEFAULT_OUT_DIR.format(
                workspace = self.workspace, target=target_norm, variant=variant_norm
            )

        if suffix:
            return os.path.join(out_dir, suffix)
        else:
            return out_dir

class BazelBuilder:
    """Helper class for building with Bazel"""

    def __init__(
            self, target_list, skip_list, out_dir, cache_dir,
            dry_run, target_build_variant, user_opts):
        self.workspace = os.path.realpath(
            os.path.join(os.path.dirname(os.path.realpath(__file__)), "..")
        )
        self.bazel_bin = os.path.join(self.workspace, "tools", "bazel")
        if not os.path.exists(self.bazel_bin):
            logging.error("failed to find Bazel binary at %s", self.bazel_bin)
            sys.exit(1)
        self.kernel_dir = os.path.basename(
            (os.path.dirname(os.path.realpath(__file__)))
        )

        for t, v in target_list:
            if not t or not v:
                logging.error("invalid target_variant combo \"%s_%s\"", t, v)
                sys.exit(1)

        self.output_user_root = "--output_user_root=" + os.path.join(cache_dir, "bazel-cache")
        self.output_root = "--output_root=" + cache_dir
        self.cache_dir = cache_dir
        self.target_list = target_list
        self.skip_list = skip_list
        self.dry_run = dry_run
        self.target_build_variant = target_build_variant
        self.user_opts = user_opts
        self.process_list = []
        if len(self.target_list) > 1 and out_dir:
            logging.error("cannot specify multiple targets with one out dir")
            sys.exit(1)
        else:
            self.out_dir = out_dir

        self.setup_extensions()

    def __del__(self):
        for proc in self.process_list:
            try:
                proc.kill()
                proc.wait()
            except OSError:
                pass

    def setup_extensions(self):
        """Set up the extension files if needed"""
        for (ext, def_src) in [
            (MSM_EXTENSIONS, DEFAULT_MSM_EXTENSIONS_SRC),
            (ABL_EXTENSIONS, DEFAULT_ABL_EXTENSIONS_SRC),
        ]:
            ext_path = os.path.join(self.workspace, ext)
            # If the file doesn't exist or is a dead link, link to the default
            try:
                os.stat(ext_path)
            except OSError as e:
                if e.errno == errno.ENOENT:
                    logging.info(
                        "%s does not exist or is a broken symlink... linking to default at %s",
                        ext,
                        def_src,
                    )
                    if os.path.islink(ext_path):
                        os.unlink(ext_path)
                    os.symlink(def_src, ext_path)
                else:
                    raise e

    def _build_files_hash(self):
        """Hash every BUILD/bzl file under kernel_dir.

        Any change that affects the dist-target set - a BUILD rule edit, a new
        target added, or a target removed - will change this hash and force a
        fresh Bazel query on the next run.
        """
        kernel_path = os.path.join(self.workspace, self.kernel_dir)
        h = hashlib.sha256()
        seen = set()
        for root, dirs, files in os.walk(kernel_path, followlinks=True):
            dirs.sort()
            for fname in sorted(files):
                if fname not in ("BUILD", "BUILD.bazel") and not fname.endswith(".bzl"):
                    continue
                fpath = os.path.join(root, fname)
                real = os.path.realpath(fpath)
                if real in seen:
                    continue
                seen.add(real)
                rel = os.path.relpath(fpath, kernel_path)
                h.update(rel.encode())
                try:
                    with open(fpath, "rb") as f:
                        h.update(f.read())
                except OSError:
                    pass
        # Also hash the extension bzl files (and their symlink targets)
        # since they live outside kernel_dir and may be replaced or broken
        for ext in (MSM_EXTENSIONS, ABL_EXTENSIONS):
            ext_path = os.path.join(self.workspace, ext)
            real_path = os.path.realpath(ext_path)
            # Hash both path strings for cache sensitivity to symlink changes
            for candidate in (ext_path, real_path):
                rel = os.path.relpath(candidate, self.workspace)
                h.update(rel.encode())
            # Read file content only once (via the real path)
            if real_path not in seen:
                seen.add(real_path)
                try:
                    with open(real_path, "rb") as f:
                        h.update(f.read())
                except OSError:
                    h.update(b"missing")
        return h.hexdigest()[:16]

    def _query_cache_key(self):
        """Stable key over the inputs that determine the target list."""
        content = "{}:{}:{}:{}:{}".format(
            _QUERY_CACHE_VERSION,
            self.kernel_dir,
            ",".join("{}:{}".format(t, v) for t, v in sorted(self.target_list)),
            ",".join(sorted(self.skip_list)),
            self._build_files_hash(),
        )
        return hashlib.sha256(content.encode()).hexdigest()[:16]

    def get_build_targets(self):
        """Query for build targets, using a disk cache to avoid repeated Bazel queries."""
        cache_file = os.path.join(
            self.cache_dir, "target_query_cache_{}.json".format(self._query_cache_key())
        )

        if os.path.exists(cache_file):
            try:
                with open(cache_file) as f:
                    cached = json.load(f)
                if cached.get("version") == _QUERY_CACHE_VERSION:
                    logging.info("Using cached build targets (skipping Bazel query).")
                    targets = [
                        Target(t["workspace"], t["target"], t["variant"], t["bazel_label"])
                        for t in cached["targets"]
                    ]
                    targets.sort()
                    return targets
            except Exception as e:
                logging.debug("Query cache read failed (%s); re-running query.", e)

        logging.info("Querying build targets...")

        targets = []
        for t, v in self.target_list:
            if v == "ALL":
                if self.out_dir:
                    logging.error("cannot specify multiple targets (ALL variants) with one out dir")
                    sys.exit(1)

                skip_list_re = [
                    re.compile(r"//{}:{}_.*_{}_dist".format(self.kernel_dir, t, s))
                    for s in self.skip_list
                ]
                query = 'filter("{}_.*_dist$", attr(generator_function, define_{}, {}/...))'.format(
                    t, t.replace("-", "_"), self.kernel_dir
                )
            else:
                skip_list_re = [
                    re.compile(r"//{}:{}_{}_{}_dist".format(self.kernel_dir, t, v, s))
                    for s in self.skip_list
                ]
                query = 'filter("{}_{}(.*_dist)?$", attr(generator_function, define_{}, {}/...))'.format(
                    t, v, t.replace("-", "_"), self.kernel_dir
                )

            cmdline = [
                self.bazel_bin,
                self.output_root,
                self.output_user_root,
                "query",
                "--ui_event_filters=-info",
                "--noshow_progress",
                query,
            ]

            logging.debug('Running "%s"', " ".join(cmdline))

            try:
                query_cmd = subprocess.Popen(
                    cmdline, cwd=self.workspace, stdout=subprocess.PIPE
                )
                self.process_list.append(query_cmd)
                label_list = [l.decode("utf-8") for l in query_cmd.stdout.read().splitlines()]
            except Exception as e:
                logging.error(e)
                sys.exit(1)

            self.process_list.remove(query_cmd)

            if not label_list:
                logging.error(
                    "failed to find any Bazel targets for target/variant combo %s_%s",
                    t,
                    v,
                )
                sys.exit(1)

            for label in label_list:
                if any((skip_re.match(label) for skip_re in skip_list_re)):
                    continue

                if v == "ALL":
                    real_variant = re.search(
                        r"//{}:{}_([^_]+)_".format(self.kernel_dir, t), label
                    ).group(1)
                else:
                    real_variant = v

                targets.append(
                    Target(self.workspace, t, real_variant, label, self.out_dir)
                )
                logging.debug("Adding target %s", label)

        # Sort build targets by label string length to guarantee the base target goes
        # first when copying to output directory
        targets.sort()

        # Persist the query result so subsequent runs skip this Bazel query entirely.
        try:
            os.makedirs(self.cache_dir, exist_ok=True)
            with open(cache_file, "w") as f:
                json.dump({
                    "version": _QUERY_CACHE_VERSION,
                    "targets": [
                        {"workspace": t.workspace, "target": t.target,
                         "variant": t.variant, "bazel_label": t.bazel_label}
                        for t in targets
                    ],
                }, f)
        except Exception as e:
            logging.debug("Query cache write failed (%s); continuing without cache.", e)

        return targets

    def clean_legacy_generated_files(self):
        """Clean generated files from legacy build to avoid conflicts with Bazel"""
        for f in glob.glob(
                "{}/soc-repo/arch/arm64/configs/vendor/*_defconfig".format(
                    self.workspace)):
            os.remove(f)

        f = os.path.join(
            self.workspace, "bootable", "bootloader", "edk2",
            "Conf", ".AutoGenIdFile.txt")
        if os.path.exists(f):
            os.remove(f)

        for pyc in glob.iglob(
                os.path.join(self.workspace, 'bootable', '**', '*.pyc'),
                recursive=True):
            os.remove(pyc)

    def bazel(
        self,
        bazel_subcommand,
        targets,
        extra_options=None,
        bazel_target_opts=None,
    ):
        """Execute a bazel command"""
        if os.environ.get("BAZEL_BUILD_TRACER"):
            pkg_path = os.environ.get("PATH_TO_FILER")
            cmd = [
                "python3",
                os.path.join(pkg_path, "init_bazel_tracing.py"),
                "--working-dir", os.getcwd(),
            ]
            print("Running %s" % " ".join(cmd))
            cmd_proc = subprocess.Popen(cmd)
            self.process_list.append(cmd_proc)
            cmd_proc.wait()
            try:
                if cmd_proc.returncode != 0:
                    print("BAZEL_BUILD_TRACER: Failed to run %s" % " ".join(cmd))
                    sys.exit(cmd_proc.returncode)
            except Exception as e:
                logging.error(e)
                sys.exit(1)
            print("BAZEL_BUILD_TRACER: Tracer has been initialized")
        cmdline = [self.bazel_bin, self.output_root, self.output_user_root, bazel_subcommand]
        logging.info('targets = "%s"', [t.bazel_label for t in targets])
        if extra_options:
            cmdline.extend(extra_options)
        cmdline.extend([t.bazel_label for t in targets])
        if bazel_target_opts is not None:
            cmdline.extend(["--"] + bazel_target_opts)

        cmdline_str = " ".join(cmdline)
        try:
            logging.info('Running "%s"', cmdline_str)
            build_proc = subprocess.Popen(cmdline, cwd=self.workspace)
            self.process_list.append(build_proc)
            build_proc.wait()
            if build_proc.returncode != 0:
                sys.exit(build_proc.returncode)
        except Exception as e:
            logging.error(e)
            sys.exit(1)

        self.process_list.remove(build_proc)

    def build_targets(self, targets):
        """Run "bazel build" on all targets in parallel"""
        self.bazel("build", targets, extra_options=self.user_opts)

    def _get_bazel_bin(self):
        """Return the bazel-bin path by querying the warm Bazel server (~1s)."""
        try:
            out = subprocess.check_output(
                [self.bazel_bin, self.output_root, self.output_user_root,
                 "info", "bazel-bin"],
                cwd=self.workspace, stderr=subprocess.DEVNULL,
            )
            return out.decode().strip()
        except subprocess.CalledProcessError as e:
            logging.error("bazel info bazel-bin failed: %s", e)
            sys.exit(1)

    def run_targets(self, targets):
        """Run dist targets in parallel by executing the built scripts directly.

        After bazel build the dist executables already exist under bazel-bin.
        Running them directly avoids spawning N cold Bazel servers and the
        I/O contention and /tmp exhaustion that caused the parallel-server
        approach to regress build time by 3-15x.
        """
        bazel_bin = self._get_bazel_bin()
        opts_content = ("\n".join(self.user_opts) + "\n") if self.user_opts else "\n"

        def _get_out_dir(target):
            if any(
                re.match(r"//{}:.*_{}_dist".format(self.kernel_dir, h), target.bazel_label)
                for h in HOST_TARGETS
            ):
                return target.get_out_dir("host")
            elif any(
                re.match(r"//{}:.*{}.*_dist".format(self.kernel_dir, t), target.bazel_label)
                for t in PHONY_TARGETS
            ):
                return os.path.join(target.get_out_dir() + "16k", "dist")
            return target.get_out_dir("dist")

        def _run_one(target):
            # //soc-repo:art_defconfig_dist -> {bazel_bin}/soc-repo/art_defconfig_dist
            pkg  = target.bazel_label[2:].split(":")[0]
            name = target.bazel_label.split(":")[1]
            script = os.path.join(bazel_bin, pkg, name)

            # Fallback 1: cross-package targets whose scripts live outside
            # the default soc-repo/ path in bazel-bin.
            if not os.path.isfile(script):
                result = subprocess.run(
                    ["find", bazel_bin, "-maxdepth", "5",
                     "-name", name, "-type", "f"],
                    capture_output=True, text=True,
                )
                found = result.stdout.strip().split("\n")[0]
                if found and os.path.isfile(found):
                    script = found

            # Fallback 2: alias targets (e.g. art16k_perf_abl_dist aliases
            # art_perf_abl_dist - no dedicated executable exists in bazel-bin).
            # Fall back to bazel run on the warm server, serialised by a lock
            # since the server handles one command at a time.
            if not os.path.isfile(script):
                return _run_via_bazel(target)

            out_dir = _get_out_dir(target)
            os.makedirs(out_dir, exist_ok=True)

            env = os.environ.copy()
            runfiles_dir = script + ".runfiles"
            if os.path.isdir(runfiles_dir):
                env["RUNFILES_DIR"] = runfiles_dir

            # Use per-target runfiles dir as cwd so all manifest
            # relative paths resolve correctly via symlinks
            with _dir_locks_lock:
                if out_dir not in _dir_locks:
                    _dir_locks[out_dir] = threading.Lock()
                _out_dir_lock = _dir_locks[out_dir]
            with _out_dir_lock:
                runfiles_main = os.path.join(runfiles_dir, '_main')
                script_cwd = runfiles_main if os.path.isdir(runfiles_main) else bazel_bin
                proc = subprocess.Popen(
                    [script, "--destdir", out_dir],
                    cwd=script_cwd, env=env,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                )
                stdout, _ = proc.communicate()
                for line in stdout.decode("utf-8", errors="replace").splitlines():
                    logging.info(line)
                return proc.returncode, target, out_dir

        _bazel_lock = threading.Lock()
        _dir_locks = {}
        _dir_locks_lock = threading.Lock()

        def _run_via_bazel(target):
            """Warm-server bazel run for alias targets that have no own executable."""
            out_dir = _get_out_dir(target)
            os.makedirs(out_dir, exist_ok=True)
            cmdline = (
                [self.bazel_bin, self.output_root, self.output_user_root, "run"]
                + self.user_opts
                + [target.bazel_label, "--", "--destdir", out_dir]
            )
            with _bazel_lock:
                proc = subprocess.Popen(
                    cmdline, cwd=self.workspace,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                )
                stdout, _ = proc.communicate()
            for line in stdout.decode("utf-8", errors="replace").splitlines():
                logging.info(line)
            return proc.returncode, target, out_dir

        workers = min(len(targets), _MAX_DIST_WORKERS)
        logging.info(
            "Running %d dist targets in parallel (%d workers).", len(targets), workers)
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            results = list(pool.map(_run_one, targets))

        failed = [(t, rc) for rc, t, _ in results if rc != 0]
        if failed:
            for t, rc in failed:
                logging.error("Dist target failed (rc=%d): %s", rc, t.bazel_label)
            sys.exit(failed[0][1])

        for _, target, out_dir in results:
            self.write_opts(out_dir, opts_content)
            if out_dir == target.get_out_dir("dist"):
                self.setup_kbdev_symlinks(out_dir)

    def setup_kbdev_symlinks(self, out_dir):
        """Setup k*.img sylinks needed for test builds"""
        images = [
            "abl.elf", "boot.img", "dtbo.img",
            "init_boot.img", "super.img", "vendor_boot.img",
        ]
        for img in images:
            src_path = os.path.join(out_dir, img)
            dst_path = os.path.join(out_dir, "k" + img)
            dst_exists = os.path.islink(dst_path) or os.path.exists(dst_path)
            if os.path.isfile(src_path) and not dst_exists:
                try:
                    os.symlink(src_path, dst_path)
                except OSError as e:
                    logging.warning("Failed to create symlink for %s: %s", img, e)

    def run_menuconfig(self):
        """Run menuconfig on all target-variant combos class is initialized with"""
        for t, v in self.target_list:
            menuconfig_label = "//{}:{}_{}_config".format(self.kernel_dir, t, v)
            menuconfig_target = [Target(self.workspace, t, v, menuconfig_label, self.out_dir)]
            self.bazel("run", menuconfig_target, bazel_target_opts=["menuconfig"])

    def build_run_gbl(self):
        """Build the GBL target using the existing Bazel interface"""
        gbl_target = Target(
            self.workspace,
            target="bootloader",
            variant="gbl",
            bazel_label="//bootable/libbootloader:gbl_efi_dist"
        )
        self.bazel(
            "run",
            [gbl_target],
            extra_options=["--config=gbl"],
        )

    def write_opts(self, out_dir, content=None):
        if content is None:
            content = ("\n".join(self.user_opts) + "\n") if self.user_opts else "\n"
        with open(os.path.join(out_dir, "build_opts.txt"), "w") as opt_file:
            opt_file.write(content)

    def build(self):
        """Determine which targets to build, then build them"""
        targets_to_build = self.get_build_targets()
        self._targets = targets_to_build

        if not targets_to_build:
            logging.error("no targets to build")
            sys.exit(1)

        for user_opt in self.user_opts:
            if "--lto" in user_opt:
                logging.error("--lto is not supported now, please remove")
                sys.exit(1)

        if self.skip_list:
            self.user_opts.extend([
                "--//soc-repo:skip_{}=true".format(s)
                for s in self.skip_list if s != 'abi'])

        self.user_opts.append("--incompatible_sandbox_hermetic_tmp=false")
        self.user_opts.append("--noenable_workspace")
        self.user_opts.append(
            "--override_module=rules_kotlin="
            "%workspace%/build/kernel/kleaf/bzlmod/fake_modules/rules_kotlin")
        self.user_opts.append(
            "--override_module=protobuf="
            "%workspace%/build/kernel/kleaf/bzlmod/fake_modules/protobuf")
        self.user_opts.append(
            "--override_module=rules_java="
            "%workspace%/build/kernel/kleaf/bzlmod/fake_modules/rules_java")
        self.user_opts.append("--symlink_prefix=/")

        if self.target_build_variant:
          self.user_opts.extend([
              "--//bootable/bootloader/edk2:target_build_variant={}".format(
                  self.target_build_variant)])
          logging.info('The target_build_variant = %s', self.target_build_variant)

        if self.dry_run:
            self.user_opts.append("--nobuild")

        logging.info(
            "Building the following targets:\n%s",
            "\n".join([t.bazel_label for t in targets_to_build])
        )

        self.clean_legacy_generated_files()

        logging.info("Building targets...")
        self.build_targets(targets_to_build)

        if not self.dry_run:
            self.run_targets(targets_to_build)

def build_gvm_image(variant):
    workspace = os.path.realpath(
        os.path.join(os.path.dirname(os.path.realpath(__file__)), "..")
    )
    VM_BOOTLOADER_SRC = os.path.join(
        workspace,
        "prebuilts",
        "qcom_boot_artifacts",
        "vm-bootloader",
        "gvm-pilsplitter.sh",
    )

    if not os.path.exists(VM_BOOTLOADER_SRC):
        logging.info("gvm-pilsplitter.sh not found at prebuilts path")
        VM_BOOTLOADER_SRC = None
        for root, dirs, files in os.walk('.'):
            for file in files:
                if file == "gvm-pilsplitter.sh":
                    VM_BOOTLOADER_SRC = os.path.join(root, file)
                    break
            if VM_BOOTLOADER_SRC:
                break

    if VM_BOOTLOADER_SRC != None:
        if variant == "ALL":
            for gh_variant in GH_VARIANTS:
                subprocess.check_call([VM_BOOTLOADER_SRC, gh_variant])
        else:
            subprocess.check_call([VM_BOOTLOADER_SRC, variant])

def main():
    """Main script entrypoint"""
    parser = argparse.ArgumentParser(description="Build kernel platform with Bazel")

    parser.add_argument(
        "-t",
        "--target",
        metavar=("TARGET", "VARIANT"),
        action="append",
        nargs=2,
        required=True,
        help=('Target and variant to build (e.g. -t kalama gki).'
              ' May be passed multiple times.'
              ' A special VARIANT may be passed, "ALL",'
              ' which will build all variants for a particular target'),
    )
    parser.add_argument(
        "-s",
        "--skip",
        metavar="BUILD_RULE",
        action="append",
        default=[],
        help=("Skip specific build rules (e.g. --skip abl will skip"
              " the //soc-repo:<target>_<variant>_abl build)"),
    )
    parser.add_argument(
        "-o",
        "--out_dir",
        metavar="OUT_DIR",
        help=('Specify the output distribution directory'
              ' (by default, "$PWD/out/msm-kernel-<target>-variant")'),
    )
    parser.add_argument(
        "--log",
        metavar="LEVEL",
        default="info",
        choices=["debug", "info", "warning", "error"],
        help="Log level (debug, info, warning, error)",
    )
    parser.add_argument(
        "-c",
        "--menuconfig",
        action="store_true",
        help="Run menuconfig for <target>-<variant> and exit without building",
    )
    parser.add_argument(
        "-d",
        "--dry-run",
        action="store_true",
        help="Perform a dry-run of the build which will perform loading/analysis of build files",
    )
    parser.add_argument(
        "--cache_dir",
        metavar="CACHE_DIR",
        default=DEFAULT_CACHE_DIR,
        help='Specify the bazel cache directory (defaults to ' + DEFAULT_CACHE_DIR + ')'
    )
    parser.add_argument(
        "-g",
        "--gki-headers",
        action="store_true",
        help="(DEPRECATED) Compile with common headers instead of msm-kernel"
    )
    parser.add_argument(
        "--build_gbl",
        action="store_true",
        help="Compile GBL"
    )
    parser.add_argument(
        "--target_build_variant",
        choices=["userdebug", "user", "eng"],
        help="target build variant (userdebug, user, eng)",
    )

    args, user_opts = parser.parse_known_args(sys.argv[1:])

    logging.basicConfig(
        level=getattr(logging, args.log.upper()),
        format="[{}] %(levelname)s: %(message)s".format(os.path.basename(sys.argv[0])),
    )

    args.skip.extend(DEFAULT_SKIP_LIST)

    if args.gki_headers:
        logging.warning("--gki-headers/-g option is deprecated.")


    builder = BazelBuilder(
        args.target,
        args.skip,
        args.out_dir,
        args.cache_dir,
        args.dry_run,
        args.target_build_variant,
        user_opts
    )
    try:
        if args.menuconfig:
            builder.run_menuconfig()
        elif args.build_gbl:
            builder.build_run_gbl()
        else:
            builder.build()
    except KeyboardInterrupt:
        logging.info("Received keyboard interrupt... exiting")
        del builder
        sys.exit(1)

    if args.dry_run:
        logging.info("Dry-run completed successfully!")
    else:
        for target in args.target:
            if target[0] == "autogvm":
                if target[1] != "defconfig" and target[1] != "debug-defconfig":
                    build_gvm_image(target[1])
        logging.info("Build completed successfully!")

if __name__ == "__main__":
    main()
