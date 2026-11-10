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
    # Assert every entry in the prefix, including directories retained by dpkg.
    bad=$(find -P /opt/nekoray -xdev \( ! -uid 0 -o ! -gid 0 -o -perm /6022 \) -print)
    test -z "$bad" || { printf 'Unsafe installed paths:\n%s\n' "$bad" >&2; return 1; }
    find -P /opt/nekoray -xdev -printf '%u:%g %m %p\n'
    getcap /opt/nekoray/nekobox_core | grep -Fx '/opt/nekoray/nekobox_core cap_net_admin=ep'
    dpkg-query -L nekoray | grep -Fx /usr/share/applications/nekoray.desktop
    dpkg-query -S /usr/share/applications/nekoray.desktop | grep -Fx 'nekoray: /usr/share/applications/nekoray.desktop'
    cmp "$work/control/preinst" /var/lib/dpkg/info/nekoray.preinst
    cmp "$work/control/postinst" /var/lib/dpkg/info/nekoray.postinst
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

# Outside-prefix sentinel: both metadata and bytes must survive refusals.
sentinel="$work/outside-sentinel"
printf 'outside prefix: unchanged\n' > "$sentinel"
chmod 0664 "$sentinel"
sentinel_state() { stat -c '%u:%g:%a' "$sentinel"; sha256sum "$sentinel"; }
sentinel_state > "$work/sentinel-before"
assert_sentinel() {
    sentinel_state > "$work/sentinel-after"
    cmp "$work/sentinel-before" "$work/sentinel-after"
    echo 'outside sentinel: owner, mode and contents UNTOUCHED'
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
    grep -F "sudo apt purge nekoray" "$work/$label.log"
    grep -F "sudo dpkg --purge nekoray" "$work/$label.log"
    grep -F 'pre-installation script subprocess returned error exit status 1' "$work/$label.log"
    assert_sentinel
    echo "$label: preinst refusal PASS (apt exit $rc)"
}

sudo apt-get update
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

# Exact upstream bytes, with their original non-root-owned prefix: no fixture
# ownership changes here, so this tests the real historical upgrade trigger.
sudo apt-get install -y "$baseline"
test "$(dpkg-query -W -f='${Version}' nekoray)" = 4.0.1
test "$(dpkg-query -W -f='${Status}' nekoray)" = 'install ok installed'
stat -c 'baseline owner=%u:%g mode=%a %n' /opt/nekoray
test "$(stat -c %u:%g /opt/nekoray)" != 0:0
sha256sum /opt/nekoray/nekobox_core /usr/share/applications/nekoray.desktop > "$work/baseline-files-before"
stat -c '%u:%g:%a' /opt/nekoray > "$work/baseline-prefix-before"
assert_refused upgrade-from-401
# dpkg's abort-upgrade recovery must preserve the configured old package.
dpkg -s nekoray
test "$(dpkg-query -W -f='${Version}' nekoray)" = 4.0.1
test "$(dpkg-query -W -f='${Status}' nekoray)" = 'install ok installed'
sha256sum -c "$work/baseline-files-before"
stat -c '%u:%g:%a' /opt/nekoray > "$work/baseline-prefix-after"
cmp "$work/baseline-prefix-before" "$work/baseline-prefix-after"
sudo apt-get check
sudo dpkg --audit > "$work/refusal-audit"
test ! -s "$work/refusal-audit"
echo 'upgrade-from-401: old 4.0.1 still installed/configured; payload unchanged PASS'

# Follow the error's documented remedy; upstream's untracked desktop launcher
# may survive its purge, and is adopted as a tracked file by the new package.
sudo apt-get purge -y nekoray
test ! -e /opt/nekoray && test ! -L /opt/nekoray
sudo apt-get install -y "$subject"
assert_installed purge-401-then-install

# Root-owned prefix, attacker-owned nested directory and planted symlink.
# This is a static fixture on a disposable runner; no hostile process runs.
sudo mkdir -p /opt/nekoray/adversarial-probe/nested
sudo ln -s "$sentinel" /opt/nekoray/adversarial-probe/nested/outside
sudo chown -hR 12345:12345 /opt/nekoray/adversarial-probe
sudo chmod 0777 /opt/nekoray/adversarial-probe /opt/nekoray/adversarial-probe/nested
assert_refused adversarial-nested-tree
test "$(stat -c %u:%g /opt/nekoray/adversarial-probe/nested)" = 12345:12345
test "$(stat -c %a /opt/nekoray/adversarial-probe/nested)" = 777
test -L /opt/nekoray/adversarial-probe/nested/outside
test "$(dpkg-query -W -f='${Version}' nekoray)" = "$version"
test "$(dpkg-query -W -f='${Status}' nekoray)" = 'install ok installed'
# Administrator cleanup of the inspected, non-concurrent test fixture.
sudo rm -r /opt/nekoray/adversarial-probe
assert_installed after-adversarial-refusal
assert_sentinel
sudo apt-get purge -y nekoray
assert_purged
sudo apt-get check
sudo dpkg --audit > "$work/final-audit"
test ! -s "$work/final-audit"
echo 'Debian fresh install, safe reinstall, 4.0.1 refusal/remedy and adversarial sentinel lifecycle PASS'
