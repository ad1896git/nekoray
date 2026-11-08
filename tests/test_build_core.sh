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

# Exercise source preparation without fetching real dependencies or compiling Go.
# Load the production helpers, stopping before the first prepare_source call.
sed -n '/^verify_source() {/,/^prepare_source sing-box/{ /^prepare_source sing-box/!p; }' \
    libs/build_core.sh > "$tmp_dir/prepare-source.sh"
fixture="$tmp_dir/fixture"
mkdir -p "$fixture"
git -C "$fixture" init -q
echo populated > "$fixture/tracked"
git -C "$fixture" add tracked
git -C "$fixture" -c user.name='Build core test' \
    -c user.email='build-core-test@example.invalid' -c commit.gpgsign=false \
    commit -qm 'Pinned fixture'
pin=$(git -C "$fixture" rev-parse HEAD)
mkdir -p "$tmp_dir/remotes" "$tmp_dir/sources"
git clone -q --bare "$fixture" "$tmp_dir/remotes/example.git"
stdout="$tmp_dir/prepare.stdout"
stderr="$tmp_dir/prepare.stderr"
src_dir="$tmp_dir/sources"
source "$tmp_dir/prepare-source.sh"
(prepare_source example "$pin" "$tmp_dir/remotes") >"$stdout" 2>"$stderr" || fail 'fresh pinned clone failed'
[[ $(cat "$src_dir/example/tracked") == populated ]] || fail 'fresh clone not populated'
[[ -z $(git -C "$src_dir/example" status --porcelain) ]] || fail 'fresh clone not clean'
# A clean pinned checkout must work even when the remote is unavailable.
(prepare_source example "$pin" "$tmp_dir/missing") >"$stdout" 2>"$stderr" || fail 'offline pinned reuse failed'
for dirty in tracked untracked; do
    echo keep >> "$src_dir/example/$dirty"
    if (prepare_source example "$pin" "$tmp_dir/remotes") >"$stdout" 2>"$stderr"; then
        fail "accepted pre-existing $dirty change"
    fi
    assert_stderr 'build_core: source mismatch: example'
    [[ $(tail -n 1 "$src_dir/example/$dirty") == keep ]] || fail 'dirty file changed'
    # Restore only this test's tracked fixture; leave the untracked case last.
    if [[ $dirty == tracked ]]; then git -C "$src_dir/example" restore tracked; fi
done

# Check the real module-to-source pin gate offline, without fetching or building
# the large dependency graph. A stale source pin, wrong fork, or directory
# override must fail before compilation.
sed -n '/^verify_module_pin() {/,/^}/p' libs/build_core.sh > "$tmp_dir/module-pin.sh"
source "$tmp_dir/module-pin.sh"
die() { printf 'build_core: %s\n' "$*" >&2; exit 1; }
work_dir="$tmp_dir/module-pin"
mkdir -p "$work_dir"
module_pin=8bd0e6be17a44ccfcb55c7812f1b389f1b3b80c1
cat > "$work_dir/go.mod" <<EOF
module module_pin_test
go 1.19
require github.com/sagernet/sing-box v1.0.0
replace github.com/sagernet/sing-box => github.com/ad1896git/sing-box v0.0.0-20261106182936-8bd0e6be17a4
EOF
stdout="$tmp_dir/module-pin.stdout"
stderr="$tmp_dir/module-pin.stderr"
(
    cd "$work_dir"
    export GOWORK=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
    verify_module_pin github.com/sagernet/sing-box "$module_pin" github.com/ad1896git/sing-box
) >"$stdout" 2>"$stderr" || fail 'valid module/source pin rejected'
for invalid in stale-pin wrong-fork local-path; do
    (
        cd "$work_dir"
        export GOWORK=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off
        expected=$module_pin
        module=github.com/ad1896git/sing-box
        case $invalid in
            stale-pin) expected=0000000000000000000000000000000000000000 ;;
            wrong-fork) module=github.com/another-fork/sing-box ;;
            local-path) go mod edit -replace=github.com/sagernet/sing-box=./local ;;
        esac
        verify_module_pin github.com/sagernet/sing-box "$expected" "$module"
    ) >"$stdout" 2>"$stderr" && fail "accepted $invalid module/source pin"
    assert_stderr 'module/source pin mismatch: github.com/sagernet/sing-box'
done
bash "$repo_dir/tests/test_get_source.sh"
printf '%s\n' 'test_build_core: PASS'
