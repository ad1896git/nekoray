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

sudo apt-get update
sudo apt-get install -y "$subject"
assert_installed fresh
sudo apt-get remove -y nekoray
test ! -e /opt/nekoray && test ! -e /usr/share/applications/nekoray.desktop
sudo apt-get purge -y nekoray
assert_purged

sudo apt-get install -y "$baseline"
test "$(dpkg-query -W -f='${Version}' nekoray)" = 4.0.1
stat -c 'baseline owner=%u:%g mode=%a %n' /opt/nekoray
# Exercise retained nested directories/files as well as the original defect.
sudo mkdir -p /opt/nekoray/ownership-probe/nested
sudo touch /opt/nekoray/ownership-probe/nested/file
sudo chown -hR 12345:12345 /opt/nekoray/ownership-probe
sudo chmod -R a+rwX /opt/nekoray/ownership-probe
sudo chown -h 12345:12345 /opt/nekoray
sudo chmod go+w /opt/nekoray
sudo apt-get install -y "$subject"
assert_installed upgrade
# A repeated configure must restore ownership/modes and then restore setcap.
sudo chown -h 12345:12345 /opt/nekoray/nekobox_core
sudo chmod go+w /opt/nekoray/nekobox_core
sudo dpkg-reconfigure nekoray
assert_installed reconfigure
sudo rm -r /opt/nekoray/ownership-probe
# Every remaining prefix entry must be tracked by the new package.
find -P /opt/nekoray -xdev -print > "$work/paths"
while IFS= read -r path; do dpkg-query -S "$path"; done < "$work/paths"
sudo apt-get purge -y nekoray
assert_purged
sudo apt-get check
sudo dpkg --audit > "$work/final-audit"
test ! -s "$work/final-audit"
echo 'Debian fresh-install and 4.0.1 upgrade lifecycle PASS'
