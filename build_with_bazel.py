#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

import argparse
import errno
import glob
import hashlib
import json
import logging
import os
import re
import sys
import subprocess
import concurrent.futures
import shutil
import threading

HOST_TARGETS = ["dtc"]
PHONY_TARGETS = ["16k"]
SUPPLEMENTARY_SUFFIXES = ("_abl_dist", "_kti_dist", "_vm_images_dist")
DEFAULT_SKIP_LIST = ["abi"]
MSM_EXTENSIONS = "build/msm_kernel_extensions.bzl"
ABL_EXTENSIONS = "build/abl_extensions.bzl"
DEFAULT_MSM_EXTENSIONS_SRC = "../msm-kernel/msm_kernel_extensions.bzl"
DEFAULT_ABL_EXTENSIONS_SRC = "../bootable/bootloader/edk2/abl_extensions.bzl"
DEFAULT_OUT_DIR = "{workspace}/out/msm-kernel-{target}-{variant}"

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
            dry_run, gki_headers, user_opts):
        BazelBuilder.targets_done = []
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
        self.bazel_cache = "--output_user_root=" + cache_dir
        self.target_list = target_list
        self.skip_list = skip_list
        self.dry_run = dry_run
        self.gki_headers = gki_headers
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
        if self.out_dir and any(v == "ALL" for _, v in self.target_list):
            logging.error("cannot specify multiple targets (ALL variants) with one out dir")
            sys.exit(1)

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
                        Target(
                            t["workspace"],
                            t["target"],
                            t["variant"],
                            t["bazel_label"],
                            self.out_dir,
                        )
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
                skip_list_re = [
                    re.compile(r"//{}:{}[^-]*_.*_{}_dist".format(self.kernel_dir, t, s))
                    for s in self.skip_list
                ]
                query = (
                    'filter("{}[^-]*_.*_dist$",'
                    ' attr(generator_function, define_msm_platforms, {}/...))'
                ).format(
                    t, self.kernel_dir)
            else:
                skip_list_re = [
                    re.compile(r"//{}:{}[^-]*_{}_{}_dist".format(self.kernel_dir, t, v, s))
                    for s in self.skip_list
                ]
                query = (
                    'filter("{}[^-]*_{}.*_dist$",'
                    ' attr(generator_function, define_msm_platforms, {}/...))'
                ).format(
                    t, v, self.kernel_dir)

            cmdline = [
                self.bazel_bin,
                self.bazel_cache,
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
                        r"//{}:[^_]*_([^_]+)_".format(self.kernel_dir), label
                    ).group(1)
                else:
                    real_variant = v

                targets.append(
                    Target(self.workspace, t, real_variant, label, self.out_dir)
                )

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
                "{}/msm-kernel/arch/arm64/configs/vendor/*_defconfig".format(
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
            curr_targets = [t.bazel_label for t in targets]
            num_targets_done = len(BazelBuilder.targets_done)
            if extra_options is None:
                extra_options = ['--sandbox_debug', '--noreuse_sandbox_directories']
            if '--sandbox_debug' not in extra_options:
                extra_options = extra_options + ['--sandbox_debug']
            if '--noreuse_sandbox_directories' not in extra_options:
                extra_options = extra_options + ['--noreuse_sandbox_directories']
            logging.info('Under a build tracer')
            logging.info("self.bazel_cache = %s" % (self.bazel_cache))
            logging.info("self.workspace = %s" % (self.workspace))
            logging.info("self.bazel_bin = %s" % (self.bazel_bin))
            if num_targets_done == 0:
                logging.info('No target built so far')
                extra_options = extra_options + ['--discard_analysis_cache']
                cmds_with_outputs = [\
                        [self.bazel_bin, self.bazel_cache, 'version'], \
                        ['find', self.bazel_cache.split("=")[1], '-name', 'A-server.jar', '-print'], \
                        ['find', self.bazel_cache.split("=")[1], '-type', 'f', '-name', 'java', '-print'], \
                        ]
                outputs = []
                for o_cmd in cmds_with_outputs:
                    try:
                        logging.info('Running "%s"', " ".join(o_cmd))
                        cmd_proc = subprocess.Popen(o_cmd, cwd=self.workspace, stdout=subprocess.PIPE)
                        self.process_list.append(cmd_proc)
                        outputs.append([l.decode("utf-8") for l in cmd_proc.stdout.read().splitlines()])
                    except Exception as e:
                        logging.error('Command Failed: "%s"', " ".join(o_cmd))
                        logging.error(e)
                        sys.exit(1)
                    self.process_list.remove(cmd_proc)
                bazel_version = outputs[0][0].split(':')[1].strip()
                aserver_path = outputs[1][0]
                java_path = outputs[2][0]
                logging.info('Bazel Version = %s', bazel_version)
                logging.info('Absolute Path to A-server.jar is "%s"' % aserver_path)
                logging.info('Absolute Path to java is "%s"' % java_path)
                if bazel_version < '7.1.1':
                    inst_branch = 'kp3'
                elif bazel_version < '7.2.1':
                    inst_branch = 'kp4'
                else:
                    inst_branch = 'kp5'
                if os.path.isfile('%s/kernel_platform/prebuilts/jdk/jdk11/linux-x86/bin/java' % (self.workspace)):
                    repo_path = self.workspace
                else:
                    repo_path = '%s/..' % (self.workspace)
                commands = [ \
                        'cp -R %s/%s %s' % (pkg_path, inst_branch, repo_path),
                        'tar xfz %s/%s --directory %s' % (pkg_path, os.environ.get("JPKG"), repo_path),
                        'mkdir -p %s/aspectj-1.9' % (repo_path),
                        '%s/%s -jar %s/aspectj-1.9.22.1.jar -to %s/aspectj-1.9' % (repo_path, os.environ.get("JBIN"), pkg_path, repo_path), \
                        '%s/aspectj-1.9/bin/ajc -1.9 -cp %s:%s/aspectj-1.9/lib/aspectjrt.jar -outxml -outjar %s/aspectskp.jar %s/%s/aspectinstrumentation.java' % \
                        (repo_path, aserver_path, repo_path, repo_path, repo_path, inst_branch), \
                        '%s/%s/instrument.sh %s %s/aspectj-1.9/lib %s/aspectskp.jar' % (repo_path, inst_branch, repo_path, repo_path, repo_path),
                        'touch -d "10 years" %s/prebuilts/jdk/jdk11/linux-x86/bin/java' % (self.workspace),
                        " ".join([self.bazel_bin, self.bazel_cache, 'shutdown']),
                        'cp %s %s_original' % (java_path, java_path),
                        'cp %s/prebuilts/jdk/jdk11/linux-x86/bin/java %s' % (self.workspace, java_path),
                        'touch -d "10 years" %s' % (java_path),
                        ]
                for i, cmd in enumerate(commands):
                    logging.info('Running command %d : "%s"', i, cmd)
                    try:
                        if i == 5:
                            run_dir = '%s/%s' % (repo_path, inst_branch)
                        else:
                            run_dir = self.workspace
                        cmd_proc = subprocess.Popen(cmd, cwd=run_dir, shell=True)
                        self.process_list.append(cmd_proc)
                        cmd_proc.wait()
                        if cmd_proc.returncode != 0:
                            sys.exit(cmd_proc.returncode)
                    except Exception as e:
                        logging.error(e)
                        sys.exit(1)
            else:
                logging.info('Re-entering this function; Hence instrumentation is already done')
            cmdline = [self.bazel_bin, "--max_idle_secs=%s" % (os.environ.get("IDLE_TIMEOUT")), self.bazel_cache, bazel_subcommand]
        else:
            cmdline = [self.bazel_bin, self.bazel_cache, bazel_subcommand]
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

        BazelBuilder.targets_done.extend([t.bazel_label for t in targets])
        self.process_list.remove(build_proc)

    def build_targets(self, targets):
        """Run "bazel build" on all targets in parallel"""
        self.bazel("build", targets, extra_options=self.user_opts)


    def _get_bazel_bin(self):
        """Return the bazel-bin and execution_root paths from the warm Bazel server."""
        try:
            out = subprocess.check_output(
                [self.bazel_bin, self.bazel_cache, "info", "bazel-bin", "execution_root"],
                cwd=self.workspace, stderr=subprocess.DEVNULL,
            )
            lines_out = out.decode().strip().splitlines()
            bazel_bin = lines_out[0].strip()
            if len(lines_out) > 1:
                execroot = lines_out[1].strip()
            else:
                execroot = os.path.dirname(os.path.dirname(
                    os.path.dirname(bazel_bin)))
            return bazel_bin, execroot
        except subprocess.CalledProcessError as e:
            logging.error("bazel info failed: %s", e)
            sys.exit(1)

    def run_targets(self, targets):
        """Run dist targets in parallel by executing the built scripts directly.

        After bazel build the dist executables already exist under bazel-bin.
        Running them directly avoids spawning N cold Bazel servers and the
        I/O contention and /tmp exhaustion that caused the parallel-server
        approach to regress build time by 3-15x.
        """
        bazel_bin, execroot = self._get_bazel_bin()
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
            pkg  = target.bazel_label[2:].split(":")[0]
            name = target.bazel_label.split(":")[1]
            script = os.path.join(bazel_bin, pkg, name)

            if not os.path.isfile(script):
                result = subprocess.run(
                    ["find", bazel_bin, "-maxdepth", "5",
                     "-name", name, "-type", "f"],
                    capture_output=True, text=True,
                )
                found = result.stdout.strip().split("\n")[0]
                if found and os.path.isfile(found):
                    script = found

            if not os.path.isfile(script):
                return _run_via_bazel(target)

            out_dir = _get_out_dir(target)
            os.makedirs(out_dir, exist_ok=True)

            env = os.environ.copy()
            env["BUILD_WORKSPACE_DIRECTORY"] = self.workspace
            runfiles_dir = script + ".runfiles"
            if os.path.isdir(runfiles_dir):
                env["RUNFILES_DIR"] = runfiles_dir

            with _dir_locks_lock:
                if out_dir not in _dir_locks:
                    _dir_locks[out_dir] = threading.Lock()
                _out_dir_lock = _dir_locks[out_dir]
            with _out_dir_lock:
                proc = subprocess.Popen(
                    [script, "--destdir", out_dir],
                    cwd=execroot, env=env,
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
                [self.bazel_bin, self.bazel_cache, "run"]
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

        # Split into two waves to avoid --wipe_dist_dir races:
        # Wave 1: base *_dist targets (wipe dist dir) and *_dtc_dist (writes
        #         to host dir, independent) run first in parallel.
        # Wave 2: targets in SUPPLEMENTARY_SUFFIXES add files on top of the
        #         already-populated dist dir (no wipe) and run after.
        wave1 = [t for t in targets
                 if not any(t.bazel_label.endswith(s) for s in SUPPLEMENTARY_SUFFIXES)]
        wave2 = [t for t in targets
                 if any(t.bazel_label.endswith(s) for s in SUPPLEMENTARY_SUFFIXES)]

        results = []
        if wave1:
            w1 = min(len(wave1), _MAX_DIST_WORKERS)
            logging.info("Running %d dist targets in parallel (wave 1).", len(wave1))
            with concurrent.futures.ThreadPoolExecutor(max_workers=w1) as pool:
                results += list(pool.map(_run_one, wave1))

            failed = [(t, rc) for rc, t, _ in results if rc != 0]
            if failed:
                for t, rc in failed:
                    logging.error("Dist target failed (rc=%d): %s", rc, t.bazel_label)
                sys.exit(failed[0][1])

        wave2_results = []
        if wave2:
            w2 = min(len(wave2), _MAX_DIST_WORKERS)
            logging.info("Running %d dist targets in parallel (wave 2).", len(wave2))
            with concurrent.futures.ThreadPoolExecutor(max_workers=w2) as pool:
                wave2_results = list(pool.map(_run_one, wave2))
            results += wave2_results

            failed = [(t, rc) for rc, t, _ in wave2_results if rc != 0]
            if failed:
                for t, rc in failed:
                    logging.error("Dist target failed (rc=%d): %s", rc, t.bazel_label)
                sys.exit(failed[0][1])

        for _, target, out_dir in results:
            self.write_opts(out_dir, opts_content)
            if out_dir == target.get_out_dir("dist"):
                self.setup_kbdev_symlinks(out_dir)

    def setup_kbdev_symlinks(self, out_dir):
        """Setup k*.img symlinks needed for test builds"""
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

        if self.skip_list:
            self.user_opts.extend(["--//msm-kernel:skip_{}=true".format(s) for s in self.skip_list])

        self.user_opts.extend([
            "--user_kmi_symbol_lists=//msm-kernel:android/abi_gki_aarch64_qcom",
            "--ignore_missing_projects",
            "--incompatible_sandbox_hermetic_tmp=false",
            "--nozstd_dwarf_compression",
        ])

        if self.dry_run:
            self.user_opts.append("--nobuild")

        # List of files to avoid symbolic link creation
        avoid_symlink_files = [
        "drivers/block/zram/zcomp.h",
        "drivers/block/zram/zram_drv.h",
        "include/linux/zsmalloc.h",
        "include/linux/ieee80211.h",
        ]

        try:
            if self.gki_headers:
                gki_files_path = os.path.join(self.workspace, 'msm-kernel/files_gki_aarch64.txt')
                gki_f = open(gki_files_path, 'r')
                gki_files = gki_f.readlines()
                common_d = os.path.join(self.workspace, "common")
                msm_d = os.path.join(self.workspace, "msm-kernel")
                for f in gki_files:
                    if ".h" in f:
                        f=f.strip()
                        if f in avoid_symlink_files:
                            logging.info('Skipping symbolic link creation for %s', f)
                            continue

                        logging.info('GKI header file...%s', f)
                        common_f = os.path.join(common_d, f)
                        msm_f = os.path.join(msm_d, f)
                        if os.path.exists(msm_f):
                            os.remove(msm_f)
                        os.symlink(common_f, msm_f)
                gki_f.close()

            logging.debug(
                "Building the following targets:\n%s",
                "\n".join([t.bazel_label for t in targets_to_build])
            )

            self.clean_legacy_generated_files()

            logging.info("Building targets...")
            self.build_targets(targets_to_build)

            if not self.dry_run:
                self.run_targets(targets_to_build)
        finally:
            if self.gki_headers:
                status = subprocess.Popen(["git", "checkout",
                                     "--pathspec-from-file=files_gki_aarch64.txt"],
                                    cwd=os.path.join(self.workspace, "msm-kernel"))
                status.wait()
                if status.returncode != 0:
                    logging.error("Failed to restore headers from symlinks")
                    logging.error("You might want to check your msm-kernel tree")

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
              " the //msm-kernel:<target>_<variant>_abl build)"),
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
        help="Compile with common headers instead of msm-kernel"
    )

    args, user_opts = parser.parse_known_args(sys.argv[1:])

    logging.basicConfig(
        level=getattr(logging, args.log.upper()),
        format="[{}] %(levelname)s: %(message)s".format(os.path.basename(sys.argv[0])),
    )

    args.skip.extend(DEFAULT_SKIP_LIST)

    builder = BazelBuilder(
        args.target,
        args.skip,
        args.out_dir,
        args.cache_dir,
        args.dry_run,
        args.gki_headers,
        user_opts
    )
    try:
        if args.menuconfig:
            builder.run_menuconfig()
        else:
            builder.build()
    except KeyboardInterrupt:
        logging.info("Received keyboard interrupt... exiting")
        del builder
        sys.exit(1)

    if args.dry_run:
        logging.info("Dry-run completed successfully!")
    else:
        logging.info("Build completed successfully!")

if __name__ == "__main__":
    main()
