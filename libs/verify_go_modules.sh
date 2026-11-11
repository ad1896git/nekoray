#!/usr/bin/env bash
# Verify cached dependencies with project-local modules treated as workspace roots.
# Run libs/get_source.sh first to populate the existing sibling source layout.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
export GOWORK="$repo/go/verify.go.work"
cd -- "$repo/go/cmd/nekobox_core"
go mod verify
cd -- "$repo/go/grpc_server"
go mod verify
