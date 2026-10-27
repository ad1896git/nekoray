#!/bin/bash

sudo apt-get install fuse -y

cp -r linux64 nekobox.AppDir

# The file for Appimage

cat >nekobox.AppDir/nekobox.desktop <<-EOF
[Desktop Entry]
Name=nekobox
Exec=echo "nekobox started"
Icon=nekobox
Type=Application
Categories=Network
EOF

cat >nekobox.AppDir/AppRun <<-EOF
#!/bin/bash
echo "PATH: \${PATH}"
echo "nekobox runing on: \$APPDIR"
LD_LIBRARY_PATH=\${APPDIR}/usr/lib QT_PLUGIN_PATH=\${APPDIR}/usr/plugins \${APPDIR}/nekobox -appdata "\$@"
EOF

chmod +x nekobox.AppDir/AppRun

# build

# Pinned release. AppImageKit's latest release no longer ships this file.
curl -fLSO https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
echo "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0  appimagetool-x86_64.AppImage" | sha256sum -c - || exit 1
chmod +x appimagetool-x86_64.AppImage
./appimagetool-x86_64.AppImage nekobox.AppDir

# clean

rm appimagetool-x86_64.AppImage
rm -rf nekobox.AppDir
