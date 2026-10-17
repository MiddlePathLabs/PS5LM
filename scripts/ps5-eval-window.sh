#!/usr/bin/env bash
# One console server window, safely: wait for the previous run to be gone, launch the
# wrapper, wait until THIS run's result directory exists and 8081 answers, run the bench
# command, then wait for the wrapper and check closure. Prints the result directory.
#
#   scripts/ps5-eval-window.sh APPDIR TIMEOUT_S LOGTAG -- <bench command...>
#
# The bench command runs with PS5_HOST exported; give it --shutdown so the window closes
# early (the window itself is only a bound).
set -uo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$root/scripts/env.sh"
: "${PS5_HOST:?set PS5_HOST}"
appdir=$1; timeout_s=$2; tag=$3; shift 4
[ "${1:-}" = "--" ] && shift
results="$root/test-results/ps5/PPSA99041"
lock="$root/test-results/ps5/.lock"
port=8081

say() { echo "eval-window[$tag]: $*"; }

# 1. the previous run must be finished (lock gone or its pid dead) and 8081 closed
for i in $(seq 1 60); do
    if [ -d "$lock" ]; then
        other=$(cat "$lock/pid" 2>/dev/null || true)
        if [ -n "$other" ] && kill -0 "$other" 2>/dev/null; then
            [ $i = 1 ] && say "waiting for previous run (pid $other)"
            sleep 5; continue
        fi
    fi
    break
done
if [ -d "$lock" ] && kill -0 "$(cat "$lock/pid" 2>/dev/null)" 2>/dev/null; then
    say "previous run still holds the console"; exit 6
fi
for i in $(seq 1 24); do
    nc -z -G2 "$PS5_HOST" $port >/dev/null 2>&1 || break
    [ $i = 1 ] && say "waiting for $port to close"
    sleep 5
done
nc -z -G2 "$PS5_HOST" $port >/dev/null 2>&1 && { say "$port still open; refusing"; exit 6; }
before=$(ls "$results" 2>/dev/null | grep -c '^2026')

# 2. launch the wrapper
"$root/scripts/ps5-native-test.sh" -d "$appdir" -o native-server.out -t "$timeout_s" PPSA99041 \
    > "$root/build/logs/console-$tag.log" 2>&1 &
wrapper=$!

# 3. this run's result dir must appear and 8081 must answer before any bench request
run_dir=""
for i in $(seq 1 $((timeout_s / 5))); do
    sleep 5
    kill -0 $wrapper 2>/dev/null || break
    now=$(ls "$results" 2>/dev/null | grep -c '^2026')
    [ "$now" -gt "$before" ] && run_dir=$(ls -td "$results"/2026* | head -1)
    if [ -n "$run_dir" ] && nc -z -G2 "$PS5_HOST" $port >/dev/null 2>&1; then break; fi
done
[ -n "$run_dir" ] || { kill $wrapper 2>/dev/null; say "no new result directory"; exit 6; }
nc -z -G2 "$PS5_HOST" $port >/dev/null 2>&1 || { say "8081 never opened ($run_dir)"; kill $wrapper 2>/dev/null; exit 6; }
say "window up: $run_dir"

# 4. the bench
"$@"
bench_rc=$?

# 5. wait for the wrapper and check closure
wait $wrapper
wrapper_rc=$?
closed=$(python3 -c "import json,sys;d=json.load(open('$run_dir/result.json'));print(d.get('closed'))" 2>/dev/null)
say "bench rc=$bench_rc wrapper rc=$wrapper_rc closed=$closed dir=$run_dir"
if [ "$closed" != "True" ] || [ "$wrapper_rc" != 0 ]; then
    say "WINDOW DID NOT CLOSE CLEANLY (closed=$closed, wrapper=$wrapper_rc)"
    exit 7
fi
exit $bench_rc
