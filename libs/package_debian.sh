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
python3 "$repo/libs/validate_licenses.py" debian nekoray linux

# basic
cat >nekoray/DEBIAN/control <<-EOF
Package: nekoray
Version: $version
Architecture: amd64
Maintainer: MatsuriDayo nekoha_matsuri@protonmail.com
Depends: libxcb-xinerama0, libqt5core5a, libqt5gui5, libqt5network5, libqt5widgets5, libqt5svg5, libqt5x11extras5, desktop-file-utils, libcap2-bin
Description: Qt based cross-platform GUI proxy configuration manager (backend: v2ray / sing-box)
EOF

mkdir -p nekoray/usr/share/applications
cat >nekoray/usr/share/applications/nekoray.desktop <<'EOF'
[Desktop Entry]
Name=nekoray
Comment=Qt based cross-platform GUI proxy configuration manager (backend: sing-box)
Exec=sh -c "PATH=/opt/nekoray:$PATH /opt/nekoray/nekobox -appdata"
Icon=/opt/nekoray/nekobox.png
Terminal=false
Type=Application
Categories=Network;Application;
EOF

cp "$repo/libs/debian/preinst" nekoray/DEBIAN/preinst
cp "$repo/libs/debian/postinst" nekoray/DEBIAN/postinst

cp "$repo/libs/debian/postrm" nekoray/DEBIAN/postrm

chmod 0755 nekoray/DEBIAN/preinst nekoray/DEBIAN/postinst nekoray/DEBIAN/postrm
chmod 0644 nekoray/usr/share/applications/nekoray.desktop

# desktop && PATH

dpkg-deb --root-owner-group -Zxz --build nekoray
python3 "$repo/libs/validate_debian.py" nekoray.deb
