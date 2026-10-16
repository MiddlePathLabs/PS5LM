# Mac development loop

One command builds a test payload, runs it on the console and returns how it went:

```sh
source scripts/env.sh
export PS5_HOST=192.168.1.50          # the console
scripts/ps5-test.sh hello             # exit 0: pass
```

## Prerequisites

On the Mac: Homebrew `llvm@21`, `lld`, `cmake`, `ninja`, `socat`, and Python 3. `scripts/setup-sdk.sh` installs the PS5 payload SDK into `.deps/ps5-payload-sdk`. `scripts/env.sh` sets `PS5_PAYLOAD_SDK`, `LLVM_CONFIG` and `PS5_HOST`. `PS5_HOST` defaults to `ps5`, so set it to the console's IP.

On the console:

| Service | Port | Used for |
|---|---|---|
| Payload Manager | 8084 | upload library, start (`/loadpayload:`), list and kill processes |
| ftpsrv 0.21.1 | 2121 | upload the ELF, fetch its output |
| klogsrv 0.9 | 3232 | kernel log: exit code, crash signal |

Payload Manager comes up after the jailbreak. On this console, elfldr runs inside Payload Manager and does not listen on 9021, so `scripts/send.sh`, `scripts/run.sh` and `scripts/console-setup.sh` cannot reach it. ftpsrv and klogsrv are in Payload Manager's library (`/data/pldmgr/payloads/`). After a reboot, start them from its page or with:

```sh
curl "http://$PS5_HOST:8084/loadpayload:/data/pldmgr/payloads/klogsrv/klogsrv-ps5.elf"
curl "http://$PS5_HOST:8084/loadpayload:/data/pldmgr/payloads/ftpsrv/ftpsrv-ps5.elf"
```

## What one run does

1. `make -C probes/TARGET TARGET.elf`. A build failure stops the run before anything is sent.
2. Checks that Payload Manager, FTP and the kernel log answer.
3. Starts reading the kernel log.
4. Deletes the old `/data/PS5LM/tests/TARGET.out`, uploads the ELF to `/data/pldmgr/payloads/ps5test/` and starts it. Payload Manager only starts payloads from its library folder.
5. Waits for the payload's pid in the kernel log (`<pid> EXEC .../NPXS40112/eboot.bin`). The run ends on `pid=N ... exit_value=V`, on `Sending signal(pid: N, ... signo: S)`, or when the timeout passes; on a timeout it kills the payload through `/process_kill`.
6. Fetches the payload's output over FTP and writes `result.json`.

A payload has no stdout of its own under Payload Manager. `ps5test_init()` in `probes/ps5test.h` sends stdout and stderr to `/data/PS5LM/tests/TARGET.out`, so a payload has to call it to be testable. The name it passes must be the target name.

## Exit codes

| Code | Status | Meaning |
|---|---|---|
| 0 | `pass` | exit value 0 and `PS5TEST_RESULT` with `"success":true` |
| 1 | `failed` | the test reported `"success":false`, or exited non-zero |
| 2 | `crashed` / `no_result` | killed by a signal, or no result line |
| 3 | `build_failed` | see `build.log` |
| 4 | `unreachable` / `deploy_failed` | console or a service down; see `preflight.log` |
| 5 | `timeout` | still running after `-t` seconds (default 120, or `PS5TEST_TIMEOUT`); it was killed |
| 6 | | another `ps5-test.sh` holds the console |
| 130 | | interrupted; the payload was killed |

## Results

Every run gets `test-results/ps5/TARGET/YYYYmmdd-HHMMSS/` with `build.log`, `klog.log`, `output.log`, `preflight.log` and `result.json`. `latest` and `previous` point at the last two runs. `test-results/` is in `.gitignore`.

`result.json` holds the status, exit code, duration, git commit and dirty flag, the payload's pid, exit value or signal, and the payload's own result JSON under `reported`.

## Writing a test payload

```c
#include "../ps5test.h"

int main(void) {
    const double t0 = ps5test_now_ms();
    ps5test_init("mybench");                  /* = target name */
    /* ... work, printf as usual ... */
    ps5test_result("mybench", ok, t0, "\"tok_s\":8.7,\"gib\":1.4");
    return ok ? 0 : 1;
}
```

Put it in `probes/mybench/` with a Makefile that builds `mybench.elf` (copy `probes/hello/Makefile`). Variants of one source share a directory, named `BASE-variant` (`hello-crash` builds in `probes/hello`).

`probes/hello` has four fixtures, one for each outcome the wrapper handles: `hello` (pass), `hello-fail`, `hello-crash` and `hello-hang`. After changing `ps5-test.sh`, run all four and check the exit codes are 0, 1, 2 and 5 (`-t 5` for the hang).

## Native titles

The GPU tests are native titles, not payloads: PPSA99040 (capabilities) and PPSA99041 (compute), registered through ShadowMountPlus from `/data/PS5LM/native/` ([PORTING.md](PORTING.md)). `scripts/ps5-native-test.sh` runs one cycle of them from the Mac, as `ps5-test.sh` does for payloads:

```sh
scripts/build-native-compute.sh ggml-ops                     # build and check first, as before
scripts/ps5-native-test.sh -d build/native-ggml-ops/PPSA99041 -o native-ggml-ops.out PPSA99041
scripts/ps5-native-test.sh PPSA99041                          # launch what is already deployed
```

1. Builds `probes/launch/launch-TITLE.elf`, the launcher payload.
2. Checks Payload Manager, FTP, the kernel log and ShadowMountPlus (10101), that the title is registered (`/api/v1/games/info`, read only) and that it is not still mounted from an earlier run.
3. Starts reading the kernel log.
4. With `-d APPDIR`, replaces `/data/PS5LM/native/TITLE/eboot.bin` and reads it back raw to compare checksums. Nothing else in the title folder changes.
5. Starts the launcher through Payload Manager. It refuses if any application is open, calls `sceLncUtilLaunchApp` (the calls and fallbacks are the ones Mihawk-99's `ps5vkctl` uses) and exits once the console reports the title running.
6. Watches for the title's pid, its `PS5LM_*_RESULT` or `PS5LM_*_ABORT` line, a crash signal, and the console closing it (`ProcessTerm()` for the title, then `EndAppMount` returning). The title closes itself; the script never closes or kills it.
7. Writes `result.json` in `test-results/ps5/TITLE/<time>/`, with the launcher's report, the title's result line and ShadowMountPlus's record before and after. With `-o FILE`, fetches `/data/PS5LM/tests/FILE` too.

Exit codes are `ps5-test.sh`'s, with these differences: 4 also covers a launch that failed, 5 means the title is **still running**, 6 also covers an application already open, and 7 (`not_closed`) means the title reported but did not close within 60 s. 5 and 7 need a person at the console. The default timeout is 180 s.

Building stays a separate step. ShadowMountPlus has no launch call ([api.md](https://github.com/drakmor/ShadowMountPlus/blob/1.7-beta5fix1/docs/api.md)), which is why the launch goes through a payload.

**Verified on the console** on 2026-10-15 (`test-results/ps5/PPSA99041/20261015-190216/`), and used for the first model runs since. A model workload needs `-t 600`; see [PORTING.md](PORTING.md#dense-model-baseline-decisions-pin-and-budget). Two model builds exist: `bash scripts/build-native-compute.sh model` (Qwen3.5 0.8B, reference from `scripts/build-host-model-reference.sh`) and `… moe` (Granite 3.1 1B-A400M, reference from `PS5LM_MODEL_VARIANT=moe scripts/build-host-model-reference.sh`); deploy with `-d build/native-model/PPSA99041` or `-d build/native-moe/PPSA99041`. The deployed `eboot.bin` decides which model the title loads. The script reads the kernel log with `klog_has`, never `grep -q`: under `pipefail` an early-exiting grep made `tail` die of SIGPIPE and the result line looked missing.

An agent may run it on an already-registered title, one run at a time (`AGENTS.md`). The risk does not change: a GPU hang can stop the console answering, and only a person can recover it. What changes is that nobody may be watching, so a hang, a title that does not close or `VK_ERROR_DEVICE_LOST` ends the session until the user is back. Registering new titles, ShadowMountPlus configuration and model loads stay with the user.

Vision ([PORTING.md](PORTING.md) "Vision"): `scripts/build-host-vision-reference.sh vl4b` (CPU reference, writes `build/native-vl4b/gen/`; `PS5LM_VISION_METAL=1` adds the Metal stand-in), then `bash scripts/build-native-compute.sh vl4b` and `-d build/native-vl4b/PPSA99041 -o native-model.out -t 600`. The text model and its mmproj must both be on the console.

## LAN API (server mode)

`bash scripts/build-native-compute.sh server` (Granite 3.1 1B-A400M) or `… server-qwen` (Qwen3.5 0.8B) builds PPSA99041 with llama.cpp's server inside: OpenAI-compatible API on port 8081 for one bounded window (600 s, or until `POST /ps5lm/shutdown`), then it stops, frees, prints its result and closes itself ([PORTING.md](PORTING.md#lan-api-server-mode-lifetime-and-budget-2026-10-15-night)). `scripts/host-server-rehearsal.sh run 60` runs the same server code on the Mac (CPU, 127.0.0.1:8081) first.

The test script waits for the title while you use the API, so start it in the background, one run at a time as always:

```sh
scripts/ps5-native-test.sh -d build/native-server/PPSA99041 -o native-server.out -t 960 PPSA99041 &
scripts/ps5-api-check.py --variant moe          # waits for /health, then the correctness checks
scripts/ps5-api-bench.py --label "what changed"  # 128- and 512-token prompts, 128 tokens, 1 warm-up + 3
scripts/ps5-api-vision.py --model-run RUN --reference REF.json   # vision builds (server-vl4b): one image
scripts/ps5-api-tune.py --variant dense --plan cadence   # optional: runtime knobs, each with the token check
curl -X POST -H 'Content-Type: application/json' -d '{}' http://$PS5_HOST:8081/ps5lm/shutdown
wait                                            # then read test-results/ps5/PPSA99041/latest/result.json
```

The check writes `test-results/ps5/api-check/<time>/check.json`; the benchmark writes `bench.json` and `table.md` to `test-results/ps5/api-bench/<time>/` (median, min and max of llama-server's own prompt and generation rates, and the wall time). `GET /ps5lm/stats` returns the title's counters (submissions, dispatches, submit, fence-wait and graph times) and `POST /ps5lm/knobs` sets the runtime knobs (fusion, graph optimisation, submit cadence, mat-vec workgroup, prompt chunk; a bad name or value is a 400 and fails the run); `scripts/ps5-api-tune.py` uses both and writes `test-results/ps5/api-tune/<time>/` ([PORTING.md](PORTING.md#performance-measured-tuning-2026-10-15-night)). Compare benchmarks only between runs with the same model file and settings. `-t 960` covers the title's 840 s watchdog. Without the shutdown request the window ends by itself after 600 s. The API is unauthenticated: it exists only while the window is open, and nothing restarts it.

## Common failures

- **Exit 4, Payload Manager not answering:** the console is off, in rest mode, or was rebooted without the jailbreak. `ping` still answers in that state. Jailbreak again and start Payload Manager.
- **Exit 4, nothing on 2121 or 3232:** ftpsrv or klogsrv is not running (they stop on reboot). Start them as shown above.
- **Exit 4, "payload never showed up in the kernel log":** Payload Manager accepted the ELF but nothing started. Look at `klog.log`.
- **Exit 2 after a crash:** the console writes a coredump under `devlog/system/sce_coredumps.0/`. These pile up on repeated crashes.
- **The console stops answering entirely:** a payload locking too much memory can freeze it (see `docs/CONSOLE.md`). Only a person at the console can recover from that.

## For coding agents

Run one cycle per invocation and read `test-results/ps5/TARGET/latest/result.json`; compare with `previous/result.json`. Exit code 4 or a console that stops answering means a person is needed: stop and report, don't retry in a loop. See `AGENTS.md` for the rules.
