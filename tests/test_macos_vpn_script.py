#!/usr/bin/env python3
"""Unprivileged sandbox tests for res/vpn/vpn-run-root-macos.sh (PR-22).

The script runs as the current user with NKR_RUN_DIR pointing at a private
temporary directory. A fake core stands in for nekobox_core. No root, no utun
interface, no route, DNS or proxy change: the interface name used (utun250)
does not exist, and nothing in the script creates interfaces itself.

Usage: test_macos_vpn_script.py <path to vpn-run-root-macos.sh>
"""

import hashlib
import os
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

SCRIPT_PATH = None
IFNAME = "utun250"

FAKE_CORE = r"""#!/bin/bash
# Fake nekobox_core: <exe> --disable-color run -c <config> -D <dir>
[[ "$1 $2 $3 $5" = "--disable-color run -c -D" ]] || {
    echo "bad args: $*" >&2
    exit 9
}
{ read -r mode; read -r marker; } <"$4"
printf '%s\n' "$0" "$@" >"$marker/args"
echo "fake core started"
case "$mode" in
    exit3)
        echo "fake failure line"
        exit 3
        ;;
    ignore)
        trap 'echo int >>"$marker/signals"' INT
        ;;
    *)
        trap 'echo int >>"$marker/signals"; exit 0' INT
        ;;
esac
echo "$$" >"$marker/pid.tmp"
mv "$marker/pid.tmp" "$marker/pid"
while :; do sleep 0.05; done
"""


def sha256(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def read(path):
    with open(path) as handle:
        return handle.read()


def token_of(pid):
    out = subprocess.run(
        ["ps", "-o", "uid=,state=,lstart=", "-p", str(pid)],
        capture_output=True, text=True, env={"LC_ALL": "C", "PATH": "/bin:/usr/bin"},
    ).stdout.split()
    if len(out) < 7:
        return None
    return "%d@%s@%s" % (pid, out[0], "-".join(out[2:]))


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    out = subprocess.run(["ps", "-o", "state=", "-p", str(pid)],
                         capture_output=True, text=True).stdout.strip()
    return bool(out) and not out.startswith("Z")


def wait_for(predicate, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return predicate()


class Sandbox(unittest.TestCase):
    def setUp(self):
        with open(SCRIPT_PATH, "r", encoding="utf-8") as handle:
            self.script = handle.read()
        self.root = tempfile.mkdtemp(prefix="pr22-")
        self.run_dir = os.path.join(self.root, "run")
        self.work = os.path.join(self.root, "work")
        self.marker = os.path.join(self.root, "marker")
        for path in (self.run_dir, self.work, self.marker):
            os.mkdir(path, 0o700)
        self.core = os.path.join(self.work, "nekobox_core")
        with open(self.core, "w") as handle:
            handle.write(FAKE_CORE)
        os.chmod(self.core, 0o700)
        self.config = os.path.join(self.work, "vpn.json")
        self.write_config("normal")
        self.env = {
            "NKR_RUN_DIR": self.run_dir,
            "NKR_POLL": "0.05",
            "NKR_STOP_TIMEOUT": "2",
            "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        }
        self.procs = []
        self.cores = []

    def tearDown(self):
        for proc in self.procs:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            if proc.stdout is not None:
                proc.stdout.close()
            if proc.stderr is not None:
                proc.stderr.close()
        # Only kill a fake core whose identity still matches the recorded one.
        for pid, token in self.cores:
            if token and token_of(pid) == token:
                os.kill(pid, signal.SIGKILL)
        shutil.rmtree(self.root, ignore_errors=True)

    def write_config(self, mode):
        if os.path.exists(self.config):
            os.unlink(self.config)
        fd = os.open(self.config, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as handle:
            handle.write("%s\n%s\n" % (mode, self.marker))

    def start_args(self, gui_pid=None, core_sha=None, config_sha=None,
                   ifname=IFNAME, nonce=None):
        return [
            "start", str(os.getuid()),
            str(gui_pid if gui_pid is not None else os.getpid()),
            self.core, core_sha or sha256(self.core),
            self.config, config_sha or sha256(self.config),
            ifname, nonce or str(uuid.uuid4()),
        ]

    def spawn(self, args, env=None):
        proc = subprocess.Popen(
            ["/bin/bash", "-p", "-c", self.script, "nekoray-vpn"] + args,
            env=env if env is not None else self.env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.procs.append(proc)
        return proc

    def run_script(self, args, env=None, timeout=20):
        proc = self.spawn(args, env)
        out, err = proc.communicate(timeout=timeout)
        return proc.returncode, out, err

    def path(self, name):
        return os.path.join(self.run_dir, name)

    def wait_running(self, proc):
        ok = wait_for(lambda: os.path.exists(os.path.join(self.marker, "pid"))
                      and "CORE=" in self.state_text()
                      and "CORE=\n" not in self.state_text())
        if not ok:
            proc.kill()
            out, err = proc.communicate()
            self.fail("core did not start: rc=%s err=%s" % (proc.returncode, err))
        pid = int(read(os.path.join(self.marker, "pid")))
        self.cores.append((pid, token_of(pid)))
        return pid

    def state_text(self):
        try:
            with open(self.path("state")) as handle:
                return handle.read()
        except FileNotFoundError:
            return ""

    def assert_clean(self):
        for name in ("state", "nekobox_core", "config.json",
                     "nekobox_core.tmp", "config.json.tmp", "state.tmp"):
            self.assertFalse(os.path.exists(self.path(name)), name)
        self.assertFalse(any(name.startswith(("asset-", "gate-"))
                             for name in os.listdir(self.run_dir)))

    # 1. argument validation
    def test_argument_validation(self):
        base = self.start_args()
        cases = [
            base[:-1],
            base[:3] + ["relative/core"] + base[4:],
            base[:1] + ["root"] + base[2:],
            base[:4] + ["ABC"] + base[5:],
            base[:7] + ["en0"] + base[8:],
            base[:7] + ["utun9;x"] + base[8:],
            base[:8] + ["not-a-nonce"],
            ["bogus"],
            ["cleanup"],
            ["cleanup", "x"],
        ]
        for args in cases:
            rc, _, err = self.run_script(args)
            self.assertEqual(rc, 64, (args, err))
        self.assert_clean()

    # 2. runtime directory policy
    def test_runtime_directory_policy(self):
        env = dict(self.env)
        del env["NKR_RUN_DIR"]
        rc, _, err = self.run_script(self.start_args(), env)
        self.assertEqual(rc, 77, err)
        self.assertIn("root is required", err)

        link = os.path.join(self.root, "link")
        os.symlink(self.run_dir, link)
        env = dict(self.env, NKR_RUN_DIR=link)
        rc, _, err = self.run_script(self.start_args(), env)
        self.assertEqual(rc, 77, err)

        for mode in (0o770, 0o755):
            os.chmod(self.run_dir, mode)
            rc, _, err = self.run_script(self.start_args())
            self.assertEqual(rc, 77, err)
        os.chmod(self.run_dir, 0o700)

    def test_runtime_files_are_not_trusted(self):
        outside = os.path.join(self.root, "outside")
        with open(outside, "w") as f:
            f.write("keep me")
        for name in ("lock", "state", "core.log", "nekobox_core.tmp", "asset-0"):
            path = self.path(name)
            os.symlink(outside, path)
            rc, _, err = self.run_script(["cleanup", str(os.getuid())])
            self.assertEqual(rc, 77, (name, err))
            self.assertEqual(read(outside), "keep me")
            os.unlink(path)
        os.link(outside, self.path("core.log"))
        rc, _, err = self.run_script(["cleanup", str(os.getuid())])
        self.assertEqual(rc, 77, err)
        os.unlink(self.path("core.log"))
        os.mkfifo(self.path("core.log"))
        rc, _, err = self.run_script(["cleanup", str(os.getuid())])
        self.assertEqual(rc, 77, err)
        os.unlink(self.path("core.log"))
        with open(self.path("attacker-file"), "w") as f:
            f.write("keep")
        rc, _, err = self.run_script(["cleanup", str(os.getuid())])
        self.assertEqual(rc, 77, err)
        self.assertTrue(os.path.exists(self.path("attacker-file")))

    def test_assets_are_copied_and_verified(self):
        source = os.path.join(self.work, "规则 'q' \"a\" $ `id`.srs")
        with open(source, "w") as f:
            f.write("verified asset")
        proc = self.spawn(self.start_args() + [source, sha256(source)])
        self.wait_running(proc)
        self.assertEqual(read(self.path("asset-0")), "verified asset")
        self.assertEqual(stat.S_IMODE(os.stat(self.path("asset-0")).st_mode), 0o600)
        with open(source, "w") as f:
            f.write("changed after copy")
        self.assertEqual(read(self.path("asset-0")), "verified asset")
        os.unlink(self.config)
        _, err = proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 0, err)
        self.assert_clean()
        self.write_config("normal")
        rc, _, err = self.run_script(self.start_args() + [source, "0" * 64])
        self.assertEqual(rc, 76, err)
        self.assert_clean()
        with open(source, "w"):
            pass
        rc, _, err = self.run_script(self.start_args() + [source, sha256(source)])
        self.assertEqual(rc, 77, err)
        self.assert_clean()

    def test_quoted_paths_work(self):
        special = "配置 'q' \"b\" $HOME `id` $(id)"
        new_work = os.path.join(self.root, special)
        os.rename(self.work, new_work)
        self.work = new_work
        self.core = os.path.join(self.work, "nekobox_core")
        self.config = os.path.join(self.work, "vpn.json")
        self.write_config("normal")
        proc = self.spawn(self.start_args())
        self.wait_running(proc)
        os.unlink(self.config)
        _, err = proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 0, err)
        self.assert_clean()

    def test_fast_stop_start_is_isolated(self):
        old_nonce, new_nonce = str(uuid.uuid4()), str(uuid.uuid4())
        old = self.spawn(self.start_args(nonce=old_nonce))
        old_pid = self.wait_running(old)
        old_config = self.config
        self.config = os.path.join(self.work, "vpn-%s.json" % new_nonce)
        self.write_config("normal")
        os.unlink(old_config)
        # A fast new launch can be refused while the old lock is held. It
        # must never revive the old launch or be deleted by its finalizer.
        contender = self.spawn(self.start_args(nonce=new_nonce))
        self.assertTrue(wait_for(lambda: old.poll() is not None))
        self.assertTrue(wait_for(lambda: contender.poll() is not None or
                                 "NONCE=%s" % new_nonce in self.state_text()))
        if contender.poll() is not None:
            self.assertEqual(contender.returncode, 75)
            contender.communicate(timeout=10)
            contender = None
        self.assertTrue(os.path.isfile(self.config))
        old.communicate(timeout=10)
        self.assertEqual(old.returncode, 0)
        self.assertFalse(alive(old_pid))
        if contender is None:
            os.unlink(os.path.join(self.marker, "pid"))
            new = self.spawn(self.start_args(nonce=new_nonce))
        else:
            new = contender
        self.assertTrue(wait_for(lambda: "NONCE=%s" % new_nonce in self.state_text()
                                 and os.path.isfile(os.path.join(self.marker, "pid"))
                                 and read(os.path.join(self.marker, "pid")).strip()
                                 not in ("", str(old_pid))))
        new_pid = self.wait_running(new)
        self.assertIn("NONCE=%s" % new_nonce, self.state_text())
        self.assertTrue(alive(new_pid))
        self.assertTrue(os.path.isfile(self.config))
        os.unlink(self.config)
        _, err = new.communicate(timeout=10)
        self.assertEqual(new.returncode, 0, err)
        self.assert_clean()

    def test_replacing_stop_path_does_not_revive_old_run(self):
        old = self.spawn(self.start_args())
        old_pid = self.wait_running(old)
        os.rename(self.config, self.config + ".old")
        self.write_config("normal")
        _, err = old.communicate(timeout=10)
        self.assertEqual(old.returncode, 0, err)
        self.assertFalse(alive(old_pid))
        self.assertTrue(os.path.isfile(self.config))
        self.assert_clean()

    # 3. configuration file policy
    def test_config_policy(self):
        os.chmod(self.config, 0o644)
        rc, _, err = self.run_script(self.start_args())
        self.assertEqual(rc, 77, err)
        os.chmod(self.config, 0o600)

        real = self.config + ".real"
        os.rename(self.config, real)
        os.symlink(real, self.config)
        rc, _, err = self.run_script(self.start_args(config_sha=sha256(real)))
        self.assertEqual(rc, 77, err)
        os.unlink(self.config)

        rc, _, err = self.run_script(self.start_args(config_sha="0" * 64))
        self.assertEqual(rc, 66, err)
        self.assertFalse(os.path.exists(os.path.join(self.marker, "args")))

    # 4. hash verification
    def test_hash_mismatch(self):
        rc, _, err = self.run_script(self.start_args(config_sha="1" * 64))
        self.assertEqual(rc, 76, err)
        rc, _, err = self.run_script(self.start_args(core_sha="2" * 64))
        self.assertEqual(rc, 76, err)
        self.assertFalse(os.path.exists(os.path.join(self.marker, "args")))
        self.assert_clean()

    # 5. lifecycle: start, readiness record, stop by deleting the config
    def test_lifecycle_stop_by_config_removal(self):
        nonce = str(uuid.uuid4())
        proc = self.spawn(self.start_args(nonce=nonce))
        core_pid = self.wait_running(proc)

        args = read(os.path.join(self.marker, "args")).split("\n")
        self.assertEqual(os.path.realpath(args[0]),
                         os.path.realpath(self.path("nekobox_core")))
        self.assertEqual(args[1:6], ["--disable-color", "run", "-c",
                                     self.path("config.json"), "-D"])
        state = dict(line.split("=", 1)
                     for line in self.state_text().splitlines())
        self.assertEqual(sorted(state),
                         ["CORE", "GUI", "IF", "NONCE", "SUP", "UID"])
        self.assertEqual(state["NONCE"], nonce)
        self.assertEqual(state["IF"], IFNAME)
        self.assertEqual(state["UID"], str(os.getuid()))
        self.assertEqual(state["CORE"], token_of(core_pid))
        self.assertEqual(state["GUI"], token_of(os.getpid()))
        self.assertEqual(stat.S_IMODE(os.stat(self.path("core.log")).st_mode),
                         0o600)
        self.assertEqual(stat.S_IMODE(os.stat(self.path("state")).st_mode),
                         0o644)

        started = time.time()
        os.unlink(self.config)
        out, err = proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 0, err)
        self.assertLess(time.time() - started, 5)
        self.assertEqual(read(os.path.join(self.marker, "signals")),
                         "int\n")
        self.assertFalse(alive(core_pid))
        self.assert_clean()
        log = read(self.path("core.log"))
        self.assertIn("fake core started", log)
        self.assertIn("stopping core: requested", log)
        self.assertIn("default route before:", log)
        self.assertIn("default route after:", log)

    # 6. the GUI disappears (crash cleanup)
    def test_gui_exit_stops_core(self):
        gui = subprocess.Popen(["/bin/sleep", "60"])
        self.procs.append(gui)
        proc = self.spawn(self.start_args(gui_pid=gui.pid))
        core_pid = self.wait_running(proc)
        gui.kill()
        gui.wait()
        out, err = proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 0, err)
        self.assertFalse(alive(core_pid))
        self.assertIn("stopping core: gui-exited",
                      read(self.path("core.log")))
        self.assert_clean()

    def test_signal_stops_core(self):
        proc = self.spawn(self.start_args())
        core_pid = self.wait_running(proc)
        sup = int(dict(line.split("=", 1) for line in
                       self.state_text().splitlines())["SUP"].split("@")[0])
        os.kill(sup, signal.SIGTERM)
        out, err = proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 0, err)
        self.assertFalse(alive(core_pid))
        self.assert_clean()

    def test_supervisor_crash_recovered_by_cleanup(self):
        proc = self.spawn(self.start_args())
        core_pid = self.wait_running(proc)
        sup = int(dict(line.split("=", 1) for line in
                       self.state_text().splitlines())["SUP"].split("@")[0])
        os.kill(sup, signal.SIGKILL)
        # lockf releases its kernel lock after the supervisor exits, even
        # when the core outlives it. Only cleanup's recorded token may kill it.
        self.assertTrue(wait_for(lambda: proc.poll() is not None))
        self.assertTrue(alive(core_pid))
        rc, _, err = self.run_script(["cleanup", str(os.getuid())])
        self.assertEqual(rc, 0, err)
        self.assertFalse(alive(core_pid))
        self.assert_clean()
        proc.communicate(timeout=10)

    def test_gui_gone_at_start(self):
        gui = subprocess.Popen(["/usr/bin/true"])
        gui.wait()
        rc, _, err = self.run_script(self.start_args(gui_pid=gui.pid))
        self.assertEqual(rc, 75, err)
        self.assertFalse(os.path.exists(os.path.join(self.marker, "args")))

    # 7. the core exits by itself
    def test_core_exit_reports_tail(self):
        self.write_config("exit3")
        rc, _, err = self.run_script(self.start_args())
        self.assertEqual(rc, 70, err)
        self.assertIn("core exited with status 3", err)
        self.assertIn("fake failure line", err)
        self.assert_clean()

    # 8. the core ignores SIGINT: escalate to SIGKILL
    def test_core_ignoring_sigint_is_killed(self):
        self.write_config("ignore")
        proc = self.spawn(self.start_args())
        core_pid = self.wait_running(proc)
        os.unlink(self.config)
        out, err = proc.communicate(timeout=15)
        self.assertEqual(proc.returncode, 0, err)
        self.assertFalse(alive(core_pid))
        self.assertIn("sending SIGKILL", read(self.path("core.log")))
        self.assert_clean()

    # 9. a second instance is refused while one runs
    def test_second_instance_refused(self):
        proc = self.spawn(self.start_args())
        core_pid = self.wait_running(proc)
        rc, _, err = self.run_script(self.start_args())
        self.assertEqual(rc, 75, err)
        self.assertTrue(alive(core_pid))
        self.assertIsNone(proc.poll())
        rc, _, err = self.run_script(["cleanup", str(os.getuid())])
        self.assertEqual(rc, 75, err)
        self.assertTrue(alive(core_pid))
        os.unlink(self.config)
        proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 0)

    def write_stale_state(self, core_token, uid=None):
        dead = subprocess.Popen(["/usr/bin/true"])
        dead.wait()
        sup = "%d@%d@Thu-Jan-1-00:00:00-2026" % (dead.pid, os.getuid())
        with open(self.path("state"), "w") as handle:
            handle.write("SUP=%s\nCORE=%s\nUID=%s\nGUI=%s\nIF=%s\nNONCE=%s\n" % (
                sup, core_token, uid if uid is not None else os.getuid(),
                sup, IFNAME, uuid.uuid4()))
        os.chmod(self.path("state"), 0o644)

    # 10. stale record cleanup is identity safe
    def test_stale_cleanup_is_identity_safe(self):
        helper = subprocess.Popen(["/bin/sleep", "60"])
        self.procs.append(helper)
        real = token_of(helper.pid)
        wrong = real.rsplit("-", 1)[0] + "-1999"
        self.write_stale_state(wrong)
        rc, _, err = self.run_script(["cleanup", str(os.getuid())])
        self.assertEqual(rc, 0, err)
        self.assertTrue(alive(helper.pid), "unrelated process was signalled")
        self.assertFalse(os.path.exists(self.path("state")))

        self.write_stale_state(real)
        proc = self.spawn(self.start_args())
        self.wait_running(proc)
        self.assertTrue(wait_for(lambda: not alive(helper.pid)))
        os.unlink(self.config)
        proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 0)

    # 11. cleanup without a record
    def test_cleanup_without_state(self):
        rc, _, err = self.run_script(["cleanup", str(os.getuid())])
        self.assertEqual(rc, 0, err)

    def test_invalid_state_is_not_trusted(self):
        with open(self.path("state"), "w") as handle:
            handle.write("SUP=1@0@x\nEVIL=1\n")
        rc, _, err = self.run_script(["cleanup", str(os.getuid())])
        self.assertEqual(rc, 74, err)
        self.assertTrue(os.path.exists(self.path("state")))

    def test_duplicate_or_missing_state_keys_rejected(self):
        helper = subprocess.Popen(["/bin/sleep", "60"])
        self.procs.append(helper)
        self.write_stale_state(token_of(helper.pid))
        valid = read(self.path("state"))
        for content in (valid + "CORE=\n",
                        "\n".join(line for line in valid.splitlines()
                                  if not line.startswith("CORE=")) + "\n"):
            with open(self.path("state"), "w") as f:
                f.write(content)
            rc, _, err = self.run_script(["cleanup", str(os.getuid())])
            self.assertEqual(rc, 74, err)
            self.assertTrue(alive(helper.pid))

    # 12. another user's record is not touched
    def test_other_users_record_denied(self):
        helper = subprocess.Popen(["/bin/sleep", "60"])
        self.procs.append(helper)
        self.write_stale_state(token_of(helper.pid), uid=os.getuid() + 1)
        rc, _, err = self.run_script(["cleanup", str(os.getuid())])
        self.assertEqual(rc, 77, err)
        self.assertTrue(alive(helper.pid))
        self.assertTrue(os.path.exists(self.path("state")))

    def test_existing_interface_refused(self):
        # Read-only probe of an interface macOS itself normally creates.
        if subprocess.run(["ifconfig", "utun0"],
                          capture_output=True).returncode != 0:
            self.skipTest("utun0 is not present on this machine")
        rc, _, err = self.run_script(self.start_args(ifname="utun0"))
        self.assertEqual(rc, 75, err)
        self.assertIn("already exists", err)
        self.assertFalse(os.path.exists(os.path.join(self.marker, "args")))


if __name__ == "__main__":
    if os.geteuid() == 0:
        sys.exit("Refusing sandbox execution as root")
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    SCRIPT_PATH = os.path.abspath(sys.argv.pop(1))
    unittest.main(verbosity=2)
