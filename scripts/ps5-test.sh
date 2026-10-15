#!/usr/bin/env bash
# One build/deploy/run cycle of a test payload on the console, with the result
# saved on this machine and returned as the exit code. Meant for people and
# for coding agents; see docs/MAC_DEV_LOOP.md.
#
#   scripts/ps5-test.sh hello                  probes/hello/hello.elf
#   scripts/ps5-test.sh -t 600 threadprobe     probes/threadprobe/threadprobe.elf
#
# Needs Payload Manager (8084), ftpsrv (2121) and klogsrv (3232) on the
# console. The payload calls ps5test_init() from probes/ps5test.h, which puts
# its stdout in /data/PS5LM/tests/TARGET.out; the exit code or crash comes from
# the kernel log. Results go to test-results/ps5/TARGET/<time>/.
#
# Exit codes: 0 pass, 1 test failed, 2 crashed or no result line, 3 build
# failed, 4 console unreachable or deploy failed, 5 timed out (killed),
# 6 another ps5-test.sh is running, 64 usage.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=env.sh
source "$root/scripts/env.sh"

usage() {
    echo "usage: ps5-test.sh [-t SECONDS] TARGET" >&2
    exit 64
}

timeout=${PS5TEST_TIMEOUT:-120}
while getopts "t:h" opt; do
    case $opt in
        t) timeout=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || usage
target=$1
case $target in
    *[!A-Za-z0-9_-]*|'') echo "ps5-test: bad target name '$target'" >&2; exit 64 ;;
esac
case $timeout in
    ''|*[!0-9]*) echo "ps5-test: -t takes whole seconds" >&2; exit 64 ;;
esac

host=${PS5_HOST:?set PS5_HOST to the console IP}
pm="http://$host:${PS5_PM_PORT:-8084}"
ftp="ftp://$host:${PS5_FTP_PORT:-2121}"
klog_port=${PS5_KLOG_PORT:-3232}
remote_elf=/data/pldmgr/payloads/ps5test/$target.elf
remote_out=/data/PS5LM/tests/$target.out

for t in curl nc python3 make; do
    command -v "$t" >/dev/null || { echo "ps5-test: $t is required" >&2; exit 4; }
done

# A target is probes/TARGET, or probes/BASE for BASE-variant (hello-crash).
dir="$root/probes/$target"
[ -f "$dir/Makefile" ] || dir="$root/probes/${target%%-*}"
if [ ! -f "$dir/Makefile" ]; then
    echo "ps5-test: no probes/$target/Makefile" >&2
    exit 64
fi

results="$root/test-results/ps5"
run="$results/$target/$(date +%Y%m%d-%H%M%S)"
[ -e "$run" ] && run="$run-$$"
mkdir -p "$run"

# One test at a time: mkdir is atomic, and macOS has no flock(1).
lock="$results/.lock"
if ! mkdir "$lock" 2>/dev/null; then
    other=$(cat "$lock/pid" 2>/dev/null || true)
    if [ -n "$other" ] && kill -0 "$other" 2>/dev/null; then
        echo "ps5-test: another run (pid $other) is using the console" >&2
        rmdir "$run" 2>/dev/null || true
        exit 6
    fi
    rm -rf "$lock"
    mkdir "$lock"
fi
echo $$ > "$lock/pid"

klog_pid=""
payload_pid=""
finished=0
cleanup() {
    [ -n "$klog_pid" ] && kill "$klog_pid" 2>/dev/null || true
    # Never leave a test payload running on the console.
    if [ -n "$payload_pid" ] && [ "$finished" = 0 ]; then
        curl -sS -m 5 "$pm/process_kill?pid=$payload_pid" >/dev/null 2>&1 || true
        echo "ps5-test: killed payload pid $payload_pid" >&2
    fi
    rm -rf "$lock"
}
trap cleanup EXIT
trap 'echo "ps5-test: interrupted" >&2; exit 130' INT TERM

now_ms() { python3 -c 'import time; print(int(time.time() * 1000))'; }
t0=$(now_ms)

# result STATUS EXITCODE DETAIL: writes result.json, moves latest/previous.
result() {
    local status=$1 code=$2 detail=$3
    finished=1
    python3 -I - "$run" "$target" "$status" "$code" "$detail" "$t0" "$(now_ms)" \
        "$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)" \
        "$(git -C "$root" status --porcelain 2>/dev/null | grep -q . && echo true || echo false)" \
        "${exit_value:-}" "${signal:-}" "$payload_pid" <<'EOF'
import json, os, sys
run, target, status, code, detail, t0, t1, commit, dirty, ev, sig, pid = sys.argv[1:]
reported = None
out = os.path.join(run, "output.log")
if os.path.exists(out):
    for line in open(out, errors="replace"):
        if line.startswith("PS5TEST_RESULT "):
            try:
                reported = json.loads(line[len("PS5TEST_RESULT "):])
            except ValueError:
                reported = {"unparsed": line.strip()}
res = {
    "target": target,
    "status": status,
    "exit_code": int(code),
    "detail": detail,
    "duration_ms": int(t1) - int(t0),
    "started": os.path.basename(run),
    "git_commit": commit,
    "git_dirty": dirty == "true",
    "payload_pid": int(pid) if pid else None,
    "payload_exit_value": int(ev) if ev else None,
    "payload_signal": sig or None,
    "reported": reported,
}
with open(os.path.join(run, "result.json"), "w") as f:
    json.dump(res, f, indent=2)
    f.write("\n")
EOF
    local tdir="$results/$target"
    if [ -L "$tdir/latest" ]; then
        rm -f "$tdir/previous"
        mv "$tdir/latest" "$tdir/previous"
    fi
    ln -s "$(basename "$run")" "$tdir/latest"
    echo "ps5-test: $target $status ($detail), exit $code"
    echo "ps5-test: $run"
    exit "$code"
}

# 1. Build. Nothing is sent if this fails.
echo "ps5-test: building $target"
if ! make -C "$dir" "$target.elf" > "$run/build.log" 2>&1; then
    tail -20 "$run/build.log" >&2
    result build_failed 3 "make failed, see build.log"
fi
elf="$dir/$target.elf"

# 2. Console up?
if ! curl -sS -m 5 "$pm/version" > /dev/null 2>"$run/preflight.log"; then
    result unreachable 4 "Payload Manager not answering at $pm"
fi
for p in "${PS5_FTP_PORT:-2121}" "$klog_port"; do
    if ! nc -z -G 3 "$host" "$p" 2>>"$run/preflight.log"; then
        result unreachable 4 "nothing on $host:$p; start ftpsrv and klogsrv from Payload Manager"
    fi
done

# 3. Kernel log, from before the launch.
nc "$host" "$klog_port" > "$run/klog.log" 2>/dev/null &
klog_pid=$!
sleep 1
klog_from=$(wc -c < "$run/klog.log" | tr -d ' ')

# 4. Upload and start.
curl -sS -m 10 -Q "DELE $remote_out" "$ftp/" > /dev/null 2>&1 || true
if ! curl -sS -m 60 --ftp-create-dirs -T "$elf" "$ftp$remote_elf" 2>>"$run/preflight.log"; then
    result deploy_failed 4 "FTP upload failed"
fi
reply=$(curl -sS -m 10 "$pm/loadpayload:$remote_elf" 2>&1 || true)
if [ "$reply" != "OK" ]; then
    result deploy_failed 4 "Payload Manager said: $reply"
fi
echo "ps5-test: started $target, timeout ${timeout}s"

# 5. Wait for exit, crash or timeout. elfldr starts a payload in a host
# process; its EXEC line gives the pid, the same line exit/crash refer to.
klog_new() { tail -c "+$((klog_from + 1))" "$run/klog.log"; }
exit_value=""
signal=""
deadline=$(( $(date +%s) + timeout ))
while :; do
    if [ -z "$payload_pid" ]; then
        payload_pid=$(klog_new | sed -nE 's/^<([0-9]+)> EXEC \/system\/vsh\/app\/NPXS40112\/eboot\.bin.*/\1/p' | head -1 || true)
    fi
    if [ -n "$payload_pid" ]; then
        exit_value=$(klog_new | sed -nE "s/^# process pid=$payload_pid, .* exit_value=([0-9-]+)\..*/\1/p" | head -1 || true)
        signal=$(klog_new | sed -nE "s/.*Sending signal\(pid: $payload_pid, .*signo: (0x[0-9a-f]+)\).*/\1/p" | head -1 || true)
        [ -n "$exit_value" ] || [ -n "$signal" ] && break
    fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
        break
    fi
    sleep 1
done

timed_out=0
if [ -z "$exit_value" ] && [ -z "$signal" ]; then
    timed_out=1
    if [ -n "$payload_pid" ]; then
        curl -sS -m 5 "$pm/process_kill?pid=$payload_pid" >> "$run/preflight.log" 2>&1 || true
    fi
fi
finished=1
# A crash writes a coredump first; give the kernel log a moment to finish.
[ -n "$signal" ] && sleep 4
kill "$klog_pid" 2>/dev/null || true
klog_pid=""

# 6. Output.
curl -sS -m 30 "$ftp$remote_out" > "$run/output.log" 2>>"$run/preflight.log" || true
[ -s "$run/output.log" ] && cat "$run/output.log"

if [ "$timed_out" = 1 ]; then
    if [ -z "$payload_pid" ]; then
        result unreachable 4 "payload never showed up in the kernel log"
    fi
    result timeout 5 "no exit after ${timeout}s, killed pid $payload_pid"
fi
if [ -n "$signal" ]; then
    result crashed 2 "signal $signal"
fi
line=$(grep '^PS5TEST_RESULT ' "$run/output.log" | tail -1 || true)
if [ -z "$line" ]; then
    result no_result 2 "exit_value $exit_value, no PS5TEST_RESULT line"
fi
if [ "$exit_value" != 0 ]; then
    result failed 1 "exit_value $exit_value"
fi
case $line in
    *'"success":true'*) result pass 0 "exit_value 0" ;;
    *) result failed 1 "test reported success false" ;;
esac
