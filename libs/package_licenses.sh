#!/bin/bash
# Add GUI/runtime notices. Go build output supplies licenses/go separately.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
dest=$1
platform=$2
python3 "$repo/libs/validate_licenses.py" native-source "$repo/libs/licenses" "$platform"
mkdir -p "$dest/licenses"
cp "$repo/LICENSE" "$dest/LICENSE"
cp "$repo/libs/licenses/SOURCES.txt" "$dest/THIRD-PARTY-NOTICES.txt"
cp "$repo/libs/licenses/inventory.json" "$dest/licenses/runtime-inventory.json"
cp -R "$repo/libs/licenses/common" "$dest/licenses/"
cp -R "$repo/libs/licenses/$platform" "$dest/licenses/"
cp "$repo/libs/patches/quic-go-v0.47.0-beta.2-congestion-lock.patch" "$dest/licenses/"
python3 "$repo/libs/validate_licenses.py" native "$dest" "$platform"
