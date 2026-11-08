#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
tmp_dir=$(mktemp -d "$repo_dir/.test-get-source.XXXXXXXX")
trap 'rm -rf -- "$tmp_dir"' EXIT

fail() { printf 'test_get_source: FAIL: %s\n' "$*" >&2; cat "$log" >&2; exit 1; }
log="$tmp_dir/setup.log"

# Small local repositories model an upstream origin that lacks the fork pin.
# Run the complete, unmodified fetch script; stub only the unrelated pins/patch.
git init -q "$tmp_dir/upstream"
echo upstream > "$tmp_dir/upstream/tracked"
git -C "$tmp_dir/upstream" add tracked
git -C "$tmp_dir/upstream" -c user.name=Test -c user.email=test@example.invalid \
    -c commit.gpgsign=false commit -qm upstream
old=$(git -C "$tmp_dir/upstream" rev-parse HEAD)
mkdir "$tmp_dir/fork"
git clone -q "$tmp_dir/upstream" "$tmp_dir/fork/libneko.git"
echo fork > "$tmp_dir/fork/libneko.git/tracked"
git -C "$tmp_dir/fork/libneko.git" -c user.name=Test -c user.email=test@example.invalid \
    -c commit.gpgsign=false commit -qam fork
pin=$(git -C "$tmp_dir/fork/libneko.git" rev-parse HEAD)

prepare_case() {
    case_dir="$tmp_dir/$1"
    mkdir -p "$case_dir/nekoray/libs"
    cp "$repo_dir/libs/get_source.sh" "$case_dir/nekoray/libs/"
    : > "$case_dir/nekoray/libs/env_deploy.sh"
    echo 'apply_source_patch() { :; }' > "$case_dir/nekoray/libs/apply_source_patch.sh"
    cat > "$case_dir/nekoray/libs/get_source_env.sh" <<EOF
COMMIT_SING_BOX=$old
COMMIT_SING_QUIC=$old
COMMIT_QUIC_GO=$old
COMMIT_LIBNEKO=$pin
NKR_SRC_LIBNEKO='$tmp_dir/fork'
EOF
    for name in sing-box sing-quic quic-go; do
        git clone -q "$tmp_dir/upstream" "$case_dir/$name"
    done
    log="$case_dir/fetch.log"
}

run_fetch() { (cd "$case_dir/nekoray" && bash libs/get_source.sh) > "$log" 2>&1; }
assert_pinned() {
    [[ $(git -C "$case_dir/libneko" rev-parse HEAD) == "$pin" ]] || fail 'wrong HEAD'
    [[ $(cat "$case_dir/libneko/tracked") == fork ]] || fail 'wrong working tree'
    [[ -z $(git -C "$case_dir/libneko" status --porcelain) ]] || fail 'dirty checkout'
    if git -C "$case_dir/libneko" symbolic-ref -q HEAD; then fail 'pin is not detached'; fi
}

prepare_case fresh
run_fetch || fail 'fresh clone failed'
assert_pinned
printf '%s\n' 'test_get_source: fresh clone PASS'

prepare_case upstream
git clone -q "$tmp_dir/upstream" "$case_dir/libneko"
if git -C "$case_dir/libneko" cat-file -e "$pin^{commit}" 2>/dev/null; then
    fail 'upstream fixture already contains the fork pin'
fi
run_fetch || fail 'existing upstream checkout failed to upgrade'
assert_pinned
[[ $(git -C "$case_dir/libneko" remote get-url origin) == "$tmp_dir/upstream" ]] || fail 'origin changed'
printf '%s\n' 'test_get_source: existing upstream upgrade PASS'

# Reusing the pinned checkout must be idempotent, even with no fork available.
mv "$tmp_dir/fork" "$tmp_dir/fork-offline"
run_fetch || fail 'already-pinned offline reuse failed'
assert_pinned
mv "$tmp_dir/fork-offline" "$tmp_dir/fork"
printf '%s\n' 'test_get_source: already-pinned offline reuse PASS'

prepare_case unreachable
git clone -q "$tmp_dir/upstream" "$case_dir/libneko"
echo keep > "$case_dir/user-data"
mv "$tmp_dir/fork" "$tmp_dir/fork-offline"
if run_fetch; then fail 'unreachable pin silently accepted'; fi
[[ -s $log ]] || fail 'unreachable pin produced no diagnostic'
[[ $(git -C "$case_dir/libneko" rev-parse HEAD) == "$old" ]] || fail 'failed fetch changed HEAD'
[[ $(cat "$case_dir/user-data") == keep ]] || fail 'data outside checkout changed'
printf '%s\n' 'test_get_source: unreachable pin fails loudly PASS'
