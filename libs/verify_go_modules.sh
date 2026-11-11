#!/usr/bin/env bash
# Check recorded standalone sums, then rehash the separate workspace graph.
# This is partial coverage: see docs/Build_Core.md for the exact limitations.
# Run libs/get_source.sh first to populate the existing sibling source layout.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
export GOWORK=off
for module in cmd/nekobox_core grpc_server; do
    cd -- "$repo/go/$module"
    printf 'Checking recorded standalone checksums: %s (temporary manifests)\n' "$module"
    cp go.mod "$scratch/download.mod"
    cp go.sum "$scratch/download.sum"
    go mod download -modfile="$scratch/download.mod" all
done
printf '%s\n' 'Rehashing workspace graph only; standalone cache coverage is incomplete (see docs/Build_Core.md).'
cd -- "$repo/go/cmd/nekobox_core"
GOWORK="$repo/go/verify.go.work" go mod verify
