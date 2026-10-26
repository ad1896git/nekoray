# Shared by get_source.sh and build_core.sh. Callers enable errexit.
apply_source_patch() {
    local source_dir=$1 patch=$2
    if git -C "$source_dir" apply --check -R "$patch" 2>/dev/null; then
        return 0 # Already applied.
    fi
    git -C "$source_dir" apply --check "$patch" || return 1
    git -C "$source_dir" apply "$patch"
}

# Accept precisely HEAD plus the patch, preserving build_core's protection
# against unrelated, staged, or untracked dependency edits on repeated builds.
source_patch_matches() {
    local source_dir=$1 patch=$2 index result=1
    index=$(mktemp "${TMPDIR:-/tmp}/nekobox-source-index.XXXXXXXX") || return 1
    rm -f -- "$index"
    if GIT_INDEX_FILE="$index" git -C "$source_dir" read-tree HEAD &&
        GIT_INDEX_FILE="$index" git -C "$source_dir" apply --cached "$patch" &&
        GIT_INDEX_FILE="$index" git -C "$source_dir" diff --quiet &&
        git -C "$source_dir" diff --cached --quiet &&
        [[ -z $(git -C "$source_dir" ls-files --others --exclude-standard) ]]; then
        result=0
    fi
    rm -f -- "$index"
    return "$result"
}
