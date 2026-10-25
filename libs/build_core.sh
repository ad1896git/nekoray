#!/usr/bin/env bash
# Build nekobox_core from the pinned MatsuriDayo sources by default.
# NKR_SRC_BASE overrides https://github.com/MatsuriDayo (forks/mirrors supported).
# NKR_SRC_DIR holds sing-box, sing-quic and libneko; default: repository parent.
# GOOS/GOARCH default to the host. VERSION defaults to git describe, or dev.
# NKR_CORE_OUT_DIR overrides <repository>/build-core/<GOOS>-<GOARCH>.
# Go 1.22+ is required; CI uses 1.22.12. The pinned dependencies may be
# incompatible with newer Go releases (including Go 1.27); use 1.22.12 then.
set -euo pipefail

die() { printf 'build_core: %s\n' "$*" >&2; exit 1; }
for command in go git; do
    command -v "$command" >/dev/null 2>&1 || die "required command not found: $command"
done

export CGO_ENABLED=0 GOTOOLCHAIN=local GOWORK=off
go_version=$(go version)
printf '%s\n' "$go_version"
if [[ $go_version =~ go([0-9]+)\.([0-9]+)([^0-9]|$) ]]; then
    major=${BASH_REMATCH[1]}
    minor=${BASH_REMATCH[2]}
    (( major > 1 || (major == 1 && minor >= 22) )) || die 'Go 1.22 or newer is required'
else
    die "cannot determine Go version (Go 1.22 or newer is required): $go_version"
fi

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
source "$repo_dir/libs/get_source_env.sh"
src_base=${NKR_SRC_BASE:-https://github.com/MatsuriDayo}
src_dir=${NKR_SRC_DIR:-$repo_dir/..}
mkdir -p -- "$src_dir"
src_dir=$(cd -- "$src_dir" && pwd -P)

verify_source() {
    local name=$1 expected=$2 actual status
    actual=$(git -C "$src_dir/$name" rev-parse HEAD 2>/dev/null) || actual=unavailable
    status=$(git -C "$src_dir/$name" status --porcelain) || die "cannot read working tree: $name"
    if [[ $actual != "$expected" || -n $status ]]; then
        printf 'build_core: source mismatch: %s\nexpected HEAD: %s\nactual HEAD: %s\nexpected working tree: clean\nactual working tree: %s\n' \
            "$name" "$expected" "$actual" "${status:-clean}" >&2
        exit 1
    fi
}

prepare_source() {
    local name=$1 expected=$2 actual status
    if [[ ! -e $src_dir/$name ]]; then
        git clone --no-checkout "${src_base%/}/$name.git" "$src_dir/$name"
    else
        git -C "$src_dir/$name" rev-parse --git-dir >/dev/null 2>&1 || die "not a Git checkout: $src_dir/$name"
        status=$(git -C "$src_dir/$name" status --porcelain)
        # Never let checkout hide pre-existing changes, including untracked files.
        if [[ -n $status ]]; then
            verify_source "$name" "$expected"
        fi
    fi
    actual=$(git -C "$src_dir/$name" rev-parse HEAD 2>/dev/null) || actual=unavailable
    # A clean, already pinned checkout needs no network or Git metadata writes.
    if [[ $actual != "$expected" ]]; then
        if ! git -C "$src_dir/$name" fetch "${src_base%/}/$name.git" "$expected"; then
            # Fetch failure is fatal; report the checkout mismatch before exiting.
            printf 'build_core: failed to fetch %s at %s\n' "$name" "$expected" >&2
            verify_source "$name" "$expected"
            die "failed to fetch $name at $expected"
        fi
        git -C "$src_dir/$name" checkout --detach "$expected"
    fi
    verify_source "$name" "$expected"
}

prepare_source sing-box "$COMMIT_SING_BOX"
prepare_source sing-quic "$COMMIT_SING_QUIC"
prepare_source libneko "$COMMIT_LIBNEKO"

GOOS=${GOOS:-$(go env GOHOSTOS)}
GOARCH=${GOARCH:-$(go env GOHOSTARCH)}
export GOOS GOARCH
VERSION=${VERSION:-$(git -C "$repo_dir" describe --tags --always 2>/dev/null || printf dev)}
out_dir=${NKR_CORE_OUT_DIR:-$repo_dir/build-core/$GOOS-$GOARCH}
mkdir -p -- "$out_dir"
out_dir=$(cd -- "$out_dir" && pwd -P)
binary=nekobox_core
if [[ $GOOS == windows ]]; then binary+=.exe; fi

# Use an alternate module file so custom source paths never edit tracked files.
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/nekobox-core.XXXXXXXX")
trap 'rm -rf -- "$work_dir"' EXIT
cp "$repo_dir/go/cmd/nekobox_core/go.mod" "$work_dir/go.mod"
cp "$repo_dir/go/cmd/nekobox_core/go.sum" "$work_dir/go.sum"
cd -- "$repo_dir/go/cmd/nekobox_core"
go mod edit -modfile="$work_dir/go.mod" \
    "-replace=grpc_server=$repo_dir/go/grpc_server" \
    "-replace=github.com/matsuridayo/libneko=$src_dir/libneko" \
    "-replace=github.com/sagernet/sing-box=$src_dir/sing-box" \
    "-replace=github.com/sagernet/sing-quic=$src_dir/sing-quic"
go build -mod=readonly -modfile="$work_dir/go.mod" -o "$out_dir/$binary" -trimpath \
    -ldflags "-w -s -X github.com/matsuridayo/libneko/neko_common.Version_neko=$VERSION" \
    -tags 'with_clash_api,with_gvisor,with_quic,with_wireguard,with_utls,with_ech'

if command -v shasum >/dev/null 2>&1; then
    checksum=$(cd -- "$out_dir" && shasum -a 256 "$binary")
elif command -v sha256sum >/dev/null 2>&1; then
    checksum=$(cd -- "$out_dir" && sha256sum "$binary")
else
    die 'SHA-256 requires shasum or sha256sum'
fi
{
    printf 'sha256: %s\n' "$checksum"
    printf 'sing-box: %s\nsing-quic: %s\nlibneko: %s\n' \
        "$COMMIT_SING_BOX" "$COMMIT_SING_QUIC" "$COMMIT_LIBNEKO"
} > "$out_dir/build-info.txt"
cat "$out_dir/build-info.txt"
