#!/usr/bin/env python3
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

if sys.platform != "linux":
    raise SystemExit("Linux is required")
if os.geteuid() == 0:
    raise SystemExit("Run this sandbox as a non-root user")
for command in ("bash", "flock", "stat", "mktemp"):
    if shutil.which(command) is None:
        raise SystemExit("Missing required command: " + command)

SCRIPT = Path(sys.argv[1]).resolve().read_text()
ROOT = tempfile.TemporaryDirectory(prefix="nkr-w07-")
BASE = Path(ROOT.name)
BIN = BASE / "bin"
BIN.mkdir()
PROCESSES = []
IDENTITIES = set()
LOGS = []

FAKE_IPTABLES = r'''#!/usr/bin/env python3
import fcntl
import json
import os
from pathlib import Path
import sys

root = Path(os.environ["FAKE_STATE"])
family = Path(sys.argv[0]).name
with (root / "firewall.lock").open("a") as lock:
    fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
    storage = root / (family + ".json")
    rules = json.loads(storage.read_text()) if storage.exists() else []
    operation = sys.argv[1]
    rule = sys.argv[2:]
    with (root / "calls").open("a") as output:
        output.write(json.dumps([family, operation, rule]) + "\n")
    if operation == "-C":
        raise SystemExit(0 if rule in rules else 1)
    if operation == "-I":
        if os.environ.get("FAIL_INSERT") == "1":
            raise SystemExit(1)
        rules.append(rule)
    elif operation == "-D":
        if os.environ.get("FAIL_DELETE") == "1":
            raise SystemExit(1)
        if rule not in rules:
            raise SystemExit(1)
        rules.remove(rule)
    else:
        raise SystemExit(2)
    storage.write_text(json.dumps(rules))
'''

FAKE_CORE = r'''#!/bin/bash
# A core must never execute before its identity has been published.
grep -q "^CORE=$$:" "$NKR_RUN_DIR/nekoray-vpn.state" || {
    if [[ "${FAKE_MODE:-run}" != decoy ]]; then exit 99; fi
}
printf '%s\n' "$$" >>"$FAKE_STATE/cores"
printf '%s\0' "$@" >"$FAKE_STATE/args"
printf '%s' "$PWD" >"$FAKE_STATE/cwd"
case "${FAKE_MODE:-run}" in
    quick) exit 0 ;;
    crash) exit 3 ;;
    run|decoy)
        trap 'printf "signal\n" >>"$FAKE_STATE/signals"; exit 0' INT TERM HUP
        while :; do sleep 0.05 & wait $!; done
        ;;
    stubborn)
        trap '' INT
        trap 'exit 0' TERM
        while :; do sleep 0.05; done
        ;;
    *) exit 64 ;;
esac
'''

FAKE_MV = r'''#!/bin/bash
if [[ "${HOLD_CORE_RECORD:-0}" = 1 ]] &&
   grep -q '^CORE=[0-9]' "$1"; then
    sed -n 's/^CORE=//p' "$1" >"$FAKE_STATE/blocked-core"
    printf '%s\n' "$$" >"$FAKE_STATE/blocked-mv"
    parent=$PPID
    while kill -0 "$parent" 2>/dev/null; do sleep 0.02; done
    exit 74
fi
exec /bin/mv "$@"
'''

for name, contents in (
    ("iptables", FAKE_IPTABLES),
    ("ip6tables", FAKE_IPTABLES),
    ("mv", FAKE_MV),
):
    path = BIN / name
    path.write_text(contents)
    path.chmod(0o755)

CORE = BASE / "fake core 'quoted' `ticks`"
CORE.write_text(FAKE_CORE)
CORE.chmod(0o755)


def token(pid):
    try:
        data = Path("/proc/{}/stat".format(pid)).read_text()
        fields = data[data.rfind(") ") + 2:].split()
        if fields[0] == "Z":
            return None
        return "{}:{}".format(pid, fields[19])
    except (FileNotFoundError, ProcessLookupError):
        return None


def alive(identity):
    return bool(identity) and token(int(identity.split(":")[0])) == identity


def remember(identity):
    if identity:
        IDENTITIES.add(identity)


def until(predicate, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("timed out waiting for sandbox condition")


def read_when_ready(path):
    # The fake mv writes these files without renaming, so wait for content.
    until(lambda: path.exists() and path.read_text().strip() != "")
    return path.read_text().strip()


def default_signals():
    # A parent started by ctest can hand down an ignored SIGINT or a blocked
    # SIGCHLD. Ignored signals cannot be trapped, and with SIGCHLD blocked
    # bash's wait never returns, which left the orphaned core deaf to --stop.
    # Restore dispositions and clear the mask before exec.
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, signal.SIG_DFL)
    signal.pthread_sigmask(signal.SIG_SETMASK, set())


def report_inherited_signals():
    # Diagnostic: show what this test process inherited from its runner.
    for line in open("/proc/self/status"):
        if line.startswith(("SigBlk", "SigIgn")):
            print("INHERITED", line.strip(), flush=True)


class Case:
    def __init__(self, mode="run", **extra):
        self.root = Path(tempfile.mkdtemp(dir=BASE, prefix="case-"))
        self.work = self.root / "work 'quoted' `ticks` $(touch injected)"
        self.work.mkdir()
        self.config = self.root / "config 'quoted' $(touch injected).json"
        self.config.write_text("{}")
        self.env = dict(os.environ)
        self.env.pop("PKEXEC_UID", None)
        self.env.update(
            PATH=str(BIN) + os.pathsep + os.environ["PATH"],
            NKR_RUN_DIR=str(self.root),
            FAKE_STATE=str(self.root),
            FAKE_MODE=mode,
        )
        self.env.update(extra)
        self.log = (self.root / "output").open("wb")
        LOGS.append(self.log)

    def spawn(self, parameters=None, **extra):
        environment = dict(self.env)
        environment.update(extra)
        if parameters is None:
            parameters = [str(CORE), str(self.config), str(self.work)]
        process = subprocess.Popen(
            ["/bin/bash", "-c", SCRIPT, "nekoray-vpn"] + list(parameters),
            env=environment,
            stdout=self.log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            preexec_fn=default_signals,
        )
        PROCESSES.append(process)
        return process

    def run(self, expected=0, parameters=None, **extra):
        process = self.spawn(parameters, **extra)
        actual = process.wait(timeout=15)
        assert actual == expected, (
            actual, expected, (self.root / "output").read_text()
        )
        return process

    def record(self):
        path = self.root / "nekoray-vpn.state"
        if not path.exists():
            return {}
        return dict(line.split("=", 1) for line in path.read_text().splitlines())

    def ready(self):
        def check():
            record = self.record()
            core = record.get("CORE")
            if not core or not (self.root / "cores").exists():
                return False
            remember(core)
            return alive(core)
        until(check)
        return self.record()["CORE"]

    def rules(self, family):
        path = self.root / (family + ".json")
        return json.loads(path.read_text()) if path.exists() else []

    def clean(self):
        assert not self.rules("iptables")
        assert not self.rules("ip6tables")
        assert not (self.root / "nekoray-vpn.state").exists()

    def stop(self, process):
        self.run(parameters=["--terminate"])
        assert process.wait(timeout=8) == 0
        self.clean()


def run_tests():
    report_inherited_signals()
    # Normal completion, exact arguments/cwd, and status propagation.
    case = Case("quick")
    case.run()
    case.clean()
    assert (case.root / "args").read_bytes() == (
        b"run\0-c\0" + os.fsencode(case.config) + b"\0"
    )
    assert (case.root / "cwd").read_text() == str(case.work)
    assert not (case.work / "injected").exists()
    assert not (case.root / "injected").exists()
    calls = [json.loads(line)
             for line in (case.root / "calls").read_text().splitlines()]
    assert sum(call[1] == "-I" for call in calls) == 2
    assert sum(call[1] == "-D" for call in calls) == 2

    case = Case("crash")
    case.run(expected=3)
    case.clean()

    # Every forwarded signal reaches the core and cleans both families.
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        case = Case()
        process = case.spawn()
        case.ready()
        process.send_signal(sig)
        assert process.wait(timeout=8) == 0
        assert (case.root / "signals").exists()
        case.clean()

    # Validate all three absolute paths, missing paths, and control arity.
    case = Case("quick")
    good = [str(CORE), str(case.config), str(case.work)]
    for index in range(3):
        bad = list(good)
        bad[index] = "relative"
        case.run(expected=64, parameters=bad)
    case.run(expected=66, parameters=[
        str(case.root / "missing"), str(case.config), str(case.work)])
    case.run(expected=64, parameters=["--stop", "extra"])
    case.run(expected=64, parameters=["--terminate", "extra"])
    case.clean()

    # Insertion failure does not prevent core execution or delete foreign rules.
    case = Case("quick", FAIL_INSERT="1")
    case.run()
    case.clean()
    calls = [json.loads(line)
             for line in (case.root / "calls").read_text().splitlines()]
    assert not any(call[1] == "-D" for call in calls)

    # Failed deletion is visible and leaves a recoverable state record.
    case = Case("quick", FAIL_DELETE="1")
    case.run(expected=74)
    assert case.record()
    assert case.rules("iptables")
    case.env.pop("FAIL_DELETE")
    case.run(parameters=["--stop"])
    case.clean()

    # Duplicate launch and live --stop must leave the owner intact.
    case = Case()
    owner = case.spawn()
    identity = case.ready()
    case.run(expected=75)
    case.run(expected=75, parameters=["--stop"])
    assert alive(identity)
    assert case.rules("iptables") and case.rules("ip6tables")
    case.stop(owner)

    # SIGKILL recovery, idempotence, and serialized concurrent cleaners.
    case = Case()
    owner = case.spawn()
    identity = case.ready()
    owner.kill()
    owner.wait(timeout=5)
    assert alive(identity)
    assert case.rules("iptables")
    cleaners = [case.spawn(["--stop"]) for _ in range(5)]
    assert all(process.wait(timeout=10) == 0 for process in cleaners)
    until(lambda: not alive(identity))
    case.clean()
    case.run(parameters=["--stop"])
    case.run(parameters=["--terminate"])

    # A replacement must stop the recorded orphan before launching.
    case = Case()
    owner = case.spawn()
    identity = case.ready()
    owner.kill()
    owner.wait(timeout=5)
    case.run(FAKE_MODE="quick")
    assert not alive(identity)
    case.clean()

    # A nonresponsive orphan blocks replacement and retains its state/rules.
    case = Case("stubborn")
    owner = case.spawn()
    identity = case.ready()
    owner.kill()
    owner.wait(timeout=5)
    case.run(expected=75, FAKE_MODE="quick")
    assert alive(identity)
    assert len((case.root / "cores").read_text().splitlines()) == 1
    assert case.record() and case.rules("iptables")
    os.kill(int(identity.split(":")[0]), signal.SIGTERM)
    until(lambda: not alive(identity))
    case.run(parameters=["--stop"])
    case.clean()

    # Killing the supervisor during child-record publication cannot launch core.
    case = Case(HOLD_CORE_RECORD="1")
    owner = case.spawn()
    identity = read_when_ready(case.root / "blocked-core")
    remember(identity)
    mover = int(read_when_ready(case.root / "blocked-mv"))
    mover_identity = token(mover)
    remember(mover_identity)
    owner.kill()
    owner.wait(timeout=5)
    until(lambda: not alive(identity))
    until(lambda: not alive(mover_identity))
    assert not (case.root / "cores").exists()
    case.env.pop("HOLD_CORE_RECORD")
    case.run(parameters=["--stop"])
    case.clean()

    # Foreign rules survive ordinary and crash recovery cleanup.
    case = Case()
    foreign = [
        ["INPUT", "-s", "172.19.0.2", "-d", "172.19.0.1",
         "-p", "tcp", "-j", "ACCEPT"],
        ["INPUT", "-s", "172.19.0.2", "-d", "172.19.0.1",
         "-p", "tcp", "-j", "ACCEPT",
         "-m", "comment", "--comment", "other-tool"],
    ]
    (case.root / "iptables.json").write_text(json.dumps(foreign))
    case.run(FAKE_MODE="quick")
    assert case.rules("iptables") == foreign
    owner = case.spawn()
    identity = case.ready()
    owner.kill()
    owner.wait(timeout=5)
    case.run(parameters=["--stop"])
    until(lambda: not alive(identity))
    assert case.rules("iptables") == foreign
    assert not case.rules("ip6tables")
    assert not case.record()

    # Same-name decoy is not signalled by termination.
    decoy_case = Case("decoy")
    decoy = subprocess.Popen(
        [str(CORE), "decoy"],
        env=decoy_case.env,
        stdout=decoy_case.log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
        preexec_fn=default_signals,
    )
    PROCESSES.append(decoy)
    until(lambda: (decoy_case.root / "cores").exists())
    decoy_identity = token(decoy.pid)
    remember(decoy_identity)

    case = Case()
    owner = case.spawn()
    case.ready()
    case.stop(owner)
    assert alive(decoy_identity)

    # A stale, mismatching process token must not signal that decoy.
    case = Case()
    owner = case.spawn()
    identity = case.ready()
    owner.kill()
    owner.wait(timeout=5)
    os.kill(int(identity.split(":")[0]), signal.SIGTERM)
    until(lambda: not alive(identity))
    state = case.root / "nekoray-vpn.state"
    lines = state.read_text().splitlines()
    state.write_text("\n".join(
        "CORE={}:0".format(decoy.pid) if line.startswith("CORE=") else line
        for line in lines
    ) + "\n")
    case.run(parameters=["--stop"])
    case.clean()
    assert alive(decoy_identity)
    decoy.terminate()
    assert decoy.wait(timeout=5) == 0

    # UID policy is simulated here; no polkit claim is made.
    case = Case(PKEXEC_UID="1000")
    owner = case.spawn()
    identity = case.ready()
    case.run(expected=77, parameters=["--terminate"], PKEXEC_UID="1001")
    assert alive(identity)
    case.run(parameters=["--terminate"], PKEXEC_UID="1000")
    assert owner.wait(timeout=8) == 0
    case.clean()

    owner = case.spawn()
    case.ready()
    case.run(parameters=["--terminate"], PKEXEC_UID="0")
    assert owner.wait(timeout=8) == 0
    case.clean()

    owner = case.spawn()
    identity = case.ready()
    owner.kill()
    owner.wait(timeout=5)
    case.run(expected=77, parameters=["--stop"], PKEXEC_UID="1001")
    assert alive(identity)
    case.run(parameters=["--stop"], PKEXEC_UID="1000")
    case.clean()

    # Both contenders remain live unless rejected, making double starts visible.
    for _ in range(200):
        case = Case()
        first = case.spawn()
        second = case.spawn()
        until(lambda: first.poll() is not None or second.poll() is not None)
        loser, winner = ((first, second) if first.poll() is not None
                         else (second, first))
        assert loser.returncode == 75
        case.ready()
        assert len((case.root / "cores").read_text().splitlines()) == 1
        case.stop(winner)

    print("W07 sandbox: PASS (200 race rounds)")


try:
    run_tests()
finally:
    # Only test-owned processes or captured identities are touched.
    for process in PROCESSES:
        if process.poll() is None:
            process.terminate()
    for identity in IDENTITIES:
        if alive(identity):
            try:
                os.kill(int(identity.split(":")[0]), signal.SIGTERM)
            except ProcessLookupError:
                pass
    for process in PROCESSES:
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)
    for identity in IDENTITIES:
        if alive(identity):
            try:
                os.kill(int(identity.split(":")[0]), signal.SIGKILL)
            except ProcessLookupError:
                pass
    for output in LOGS:
        output.close()
    ROOT.cleanup()
