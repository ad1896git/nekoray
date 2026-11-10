#!/bin/bash
# Package an already deployed app (after macdeployqt) and matching core notices.
# Usage: package_macos.sh /path/NekoBox.app /path/core-output /path/output.dmg
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
app=$1
core=$2
output=$3
resources="$app/Contents/Resources"
test -d "$app/Contents/MacOS"
python3 "$repo/libs/validate_licenses.py" go "$core" macos
bash "$repo/libs/package_licenses.sh" "$resources" macos
rm -rf "$resources/licenses/go"
cp -R "$core/licenses/go" "$resources/licenses/"
python3 "$repo/libs/validate_licenses.py" go "$resources" macos --binary "$app/Contents/MacOS/nekobox_core"
# Resource changes must precede the final bundle signature.
codesign --force --sign - "$app"
codesign --verify --deep --strict "$app"
stage=$(mktemp -d "${TMPDIR:-/tmp}/nekobox-dmg.XXXXXXXX")
trap 'rm -rf "$stage"' EXIT
ditto "$app" "$stage/$(basename "$app")"
ln -s /Applications "$stage/Applications"
hdiutil create -volname "NekoBox-$(cat "$repo/nekoray_version.txt")" \
    -srcfolder "$stage" -format UDZO "$output"
