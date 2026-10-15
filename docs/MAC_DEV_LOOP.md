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

## Common failures

- **Exit 4, Payload Manager not answering:** the console is off, in rest mode, or was rebooted without the jailbreak. `ping` still answers in that state. Jailbreak again and start Payload Manager.
- **Exit 4, nothing on 2121 or 3232:** ftpsrv or klogsrv is not running (they stop on reboot). Start them as shown above.
- **Exit 4, "payload never showed up in the kernel log":** Payload Manager accepted the ELF but nothing started. Look at `klog.log`.
- **Exit 2 after a crash:** the console writes a coredump under `devlog/system/sce_coredumps.0/`. These pile up on repeated crashes.
- **The console stops answering entirely:** a payload locking too much memory can freeze it (see `docs/CONSOLE.md`). Only a person at the console can recover from that.

## For coding agents

Run one cycle per invocation and read `test-results/ps5/TARGET/latest/result.json`; compare with `previous/result.json`. Exit code 4 or a console that stops answering means a person is needed: stop and report, don't retry in a loop. See `AGENTS.md` for the rules.
