#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd -- "$repo_dir"
source "$repo_dir/libs/get_source_env.sh"

# Keep fixtures in this scratchpad checkout, never in its parent directory.
tmp_dir=$(mktemp -d "$repo_dir/.test-build-core.XXXXXXXX")
trap 'rm -rf -- "$tmp_dir"' EXIT

fail() {
    printf 'test_build_core: FAIL: %s\n' "$*" >&2
    printf '%s\n' 'captured stdout:' >&2
    cat "$stdout" >&2
    printf '%s\n' 'captured stderr:' >&2
    cat "$stderr" >&2
    exit 1
}

run_build() {
    local case_dir=$1
    stdout="$case_dir/stdout"
    stderr="$case_dir/stderr"
    status=0
    # Do not let Git discover the enclosing repository for the plain fixture.
    GIT_CEILING_DIRECTORIES="$case_dir" \
        NKR_SRC_DIR="$case_dir" NKR_SRC_BASE="file://$case_dir/no-such-base" \
        bash libs/build_core.sh >"$stdout" 2>"$stderr" || status=$?
    [[ $status == 1 ]] || fail "expected exit status 1, got $status"
}

assert_stderr() {
    grep -Fq -- "$1" "$stderr" || fail "stderr missing: $1"
}

wrong_commit_dir="$tmp_dir/wrong-commit"
mkdir -p -- "$wrong_commit_dir/sing-box"
git -C "$wrong_commit_dir/sing-box" init -q
git -C "$wrong_commit_dir/sing-box" -c user.name='Build core test' \
    -c user.email='build-core-test@example.invalid' -c commit.gpgsign=false \
    commit -q --allow-empty -m 'Negative test fixture'
actual_commit=$(git -C "$wrong_commit_dir/sing-box" rev-parse HEAD)
stdout="$wrong_commit_dir/stdout"
stderr="$wrong_commit_dir/stderr"
: >"$stdout"
: >"$stderr"
[[ $actual_commit != "$COMMIT_SING_BOX" ]] || fail 'fixture matches pinned commit'
run_build "$wrong_commit_dir"
assert_stderr 'build_core: source mismatch: sing-box'
assert_stderr "expected HEAD: $COMMIT_SING_BOX"
assert_stderr "actual HEAD: $actual_commit"

not_checkout_dir="$tmp_dir/not-checkout"
mkdir -p -- "$not_checkout_dir/sing-box"
run_build "$not_checkout_dir"
assert_stderr "not a Git checkout: $not_checkout_dir/sing-box"

printf '%s\n' 'test_build_core: PASS'
