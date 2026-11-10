#!/bin/bash
# Run only on a disposable Ubuntu CI runner; installs/removes real packages.
set -Eeuo pipefail
export LC_ALL=C DEBIAN_FRONTEND=noninteractive
trap 'echo "Debian lifecycle failure at line $LINENO (exit $?)" >&2' ERR

subject=$(realpath "${1:?usage: verify_debian_lifecycle.sh package.deb}")
work=$(mktemp -d)
hostile_cwd=$(mktemp -d /var/tmp/nekoray-python-XXXXXX)
sudo_python_policy=/etc/sudoers.d/nekoray-python-regression
test ! -e "$sudo_python_policy"
trap '/usr/bin/sudo /usr/bin/rm -f -- "$sudo_python_policy"; /usr/bin/sudo /usr/bin/rm -rf -- "$hostile_cwd" "$work"' EXIT
baseline_name=nekoray-4.0.1-2024-12-12-debian-x64.deb
baseline="$work/$baseline_name"

curl --fail --location --retry 2 --max-time 180 \
    "https://github.com/MatsuriDayo/nekoray/releases/download/4.0.1/$baseline_name" \
    --output "$baseline"
printf '%s  %s\n' cdc19757372c8fb69ac78f6ebfb19442d777da15c64c26251ff67c99638ddebe "$baseline" | sha256sum -c -
sha256sum "$subject" "$baseline"
python3 libs/validate_debian.py "$subject"
test "$(dpkg-deb -f "$subject" Package)" = nekoray
test "$(dpkg-deb -f "$baseline" Version)" = 4.0.1
version=$(dpkg-deb -f "$subject" Version)
dpkg-deb -e "$subject" "$work/control"

assert_installed() {
    local label=$1 bad
    test "$(dpkg-query -W -f='${Status}' nekoray)" = 'install ok installed'
    test "$(dpkg-query -W -f='${Version}' nekoray)" = "$version"
    test ! -L /opt/nekoray && test -d /opt/nekoray
    test "$(stat -c %u:%g:%a /opt)" = 0:0:755
    echo 'FINAL_OPT_OWNER_MODE: 0:0:755'
    # Assert every entry in the prefix, including directories retained by dpkg.
    bad=$(find -P /opt/nekoray -xdev \( ! -uid 0 -o ! -gid 0 -o -perm /6022 \) -print)
    test -z "$bad" || { printf 'Unsafe installed paths:\n%s\n' "$bad" >&2; return 1; }
    find -P /opt/nekoray -xdev -printf '%u:%g %m %p\n'
    getcap /opt/nekoray/nekobox_core | grep -Fx '/opt/nekoray/nekobox_core cap_net_admin=ep'
    dpkg-query -L nekoray | grep -Fx /usr/share/applications/nekoray.desktop
    dpkg-query -S /usr/share/applications/nekoray.desktop | grep -Fx 'nekoray: /usr/share/applications/nekoray.desktop'
    cmp "$work/control/preinst" /var/lib/dpkg/info/nekoray.preinst
    cmp "$work/control/postinst" /var/lib/dpkg/info/nekoray.postinst
    cmp "$work/control/postrm" /var/lib/dpkg/info/nekoray.postrm
    sudo apt-get check
    sudo dpkg --audit > "$work/audit"
    test ! -s "$work/audit"
    echo "$label: ownership, modes, capability, launcher and dpkg state PASS"
}

assert_purged() {
    local path status kind=${1:-new}
    for path in /opt/nekoray /usr/share/doc/nekoray \
        /var/lib/dpkg/info/nekoray.preinst /var/lib/dpkg/info/nekoray.postinst /var/lib/dpkg/info/nekoray.postrm; do
        test ! -e "$path" && test ! -L "$path"
    done
    status=$(dpkg-query -W -f='${db:Status-Status}' nekoray 2>/dev/null || true)
    test -z "$status" || test "$status" = not-installed
    if [ "$kind" = old ]; then
        # 4.0.1 creates this file in postinst without tracking it in dpkg.
        test -s /usr/share/applications/nekoray.desktop
        sha256sum /usr/share/applications/nekoray.desktop > "$work/launcher-after"
        cmp "$work/launcher-before" "$work/launcher-after"
        if dpkg-query -S /usr/share/applications/nekoray.desktop; then
            echo 'Old launcher unexpectedly tracked after old package purge' >&2
            return 1
        fi
        echo 'old purge: package absent, untracked desktop launcher preserved PASS'
    else
        test ! -e /usr/share/applications/nekoray.desktop
        test ! -L /usr/share/applications/nekoray.desktop
        echo 'purge: no packaged prefix, launcher, documentation or maintainer scripts PASS'
    fi
}

# Capability checks must fail on command errors, not treat failed output as empty.
assert_no_cap() {
    local caps
    caps=$(getcap "$1")
    test -z "$caps"
}
sudo apt-get update
sudo apt-get install -y libcap2-bin strace
# sudo can require a passwd entry even for a numeric runas UID.
if ! getent passwd 1001; then
    sudo useradd --uid 1001 --no-create-home --shell /usr/sbin/nologin nekoray-original-owner
fi
test "$(sudo -u '#1001' -- id -u)" = 1001

# Execute the exact user-facing Bash blocks, so documentation drift fails CI.
recovery_command() {
    python3 - "$1" <<'PYDOC'
from pathlib import Path
import sys
text = Path("docs/PACKAGING.md").read_text()
marker = "<!-- aprime-" + sys.argv[1] + " -->\n```bash\n"
assert text.count(marker) == 1
print(text.split(marker, 1)[1].split("\n```", 1)[0])
PYDOC
}
new_deb=$subject
export new_deb
prepare_command=$(recovery_command prepare)
remove_command=$(recovery_command remove)
install_command=$(recovery_command install)
dispose_command=$(recovery_command dispose)

# Static attacker fixture: no attacker process needs to run during recovery.
# Markers use only builtins so imports cannot hide a broken payload. Include
# direct/transitive imports and interpreter/site startup names on both Python
# versions; some are frozen/preloaded, but tempfile is an effective control.
/usr/bin/python3 -I - "$hostile_cwd" <<'PYPLANT'
from pathlib import Path
import sys
root = Path(sys.argv[1])
modules = ("ctypes errno subprocess tempfile os stat sys site sitecustomize usercustomize random shutil "
           "weakref functools types re enum copyreg warnings collections "
           "contextlib operator _collections_abc io abc encodings").split()
def payload(name):
    return (f"with open({str(root / ('marker-' + name))!r}, 'w') as marker:\n"
            "    marker.write('attacker code executed')\nraise SystemExit(73)\n")
for name in modules + ['startup']:
    (root / (name + '.py')).write_text(payload(name))
user_site = root / 'userbase' / 'lib' / ('python%d.%d' % sys.version_info[:2]) / 'site-packages'
user_site.mkdir(parents=True)
for name in ('sitecustomize', 'usercustomize'):
    (user_site / (name + '.py')).write_text(payload('user-site-' + name))
for name in ('sudo', 'env', 'python3', 'dpkg', 'apt-get', 'rm'):
    command = root / name
    command.write_text(f"#!/bin/sh\nprintf '%s\\n' 'attacker command executed' > '{root}/marker-command-{name}'\nexit 73\n")
    command.chmod(0o755)
PYPLANT
sudo chown -R 1001:127 "$hostile_cwd"
test "$(stat -c %u "$hostile_cwd")" = 1001
# Deliberately exercise a policy that keeps hostile Python variables. The
# production blocks must not depend on the default sudo environment filtering.
printf '%s\n' 'Defaults env_keep += "PYTHONPATH PYTHONSTARTUP PYTHONUSERBASE"' | sudo tee "$sudo_python_policy" >/dev/null
sudo chmod 0440 "$sudo_python_policy"
sudo /usr/sbin/visudo -cf "$sudo_python_policy"
assert_no_import_marker() {
    local markers
    markers=$(/usr/bin/find "$hostile_cwd" -name 'marker-*' -print)
    test -z "$markers" || { printf 'Planted code executed:\n%s\n' "$markers" >&2; return 1; }
}
(
    cd "$hostile_cwd"
    # -S skips startup hooks so this specifically proves cwd tempfile import.
    # No PYTHONPATH is needed for the positive control.
    if /usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/bin/python3 -S - <<'PYCONTROL'
import tempfile
PYCONTROL
    then
        echo 'Non-isolated import positive control unexpectedly succeeded' >&2
        exit 1
    else
        test "$?" -eq 73
    fi
    test -s marker-tempfile
    test "$(/usr/bin/stat -c %u marker-tempfile)" = 0
    /usr/bin/sudo /usr/bin/rm -- marker-tempfile
)
assert_no_import_marker
echo 'POSITIVE_CONTROL_RESULT: PASS (non-isolated root interpreter imported cwd tempfile.py; exit 73)'

hostile_recovery() (
    local action=$1 label=${2:-unused}
    cd "$hostile_cwd"
    export PATH="$hostile_cwd:/usr/sbin:/usr/bin:/sbin:/bin"
    export PYTHONPATH="$hostile_cwd" PYTHONSTARTUP="$hostile_cwd/startup.py" PYTHONUSERBASE="$hostile_cwd/userbase"
    # Confirm sudo really preserves the hostile environment, rather than
    # silently making the regression pass by discarding it before Python.
    /usr/bin/sudo /usr/bin/python3 -I - "$hostile_cwd" <<'PYENV'
import os, sys
assert os.environ['PYTHONPATH'] == sys.argv[1]
assert os.environ['PYTHONSTARTUP'] == sys.argv[1] + '/startup.py'
assert os.environ['PYTHONUSERBASE'] == sys.argv[1] + '/userbase'
assert sys.flags.isolated and sys.flags.ignore_environment and sys.flags.no_user_site
PYENV
    case "$action" in
        prepare)
            eval "$prepare_command"
            printf '%s\n' "$quarantine_uid"
            ;;
        remove)
            /usr/bin/sudo /usr/bin/strace -f -s 512 -e trace=execve -o "$work/$label-remove-exec.log" \
                /bin/bash --noprofile --norc -e -c "$remove_command"
            ;;
        install) /bin/bash --noprofile --norc -e -c "$install_command" ;;
        dispose) /bin/bash --noprofile --norc -e -c "$dispose_command" ;;
        *) return 1 ;;
    esac
)

# Outside-prefix sentinel: both metadata and bytes must survive refusals.
mkdir "$work/outside-prefix"
sentinel="$work/outside-prefix/nekobox_core"
cp /bin/true "$sentinel"
python3 - "$sentinel" <<'PYATTR'
import os, sys
os.setxattr(sys.argv[1], "user.nekoray-lifecycle", b"must survive refusal")
PYATTR
chmod 0664 "$sentinel"
cp /bin/true "$work/outside-prefix/geoip.dat"
chmod 0640 "$work/outside-prefix/geoip.dat"
cp /bin/true "$work/outside-prefix/nekobox_core.dpkg-new"
chmod 04755 "$work/outside-prefix/nekobox_core.dpkg-new"
# Snapshot all xattrs, including security.capability, without following links.
sentinel_state() {
    stat -c '%u:%g:%a:%n' "$work/outside-prefix" "$work/outside-prefix/"*
    sha256sum "$work/outside-prefix/"*
    python3 - "$sentinel" <<'PYATTR'
import os, sys
p = sys.argv[1]
print([(k, os.getxattr(p, k, follow_symlinks=False).hex())
       for k in sorted(os.listxattr(p, follow_symlinks=False))])
PYATTR
    assert_no_cap "$sentinel"
}
sentinel_state > "$work/sentinel-before"
assert_sentinel() {
    sentinel_state > "$work/sentinel-after"
    cmp "$work/sentinel-before" "$work/sentinel-after"
    echo 'outside sentinel: owner, mode, contents and xattrs UNTOUCHED'
}
assert_refused() {
    local label=$1 rc
    if sudo apt-get install --reinstall -y "$subject" > "$work/$label.log" 2>&1; then
        cat "$work/$label.log"
        echo 'Unsafe install unexpectedly succeeded' >&2
        return 1
    else
        rc=$?
    fi
    cat "$work/$label.log"
    test "$rc" -eq 100
    grep -F 'nekoray: refusing unsafe pre-existing install:' "$work/$label.log"
    grep -F 'docs/PACKAGING.md#administrator-recovery-a-prime' "$work/$label.log"
    grep -F 'pre-installation script subprocess returned error exit status 1' "$work/$label.log"
    grep -F 'Half-Installed' "$work/$label.log"
    assert_sentinel
    echo "$label: preinst refusal PASS (apt exit $rc)"
}

sudo apt-get install -y "$subject"
assert_installed fresh
# Reinstall over a trusted tree must succeed through actual apt/dpkg unpack.
sudo apt-get install --reinstall -y "$subject"
assert_installed reinstall-safe-tree
sudo dpkg-reconfigure nekoray
assert_installed reconfigure-safe-tree
sudo apt-get remove -y nekoray
test ! -e /opt/nekoray && test ! -e /usr/share/applications/nekoray.desktop
sudo apt-get purge -y nekoray
assert_purged

# Exercise the documented STOP before touching an unsafe /opt entry itself.
# The clean package has been purged; only this disposable rootfs is changed.
if [ -d /opt ]; then sudo rmdir /opt; fi
for kind in symlink file; do
    if [ "$kind" = symlink ]; then
        sudo ln -s "$work/outside-prefix" /opt
    else
        sudo touch /opt
    fi
    if bash -e -c "$(recovery_command prepare)" > "$work/stop-$kind.log" 2>&1; then
        echo "Preparation unexpectedly accepted /opt as $kind" >&2
        exit 1
    fi
    cat "$work/stop-$kind.log"
    grep -F 'STOP: /opt is a symlink or not a directory' "$work/stop-$kind.log"
    assert_sentinel
    sudo unlink /opt
done
# Leave /opt absent so the real 4.0.1 archive recreates its original ownership.

# Inspect authenticated old removal entry points BEFORE testing the remedy.
dpkg-deb -e "$baseline" "$work/old-control"
for script in prerm postrm; do
    test ! -e "$work/old-control/$script"
    echo "old 4.0.1 $script: ABSENT (no removal-script writes)"
done
cat "$work/old-control/postinst"
grep -Fx 'setcap cap_net_admin=ep /opt/nekoray/nekobox_core' "$work/old-control/postinst"

plant_core() {
    # Native executable bytes, never run. A new inode guarantees no inherited
    # capability; preserve the old prefix owner's authority over this path.
    sudo cp /bin/true /opt/nekoray/replacement-core
    sudo chown 1001:127 /opt/nekoray/replacement-core
    sudo mv /opt/nekoray/replacement-core /opt/nekoray/nekobox_core
    assert_no_cap /opt/nekoray/nekobox_core
}
assert_half_installed() {
    dpkg -s nekoray
    test "$(dpkg-query -W -f='${Version}' nekoray)" = "$1"
    local status
    status=$(dpkg-query -W -f='${Status}' nekoray)
    echo "POST_REFUSAL_DPKG_STATE: Status: $status"
    test "$status" = 'install reinstreq half-installed'
}
recover_refused_old() {
    local label=$1 quarantine_uid
    quarantine_uid=$(hostile_recovery prepare)
    assert_no_import_marker
    test "$quarantine_uid" = 1001
    export quarantine_uid
    test "$(stat -c %u:%g:%a /opt)" = 0:0:755
    test ! -e /opt/nekoray && test ! -L /opt/nekoray
    test -e /opt/nekoray.untrusted || test -L /opt/nekoray.untrusted
    assert_sentinel
    if [ "$label" = real-capable-401 ]; then
        # A scheduled active UID runs BETWEEN the exact prepare/remove blocks.
        # Its source is the quarantined, original, already-revoked core inode.
        sudo -u '#1001' -- /usr/bin/mv /opt/nekoray.untrusted/nekobox_core /opt/attacker-cache/moved-core
        test "$(stat -c %d:%i /opt/attacker-cache/moved-core)" = "$real_core_identity"
        assert_no_cap /opt/attacker-cache/moved-core
        assert_no_cap /opt/attacker-cache/retained-core
        assert_no_cap /home/nekoray-capability-test/retained-core
        assert_no_cap /opt/attacker-cache/stale-capable-core
        echo 'ACTIVE_MOVE_RESULT: PASS (uid 1001 moved real legacy inode out of quarantine; getcap empty)'
    fi
    sha256sum /usr/share/applications/nekoray.desktop > "$work/launcher-before"
    # No old prerm/postrm exists. Trace dpkg's real script routing as well as
    # checking the old-postinst wrapper (where present) remains empty.
    hostile_recovery remove "$label" > "$work/$label-remove.log" 2>&1
    assert_no_import_marker
    cat "$work/$label-remove.log" "$work/$label-remove-exec.log"
    if grep -E '/nekoray\.(preinst|postinst|prerm|postrm)' "$work/$label-remove-exec.log"; then
        echo 'Unexpected nekoray maintainer-script execution during old removal' >&2
        return 1
    fi
    echo "OLD_REMOVAL_SCRIPTS: no nekoray maintainer scripts executed ($label)"
    assert_purged old
    test "$(stat -c %u:%g:%a /opt)" = 0:0:755
    assert_sentinel
    hostile_recovery install
    assert_no_import_marker
    assert_installed "$label-aprime-install"
    assert_sentinel
    hostile_recovery dispose
    assert_no_import_marker
    test ! -e /opt/nekoray.untrusted && test ! -L /opt/nekoray.untrusted
    assert_sentinel
    echo "PLANTED_MODULE_RESULT: PASS ($label; uid 1001 cwd, hostile PATH/PYTHON*, preserved sudo environment, no marker)"
    echo "APRIME_STEPS_RESULT: PASS ($label; quarantine removed as UID $quarantine_uid)"
    if [ "$label" = real-capable-401 ]; then
        for alias in /opt/attacker-cache/retained-core /home/nekoray-capability-test/retained-core /opt/attacker-cache/moved-core /opt/attacker-cache/stale-capable-core; do
            test -f "$alias"
            assert_no_cap "$alias"
        done
        echo 'HARDLINK_RESULT: PASS (real legacy core; /opt cache and home aliases survive disposal without capability)'
        echo 'CAP_SCAN_RESULT: PASS (separate uid-1001 inode detected, revoked and remains empty after recovery)'
    fi
}

# Synchronized old-UID collision between an early negative observation and
# fchown. Execute the published Python body verbatim; monkeypatch ONLY fchown
# to schedule a real uid-1001 filesystem operation immediately before lockdown.
# The ordinary rename control first demonstrates the former overwrite hazard.
sudo mkdir -p /opt
sudo chown 1001:127 /opt
printf '%s\n' "$prepare_command" > "$work/prepare.bash"
cat > "$work/collision.py" <<'PYCOLLISION'
from pathlib import Path
import os, subprocess, sys
prepare = Path(sys.argv[1]).read_text().split("<<'PYRECOVER'\n", 1)[1].split('\nPYRECOVER', 1)[0]
runas = ['/usr/bin/sudo', '-u', '#1001', '--', '/usr/bin/python3', '-I', '-']
def attacker(code):
    subprocess.run(runas, input=code, text=True, check=True, timeout=30)
attacker("import os; os.symlink('/nonexistent-control', '/opt/old-rename-source'); "
         "open('/opt/old-rename-destination', 'w').write('preserve me')")
# Old ordering: destination planted after an absence check can be overwritten.
os.rename('/opt/old-rename-source', '/opt/old-rename-destination')
assert os.path.islink('/opt/old-rename-destination')
os.unlink('/opt/old-rename-destination')
print('COLLISION_POSITIVE_CONTROL: PASS (ordinary rename overwrote planted file)', flush=True)
attacker("import os; os.symlink('/nonexistent-prefix-target', '/opt/nekoray')")
assert not os.path.lexists('/opt/nekoray.untrusted')  # early observation
original = os.fchown
scheduled = False
def scheduled_fchown(fd, uid, gid):
    global scheduled
    assert not scheduled and (uid, gid) == (0, 0)
    assert os.fstat(fd).st_ino == os.stat('/opt').st_ino
    attacker("open('/opt/nekoray.untrusted', 'w').write('quarantine must be preserved')")
    scheduled = True
    original(fd, uid, gid)
os.fchown = scheduled_fchown
try:
    try:
        exec(compile(prepare, '<verbatim-published-prepare>', 'exec'), {})
    except SystemExit as error:
        assert str(error).startswith('STOP: /opt/nekoray.untrusted already exists'), error
    else:
        raise AssertionError('collision unexpectedly accepted')
finally:
    os.fchown = original
assert scheduled
assert Path('/opt/nekoray.untrusted').read_text() == 'quarantine must be preserved'
assert os.path.islink('/opt/nekoray')
# Exercise the atomic primitive itself with an existing file too: EEXIST,
# preserving the planted bytes, even if a privileged actor raced the check.
import ctypes, errno
libc = ctypes.CDLL(None, use_errno=True)
rename = libc.renameat2
rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
rename.restype = ctypes.c_int
fd = os.open('/opt', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
try:
    assert rename(fd, b'nekoray', fd, b'nekoray.untrusted', 1) == -1
    assert ctypes.get_errno() == errno.EEXIST
finally:
    os.close(fd)
assert Path('/opt/nekoray.untrusted').read_text() == 'quarantine must be preserved'
print('COLLISION_RESULT: PASS (scheduled uid 1001 collision STOP; no-replace EEXIST; planted object preserved)')
os.unlink('/opt/nekoray')
os.unlink('/opt/nekoray.untrusted')
PYCOLLISION
(
    cd "$hostile_cwd"
    export PYTHONPATH="$hostile_cwd" PYTHONSTARTUP="$hostile_cwd/startup.py" PYTHONUSERBASE="$hostile_cwd/userbase"
    /usr/bin/sudo /usr/bin/python3 -I "$work/collision.py" "$work/prepare.bash"
)
assert_no_import_marker
assert_sentinel
sudo rmdir /opt

# Genuine checksum-pinned 4.0.1 core: never replace its bytes or capability.
sudo apt-get install -y "$baseline"
getcap /opt/nekoray/nekobox_core | grep -Fx '/opt/nekoray/nekobox_core cap_net_admin=ep'
real_core_identity=$(stat -c %d:%i /opt/nekoray/nekobox_core)
sudo -u '#1001' -- /usr/bin/mkdir /opt/attacker-cache
sudo mkdir -p /home/nekoray-capability-test
sudo chown 1001:127 /home/nekoray-capability-test
# /home and /opt are on the same disposable container root filesystem.
test "$(stat -c %d /home/nekoray-capability-test)" = "$(stat -c %d /opt/nekoray/nekobox_core)"
sudo -u '#1001' -- /usr/bin/ln /opt/nekoray/nekobox_core /opt/attacker-cache/retained-core
sudo -u '#1001' -- /usr/bin/ln /opt/nekoray/nekobox_core /home/nekoray-capability-test/retained-core
for alias in /opt/attacker-cache/retained-core /home/nekoray-capability-test/retained-core; do
    test "$(stat -c %d:%i "$alias")" = "$real_core_identity"
    test "$(stat -c %u "$alias")" = 1001
    getcap "$alias" | grep -F 'cap_net_admin=ep'
done
# Active-move positive control on the SAME real capable inode, before recovery:
# a move out of a quarantine-like directory retains its capability; restore it.
sudo -u '#1001' -- /usr/bin/mkdir /opt/attacker-cache/quarantine-control
sudo -u '#1001' -- /usr/bin/mv /opt/nekoray/nekobox_core /opt/attacker-cache/quarantine-control/core
sudo -u '#1001' -- /usr/bin/mv /opt/attacker-cache/quarantine-control/core /opt/attacker-cache/move-control
getcap /opt/attacker-cache/move-control | grep -F 'cap_net_admin=ep'
test "$(stat -c %d:%i /opt/attacker-cache/move-control)" = "$real_core_identity"
sudo -u '#1001' -- /usr/bin/mv /opt/attacker-cache/move-control /opt/nekoray/nekobox_core
sudo -u '#1001' -- /usr/bin/rmdir /opt/attacker-cache/quarantine-control
# Model an inode setcap'd by an earlier postinst then swapped out of the prefix.
sudo -u '#1001' -- /usr/bin/cp /opt/nekoray/nekobox_core /opt/attacker-cache/stale-capable-core
sudo /usr/sbin/setcap cap_net_admin=ep /opt/attacker-cache/stale-capable-core
test "$(stat -c %u /opt/attacker-cache/stale-capable-core)" = 1001
test "$(stat -c %d:%i /opt/attacker-cache/stale-capable-core)" != "$real_core_identity"
getcap /opt/attacker-cache/stale-capable-core | grep -F 'cap_net_admin=ep'
echo 'CAPABILITY_POSITIVE_CONTROLS: PASS (real core hardlinks and uid-1001 move retain capability; separate stale inode capable)'
assert_refused upgrade-real-capable-401
assert_half_installed 4.0.1
getcap /opt/nekoray/nekobox_core | grep -F 'cap_net_admin=ep'
# Capture stderr as evidence that the scan DETECTED this separate inode.
recover_refused_old real-capable-401 2> "$work/real-capable-recovery-stderr.log"
cat "$work/real-capable-recovery-stderr.log"
grep -F "CAP_SCAN flagged: '/opt/attacker-cache/stale-capable-core'" "$work/real-capable-recovery-stderr.log"
assert_sentinel
sudo -u '#1001' -- /usr/bin/rm -- /opt/attacker-cache/retained-core /opt/attacker-cache/moved-core /opt/attacker-cache/stale-capable-core /home/nekoray-capability-test/retained-core
sudo rmdir /opt/attacker-cache /home/nekoray-capability-test
sudo dpkg --purge nekoray
assert_purged
# dpkg may already remove empty /opt. Ensure absence before the next legacy
# install so its archive recreates the original non-root owner.
if [ -d /opt ]; then sudo rmdir /opt; fi

# Exact upstream archive installed as-is before planting a replacement inode.
sudo apt-get install -y "$baseline"
test "$(dpkg-query -W -f='${Status}' nekoray)" = 'install ok installed'
stat -c 'baseline owner=%u:%g mode=%a %n' /opt /opt/nekoray
test "$(stat -c %u:%g /opt/nekoray)" = 1001:127
test "$(stat -c %u:%g /opt)" = 1001:127
plant_core
assert_refused upgrade-from-401
assert_half_installed 4.0.1
assert_no_cap /opt/nekoray/nekobox_core
echo 'PLANTED_CORE_GETCAP_AFTER_UNWIND: empty (exact 4.0.1)'
recover_refused_old exact-401
sudo dpkg --purge nekoray
assert_purged

# Test-only historical variant: keep exact data metadata and original postinst
# body, with a tracing wrapper. No hook is present in the shipped new package.
sudo dpkg-deb -R "$baseline" "$work/traced-old"
sudo cp "$work/old-control/postinst" "$work/old-postinst-original"
sudo tee "$work/traced-old/DEBIAN/postinst" >/dev/null <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$work/old-postinst-trace"
exec /bin/sh "$work/old-postinst-original" "\$@"
EOF
sudo chmod 0755 "$work/traced-old/DEBIAN/postinst"
sudo dpkg-deb -Zxz --build "$work/traced-old" "$work/traced-old.deb"

for layout in replacement-core nested-symlink prefix-symlink nonroot-opt; do
    sudo apt-get install -y "$work/traced-old.deb"
    grep -Ex 'configure( .*)?' "$work/old-postinst-trace"
    : > "$work/old-postinst-trace"
    plant_core
    if [ "$layout" = replacement-core ]; then
        # Root-owned /opt with the original UID owning the prefix/replacement.
        sudo chown 0:0 /opt
    elif [ "$layout" = nested-symlink ]; then
        # Retain a hostile nested layout through the complete recovery, too.
        sudo chown 0:0 /opt
        sudo chown -hR 0:0 /opt/nekoray
        sudo chown 1001:127 /opt/nekoray
        sudo mkdir -p /opt/nekoray/adversarial-probe/nested
        sudo ln -s "$sentinel" /opt/nekoray/adversarial-probe/nested/outside
        sudo chown -hR 12345:12345 /opt/nekoray/adversarial-probe
        sudo chmod 0777 /opt/nekoray/adversarial-probe /opt/nekoray/adversarial-probe/nested
    elif [ "$layout" = prefix-symlink ]; then
        saved_prefix=$(sudo mktemp -d /var/tmp/nekoray-saved-XXXXXX)
        sudo mv /opt/nekoray "$saved_prefix/payload"
        sudo chown 1001 "$saved_prefix"
        sudo chown 0:0 /opt
        sudo ln -s "$work/outside-prefix" /opt/nekoray
        sudo chown -h 1001:127 /opt/nekoray
        # Preserve the moved test payload for unprivileged cleanup too.
        sudo -u '#1001' -- rm -rf -- "$saved_prefix"
        assert_no_cap "$work/outside-prefix/nekobox_core"
    elif [ "$layout" = nonroot-opt ]; then
        test "$(stat -c %u:%g /opt)" = 1001:127
        echo "ADVERSARIAL_OPT_OWNER: 1001:127"
    fi
    assert_refused "traced-401-$layout"
    assert_half_installed 4.0.1
    test ! -s "$work/old-postinst-trace"
    echo "OLD_POSTINST_RAN_ON_UNWIND: no ($layout; configure tracing positive control passed)"
    assert_no_cap /opt/nekoray/nekobox_core
    echo "PLANTED_CORE_GETCAP_AFTER_UNWIND: empty ($layout)"
    assert_sentinel
    recover_refused_old "traced-401-$layout"
    test ! -s "$work/old-postinst-trace"
    sudo dpkg --purge nekoray
    assert_purged
    test ! -s "$work/old-postinst-trace"
done

sudo apt-get install -y "$subject"
assert_installed after-traced-401-remedy

# Force a failure AFTER the complete successful preinst check, in a test-only
# new-deb variant. Real dpkg must successfully unwind over the safe installed
# tree; capture the new postrm's action and exact return code as evidence.
sudo dpkg-deb -R "$subject" "$work/fail-safe"
printf '\necho "test-only forced preinst failure" >&2\nexit 1\n' | sudo tee -a "$work/fail-safe/DEBIAN/preinst" >/dev/null
sudo cp "$work/fail-safe/DEBIAN/postrm" "$work/safe-postrm-original"
sudo tee "$work/fail-safe/DEBIAN/postrm" >/dev/null <<EOF
#!/bin/sh
/bin/sh "$work/safe-postrm-original" "\$@"
rc=\$?
printf '%s: exit %s\n' "\$*" "\$rc" >> "$work/safe-unwind-trace"
exit "\$rc"
EOF
sudo dpkg-deb --root-owner-group -Zxz --build "$work/fail-safe" "$work/fail-safe.deb"
if sudo dpkg -i "$work/fail-safe.deb" > "$work/safe-failure.log" 2>&1; then
    echo 'Forced safe preinst failure unexpectedly succeeded' >&2
    exit 1
fi
cat "$work/safe-failure.log"
grep -F 'test-only forced preinst failure' "$work/safe-failure.log"
grep -Ex 'abort-upgrade .*: exit 0' "$work/safe-unwind-trace"
assert_installed safe-tree-failed-upgrade-unwind
sudo dpkg --purge nekoray
assert_purged
sudo apt-get check
sudo dpkg --audit > "$work/final-audit"
test ! -s "$work/final-audit"
assert_sentinel
echo 'ADVERSARIAL_RESULT: replacement-core, nested-symlink, prefix-symlink, nonroot-opt PASS'
echo 'SENTINEL_RESULT: owner, mode, contents and xattrs UNTOUCHED'
echo 'Debian fresh install, safe reinstall/unwind, 4.0.1 refusal/remedy and adversarial capability/xattr lifecycle PASS'
