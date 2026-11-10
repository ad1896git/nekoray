#!/bin/bash
# Run only on a disposable Ubuntu CI runner; installs/removes real packages.
set -Eeuo pipefail
export LC_ALL=C DEBIAN_FRONTEND=noninteractive
trap 'echo "Debian lifecycle failure at line $LINENO (exit $?)" >&2' ERR

subject=$(realpath "${1:?usage: verify_debian_lifecycle.sh package.deb}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
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
    local path status
    for path in /opt/nekoray /usr/share/doc/nekoray /usr/share/applications/nekoray.desktop \
        /var/lib/dpkg/info/nekoray.preinst /var/lib/dpkg/info/nekoray.postinst /var/lib/dpkg/info/nekoray.postrm; do
        test ! -e "$path" && test ! -L "$path"
    done
    status=$(dpkg-query -W -f='${db:Status-Status}' nekoray 2>/dev/null || true)
    test -z "$status" || test "$status" = not-installed
    echo 'purge: no packaged prefix, launcher, documentation or maintainer scripts PASS'
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
sudo mkdir -m 0755 /opt

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
    quarantine_uid=$(bash -e -c "$(recovery_command prepare)")
    test "$quarantine_uid" = 1001
    export quarantine_uid
    test "$(stat -c %u:%g:%a /opt)" = 0:0:755
    test ! -e /opt/nekoray && test ! -L /opt/nekoray
    test -e /opt/nekoray.untrusted || test -L /opt/nekoray.untrusted
    assert_sentinel
    # No old prerm/postrm exists. Trace dpkg's real script routing as well as
    # checking the old-postinst wrapper (where present) remains empty.
    sudo strace -f -s 512 -e trace=execve -o "$work/$label-remove-exec.log" \
        bash -e -c "$(recovery_command remove)" > "$work/$label-remove.log" 2>&1
    cat "$work/$label-remove.log" "$work/$label-remove-exec.log"
    if grep -E '/nekoray\.(preinst|postinst|prerm|postrm)' "$work/$label-remove-exec.log"; then
        echo 'Unexpected nekoray maintainer-script execution during old removal' >&2
        return 1
    fi
    echo "OLD_REMOVAL_SCRIPTS: no nekoray maintainer scripts executed ($label)"
    assert_purged
    test "$(stat -c %u:%g:%a /opt)" = 0:0:755
    assert_sentinel
    bash -e -c "$(recovery_command install)"
    assert_installed "$label-aprime-install"
    assert_sentinel
    bash -e -c "$(recovery_command dispose)"
    test ! -e /opt/nekoray.untrusted && test ! -L /opt/nekoray.untrusted
    assert_sentinel
    echo "APRIME_STEPS_RESULT: PASS ($label; quarantine removed as UID $quarantine_uid)"
}

# Exact upstream archive installed as-is before planting a replacement inode.
sudo apt-get install -y "$baseline"
test "$(dpkg-query -W -f='${Status}' nekoray)" = 'install ok installed'
stat -c 'baseline owner=%u:%g mode=%a %n' /opt /opt/nekoray
test "$(stat -c %u:%g /opt/nekoray)" = 1001:127
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
