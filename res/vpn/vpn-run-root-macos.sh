#!/bin/bash
# macOS only. Embedded in the GUI and never read from disk at run time.
# Production invocation (root, through one osascript administrator prompt):
#   /bin/bash -p -c '<this script>' nekoray-vpn start <uid> <gui-pid> \
#       <core> <core-sha256> <config> <config-sha256> <ifname> <nonce> \
#       [<asset-source> <asset-sha256>]...
#   /bin/bash -p -c '<this script>' nekoray-vpn cleanup <uid>
# Stop: the GUI deletes <config>. Crash cleanup: the GUI process disappears.
# Exit codes: 0 stopped, 64 usage, 66 missing input, 69 tool missing,
# 70 core exited by itself, 74 I/O or incomplete cleanup, 75 busy,
# 76 hash mismatch, 77 unsafe path/ownership or permission denied.
set -u
set -m
umask 077
LC_ALL=C
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export LC_ALL PATH
unset BASH_ENV ENV CDPATH GLOBIGNORE
trap '' PIPE HUP

die() {
    printf 'nekoray-vpn: %s\n' "$2" >&2
    exit "$1"
}

for tool in lockf perl ps ifconfig route mv rm stat chmod tail sleep mkdir; do
    command -v "$tool" >/dev/null 2>&1 || die 69 "$tool is required"
done

[[ -n "${BASH_EXECUTION_STRING:-}" ]] || die 64 "must be run with bash -c"

RE_NUM='^[0-9]+$'
RE_POLL='^[0-9]+(\.[0-9]+)?$'
RE_SHA='^[0-9a-f]{64}$'
RE_IF='^utun[0-9]{1,3}$'
RE_NONCE='^[0-9a-f-]{36}$'
RE_TOKEN='^[0-9]+@[0-9]+@[A-Za-z0-9:-]+$'

STOP_TIMEOUT=12
POLL=0.2
if (( EUID == 0 )); then
    RUN_DIR=/var/run/nekoray-vpn
    if [[ ! -e "$RUN_DIR" && ! -L "$RUN_DIR" ]]; then
        mkdir -m 0700 "$RUN_DIR" 2>/dev/null || [[ -d "$RUN_DIR" ]] ||
            die 74 "cannot create runtime directory"
    fi
else
    # Test mode only: an unprivileged private directory replaces /var/run.
    RUN_DIR=${NKR_RUN_DIR:-}
    [[ -n "$RUN_DIR" ]] || die 77 "root is required"
    STOP_TIMEOUT=${NKR_STOP_TIMEOUT:-12}
    POLL=${NKR_POLL:-0.2}
fi
[[ "$STOP_TIMEOUT" =~ $RE_NUM && "$POLL" =~ $RE_POLL ]] ||
    die 64 "invalid timing parameters"

[[ "$RUN_DIR" = /* && -d "$RUN_DIR" && ! -L "$RUN_DIR" ]] ||
    die 77 "invalid runtime directory"
dir_info=$(stat -f '%u %Lp' "$RUN_DIR") ||
    die 74 "cannot inspect runtime directory"
set -- $dir_info "$@"
[[ "$1" = "$EUID" ]] || die 77 "runtime directory has another owner"
[[ "$2" = 700 ]] || die 77 "runtime directory must have mode 0700"
shift 2

LOCKFILE="$RUN_DIR/lock"
STATE="$RUN_DIR/state"
CORE_COPY="$RUN_DIR/nekobox_core"
CONFIG_COPY="$RUN_DIR/config.json"
LOG="$RUN_DIR/core.log"
# The parent (/var/run) and the directory itself cannot be changed by the
# requester. Refuse pre-existing links, hard links, foreign owners or writable
# files before opening them. Never chmod/chown an untrusted existing inode.
safe_file() {
    [[ ! -e "$1" && ! -L "$1" ]] && return 0
    [[ -f "$1" && ! -L "$1" ]] || die 77 "unsafe runtime file"
    local info
    info=$(stat -f '%u %Lp %l' "$1") || die 74 "cannot inspect runtime file"
    set -- $info
    [[ "$1" = "$EUID" && "$3" = 1 ]] || die 77 "unsafe runtime file owner or links"
    (( (8#$2 & 0022) == 0 )) || die 77 "runtime file is writable by others"
}
safe_file "$LOCKFILE"

validate_runtime_files() {
    local path name
    for path in "$RUN_DIR"/* "$RUN_DIR"/.[!.]* "$RUN_DIR"/..?*; do
        [[ -e "$path" || -L "$path" ]] || continue
        name=${path##*/}
        case "$name" in
            lock|state|state.tmp|nekobox_core|nekobox_core.tmp|config.json|config.json.tmp|core.log) ;;
            asset-*) [[ "$name" =~ ^asset-[0-9]+(\.tmp)?$ ]] || die 77 "unexpected runtime file" ;;
            gate-*) [[ "${name#gate-}" =~ $RE_NONCE ]] || die 77 "unexpected runtime file" ;;
            *) die 77 "unexpected runtime file" ;;
        esac
        safe_file "$path"
    done
}

# Identity of a live, non-zombie process: PID@UID@START.
token_of() {
    local out state uid
    [[ "$1" =~ $RE_NUM ]] && (( $1 > 1 )) || return 1
    out=$(ps -o uid=,state=,lstart= -p "$1" 2>/dev/null) || return 1
    set -- "$1" $out
    [[ $# -ge 8 ]] || return 1
    uid=$2
    state=$3
    [[ "$uid" =~ $RE_NUM && "$state" != Z* ]] || return 1
    local pid=$1
    shift 3
    local IFS=-
    printf '%s@%s@%s\n' "$pid" "$uid" "$*"
}

same_process() {
    [[ "$1" =~ $RE_TOKEN ]] || return 1
    [[ "$(token_of "${1%%@*}")" = "$1" ]]
}

token_uid() {
    local rest=${1#*@}
    printf '%s\n' "${rest%%@*}"
}

log() {
    [[ -n "${LOG_READY:-}" ]] || return 0
    printf '[nekoray-vpn] %s\n' "$*" >>"$LOG" 2>/dev/null
}

wait_gone() {
    local deadline=$((SECONDS + $2))
    while same_process "$1"; do
        (( SECONDS < deadline )) || return 1
        sleep "$POLL"
    done
    return 0
}

# Signals only the process whose identity still equals the recorded token.
stop_token() {
    local token=$1 pid
    [[ -n "$token" ]] || return 0
    same_process "$token" || return 0
    pid=${token%%@*}
    kill -INT "$pid" 2>/dev/null
    wait_gone "$token" "$STOP_TIMEOUT" && return 0
    log "core did not exit after SIGINT; sending SIGKILL"
    same_process "$token" || return 0
    kill -KILL "$pid" 2>/dev/null
    wait_gone "$token" 3
}

remove_copies() {
    rm -f "$CORE_COPY" "$CORE_COPY.tmp" "$CONFIG_COPY" "$CONFIG_COPY.tmp" \
        "$STATE.tmp" "$RUN_DIR"/asset-* "$RUN_DIR"/gate-*
}

read_state() {
    REC_SUP=
    REC_CORE=
    REC_UID=
    REC_GUI=
    REC_IF=
    REC_NONCE=
    [[ -e "$STATE" ]] || return 1
    [[ -f "$STATE" && ! -L "$STATE" ]] || return 2

    local key value seen='|'
    while IFS='=' read -r key value; do
        [[ "$seen" != *"|$key|"* ]] || return 2
        seen="$seen$key|"
        case "$key" in
            SUP) REC_SUP=$value ;;
            CORE) REC_CORE=$value ;;
            UID) REC_UID=$value ;;
            GUI) REC_GUI=$value ;;
            IF) REC_IF=$value ;;
            NONCE) REC_NONCE=$value ;;
            *) return 2 ;;
        esac
    done <"$STATE"

    [[ "$seen" = *'|CORE|'* ]] || return 2
    [[ "$REC_SUP" =~ $RE_TOKEN && "$REC_UID" =~ $RE_NUM &&
       "$REC_GUI" =~ $RE_TOKEN && "$REC_IF" =~ $RE_IF &&
       "$REC_NONCE" =~ $RE_NONCE ]] || return 2
    [[ -z "$REC_CORE" || "$REC_CORE" =~ $RE_TOKEN ]] || return 2
    return 0
}

# Runs inside the lock: no supervisor of a previous instance is alive.
cleanup_dead() {
    local rc=0
    read_state || rc=$?
    case "$rc" in
        0) ;;
        1) remove_copies || return 74; return 0 ;;
        *) printf 'nekoray-vpn: %s\n' "invalid instance record" >&2
           return 74 ;;
    esac
    if [[ "$REQ_UID" != 0 && "$REQ_UID" != "$REC_UID" ]]; then
        printf 'nekoray-vpn: denied: Tun instance belongs to uid %s\n' \
            "$REC_UID" >&2
        return 77
    fi
    if same_process "$REC_SUP"; then
        printf 'nekoray-vpn: %s\n' "recorded supervisor is still running" >&2
        return 75
    fi
    stop_token "$REC_CORE" || {
        printf 'nekoray-vpn: %s\n' "recorded core did not exit" >&2
        return 74
    }
    remove_copies || return 74
    rm -f "$STATE" || return 74
    return 0
}

check_uid() {
    [[ "$1" =~ $RE_NUM ]] || die 64 "invalid uid"
}

parse_start() {
    [[ $# -ge 8 && $(( ($# - 8) % 2 )) -eq 0 && $# -le 264 ]] || die 64 "usage: start <uid> <gui-pid> <core> <core-sha256> <config> <config-sha256> <ifname> <nonce>"
    REQ_UID=$1
    GUI_PID=$2
    CORE=$3
    CORE_SHA=$4
    CONFIG=$5
    CONFIG_SHA=$6
    IFNAME=$7
    NONCE=$8
    check_uid "$REQ_UID"
    [[ "$GUI_PID" =~ $RE_NUM ]] || die 64 "invalid NekoBox pid"
    [[ "$CORE" = /* && "$CONFIG" = /* ]] || die 64 "paths must be absolute"
    [[ "$CORE_SHA" =~ $RE_SHA && "$CONFIG_SHA" =~ $RE_SHA ]] ||
        die 64 "invalid sha256"
    [[ "$IFNAME" =~ $RE_IF ]] || die 64 "invalid interface name"
    [[ "$NONCE" =~ $RE_NONCE ]] || die 64 "invalid nonce"
    shift 8
    ASSETS=("$@")
    while [[ $# -gt 0 ]]; do
        [[ "$1" = /* && "$2" =~ $RE_SHA ]] || die 64 "invalid asset arguments"
        shift 2
    done
}

check_inputs() {
    [[ -f "$CORE" && -x "$CORE" ]] || die 66 "core is missing"
    [[ -e "$CONFIG" || -L "$CONFIG" ]] ||
        die 66 "configuration is missing (launch cancelled)"
    [[ -f "$CONFIG" && ! -L "$CONFIG" ]] ||
        die 77 "configuration must be a regular file"
    local info
    info=$(stat -f '%u %Lp %z' "$CONFIG") ||
        die 66 "configuration is missing (launch cancelled)"
    set -- $info
    [[ "$1" = "$REQ_UID" ]] || die 77 "configuration has another owner"
    (( (8#$2 & 077) == 0 )) || die 77 "configuration is readable by others"
    (( $3 <= 1048576 )) || die 77 "configuration is too large"
}

# The GUI allocates an exclusive UUID-named file for every run. Also observe
# its inode, so recreating a pathname cannot revive an already stopped run.
launch_alive() {
    [[ -f "$CONFIG" && ! -L "$CONFIG" ]] &&
        [[ "$(stat -f '%d:%i:%c' "$CONFIG" 2>/dev/null)" = "$LAUNCH_ID" ]]
}

copy_verified() {
    local source=$1 target=$2 sum=$3 mode=$4 tmp="$2.tmp" rc limit=536870912
    [[ "$source" != "$CONFIG" ]] || limit=1048576
    rm -f "$tmp" || return 74
    # macOS ships Perl (also used by shasum). No requester environment/modules.
    # Open once without following links or waiting for a FIFO writer, then
    # validate the descriptor, not a pathname checked before open. New files
    # contain bytes only, so no source xattrs/quarantine are carried across.
    /usr/bin/env -i PATH="$PATH" LC_ALL=C /usr/bin/perl -e '
        use strict;
        use Fcntl qw(:DEFAULT :mode SEEK_SET);
        use Digest::SHA ();
        my ($source, $target, $expected, $limit, $uid, $config) = @ARGV;
        sub fail { print STDERR "nekoray-vpn: $_[1]\n"; exit $_[0]; }
        sysopen(my $in, $source, O_RDONLY | O_NONBLOCK | O_NOFOLLOW)
            or fail(77, "cannot safely open staging input");
        my @st = stat($in);
        @st && S_ISREG($st[2]) && $st[7] <= $limit
            or fail(77, "staging input must be a bounded regular file");
        if ($source eq $config) {
            $st[4] == $uid && ($st[2] & 077) == 0
                or fail(77, "unsafe configuration owner or permissions");
        }
        sysopen(my $out, $target, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600)
            or fail(74, "cannot create staging file");
        # Copy only the size of the opened inode, even if it keeps growing.
        my $left = $st[7];
        while ($left) {
            my $n = sysread($in, my $buf, $left > 65536 ? 65536 : $left);
            defined($n) && $n > 0 or fail(74, "short staging read");
            my $offset = 0;
            while ($offset < $n) {
                my $written = syswrite($out, $buf, $n - $offset, $offset);
                defined($written) && $written > 0 or fail(74, "staging write failed");
                $offset += $written;
            }
            $left -= $n;
        }
        my $extra = sysread($in, my $buf, 1);
        defined($extra) && $extra == 0 or fail(77, "staging input grew or read failed");
        close($in) or fail(74, "cannot close staging input");
        my $position = sysseek($out, 0, SEEK_SET);
        defined($position) && $position == 0 or fail(74, "cannot rewind staging file");
        Digest::SHA->new(256)->addfile($out)->hexdigest eq $expected
            or fail(76, "staging input changed after authorization (hash mismatch)");
        close($out) or fail(74, "cannot close staging file");
    ' -- "$source" "$tmp" "$sum" "$limit" "$REQ_UID" "$CONFIG" &
    COPY_CHILD=$!
    # One deadline covers ALL inputs and copied-byte hashing. The helper has no
    # children; cancel and reap it before releasing the machine-wide lock.
    while kill -0 "$COPY_CHILD" 2>/dev/null; do
        if (( SECONDS >= COPY_DEADLINE )); then
            printf 'nekoray-vpn: staging deadline exceeded\n' >&2
            return 74
        fi
        if (( STOP_SIGNAL )) || ! launch_alive || ! same_process "$GUI_TOKEN"; then
            exit 0
        fi
        sleep 0.05
    done
    wait "$COPY_CHILD"
    rc=$?
    COPY_CHILD=
    (( rc == 0 )) || return "$rc"
    chmod "$mode" "$tmp" || { rm -f "$tmp"; return 74; }
    mv -f "$tmp" "$target" || { rm -f "$tmp"; return 74; }
}

write_state() {
    local tmp="$STATE.tmp"
    rm -f "$tmp" || return 74
    printf 'SUP=%s\nCORE=%s\nUID=%s\nGUI=%s\nIF=%s\nNONCE=%s\n' \
        "$SUP_TOKEN" "$CORE_TOKEN" "$REQ_UID" "$GUI_TOKEN" "$IFNAME" \
        "$NONCE" >"$tmp" || { rm -f "$tmp"; return 74; }
    chmod 0644 "$tmp" || { rm -f "$tmp"; return 74; }
    mv -f "$tmp" "$STATE" || { rm -f "$tmp"; return 74; }
}

default_route() {
    route -n get default 2>&1 | tr '\n' ' '
}

finish() {
    local rc=$? i
    trap - EXIT
    trap '' INT TERM
    if [[ -n "$COPY_CHILD" ]]; then
        # During staging this is our only background job. Address the shell
        # job, so an already completed helper cannot lead to a reused PID kill.
        kill -KILL %+ 2>/dev/null
        wait "$COPY_CHILD" 2>/dev/null
        COPY_CHILD=
    fi
    if [[ -n "$CORE_TOKEN" ]] && same_process "$CORE_TOKEN"; then
        stop_token "$CORE_TOKEN" || {
            printf 'nekoray-vpn: %s\n' \
                "core did not exit; state kept for cleanup" >&2
            exit 74
        }
    fi
    if [[ -n "$CHILD" ]]; then wait "$CHILD" 2>/dev/null; fi
    if [[ -n "$LOG_READY" ]]; then
        for ((i = 0; i < 15; ++i)); do
            ifconfig "$IFNAME" >/dev/null 2>&1 || break
            sleep 0.2
        done
        if ifconfig "$IFNAME" >/dev/null 2>&1; then
            log "warning: $IFNAME still exists after stop"
            printf 'nekoray-vpn: warning: %s still exists after stop\n' \
                "$IFNAME" >&2
        fi
        log "default route after: $(default_route)"
    fi
    remove_copies || { printf 'nekoray-vpn: cleanup incomplete\n' >&2; exit 74; }
    rm -f "$STATE" || rc=74
    exit "$rc"
}

start_locked() {
    parse_start "$@"
    validate_runtime_files
    check_inputs
    LAUNCH_ID=$(stat -f '%d:%i:%c' "$CONFIG") || die 66 "configuration disappeared"
    cleanup_dead || exit $?

    if ifconfig "$IFNAME" >/dev/null 2>&1; then
        die 75 "$IFNAME already exists; another Tun may be running (restart macOS if it persists)"
    fi

    GUI_TOKEN=$(token_of "$GUI_PID") || die 75 "NekoBox process is gone"
    [[ "$(token_uid "$GUI_TOKEN")" = "$REQ_UID" ]] ||
        die 77 "NekoBox process belongs to another user"
    SUP_TOKEN=$(token_of "$$") || die 74 "cannot identify supervisor"

    CORE_TOKEN=
    LOG_READY=
    CHILD=
    COPY_CHILD=
    COPY_DEADLINE=$((SECONDS + 30))
    STOP_SIGNAL=0
    trap 'STOP_SIGNAL=1' INT TERM
    trap finish EXIT

    copy_verified "$CORE" "$CORE_COPY" "$CORE_SHA" 0700 || exit $?
    copy_verified "$CONFIG" "$CONFIG_COPY" "$CONFIG_SHA" 0600 || exit $?
    local index=0
    while (( index < ${#ASSETS[@]} )); do
        [[ -f "${ASSETS[index]}" ]] || die 66 "asset is missing"
        copy_verified "${ASSETS[index]}" "$RUN_DIR/asset-$((index / 2))" \
            "${ASSETS[index + 1]}" 0600 || exit $?
        # sing-box would replace an empty geo database with an automatic
        # download. Never allow that unverified fallback in the root core.
        [[ -s "$RUN_DIR/asset-$((index / 2))" ]] || die 77 "asset is empty"
        index=$((index + 2))
    done
    cd "$RUN_DIR" || exit 74

    rm -f "$LOG" || exit 74
    : >"$LOG" || exit 74
    chmod 0600 "$LOG" || exit 74
    LOG_READY=1
    write_state || exit 74
    log "default route before: $(default_route)"

    (( STOP_SIGNAL )) && exit 0
    launch_alive || exit 0

    # Publish the child identity before allowing exec. If the supervisor dies
    # before publication, the waiting child exits without starting the core.
    local gate="$RUN_DIR/gate-$NONCE"
    (
        trap - INT TERM
        while [[ ! -f "$gate" ]]; do
            same_process "$SUP_TOKEN" || exit 0
            sleep "$POLL"
        done
        same_process "$SUP_TOKEN" && launch_alive || exit 0
        exec /usr/bin/env -i PATH="$PATH" LC_ALL=C HOME="$RUN_DIR" \
            "$CORE_COPY" --disable-color run -c "$CONFIG_COPY" -D "$RUN_DIR"
    ) </dev/null >>"$LOG" 2>&1 &
    CHILD=$!
    CORE_TOKEN=$(token_of "$CHILD") || CORE_TOKEN=
    if [[ -z "$CORE_TOKEN" ]] && kill -0 "$CHILD" 2>/dev/null; then
        kill -KILL "$CHILD" 2>/dev/null
        wait "$CHILD" 2>/dev/null
        die 74 "cannot identify core"
    fi
    write_state || exit 74
    : >"$gate" || exit 74
    log "core started: $CORE_TOKEN"

    local reason= n=0
    while :; do
        if ! kill -0 "$CHILD" 2>/dev/null; then reason=core-exited; break; fi
        if (( ++n % 10 == 0 )) && ! same_process "$CORE_TOKEN"; then
            reason=core-exited
            break
        fi
        if (( STOP_SIGNAL )); then reason=signal; break; fi
        if ! launch_alive; then reason=requested; break; fi
        if ! kill -0 "$GUI_PID" 2>/dev/null; then reason=gui-exited; break; fi
        if (( n % 10 == 0 )) && ! same_process "$GUI_TOKEN"; then
            reason=gui-exited
            break
        fi
        sleep "$POLL"
    done

    local core_rc
    if [[ "$reason" = core-exited ]]; then
        wait "$CHILD"
        core_rc=$?
        CORE_TOKEN=
        log "core exited by itself with status $core_rc"
        {
            printf 'nekoray-vpn: core exited with status %s\n' "$core_rc"
            tail -n 20 "$LOG" 2>/dev/null
        } >&2
        exit 70
    fi

    log "stopping core: $reason"
    if ! stop_token "$CORE_TOKEN"; then
        printf 'nekoray-vpn: %s\n' \
            "core did not exit; state kept for cleanup" >&2
        trap - EXIT
        exit 74
    fi
    wait "$CHILD" 2>/dev/null
    CORE_TOKEN=
    log "core stopped: $reason"
    exit 0
}

MODE=${1:-}
[[ $# -gt 0 ]] && shift

case "$MODE" in
    start)
        parse_start "$@"
        check_inputs
        exec lockf -k -s -t 0 "$LOCKFILE" /bin/bash -p -c \
            "$BASH_EXECUTION_STRING" nekoray-vpn start-locked "$@"
        ;;
    start-locked)
        start_locked "$@"
        ;;
    cleanup)
        [[ $# -eq 1 ]] || die 64 "usage: cleanup <uid>"
        check_uid "$1"
        exec lockf -k -s -t 0 "$LOCKFILE" /bin/bash -p -c \
            "$BASH_EXECUTION_STRING" nekoray-vpn cleanup-locked "$@"
        ;;
    cleanup-locked)
        [[ $# -eq 1 ]] || die 64 "usage: cleanup <uid>"
        check_uid "$1"
        REQ_UID=$1
        validate_runtime_files
        cleanup_dead
        exit $?
        ;;
    *)
        die 64 "usage: start ... | cleanup <uid>"
        ;;
esac
