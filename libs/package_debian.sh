#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
python3 "$repo/libs/validate_licenses.py" package linux64 linux

version="${1#v}"  # Debian versions must start with a digit

mkdir -p nekoray/DEBIAN
mkdir -p nekoray/opt
cp -r linux64 nekoray/opt/
mv nekoray/opt/linux64 nekoray/opt/nekoray
rm -rf nekoray/opt/nekoray/usr

# Install distribution documentation in the Debian policy location.
mkdir -p nekoray/usr/share/doc/nekoray
mv nekoray/opt/nekoray/LICENSE nekoray/usr/share/doc/nekoray/LICENSE
mv nekoray/opt/nekoray/THIRD-PARTY-NOTICES.txt nekoray/usr/share/doc/nekoray/copyright
mv nekoray/opt/nekoray/licenses nekoray/usr/share/doc/nekoray/licenses

# basic
cat >nekoray/DEBIAN/control <<-EOF
Package: nekoray
Version: $version
Architecture: amd64
Maintainer: MatsuriDayo nekoha_matsuri@protonmail.com
Depends: libxcb-xinerama0, libqt5core5a, libqt5gui5, libqt5network5, libqt5widgets5, libqt5svg5, libqt5x11extras5, desktop-file-utils, libcap2-bin
Description: Qt based cross-platform GUI proxy configuration manager (backend: v2ray / sing-box)
EOF

cat >nekoray/DEBIAN/postinst <<'EOF'
#!/bin/sh
set -e

case "$1" in
    configure) ;;
    abort-upgrade|abort-remove|abort-deconfigure) exit 0 ;;
    *) echo "nekoray postinst: unsupported argument: $1" >&2; exit 1 ;;
esac

if [ ! -s /usr/share/applications/nekoray.desktop ]; then
    cat >/usr/share/applications/nekoray.desktop <<'END'
[Desktop Entry]
Name=nekoray
Comment=Qt based cross-platform GUI proxy configuration manager (backend: sing-box)
Exec=sh -c "PATH=/opt/nekoray:$PATH /opt/nekoray/nekobox -appdata"
Icon=/opt/nekoray/nekobox.png
Terminal=false
Type=Application
Categories=Network;Application;
END
fi

if ! setcap cap_net_admin=ep /opt/nekoray/nekobox_core; then
    echo "WARNING: nekoray: failed to set CAP_NET_ADMIN on nekobox_core; TUN is unavailable. Check libcap2-bin and filesystem capability support, then run dpkg --configure nekoray." >&2
    exit 1
fi

update-desktop-database
EOF

sudo chmod 0755 nekoray/DEBIAN/postinst

# desktop && PATH

sudo dpkg-deb -Zxz --build nekoray
