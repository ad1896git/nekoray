#!/bin/bash
# Validate the assembled deployment before creating a Windows or Linux ZIP.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
platform=$1
output=$2
case "$platform" in
    linux) payload=linux64 ;;
    windows) payload=windows64 ;;
    *) echo "Unsupported ZIP platform: $platform" >&2; exit 1 ;;
esac
python3 "$repo/libs/validate_licenses.py" package "$payload" "$platform"
mv "$payload" nekoray
python3 "$repo/libs/validate_licenses.py" package nekoray "$platform"
zip -r "$output" nekoray
rm -rf nekoray
