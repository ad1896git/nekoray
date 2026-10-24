#!/bin/bash
# Linux only. Production invocation:
# pkexec /bin/bash -c '<embedded script>' nekoray-vpn <core> <config> <workdir>
# Controls: --terminate, --stop
set -u
set -m
umask 077

die() {
    printf '%s\n' "$2" >&2
    exit "$1"
}

if (( EUID == 0 )); then
    RUN_DIR=/run
    PATH=/usr/sbin:/usr/bin:/sbin:/bin
    export PATH
else
    RUN_DIR=${NKR_RUN_DIR:-}
    [[ -n "$RUN_DIR" ]] || die 77 "root is required"
fi

[[ "$RUN_DIR" = /* && -d "$RUN_DIR" && ! -L "$RUN_DIR" ]] ||
    die 66 "invalid runtime directory"

owner=$(stat -c %u "$RUN_DIR") || die 74 "cannot inspect runtime directory"
mode=$(stat -c %a "$RUN_DIR") || die 74 "cannot inspect runtime directory"
[[ "$owner" = "$EUID" ]] || die 77 "runtime directory has another owner"
(( (8#$mode & 0022) == 0 )) || die 77 "runtime directory is writable by others"

for tool in flock iptables ip6tables; do
    command -v "$tool" >/dev/null 2>&1 || die 69 "$tool is required"
done

REQ_UID=${PKEXEC_UID:-$EUID}
[[ "$REQ_UID" =~ ^[0-9]+$ ]] || die 64 "invalid requester uid"

LOCKFILE="$RUN_DIR/nekoray-vpn.lock"
STATE="$RUN_DIR/nekoray-vpn.state"
[[ ! -L "$LOCKFILE" && ! -L "$STATE" ]] ||
    die 77 "runtime files must not be symlinks"

exec 9>>"$LOCKFILE" || die 74 "cannot open lock"
chmod 600 "$LOCKFILE" 9>&- || die 74 "cannot protect lock"

proc_token() {
    local data rest
    local -a fields
    [[ "$1" =~ ^[0-9]+$ ]] && (( $1 > 1 )) || return 1
    IFS= read -r data <"/proc/$1/stat" 2>/dev/null || return 1
    rest=${data##*) }
    read -r -a fields <<<"$rest"
    [[ ${#fields[@]} -ge 20 && "${fields[0]}" != Z ]] || return 1
    printf '%s:%s\n' "$1" "${fields[19]}"
}

same_process() {
    [[ "$1" =~ ^[0-9]+:[0-9]+$ ]] || return 1
    [[ "$(proc_token "${1%%:*}")" = "$1" ]]
}

read_record() {
    REC_SUP=
    REC_CORE=
    REC_UID=
    REC_CMT=
    [[ -e "$STATE" ]] || return 0
    [[ -f "$STATE" && ! -L "$STATE" ]] || return 74

    local key value
    while IFS='=' read -r key value; do
        case "$key" in
            SUP) REC_SUP=$value ;;
            CORE) REC_CORE=$value ;;
            UID) REC_UID=$value ;;
            CMT) REC_CMT=$value ;;
            *) return 74 ;;
        esac
    done <"$STATE"

    [[ "$REC_SUP" =~ ^[0-9]+:[0-9]+$ &&
       "$REC_UID" =~ ^[0-9]+$ &&
       "$REC_CMT" =~ ^nekoray-vpn:[0-9a-f-]+$ ]] || return 74
    [[ -z "$REC_CORE" || "$REC_CORE" =~ ^[0-9]+:[0-9]+$ ]] || return 74
}

allowed() {
    [[ "$REQ_UID" = 0 || "$REQ_UID" = "$REC_UID" ]]
}

r4() {
    iptables "$1" INPUT -s 172.19.0.2 -d 172.19.0.1 \
        -p tcp -j ACCEPT -m comment --comment "$2" 9>&-
}

r6() {
    ip6tables "$1" INPUT -s fdfe:dcba:9876::2 -d fdfe:dcba:9876::1 \
        -p tcp -j ACCEPT -m comment --comment "$2" 9>&-
}

delete_rules() {
    local fn=$1 comment=$2 rc
    while :; do
        "$fn" -C "$comment" 2>/dev/null
        rc=$?
        case "$rc" in
            0) "$fn" -D "$comment" || return 74 ;;
            1) return 0 ;;
            *) return 74 ;;
        esac
    done
}

stop_token() {
    local token=$1 i
    [[ -n "$token" ]] || return 0
    same_process "$token" || return 0
    kill -INT "${token%%:*}" 2>/dev/null || {
        same_process "$token" && return 75
        return 0
    }
    for ((i = 0; i < 100; ++i)); do
        same_process "$token" || return 0
        sleep 0.05 9>&-
    done
    printf '%s\n' "recorded core has not exited" >&2
    return 75
}

cleanup_dead() {
    read_record || return $?
    [[ -n "$REC_SUP" ]] || return 0
    allowed || {
        printf '%s\n' "denied: instance belongs to uid $REC_UID" >&2
        return 77
    }
    same_process "$REC_SUP" && return 75
    stop_token "$REC_CORE" || return $?

    local failed=0
    delete_rules r4 "$REC_CMT" || failed=1
    delete_rules r6 "$REC_CMT" || failed=1
    (( failed == 0 )) || return 74

    rm -f "$RUN_DIR/nekoray-vpn.ready.${REC_CMT#nekoray-vpn:}" "$STATE" \
        9>&- || return 74
}

case "${1:-}" in
    --terminate)
        [[ $# -eq 1 ]] || die 64 "unexpected control arguments"
        if flock -n 9; then
            exit 0
        fi
        read_record || die 74 "invalid instance record"
        [[ -n "$REC_SUP" ]] || die 75 "supervisor is starting; retry"
        allowed || die 77 "denied: instance belongs to uid $REC_UID"
        same_process "$REC_SUP" || die 75 "supervisor is changing; retry"
        kill -INT "${REC_SUP%%:*}" || die 75 "cannot signal supervisor"
        flock -w 5 9 || die 75 "supervisor has not exited"
        cleanup_dead
        exit $?
        ;;
    --stop)
        [[ $# -eq 1 ]] || die 64 "unexpected control arguments"
        flock -w 3 9 || die 75 "a supervisor is running; use --terminate"
        cleanup_dead
        exit $?
        ;;
esac

[[ $# -eq 3 ]] ||
    die 64 "usage: <core> <config> <workdir> | --terminate | --stop"

CORE=$1
CONFIG=$2
WORKDIR=$3

for path in "$CORE" "$CONFIG" "$WORKDIR"; do
    [[ "$path" = /* ]] || die 64 "all paths must be absolute"
done
[[ -x "$CORE" && -f "$CONFIG" && -d "$WORKDIR" ]] ||
    die 66 "core/config/workdir missing"

flock -n 9 || die 75 "another instance is running"
cleanup_dead || exit $?

SUP=$(proc_token "$$") || die 74 "cannot identify supervisor"
IFS= read -r instance_id </proc/sys/kernel/random/uuid ||
    die 74 "cannot create instance identity"
CMT="nekoray-vpn:$instance_id"
GATE="$RUN_DIR/nekoray-vpn.ready.$instance_id"
CORE_TOKEN=
CHILD=
REQUESTED=0
ADDED4=0
ADDED6=0

write_record() {
    local tmp
    tmp=$(mktemp "$STATE.XXXXXX") || return 74
    if ! printf 'SUP=%s\nCORE=%s\nUID=%s\nCMT=%s\n' \
        "$SUP" "$CORE_TOKEN" "$REQ_UID" "$CMT" >"$tmp"; then
        rm -f "$tmp" 9>&-
        return 74
    fi
    if ! mv "$tmp" "$STATE" 9>&-; then
        rm -f "$tmp" 9>&-
        return 74
    fi
}

forward() {
    REQUESTED=1
    if [[ -n "$CHILD" ]]; then
        kill -INT "$CHILD" 2>/dev/null || :
    fi
}

finish() {
    local rc=$? failed=0
    trap - EXIT INT TERM HUP
    if [[ -n "$CORE_TOKEN" ]] && same_process "$CORE_TOKEN"; then
        stop_token "$CORE_TOKEN" || failed=1
    fi
    if (( failed == 0 )); then
        if (( ADDED4 )); then
            delete_rules r4 "$CMT" || failed=1
        fi
        if (( ADDED6 )); then
            delete_rules r6 "$CMT" || failed=1
        fi
    fi
    if (( failed == 0 )); then
        rm -f "$GATE" "$STATE" 9>&- || failed=1
    fi
    if (( failed != 0 )); then
        printf '%s\n' "cleanup incomplete; retain state for --stop" >&2
        (( rc != 0 )) || rc=74
    fi
    exit "$rc"
}

trap forward INT TERM HUP
write_record || die 74 "cannot record supervisor"
trap finish EXIT

cd "$WORKDIR" || exit 66
r4 -C "$CMT" 2>/dev/null || {
    if r4 -I "$CMT"; then ADDED4=1
    else printf '%s\n' "[Warning] iptables rule not added" >&2
    fi
}
r6 -C "$CMT" 2>/dev/null || {
    if r6 -I "$CMT"; then ADDED6=1
    else printf '%s\n' "[Warning] ip6tables rule not added" >&2
    fi
}

(( REQUESTED == 0 )) || exit 130

(
    exec 9>&-
    trap - INT TERM HUP
    while [[ ! -e "$GATE" ]]; do
        same_process "$SUP" || exit 75
        sleep 0.02
    done
    exec "$CORE" run -c "$CONFIG"
) &
CHILD=$!

CORE_TOKEN=$(proc_token "$CHILD") || {
    wait "$CHILD"
    exit 74
}
if ! write_record; then
    kill -TERM "$CHILD" 2>/dev/null || :
    wait "$CHILD"
    exit 74
fi

if (( REQUESTED != 0 )); then
    kill -INT "$CHILD" 2>/dev/null || :
else
    : >"$GATE" || exit 74
fi

wait "$CHILD"
rc=$?
while same_process "$CORE_TOKEN"; do
    wait "$CHILD"
    rc=$?
done
exit "$rc"
