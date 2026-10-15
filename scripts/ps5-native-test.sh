#!/usr/bin/env bash
# One deploy/launch/watch cycle of a registered PS5LM native title, with the
# result saved on this machine and returned as the exit code. The native
# counterpart of ps5-test.sh; see docs/MAC_DEV_LOOP.md ("Native titles").
#
#   scripts/ps5-native-test.sh PPSA99041                    launch what is deployed
#   scripts/ps5-native-test.sh -d build/native-ggml-ops/PPSA99041 PPSA99041
#                                                           replace eboot.bin first
#   scripts/ps5-native-test.sh -o native-ggml-ops.out PPSA99041
#                                                           also fetch /data/PS5LM/tests/<file>
#
# Building stays separate (scripts/build-native-compute.sh and its validator).
# The launch is probes/launch, a payload started through Payload Manager that
# calls sceLncUtilLaunchApp and exits. The title reports through the kernel log
# (PS5LM_*_RESULT) and closes itself; this script never closes or kills it.
#
# Needs Payload Manager (8084), ftpsrv (2121), klogsrv (3232) and
# ShadowMountPlus (10101) on the console, and the title registered there.
# Results go to test-results/ps5/TITLE/<time>/.
#
# Exit codes: 0 pass, 1 test failed, 2 crashed or no result, 3 build failed,
# 4 console unreachable, deploy or launch failed, 5 timed out (title left
# running), 6 the console is busy (another run, or an application is open),
# 7 the title reported but did not close, 64 usage. 5 and 7 need a person.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=env.sh
source "$root/scripts/env.sh"

usage() {
    echo "usage: ps5-native-test.sh [-t SECONDS] [-d APPDIR] [-o OUTFILE] TITLE" >&2
    exit 64
}

timeout=${PS5TEST_TIMEOUT:-180}
appdir=""
outfile=""
while getopts "t:d:o:h" opt; do
    case $opt in
        t) timeout=$OPTARG ;;
        d) appdir=$OPTARG ;;
        o) outfile=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || usage
title=$1
# Only titles PS5LM registered; probes/launch refuses anything else as well.
case $title in
    PPSA99040|PPSA99041) ;;
    *) echo "ps5-native-test: $title is not a PS5LM title" >&2; exit 64 ;;
esac
case $timeout in
    ''|*[!0-9]*) echo "ps5-native-test: -t takes whole seconds" >&2; exit 64 ;;
esac
case $outfile in
    *[!A-Za-z0-9._-]*) echo "ps5-native-test: bad -o file name" >&2; exit 64 ;;
esac
if [ -n "$appdir" ] && [ ! -f "$appdir/eboot.bin" ]; then
    echo "ps5-native-test: no $appdir/eboot.bin" >&2
    exit 64
fi

host=${PS5_HOST:?set PS5_HOST to the console IP}
pm="http://$host:${PS5_PM_PORT:-8084}"
ftp="ftp://$host:${PS5_FTP_PORT:-2121}"
smp="http://$host:${PS5_SMP_PORT:-10101}"
klog_port=${PS5_KLOG_PORT:-3232}
launcher=launch-$title
remote_launcher=/data/pldmgr/payloads/ps5test/$launcher.elf
remote_launcher_out=/data/PS5LM/tests/$launcher.out
remote_eboot=/data/PS5LM/native/$title/eboot.bin

for t in curl nc python3 make shasum; do
    command -v "$t" >/dev/null || { echo "ps5-native-test: $t is required" >&2; exit 4; }
done

results="$root/test-results/ps5"
run="$results/$title/$(date +%Y%m%d-%H%M%S)"
[ -e "$run" ] && run="$run-$$"
mkdir -p "$run"

# The same lock as ps5-test.sh: one run on the console at a time.
lock="$results/.lock"
if ! mkdir "$lock" 2>/dev/null; then
    other=$(cat "$lock/pid" 2>/dev/null || true)
    if [ -n "$other" ] && kill -0 "$other" 2>/dev/null; then
        echo "ps5-native-test: another run (pid $other) is using the console" >&2
        rmdir "$run" 2>/dev/null || true
        exit 6
    fi
    rm -rf "$lock"
    mkdir "$lock"
fi
echo $$ > "$lock/pid"

klog_pid=""
cleanup() {
    [ -n "$klog_pid" ] && kill "$klog_pid" 2>/dev/null || true
    rm -rf "$lock"
}
trap cleanup EXIT
trap 'echo "ps5-native-test: interrupted; the title, if started, is still running" >&2; exit 130' INT TERM

now_ms() { python3 -c 'import time; print(int(time.time() * 1000))'; }
t0=$(now_ms)
title_pid=""
signal=""
closed=false
deployed_sha=""

# result STATUS EXITCODE DETAIL: writes result.json, moves latest/previous.
result() {
    local status=$1 code=$2 detail=$3
    [ -n "$klog_pid" ] && { kill "$klog_pid" 2>/dev/null || true; klog_pid=""; }
    python3 -I - "$run" "$title" "$status" "$code" "$detail" "$t0" "$(now_ms)" \
        "$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)" \
        "$(git -C "$root" status --porcelain 2>/dev/null | grep -q . && echo true || echo false)" \
        "$title_pid" "$signal" "$closed" "$deployed_sha" <<'EOF'
import json, os, re, sys
run, title, status, code, detail, t0, t1, commit, dirty, pid, sig, closed, sha = sys.argv[1:]

def last_json(path, pattern):
    found = None
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            m = re.search(pattern, line)
            if m:
                try:
                    found = json.loads(m.group(1))
                except ValueError:
                    found = {"unparsed": line.strip()}
    return found

klog = os.path.join(run, "klog.log")
res = {
    "target": title,
    "status": status,
    "exit_code": int(code),
    "detail": detail,
    "duration_ms": int(t1) - int(t0),
    "started": os.path.basename(run),
    "git_commit": commit,
    "git_dirty": dirty == "true",
    "deployed_eboot_sha256": sha or None,
    "launch": last_json(os.path.join(run, "launch.log"), r"^PS5TEST_RESULT (\{.*\})"),
    "title_pid": int(pid) if pid else None,
    "title_signal": sig or None,
    "closed": closed == "true",
    "reported": last_json(klog, r"PS5LM_[A-Z]+_RESULT (\{.*\})"),
    "abort": last_json(klog, r"PS5LM_[A-Z]+_ABORT (\{.*\})"),
}
for name in ("title-before.json", "title-after.json"):
    p = os.path.join(run, name)
    try:
        info = json.load(open(p))
        res[name[:-5].replace("-", "_")] = {k: info.get(k) for k in ("installed", "mounted", "status")}
    except (OSError, ValueError):
        pass
with open(os.path.join(run, "result.json"), "w") as f:
    json.dump(res, f, indent=2)
    f.write("\n")
EOF
    local tdir="$results/$title"
    if [ -L "$tdir/latest" ]; then
        rm -f "$tdir/previous"
        mv "$tdir/latest" "$tdir/previous"
    fi
    ln -s "$(basename "$run")" "$tdir/latest"
    echo "ps5-native-test: $title $status ($detail), exit $code"
    echo "ps5-native-test: $run"
    exit "$code"
}

# title_info FILE: ShadowMountPlus's record of the title (read only).
title_info() {
    curl -sS -m 10 -X POST -H 'Content-Type: application/json' \
        -d "{\"title_id\":\"$title\"}" "$smp/api/v1/games/info" > "$run/$1" 2>>"$run/preflight.log"
}
# field FILE KEY: one top-level value from a JSON file, as JSON.
field() {
    python3 -I -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1])).get(sys.argv[2])))' \
        "$run/$1" "$2" 2>/dev/null || echo null
}

# 1. Build the launcher. Nothing is sent if this fails.
echo "ps5-native-test: building $launcher"
if ! make -C "$root/probes/launch" "$launcher.elf" > "$run/build.log" 2>&1; then
    tail -20 "$run/build.log" >&2
    result build_failed 3 "make failed, see build.log"
fi

# 2. Console up, title registered and not running.
if ! curl -sS -m 5 "$pm/version" > /dev/null 2>"$run/preflight.log"; then
    result unreachable 4 "Payload Manager not answering at $pm"
fi
for p in "${PS5_FTP_PORT:-2121}" "$klog_port"; do
    if ! nc -z -G 3 "$host" "$p" 2>>"$run/preflight.log"; then
        result unreachable 4 "nothing on $host:$p; start ftpsrv and klogsrv from Payload Manager"
    fi
done
if ! title_info title-before.json; then
    result unreachable 4 "ShadowMountPlus not answering at $smp"
fi
if [ "$(field title-before.json installed)" != true ]; then
    result deploy_failed 4 "$title is not registered (see title-before.json); registering needs the user"
fi
if [ "$(field title-before.json mounted)" != false ]; then
    result busy 6 "$title is still mounted: the last run may not have closed; a person is needed"
fi

# 3. Kernel log, from before anything changes.
nc "$host" "$klog_port" > "$run/klog.log" 2>/dev/null &
klog_pid=$!
sleep 1
klog_from=$(wc -c < "$run/klog.log" | tr -d ' ')
klog_new() { tail -c "+$((klog_from + 1))" "$run/klog.log"; }
# grep -q stops at the first match, tail then dies of SIGPIPE, and pipefail
# counts that as a failure once the log is long: read everything instead.
klog_has() { klog_new | grep -E "$1" > /dev/null; }

# 4. Deploy: only eboot.bin changes, read back raw (SELF turns off ftpsrv's
# decryption for that connection) and compared.
if [ -n "$appdir" ]; then
    deployed_sha=$(shasum -a 256 "$appdir/eboot.bin" | cut -d' ' -f1)
    echo "ps5-native-test: uploading eboot.bin ($deployed_sha)"
    if ! curl -sS -m 600 -T "$appdir/eboot.bin" "$ftp$remote_eboot" 2>>"$run/preflight.log"; then
        result deploy_failed 4 "FTP upload of eboot.bin failed"
    fi
    curl -sS -m 600 -Q "SELF" "$ftp$remote_eboot" -o "$run/eboot.readback" 2>>"$run/preflight.log" || true
    back=$(shasum -a 256 "$run/eboot.readback" 2>/dev/null | cut -d' ' -f1)
    rm -f "$run/eboot.readback"
    if [ "$back" != "$deployed_sha" ]; then
        result deploy_failed 4 "eboot.bin readback does not match ($back)"
    fi
fi
if [ -n "$outfile" ]; then
    curl -sS -m 10 -Q "DELE /data/PS5LM/tests/$outfile" "$ftp/" > /dev/null 2>&1 || true
fi

# 5. Launch: the launcher refuses if any application is open, and exits once
# the console reports the title running.
curl -sS -m 10 -Q "DELE $remote_launcher_out" "$ftp/" > /dev/null 2>&1 || true
if ! curl -sS -m 60 --ftp-create-dirs -T "$root/probes/launch/$launcher.elf" "$ftp$remote_launcher" 2>>"$run/preflight.log"; then
    result deploy_failed 4 "FTP upload of the launcher failed"
fi
reply=$(curl -sS -m 10 "$pm/loadpayload:$remote_launcher" 2>&1 || true)
if [ "$reply" != "OK" ]; then
    result deploy_failed 4 "Payload Manager said: $reply"
fi
echo "ps5-native-test: launching $title, timeout ${timeout}s"

launcher_pid=""
launcher_done=""
deadline=$(( $(date +%s) + 30 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if [ -z "$launcher_pid" ]; then
        launcher_pid=$(klog_new | sed -nE 's/^<([0-9]+)> EXEC \/system\/vsh\/app\/NPXS40112\/eboot\.bin.*/\1/p' | head -1 || true)
    fi
    if [ -n "$launcher_pid" ]; then
        launcher_done=$(klog_new | grep -E -m1 "^# process pid=$launcher_pid, .* exit_value=|Sending signal\(pid: $launcher_pid," || true)
        [ -n "$launcher_done" ] && break
    fi
    sleep 1
done
curl -sS -m 30 "$ftp$remote_launcher_out" > "$run/launch.log" 2>>"$run/preflight.log" || true
[ -s "$run/launch.log" ] && cat "$run/launch.log"
launch_line=$(grep '^PS5TEST_RESULT ' "$run/launch.log" | tail -1 || true)
case $launch_line in
    *'"success":true'*) ;;
    *'"stage":"busy"'*) result busy 6 "another application is open; see launch.log" ;;
    '') result launch_failed 4 "the launcher reported nothing; see klog.log" ;;
    *) result launch_failed 4 "the launcher could not start $title; see launch.log" ;;
esac

# 6. Watch the title: its pid, its result line, a crash signal, and the
# console closing it (ProcessTerm for the title, then EndAppMount returning).
reported=""
reported_at=""
deadline=$(( $(date +%s) + timeout ))
while :; do
    if [ -z "$title_pid" ]; then
        title_pid=$(klog_new | sed -nE 's/^<([0-9]+)> EXEC \/app0\/eboot\.bin .*category=native_game.*/\1/p' | head -1 || true)
    fi
    if [ -n "$title_pid" ] && [ -z "$signal" ]; then
        signal=$(klog_new | sed -nE "s/.*Sending signal\(pid: $title_pid, .*signo: (0x[0-9a-f]+)\).*/\1/p" | head -1 || true)
    fi
    if [ -z "$reported" ] && klog_has 'PS5LM_[A-Z]+_(RESULT|ABORT) \{'; then
        reported=1
        reported_at=$(date +%s)
    fi
    if klog_has "ProcessTerm\(\) big/mini app title_id = \[$title\]" &&
        klog_has 'EndAppMount\(0x[0-9a-f]+\) returned'; then
        closed=true
        break
    fi
    # A result with no close within 60 s is its own failure.
    if [ -n "$reported_at" ] && [ "$(date +%s)" -ge $((reported_at + 60)) ]; then
        break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep 1
done
# The result and the close can land within one poll; look for the result again.
if [ -z "$reported" ] && klog_has 'PS5LM_[A-Z]+_(RESULT|ABORT) \{'; then
    reported=1
fi
sleep 2
title_info title-after.json || true

if [ -n "$outfile" ]; then
    curl -sS -m 60 "$ftp/data/PS5LM/tests/$outfile" > "$run/$outfile" 2>>"$run/preflight.log" || true
fi
klog_new | grep -E 'PS5LM_[A-Z]+_' || true

if [ -z "$title_pid" ] && [ "$closed" = false ]; then
    result launch_failed 4 "the launcher reported success but the title never showed up in the kernel log"
fi
if [ -n "$signal" ]; then
    result crashed 2 "signal $signal$( [ "$closed" = true ] || echo ', not closed: a person is needed')"
fi
if [ -z "$reported" ]; then
    if [ "$closed" = true ]; then
        result no_result 2 "closed without a PS5LM_*_RESULT line"
    fi
    result timeout 5 "nothing after ${timeout}s; $title is still running: a person is needed"
fi
if [ "$closed" = false ]; then
    result not_closed 7 "$title reported but did not close: a person is needed"
fi
if klog_has 'PS5LM_[A-Z]+_ABORT \{'; then
    result failed 1 "the title reported an abort"
fi
if klog_new | grep -E 'PS5LM_[A-Z]+_RESULT \{' | tail -1 | grep -q '"success":true'; then
    result pass 0 "reported success, closed"
fi
result failed 1 "the title reported success false"
