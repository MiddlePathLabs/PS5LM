# Porting notes

What it took to build llama.cpp for the PS5, and why. Upstream is pinned at `b11327` (2026-10-01).

## The target

The SDK compiles with clang's own PS5 target, `x86_64-sie-ps5`, which defines `__PROSPERO__`, `__SCE__` and `__FreeBSD__ 9`. Its CMake toolchain (`prospero-cmake`) presents the console as FreeBSD on x86_64. Programs link dynamically against stubs of Sony's libraries (`libSceLibcInternal`, `libkernel_web`, `libSceNet`, ...) and run as payloads under elfldr.

ggml and libllama build for it as they are. Two source patches so far, for the console's CPUs rather than for building (below).

## Build fixes (all in `scripts/build-llama.sh`)

1. **`posix_spawn_file_actions_addchdir_np` is missing.** The SDK's libc predates it, and `common/subproc.cpp` (through `vendor/sheredom/subprocess.h`) calls it. The header lets the caller turn it off: `-DSUBPROCESS_HAVE_CWD=0`. Nothing on the console spawns processes.
2. **There is no `libm`.** ggml links `m` unless `MATH_LIBRARY` is set; the math functions live in Sony's libc, so `-DMATH_LIBRARY=SceLibcInternal`.
3. **`accept4` is declared but not exported.** The SDK's `sys/socket.h` declares it (FreeBSD 10), libkernel exports only `accept`, and cpp-httplib's server loop calls `accept4(..., SOCK_CLOEXEC)`. `ps5/compat/compat.c` implements it on `accept` and `fcntl`, and goes last on every link line through `CMAKE_C_STANDARD_LIBRARIES` and `CMAKE_CXX_STANDARD_LIBRARIES`.

Build options: AVX, AVX2, FMA, F16C and BMI2 on, AVX-512 off (Zen 2), no OpenMP (ggml's own thread pool), static libraries, no OpenSSL. mbedTLS is built with `MBEDTLS_THREADING_C` and `MBEDTLS_THREADING_PTHREAD` (`scripts/build-mbedtls.sh`): ps5lm downloads over 16 connections at once, TLS 1.3 handshakes share PSA's global key store, and that store is only thread-safe with both (mbedTLS `docs/architecture/psa-thread-safety`; pacbrew's PS5 package sets them too).

## Leaving a tool (`ps5/compat/exit.cpp`)

After `main` returns, the SDK's start code (`crt/crt.c`) unloads the Sony modules the payload loaded and calls Sony's `exit()`, which runs every C++ static destructor registered with `__cxa_atexit` while other threads may still run. The SDK moved to this in 2024 (commit f19dd3e); before, payloads ended with the `_exit` syscall. `[SceLibc] A heap error is detected` comes at exit, after the tool's work is done, which points at this phase. `ps5/CMakeLists.txt` links `compat/exit.cpp` into the tools with `--wrap=main`: the start code's call lands in `__wrap_main`, which runs the tool's `main`, drains llama.cpp's log thread and stdio, and leaves through `_Exit`. ps5lm never returns from `main` and leaves the same way when asked to quit.

ProsperoAI's libc replacements (`pthread_once`, `strtof`, `fseek`, an mmap allocator for large blocks) are not copied: ProsperoAI is a native app on a clean-room `libc.prx` whose stubs they patch over, while a payload binds to Sony's own libc, and a second allocator would bring exactly the mixed frees the heap check catches.

## Source patches (`patches/`, applied by `scripts/build-llama.sh`)

1. **`0001-ps5-safe-cpu-defaults.patch`.** A payload may only use 5 of the 16 logical CPUs, and system services share them. llama.cpp's defaults (one thread per physical core, busy-waiting at `--poll 50`) took all five and the console shut down. On `__PROSPERO__` the default thread count becomes the CPUs in the affinity mask (`scePthreadGetaffinity`) minus two, and polling defaults to 0. `-t` and `--poll` still override.
2. **`0002-ps5-thread-affinity-and-priority.patch`.** The SDK's compiler defines `__FreeBSD__` but not `__linux__`, so ggml's thread pinning and priority fell into its "unsupported platforms" branch and `-C`, `--cpu-strict` and `--prio` did nothing. On `__PROSPERO__` they go through `scePthreadSetaffinity` and `scePthreadSetprio`. Priority is only lowered (`--prio -1` gives 720; the default is 700, and 256 runs first), the way PS5SX2 and PS5CEMU-HAR only raise threads that mostly sleep. The calling thread is never pinned: threads it starts later inherit its mask, which left a PS5SX2 helper thread stuck on one CPU (PS5SX2 commit f9abb4a). `set_process_priority` refuses to raise anything, since what `setpriority` does to a payload is unknown.

## The build

`ps5/CMakeLists.txt` is the top-level project: it adds llama.cpp as a subdirectory and the `ps5lm` payload next to it. `ps5lm` links `llama-server-impl` and calls `llama_server(argc, argv)` itself, and carries its two pages and the Mozilla CA list inside the binary (`ps5lm_embed`). cpp-httplib is compiled with `CPPHTTPLIB_MBEDTLS_SUPPORT` everywhere, so its classes match in every file that includes it, and mbedTLS (`scripts/build-mbedtls.sh`) is linked last with the compat library.

## Output

`build/llama-ps5/bin/`: `llama-cli` (20 MB), `llama-server` (20 MB), `llama-bench` (13 MB), position independent FreeBSD ELFs that import from `libSceLibcInternal.sprx`, `libkernel_web.sprx` and `libSceNet.sprx`. The only weak import is `__cxa_thread_atexit_impl`, which libc++abi checks for before use.

`llvm-readelf` warns that `PT_DYNAMIC` is larger than `.dynamic`: the SDK's linker script puts `.dynsym`, `.dynstr` and `.rela.dyn` in the same segment, and the SDK's own samples have the same layout.

## Native-title preflight (capabilities and tiny GPU compute passed)

```sh
bash scripts/build-native-capabilities.sh
```

This builds `build/native-capabilities/PPSA99040/` and `PPSA99040.zip` with
`eboot.bin`, metadata, presentation assets and a clean-room `sce_module/libc.prx`.
The title ID is a local development choice; verify that it is unused before
registration. The boilerplate's artwork remains attributed to its author; it is
not PS5LM artwork.

The script pins [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
at `b8dc4a3fe2e05e826b037bd68824751c7468a6d7`, compiles its C++20 converter
against host zlib, and uses our SDK v0.43 setup by default. `PS5_PAYLOAD_SDK`,
`PS5_CLANG` and `CXX` can override tool paths. `manifest.json` records compiler
versions, the SDK path and packaged-file SHA-256 values. Rebuild the SDK with
`scripts/setup-sdk.sh` if its provenance is uncertain. This is a native CRT and
fSELF conversion path, not a payload ELF renamed to `eboot.bin`.

The runtime is generated twice, compared byte-for-byte, and checked against the
upstream raw and signed release hashes. The converter checks both signed
containers' integrity. ZIP entries use mode `0777`; extraction/FTP must preserve
executable permissions.

`ps5/native-capabilities/main.c` queries the CPU model (`sysctlbyname("hw.model")`),
CPU affinity and direct/flexible memory budgets, logs `[ps5lm-native]`, then requests
shell-managed closure. It allocates no test buffers, writes no files, requests
no elevation and does **not** execute GPU work. A `PS5LM_NATIVE_RESULT` success
only means the queries succeeded; separately observe process exit and absence of
crash records. Availability is not a safe allocation ceiling.

### Hardware launch gate (capabilities and tiny GPU correctness passed)

Keep **PPSA99040 query-only capabilities as the first native hardware launch**.
Observe its result, process exit status and absence of crash records separately.
Only after successful capabilities and a clean exit may PPSA99041 be considered.
Neither local building nor a payload run establishes native launch or GPU execution.
These build scripts never contact a console, load payloads or register titles.

The investigated setup is unmodified ShadowMountPlus `1.7-beta5fix1` plus
standalone Kstuff Lite `v1.11`, not etaHEN. The current ShadowMount release
explicitly says not to combine it with etaHEN. On 2026-10-15, the pinned loaders
started on the CFI-7019 Pro / 12.70 and registered PPSA99040 successfully.
After the failed attempts below, capabilities and clean shell-managed close passed.
One 64-integer GPU dispatch and clean closure also passed; ggml/model correctness
and inference performance remain unverified.
See the [loader release](https://github.com/drakmor/ShadowMountPlus/releases/tag/1.7-beta5fix1)
and [Kstuff release](https://github.com/EchoStretch/kstuff-lite/releases/tag/v1.11).

Its documented manual-source API can retain
`/data/PS5LM/native/PPSA99040`; `/data/homebrew` is not mandatory.
The approved GPU source is `/data/PS5LM/native/PPSA99041`.
The user approved the loader startup/configuration/registration exceptions and
one PPSA99040 query-only launch on 2026-10-15, then confirmed a clean session.
After capabilities and clean close passed, the user separately approved
PPSA99041 staging/registration and one monitored tiny GPU launch on 2026-10-15.
This does not authorize models, broad graphics tests or repeated GPU launches.
They include `/data/shadowmount` state, `/data/.kstuff_noautomount`, runtime
ShellCore hooks and `/system_ex` mounts, the `FAKE10101` shortcut, platform
metadata for registered titles and upstream global `snd0Info` normalization.
This is loader-managed database activity, not permission for us to issue SQL.
No direct shell-database editor or custom system patch is included here.

On 2026-10-15 the user approved agent launches: once `scripts/ps5-native-test.sh`
exists, an agent may launch an already-registered native title itself, one run
at a time, reading the result and confirming the title closed before the next
([MAC_DEV_LOOP.md](MAC_DEV_LOOP.md#native-titles)). Registering titles, loader
configuration and models still need the user's approval.

The user must arrange a clean compatible jailbreak session without etaHEN or
BackPork if necessary, close existing games, and later select the registered
capabilities title and Play. The capabilities-only approval does not authorize
autoload/catalog changes. The API is unauthenticated, so its approved binding
at `192.168.1.50:10101` must remain on a trusted network.

The deployed config uses the empty `/data/PS5LM/native-loader-scan` root and
the two approved manual sources, `/data/PS5LM/native/PPSA99040` and
`/data/PS5LM/native/PPSA99041`. Batch installation,
automatic removal, emulator/AMPR updates and fakelib overlays are disabled;
fan control remains with the system. The loader reported global startup
`snd0Info` maintenance changed zero rows; registration normalized one title row.
Evidence: `test-results/ps5/native-launch/20261015-090646/` (local, ignored).
Kernel-log collection started before either loader. Its initial 30-minute window
expired before Play; reconnecting recovered the retained native launch/crash log.
The first PPSA99040 launch ran as PID 132 and received SIGSEGV (signal 11),
with RIP/fault address zero and a recorded caller at `0x40016b`.
Neither the capabilities begin marker nor `PS5LM_NATIVE_RESULT` was present.
The process terminated through the crash-report path, not a clean application exit;
no native memory budget was obtained. Do not advance to PPSA99041.

Local audit identified payload-library identities in the failed executable:
`libkernel_web.prx` / `libkernel_sys.prx` and their matching SCE import-library
records, whereas the native process loaded `libkernel.sprx`. The repaired local
capabilities build explicitly links the existing SDK `libkernel.so` and
`libSceLibcInternal.so`, not the payload-stub wildcard. The payload-only
`sceKernelGetHwModelName` query is replaced by the public `hw.model` sysctl;
this identifies the CPU, not the console's CFI model. Memory/affinity queries and
the requirement for every query to succeed are unchanged.

`scripts/validate-native-capabilities.py` rejects payload dependency identities
and checks plain-symbol providers, converted NIDs and native module/library
records. It rejected the original build and passed all 15 imports after repair.
The rebuilt package passed runtime reproducibility/hash and fSELF integrity
checks locally. The user authorized one monitored capabilities retry; the repaired
`eboot.bin` was deployed with raw byte-exact readback and unchanged companion
runtime/registration. The original installed executable was preserved locally.
Retry evidence: `test-results/ps5/native-capabilities-retry/20261015-124122/`.
The retry ran as PID 135 and reported successful queries: 12 GiB direct memory,
448 MiB configured / 442 MiB available flexible memory, affinity `0x1fff`
(13 allowed logical CPUs), 16 CPUs online and CPU model `100-000000735`.
These are reported budgets, not tested safe allocation limits; no test buffers
or GPU work were performed. After the result, the log recorded an `exit(0)` call
followed by SIGSYS (signal 12), RIP `0x8000003ac` (`libkernel` + `0x3ac`),
with recorded caller `0x40017c`. PID 135 was absent afterward.
The import repair enabled the queries, but native termination still failed:
an exit-call log is not clean-exit proof. PPSA99041 remains unauthorized and blocked.
The 30-minute capture had expired before the user's next message; reconnecting
recovered the retained result/crash evidence.

The previous local termination repair followed the pinned boilerplate's
[native close example](https://github.com/blackbearreloaded/ps5-native-app-boilerplate/blob/b8dc4a3fe2e05e826b037bd68824751c7468a6d7/examples/self-update/src/main.cpp#L119-L127):
call `sceSystemServiceLoadExec("exit", NULL)` and sleep while the shell closes the
title, rather than return from `main` into the CRT's libc `exit`. There are no
owned test resources to release; normal native unloading is shell-managed.
`PS5LM_NATIVE_EXIT_REQUEST` records the intended status before the call.
If that LoadExec call returned, `PS5LM_NATIVE_EXIT_RETURN` recorded zero or
`PS5LM_NATIVE_ABORT` records rejection; rejection invalidates overall success.
The app remains sleeping for manual Close Game if closure fails, never falling
back to a raw exit syscall. None of these markers proves clean termination.

This local build explicitly adds `libSceSystemService` and passes all 17 native
import bindings, pinned runtime checks, fSELF integrity and ZIP content checks.
A throwaway host control-flow smoke exercised successful queries, failed queries
and rejected closure, preserving intended status and reaching the sleeping wait
instead of returning. This is not PS5 lifecycle proof. Evidence:
`test-results/local/native-capabilities-termination/20261015-135158/`.
The user authorized one shell-exit capabilities retry. The new executable was
deployed with raw byte-exact readback; the old executable was preserved locally,
and companion runtime/registration remained unchanged.
Evidence: `test-results/ps5/native-capabilities-shell-exit/20261015-135534/`.
The monitored launch (PID 138) repeated all successful query values and requested
status zero. ShellCore recorded `NOTE_EXIT`, `Kill for LoadExec(0x8a) => 0`,
and removal of PID 138, with no observed target-process fatal signal.
However, it then interpreted `exit` as an executable path, created replacement
PID 139 (`SceSysCore.elf`), and failed that spawn with `0x80aa001a`, errno 22
(`EINVAL`), and SIGSYS (12). The app was unloaded and both PIDs were absent
afterward; existing loader services remained responsive.
Result: queries passed, exit-spawn failed. The documented upstream close pattern
does not establish a clean lifecycle on this setup. At this stage the clean-exit
gate was still failed. No GPU registration or launch was authorized.

The user then approved read-only retrieval of
`/system/common/lib/libSceSystemService.sprx` for local ABI inspection. The FTP
server's default SELF decryption produced a 365748-byte ELF, SHA-256
`03c3c341a3e209cce555c9f42049a6c18b3c451d0bbb2b7c248b9b4e52e0a3d3`.
No console writes, hooks or launches were performed for that inspection.
Evidence: `test-results/local/native-close-api/20261015-140400/` (ignored;
the system module is not redistributed).

The current local package calls `int sceShellCoreUtilExitApp(void)`, not LoadExec.
In this firmware module, NID `hnvi3LgVeDM` is exported at `0x10af0` from library
`libSceShellCoreUtil` within module `libSceSystemService`. Disassembly shows no
incoming argument consumption: the wrapper sends close request 2 with a zeroed
value flag and returns the IPC status. It does not pass an executable path.
This establishes the candidate ABI and binding, not runtime permission or clean
exit on the Pro. The sleeping wait, intended-status marker and rejection abort
remain; the diagnostic stage is now `shellcore_exit_app`.

`patches/native/0003-native-import-library-override.patch` adds an explicit
`--stub-library module=library` converter option, applied to a generated host
source copy. The capabilities recipe selects
`libSceSystemService=libSceShellCoreUtil` while retaining the real module SONAME.
The converter supports one import library per module; the capabilities validator
therefore rejects any other SystemService symbol in this close-only binding.
It also checks exact module/library IDs and accepts an optional third argument
for read-only verification against a local firmware module:
`python3 scripts/validate-native-capabilities.py build/native-capabilities .deps/ps5-payload-sdk MODULE.elf`.

The build passed all 17 import bindings, firmware close-export matching, pinned
runtime reproducibility/hashes, fSELF integrity and ZIP content checks. Omitting
the converter library override was exercised and rejected by the validator.
Four throwaway host control-flow cases covered query success/failure crossed
with close acceptance/rejection; none returned through libc exit.
The user authorized one direct-close capabilities retry. Its executable was
deployed with byte-exact raw readback; companion runtime/registration are unchanged
and the previous executable was preserved locally.
Evidence: `test-results/ps5/native-capabilities-direct-close/20261015-141650/`.
The monitored direct-close launch passed as PID 140, app ID `0x6018`:
`PS5LM_NATIVE_RESULT` reported success, and `sceShellCoreUtilExitApp` returned zero.
ShellCore recorded PID-specific `NOTE_EXIT`, `Kill process: 0x8c => 0`, process
removal, app unload, and completed `EndAppMount`; the subsequent process list
contained no PID 140 and the loader reported no remaining title runtime mount.
No abort, fatal signal, crash-report start or exit-spawn failure was observed in
the captured launch window. Payload Manager, loader API, FTP and kernel-log
services answered afterward. This is clean shell-managed closure, not a
conventional process `exit(0)` return-code observation.

Native queries again reported 12 GiB direct memory, 448 MiB configured / 442 MiB
available flexible memory and affinity `0x1fff` (13 allowed logical CPUs).
No test buffers were allocated and no GPU work ran. These are reported budgets,
not validated safe allocation limits. The capabilities/clean-close gate is now
satisfied. PPSA99041's first authorized hardware attempt failed during runtime
setup before Vulkan submission; another launch requires authorization.


The compute build now explicitly links native `libkernel`, not payload
`libkernel_web`/`libkernel_sys` variants, and selects the same close-only
`libSceShellCoreUtil` import library inside `libSceSystemService` as capabilities.
The validator rejects payload identities, wrong module/library IDs and raw
`_exit`/`_Exit` imports. The legacy CRT's `exit` import remains, but probe `main`
never returns into that path.
For byte-exact native SELF readback, issue `SELF` once per FTP connection to
disable ftpsrv's default on-the-fly SELF decryption before `RETR`.

Supported cleanup: Close Game, remove its manual source, unmount if prepared,
then request `/api/v1/games/uninstall` and await asynchronous completion.
Keep the source folder unless deletion is separately requested. Full loader
retirement also removes its shortcut and uses `/data/shadowmount/STOP` for
orderly teardown; let the loader release its automount sentinel. The user
reboots to return to an unpatched Kstuff session. Uninstall is not a promise
of byte-identical rollback of global loader maintenance.
See the [supported API](https://github.com/drakmor/ShadowMountPlus/blob/1.7-beta5fix1/docs/api.md).

### Local native compute package

`ps5/native-compute/` is a separate native application, PPSA99041. It executes
one workgroup of 64 unsigned integers, `out[i] = i * 3 + 7`, into a 256-byte
storage buffer initially filled with a nonmatching sentinel. The CPU verifies
every output. There is no application graphics/display work, privilege request,
model loading or inference.

Limits and failure behavior:

- Vulkan buffer allocation **and required alignment** must each be at most 1 MiB.
- A fixed, allocation-free ledger caps aggregate static RADV/SDK direct-memory
  requests at **128 MiB**, conservatively charging 64 KiB units. It includes
  the driver's 16 MiB ring, heap segments and wrapped thread stacks. SDK heap
  granularity is reduced to 1 MiB; flexible-heap fallback terminates the probe.
- Executable/image backing, the initial stack, kernel resources and allocations
  internal to imported system modules are **not** capped by static wrapping.
  Large driver/heap virtual reservations are not equivalent to physical allocation.
- Fence timeout request: **2 seconds**. An independently scheduled watchdog has a
  **30-second** deadline and stays armed through cleanup and shell-managed close.
  Thread creation, scheduling, a wedged kernel/GPU and SystemService IPC remain
  hardware limits; this is not a console recovery mechanism.
- No `vkDeviceWaitIdle`. If a submission was attempted without fence success,
  terminate without application-side Vulkan destruction of GPU-owned buffers.
- Host-write/compute-write/host-read barriers and noncoherent flush/invalidate
  surround the dispatch/readback. A completion marker is not clean-exit proof.
- All normal, early-failure, unsafe-submission and watchdog paths request native
  closure through `sceShellCoreUtilExitApp`; no raw process-exit fallback or CRT
  return is used by the probe. Normal completion cleans Vulkan resources first;
  uncertain submissions still skip application-side destruction.
- Both the main thread and watchdog can request closure. A stuck close request
  does not deliberately disarm the watchdog, but the module's internal locks or
  a kernel wedge can block both callers. Rejected close requests emit ABORT with
  `RC_CLOSE=10` and the actual `close_rc`, then sleep pending manual Close Game.

Results use kernel-log `PS5LM_COMPUTE_RESULT` JSON; a later
`PS5LM_COMPUTE_ABORT` invalidates a completion marker. Logs include Vulkan and
process-intended status, phase timings, mismatch details and physical budget
counters. Mesa stdout/stderr append only to
`/data/PS5LM/tests/native-compute.out`. The disk shader cache is disabled before
Mesa calls; its explicit path is `/data/PS5LM/cache/native-compute`, overriding
the port's `/app0/radv-shader-cache` default. A closed static-caller environment
disables inherited dumps/tracing. Writable opens and mkdirs reject unexpected
paths rather than silently redirecting output. Imported modules' internal I/O
is outside those linker guards.

`PS5LM_COMPUTE_EXIT_REQUEST` / `PS5LM_COMPUTE_EXIT_RETURN` describe the close
request, not proof of process removal. The watchdog can still invalidate a
completion marker while teardown or close is pending.

Setup failures additionally emit bounded kernel-log `PS5LM_COMPUTE_SETUP_ERROR`
JSON before cleanup, containing the specific `setup_*` stage, target path or
environment key, actual `call_rc`, captured `errno`, `errno_valid`, and observed
file `mode` for type-check failures. `errno_valid=false` distinguishes validation
failures and `setvbuf` failures from syscall errno; stale errno is not reported
as a cause. A null `freopen` result is recorded as `call_rc=0`.
The final result's stage names that same failing setup operation. No setup
operation is skipped, retried or relaxed by these diagnostics.


### Reproducing the local GPU build

Prepared source/SDK pins:

| Input | Revision |
| --- | --- |
| PS5_Vulkan | `f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed` |
| PS5_Mesa | `1ba9874f478657ccf6da72ce55f0260164cc0bd0` |
| PS5_PayloadSDK platform fork | `b83202be73e930050e00de0f1b4d9ec46c0391bf` over verified SDK v0.42 |
| compiler-rt | LLVM `21.1.8`, source SHA-256 `dd54ae21aee1780fac59445b51ebff601ad016b31ac3a7de3b21126fd3ccb229` |

The SDK at `.deps/PS5_Vulkan/.deps/native/ps5-payload-sdk` is distinct from our
stock payload SDK v0.43. Do not substitute the stock SDK for its platform library.
Sources are under `.deps/PS5_Vulkan`, `.deps/PS5_Mesa`, `.deps/PS5_PayloadSDK`.
If reconstructing that SDK, export the pinned fork with `git archive` and run
its `platform/tools/setup-sdk.sh SDK_PATH REVISION CACHE_PATH` from the export.
On Mac put Homebrew GNU Make and coreutils `libexec/gnubin` on PATH for that
bootstrap; it also requires wget, flock and unzip.

The demonstrated Mac host tools are LLVM/Clang 23.1.3, matching
SPIRV-LLVM-Translator 23.1.2 and libclc 23.1.3, glslang 16.6.0, SPIRV-Tools
1.4.363.0, Ninja 1.13.2 and Meson 1.10.2. Target compilation uses Clang 21.1.8.
`build-native-radv.sh` checks the compiler versions, pins the Python generators
in `.deps/radv-python`, and records its commands in source plus artifact provenance.
The package builder additionally requires host C++20/zlib and `patch`.

From the prepared sources and SDK, with those tool versions installed:

```sh
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
bash scripts/build-native-radv.sh
bash scripts/build-native-compute.sh
```

The second command builds compiler-rt through `scripts/build-ps5-builtins.sh`.
The **actual final no-builtins diagnostic link** requires only
`__emutls_get_address` and `__cpu_model`; integer-division helpers observed in
the uncollected archive disappear with dead-section collection. The runtime
archive therefore contains only upstream `emutls.c` and `cpu_model/x86.c`,
compiled for **x86_64-sie-ps5 with the prepared target headers**. No Darwin
runtime or unresolved-symbol suppression is used.

`patches/native/0001-target-builtins-override.patch` lets the existing link recipe
consume this explicit target archive. `0002-no-proc-fd-diagnostic.patch`
omits only the Linux proc-fd/readlink diagnostic on PS5. The package builder
recompiles that one object using its formal Meson compile recipe and derives a
separate archive, leaving the prepared driver untouched. `posix.c` adapts the
SDK's real `ps5_mkstemps` to `mkstemp`. This removes known-inaccessible
libkernel_sys/WebKit imports rather than pretending their stubs work natively.
Whole-archive linkage preserves weak dispatch references; section collection
removes unreachable code. Wrapper aliases are explicitly localized in a map.
Native patches stay below `patches/native/`, outside the llama builder's
`patches/*.patch` glob.
The generated native converter also applies
`0003-native-import-library-override.patch` with
`--stub-library libSceSystemService=libSceShellCoreUtil`. The module SONAME stays
`libSceSystemService`; only its selected import library differs. Mixed imports
from other SystemService libraries are rejected by the close-only validator.

Outputs:

- `build/native-compute/PPSA99041/` and deterministic `PPSA99041.zip`.
- `manifest.json`: source/build-script/package hashes, compiler versions,
  driver derivation, compiler-rt provenance and hardware gate.
- `runtime-audit.log`, `archive-extraction.tsv`, `link.map`: real failed
  no-builtins link and successful strict-link extraction evidence.
- `imports.json`: declared modules, plain-symbol providers and selected native
  libraries; validation checks exact module/library records and converted NIDs,
  the companion runtime contract, native ABI, package contents, fSELF integrity
  and reconstructed stored bytes.

The fSELF writer intentionally omits an unmapped, zero-memsz PT_NOTE tail;
validation permits only that omission, preserving every stored/loadable byte
and recording omitted-note hashes. The byte-pinned upstream libc companion has
no legacy section-table tail; its program/dynamic tables and import manifest
are checked instead of relying on section-oriented readelf output.

The 2026-10-15 local repair passed strict linking, SPIR-V validation, 258 native
NID imports, pinned companion/runtime checks and package integrity. The updated
validator rejected the original payload-library package. Six throwaway host
smokes exercised close acceptance/rejection, early fatal failure, watchdog
deadline, late watchdog invalidation of success, and aggregate-budget/exact-release
invariants. The harness substituted PS5 APIs and omitted only the x86 PAUSE hint;
it did not run Mesa or a GPU and did not test concurrent scheduling or native
close IPC under driver failure. Evidence:
`test-results/local/native-compute-preflight/20261015-142028/`.
The user subsequently approved PPSA99041 staging, registration and one monitored
tiny GPU launch. All seven packaged files were staged under
`/data/PS5LM/native/PPSA99041` with byte-exact raw readback; the existing runtime
and metadata were included unchanged from the audited package.
The documented manual-source API registered **PS5LM Native Compute**, with
exactly the capabilities and compute sources present. No loaders were reinjected
and no scan-root/config changes were made. Kernel-log capture started before
registration with a two-hour bound.
Evidence: `test-results/ps5/native-compute/20261015-144615/`.
The first launch ran as PID 141, app ID `0x8018`. It reported failure
`rc=2`, stage `runtime_setup`, before any Vulkan submission: zero verified words,
no buffer allocation, no fence completion. The static direct-memory ledger
reported an 18 MiB peak/live charge at the marker, with zero denials/errors;
this is startup bookkeeping, not a GPU memory measurement or total residency.
`cleanup_complete=true` here does not prove GPU cleanup: no Vulkan probe ran.

The close API returned zero, and ShellCore recorded PID-specific exit/removal,
app unload and completed `EndAppMount`; PID 141 was absent afterward and the
loader reported no remaining title runtime mount. No ABORT or crash was observed;
HTTP loader/manager and FTP services answered. This is a cleanly closed failed
test, not GPU correctness.

`/data/PS5LM/tests/native-compute.out` was absent afterward. Loader logs confirm
that `/data` was mapped into this title's sandbox before execution and unmounted
afterward, so do not assume the disabled fakelib overlays removed data access.
That deployed build did not report the exact failing operation or errno.
A local diagnostic-only revision now covers all ten setup failure branches.
Thirteen throwaway host smokes checked their target/return/errno/mode reports
and acceptance of absent output, existing regular output and existing directories.
The full native build, 258 import bindings, SPIR-V and package integrity passed.
Evidence: `test-results/local/native-compute-setup-diagnostics/20261015-145256/`.
The user authorized deployment of the diagnostic revision and one monitored
PPSA99041 retry. The executable was replaced with byte-exact raw readback and
the installed predecessor preserved locally; companion runtime, registration,
loader instances and configuration are unchanged.
Evidence: `test-results/ps5/native-compute-setup-diagnostics/20261015-145941/`.
The diagnostic retry ran as PID 142, app ID `0xa018`, and identified
`setup_lstat_directory`: `lstat("/data/PS5LM")` returned `-1`, errno 1 (`EPERM`).
No Vulkan submission or buffer allocation occurred; zero words were verified.
The tracked direct-memory peak/live charge was again 18 MiB with no ledger
denials/errors. `mode=0` is not a measured file type: lstat failed.
The close request returned zero, PID-specific shell exit/removal and completed
`EndAppMount` were observed, the runtime mount was released, and services
answered afterward. No later ABORT, fatal signal or exit-spawn failure appeared.

The user authorized future read-only investigation without per-step approval.
Reading `/system/common/lib/libkernel.sprx` produced a 563688-byte decrypted ELF,
SHA-256 `a3fb057464c74541c1e8edb0701023b20fe7d3d4746fe38324cea4b727cb7381`.
Its `lstat` export resolves through the expected `libkernel` library to syscall
190 at `0xe80`; `fstat` uses syscall 189 at `0xe60`. There is no exported
`sceKernelLstat` or `fstatat`. This excludes a wrong-library binding for the
observed lstat call, but does not establish a blanket syscall policy.
The native `sceKernelStat` wrapper uses the following-stat route, so it is not
a substitute for a no-follow guard.

The current local compute build changes only setup metadata inspection to the
pinned native helper's descriptor pattern:
`open(O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)` followed by `fstat` and
`close`, adding `O_DIRECTORY` for directory checks. Only a missing output
(`ENOENT`) is accepted; symlinks, dangling symlinks, wrong types and other open
errors fail closed. Every opened descriptor is closed even when fstat fails;
the original fstat errno is preserved across close. Close failure also fails
setup. Stage-specific open/fstat/close diagnostics identify the rejected call.
No global lstat shim, following-stat fallback, permission change or elevation
is introduced. This requires read access, unlike a metadata-only lstat lookup;
it is deliberately more conservative. Existing output nodes can be opened
read-only before fstat rejects a special type; O_NONBLOCK prevents FIFO blocking,
not arbitrary device side effects. O_NOFOLLOW guards the final component, as
the prior lstat checks did; it is not an all-components race-free sandbox.
Reference: the pinned boilerplate's
[descriptor-based file guard](https://github.com/blackbearreloaded/ps5-native-app-boilerplate/blob/b8dc4a3fe2e05e826b037bd68824751c7468a6d7/examples/self-update-helper/files.cpp#L115-L125).

The full native build, 258 import bindings, SPIR-V/runtime/package checks and
14 host smokes passed. Real host filesystem cases covered directory/regular
file acceptance, absent output versus absent directory, file/directory/dangling
symlinks, FIFO rejection without blocking, wrong types, descriptor closure and
fstat/close errors with preserved errno. Native wrappers were inspected read-only;
the guarded metadata route subsequently passed on the Pro in the run below.
Evidence: `test-results/local/native-metadata/20261015-153632/`.
The user approved one metadata-repair compute retry and removed the requirement
for separate future deployment approval. This does not authorize unattended
repeated GPU launches or relax the one-bounded-test-at-a-time safety rules.
The new executable is deployed with byte-exact raw readback; the predecessor is
preserved locally and companion runtime, registration, loaders and configuration
are unchanged.
Evidence: `test-results/ps5/native-compute-metadata/20261015-160502/`.
The metadata-repair launch passed as PID 143, app ID `0xc018`. Runtime setup
completed and RADV submitted the compute workload. The fence completed, all
64 outputs matched `i * 3 + 7`, mismatches were zero, and application Vulkan
cleanup completed. The Vulkan allocation was 256 bytes, memory type 5,
host-coherent. The wrapped direct-memory ledger peaked at 38273024 bytes
(36.5 MiB), with zero denials/errors; 37814272 bytes remained charged at the
result marker. That marker does not identify their owners or establish total
residency or a leak; ShellCore subsequently removed the process.

Reported phase timings were init 73129 us, compile 384 us, submit/wait 219 us,
verify 1 us, cleanup 128 us, total 73892 us. This is one tiny dispatch, not an
inference or throughput benchmark. No cache warmup or repeated benchmark ran.
The output file contained `ac_compute_device_uuid's output is based on invalid
pci bus info.`; device UUID provenance remains a limitation, not a readback failure.

The native close API returned zero. PID-specific `NOTE_EXIT`, successful shell
kill/removal, app unload and completed `EndAppMount` were observed; PID 143 was
absent from the post-run process list and the loader reported no remaining
runtime mount. No later ABORT, setup error, fatal signal, crash-report start or
exit-spawn failure appeared. Payload Manager, loader API, FTP and kernel-log
services answered afterward. The independently recorded result is `pass`;
the in-process marker deliberately retains `clean_exit_proven=false` because
only the external observation establishes clean closure.

This satisfies the tiny native GPU correctness gate. It does not validate
ggml operators, model residency, larger allocations, timeout recovery or LAN
inference. Next is local llama.cpp/ggml-vulkan integration, then a small resident
dense model and a smaller resident MoE with repeatable benchmarks; SSD expert
streaming remains deferred.


### Native llama / bounded ggml integration

`bash scripts/build-native-compute.sh ggml-ops` builds the **separate local**
`build/native-ggml-ops/PPSA99041.zip`. Default `compute` still builds the tiny
integer probe in `build/native-compute`; neither command contacts the console.
Both use the existing PPSA99041 identity, so they are alternative executables,
not titles to register or launch together.

`scripts/build-native-llama.sh` exports llama commit
`552f18f912a32ea86edf82e2b76431cb7131538d` into `build/native-llama/source`,
applies the existing CPU patches plus `patches/ggml-vulkan/*.patch`, and builds
static libllama, ggml CPU and ggml Vulkan with the pinned **native platform SDK**.
The dependency working tree is not patched. C++ exceptions/RTTI are explicitly
enabled for llama/Vulkan-Hpp; the SDK target otherwise defaults exceptions off.
Shader generation runs on the Mac with host glslc. The library manifest records
the source pin, patch/library hashes and aggregate shader digest; it is the
source-provenance authority, not parent-repository Git discovery in the export.

Native-only patches replace the Vulkan graph's unbounded CPU fence spin with a
sleeping wait and disable unsupported debugger-process backtraces. Native backend
initialization uses the compiled-in registry; explicit plugin paths are rejected,
not searched through `/proc` or the current directory. The static link binds the
two remaining direct Vulkan calls to Mesa's `vk_common_CmdCopyBuffer` and
`vk_common_GetPhysicalDeviceFeatures2`; Vulkan-Hpp's proc-address path goes through
`ggml-dispatch.c`, not a system Vulkan loader or payload import.

The initial runner in `ps5/native-compute/ggml-ops.cpp` compared five fixed FP32
graphs: ADD/MUL/RMS_NORM on 8×8 tensors, MUL_MAT with K=32 and 8×8 output, and
MUL_MAT_ID with two resident experts and alternating selections, 64 values each.
It used one CPU reference thread, rejected unsupported GPU nodes without CPU
fallback, seeded output with NaNs, rejected nonfinite values and compared every
element with `abs_error <= 1e-4 + 1e-3 * abs(reference)`. A mismatch stopped the run.
No CLI filters, broad upstream suite, model or API ran. Its hardware evidence is
recorded below; the expanded suite replaces these already-passed cases.

The shared compute runtime retains its no-follow metadata guards, restricted
output/cache paths, 128 MiB static direct-memory ledger, 1 MiB SDK heap granularity
and 30-second watchdog through shell closure. Ops output is restricted to
`/data/PS5LM/tests/native-ggml-ops.out`; its cache is disabled.
The guarded dispatch caps Vulkan buffers, allocations and required alignment at
1 MiB, permits at most 32 queue-submit calls (four submit records each, eight
command buffers per record), and at most 64 workgroups per dispatch with Z=1.
Fence/semaphore waits are capped at two seconds; idle waits are refused.
Async execution, cooperative matrices, fusion and graph optimization are disabled
for this first bounded comparison. Unsupported optional proc lookups return NULL.
Submission/wait errors, static aborts and caught exceptions request native close,
without deliberately unwinding GPU-owned resources; recovery is not guaranteed.

`PS5LM_COMPUTE_RESULT` identifies current `workload:"ggml-attn-prefill-scoped-fp32"`;
earlier tags include `"ggml-ops-f32"`, `"ggml-ops-quant-attn"`,
`"ggml-attn-decode-diagnostic"` and `"ggml-attn-decode-fp32"`.
Legacy integer-specific buffer, coherence and phase-timing fields are not ggml allocation/performance measurements.
For this mode, `cleanup_complete` means graph buffers/contexts/backends were
released; process-lifetime registry/device objects remain until shell removal.
Executable/shader backing and imported modules remain outside the static ledger.
A result still needs PID-matched external close/mount evidence and no later ABORT.

Initial local verification passed: strict native link, 277 audited NID imports, pinned
companion/fSELF/package checks; all 1,567 generated SPIR-V modules validated.
A throwaway host smoke exercised all five graphs and independently checked their
320 CPU outputs against scalar formulas. Its second backend was explicitly CPU,
not a simulated GPU correctness claim. A dispatch-boundary smoke covered
allocation/alignment caps, timeout/device-loss closure, submission/dispatch bounds,
idle refusal and optional instance/device lookup behavior. Default tiny-compute
packaging also passed with its 258 imports. Hardware operator evidence follows.

The operator executable is deployed to the existing PPSA99041 source, with
staged/final raw FTP readback verified and the former successful integer-probe
executable preserved locally. Companion runtime, existing metadata/licenses and
registration were verified unchanged; the llama MIT notice was added.
New eboot SHA-256:
`fdbaff61ac10e0645b08705ac816321e6a263b881711bca0b0ff3e3a361ef4c8`.
Evidence: `test-results/ps5/native-ggml-ops/20261015-175624/`, including
`local-result.json`, `deployment.json`, `build-manifest.json`, `result.json`,
`native-ggml-ops.out`, `klog.log` and post-run process/loader state.
The user selected Play once. PID 144 / app ID `0xe018` passed all five operators:
320/320 values checked, zero mismatches. Maximum absolute errors were zero for
ADD/MUL, `2.38418579e-7` for RMS_NORM/MUL_MAT and `2.68220901e-7` for MUL_MAT_ID.
The fence and explicit graph/context/backend cleanup completed. Wrapped direct
memory peaked at 48,103,424 bytes (45.875 MiB), with zero denials/ledger errors;
47,841,280 bytes remained charged at the marker. This is not total residency or
a leak measurement; process-lifetime device/registry objects remain until close.
The reported whole-run time was 819,647 us, including the CPU reference work:
one bounded correctness run, not an inference/throughput benchmark.

The native close API returned zero. PID-specific NOTE_EXIT, successful shell
kill, process deletion, completed EndAppMount and loader runtime release were
observed; PID 144 was absent afterward and the title was unmounted. No later
ABORT, target fatal signal, crash-report start or replacement-spawn failure was
observed. The generic ResArbitrator TestId `CRASH KILL_END` label used PID
`0xffffffff` during suspend/STOP; the prior passing integer run has the same
label, so it is not treated alone as target-crash evidence. Services answered
afterward and local kernel-log capture was stopped. The external result is
`pass` / `clean_exit:true`; the in-process `clean_exit_proven:false` is intentional.

The driver still warns about invalid PCI bus info for UUID generation. Its
async-disabled warning mentions Intel even though this title explicitly disables
async on the reported AMD device; no warning was suppressed. The five FP32
graphs are now hardware-verified, not quantized operators, attention, complete
model execution/residency, timeout recovery or LAN inference. Extend bounded
operator coverage for the selected dense model before introducing its weights.

### Expanded bounded quantized/attention checks

The `ggml-ops` build mode previously prepared these six fixed cases, replacing
the already-passed FP32 suite. Its shared workload contract expected 840 values,
checked against the C++ case table at compile time. Hardware results follow.

| Case | Shape | Output values |
| --- | --- | ---: |
| Q4_K_MATVEC | K=256, eight output rows, one input column | 8 |
| Q4_K_MATMUL | K=256, eight output rows, eight input columns | 64 |
| Q6_K_MATMUL | Same matrix shape, Q6_K weights | 64 |
| Q4_K_MOE | K=256, eight rows, two resident experts, two selections × four tokens | 64 |
| ATTN_DECODE | Head size 64, two Q heads / one KV head, 32 KV tokens, one query | 128 |
| ATTN_PREFILL | Same GQA geometry, four queries, causal mask | 512 |

Matmul inputs use deterministic varied weights and ternary activations, exactly
representable by both the CPU's Q8_K activation conversion and GPU FP16 path.
Both backends receive the same quantized weights. These cases do not validate
arbitrary activation-rounding behavior. Attention uses FP32 Q, FP16 K/V, a FP16
causal mask, scale 1/8, and requested FP32 accumulation. There is no model file,
model memory allocation or server; the stale `.gguf.part` is ignored, not deleted.

The pinned CPU flash-attention implementation accumulates V in FP16 when its
V tensor is FP16, even for `GGML_PREC_F32` (`ggml-cpu/ops.cpp`'s
`ggml_compute_forward_flash_attn_ext_f16_one_chunk`). An initial host check
observed about `1.51e-4` error at one element against scalar attention.
The reference therefore uses FP32 cache tensors holding the **exact FP16-rounded
K/V values** sent to the GPU, keeping CPU accumulation in FP32 without changing
GPU input precision or weakening tolerance to accommodate the CPU FP16 path.

Every value must be finite and satisfy `abs_error <= atol + rtol * abs(reference)`.
Quantized matmul uses atol=0.002 and rtol=0.002; attention uses atol=0.0001 and
rtol=0.001. These thresholds are fixed before a GPU run. Failure stops the suite;
case-begin, unsupported-backend and first-mismatch-index diagnostics identify it.
The 128 MiB direct-memory, 1 MiB allocation/alignment, 32-submit, dispatch,
two-second wait and 30-second watchdog limits are unchanged.

The host smoke exercised all six CPU reference graphs and independently checked
all 840 results with scalar dequantized dot products and stable causal softmax.
Maximum differences were at most `1.90734863e-6` for matmul and `1.49011612e-7`
for attention. No host GPU substitution is treated as hardware proof.
The native package passed strict linking, 277 native NID imports, pinned-runtime
and fSELF/package validation. Its eboot SHA-256 is
`48e8916997e9fd75a1ca120663e8ed024a0ebe73cbf97520c79993273721f8cd`.
The expanded hardware result below is a numerical failure; full model graphs
and residency remain unverified.

This expanded executable is deployed to the existing PPSA99041 source, with
staged/final raw readback verified. The passing FP32 executable and output were
preserved locally; the remote old output was then cleared to prevent mixed rows.
Runtime, metadata, licenses, registration and loader instances are unchanged.
All four services answered afterward. Evidence:
`test-results/ps5/native-ggml-quant-attn/20261015-181614/`, including
`local-result.json`, `host-reference.json`, `deployment.json`, build manifest,
`result.json`, `native-ggml-ops.out`, kernel/loader logs and post-run state.
The user launched once: PID 145 / app ID `0x18`. All four quantized cases passed,
200 outputs total, maximum absolute error `1.90734863e-6`. ATTN_DECODE then
failed one of 128 outputs (first mismatch index 77, case-wide maximum absolute
error `0.000507116318`). The suite stopped; ATTN_PREFILL was **not run**.
The result reports rc=6, 328 checked values and one mismatch. Neither tolerance
was changed. Fence and explicit cleanup completed, tracked peak 38.875 MiB,
zero allocation denials/ledger errors. Close returned zero; PID-specific NOTE_EXIT,
successful shell kill/deletion, EndAppMount and loader runtime release were
observed, with no later ABORT, target fatal signal, crash-report start or
replacement-spawn failure. The process was absent and title unmounted afterward;
all services answered and local log capture was stopped. External status is
`failed` with `clean_exit:true`, not a crash or timeout.

Source-level lead, **not a proven root cause**: `ggml-vulkan.cpp:2063-2069`
selects the FP16 scalar variant on this FP16-capable device even for `f32acc`.
`vulkan-shaders-gen.cpp:713-724` sets ACC_TYPE to float while FLOAT_TYPE remains
float16_t. The scalar `flash_attn.comp` uses FLOAT_TYPEV4 for Qf and Of and
FLOAT_TYPE for final normalization, so FP32 accumulation does not mean all
intermediates are FP32. This is a candidate precision explanation, not evidence
of a RADV compiler bug. Exact failing GPU/reference values and the selected
runtime shader/tuning were not logged. The next diagnostic should capture them
and isolate attention precision without relaxing tolerances or rerunning the
already-passed matmul cases. No additional native run or model upload was made;
the partial model remains untouched.

### Attention-only numerical diagnostic

The current `ggml-ops` executable runs **one** ATTN_DECODE graph (128 outputs),
not the four passed matmul cases or the untested prefill case. Q/K/V generation,
GQA dimensions, mask, scale, requested accumulation and atol/rtol are unchanged
from the failing decode case. The CPU reference still stores the exact
FP16-rounded cache values in F32 tensors to avoid its FP16 value accumulator.
This is an instrumented baseline, **not a numerical fix**.

`PS5LM_ATTN_VALUE` records every index, exact CPU/GPU binary32 bit patterns as
eight hexadecimal digits, and the comparison result. Bits remain valid JSON
for NaNs/infinities; the comparator rejects nonfinite values without dropping
their evidence. `PS5LM_GGML_OP` reports mismatch count, first index and maximum
**finite** absolute error. Use these records rather than the kernel marker's
legacy integer expected/actual/first-mismatch fields for floating-point failures.

`patches/ggml-vulkan/0003-native-attention-diagnostics.patch` adds read-only
native logging, enabled by the probe's closed
`GGML_VK_PS5_ATTN_DIAGNOSTICS=1` environment entry:

- `PS5LM_ATTN_PIPELINE`: selected SPIR-V variant, byte size, Br/Bc, D split,
  row split, workgroup/subgroup size, alignment, F32 accumulation, FP16/dot2
  selection and specialization flags.
- `PS5LM_ATTN_DISPATCH`: Q/KV geometry, GQA ratio, split-K count/chunk size,
  scalar/cooperative path, tile sizes, scale, mask and accumulation request.

The patch is applied only to the generated llama source export; no shader
arithmetic, pipeline choice, tolerance, memory/submission cap or watchdog changes.
The existing all-FP32 scalar shader is available for a **separate later**
controlled comparison. Do not switch precision before collecting the original
failing-path readback, or run two uncontrolled variants in the same launch.

Host smoke passed: 128 CPU reference outputs matched independent scalar causal
attention (maximum absolute difference `1.1920929e-7`); exact-bit JSON roundtrips,
NaN/infinity rejection, mismatch indexing, and the tolerance boundary were
exercised. Native build passed 277 import bindings, pinned companion and package/
fSELF validation. Deployed eboot SHA-256:
`496dfa1567bd476d44745f14d08fd498212cb3316835022b9992a92335fb20bf`.
Staged/final raw readback and all unchanged companion files were verified;
the previous executable/output were preserved. Registration/loaders are unchanged.
Evidence: `test-results/ps5/native-ggml-attn-diagnostic/20261015-183433/`,
including `result.json`, exact decoded values, pipeline/dispatch logs and
`rounding-emulation.json`. The user-selected run (PID 146 / app ID `0x2018`)
finished and closed cleanly; the user reported the transient black screen cleared.
PID deletion, EndAppMount and loader runtime release completed, the title was
unmounted, and services answered. Capture was stopped. There were no allocation
denials/ledger errors, tracked peak 36.625 MiB. Whole-run time was 9.737231 s,
including diagnostics/file output, not a GPU throughput measurement.

The captured variant was `fp16_f32acc`, Br=2, Bc=32, D_split=8, row_split=1,
workgroup=128, subgroup=32, aligned, dot2=false, flags=2. Dispatch used GQA ratio
2, one query/two heads/32 cached tokens, split_k=1, scalar path=0.
The same one-element failure remained: index 77, reference bits `3eae7bc1`
(0.3407879173755646), GPU bits `3eae4000` (0.34033203125), absolute error
0.0004558861255645752 versus allowed 0.00044078791737556456.
The case-wide maximum error, 0.0005071163177490234, was at index 62 and passed
its larger relative allowance; do not confuse it with the failing element.

All 128 GPU outputs are exactly FP16-representable; only 53 equal the nearest
FP16 rounding of the CPU reference. A host scalar emulation following the
observed shader geometry, FP32 dot reductions, FP16 Q/probability/output
temporaries and fused FP16 multiply-add reproduced **all 128 GPU outputs
bit-for-bit**. The nonfused variant matched 115. This strongly attributes this
case's error to the selected mixed-precision arithmetic, not unexplained driver
corruption. It is not proof for other graphs. Keep tolerance unchanged and use
the existing FP32 scalar variant for the next separate controlled comparison.

### Controlled FP32 scalar comparison (decode passed on hardware)

The controlled decode executable added `GGML_VK_DISABLE_F16=1` to the closed
probe environment and changed only the workload tag to `"ggml-attn-decode-fp32"`.
The backend's existing force-disable switch sets device->fp16=false and selects
`flash_attn_f32_f16_fp32`; FP16 K/V **storage and values remain unchanged**.
This is a controlled compute-precision comparison, not a model-wide policy
or a claim that the GPU lacks FP16 support.

The runner source, shader sources/compiled shader digest and backend patches are
unchanged from the instrumented baseline. The only changed probe source files
are main.c (the environment entry) and ggml-ops.h (the workload tag). The same
one-query GQA graph, CPU reference, 128 outputs, atol/rtol, exact-bit logs and
all physical-allocation/submission/fence/watchdog limits remain in effect.

The selected shader artifact passed SPIR-V validation. A disassembly audit found
123 floating arithmetic/extended instructions with FP32 result types and none
with FP16 result types; the FP16 input storage conversions are not removed.
Artifact size is 82,152 bytes, SHA-256
`0cc37887dfce5f3d4fc0248680c0999105fa24ad85400fecfea983338b92f862`.
The unchanged CPU reference/comparator host smoke passed again. Native build
passed 277 import bindings and companion/fSELF/package checks.

The separate executable is deployed with staged/final raw byte verification;
its predecessor and output are preserved and companion/registration/loaders
remain unchanged. Eboot SHA-256:
`ad6b91672bf076f5e084dba73a71f5e02ab9a425065cf1df729f22e7e205f2c9`.
Evidence: `test-results/ps5/native-ggml-attn-fp32/20261015-184307/`, including
`result.json`, all decoded values, pipeline/dispatch output, kernel/loader logs
and post-run process/mount/service state. The user-selected run passed as
PID 147 / app ID `0x4018`: actual pipeline `"fp32"`, SPIR-V size 82,152,
all 128 outputs within the unchanged tolerance, zero mismatches. The CPU
reference bits and dispatch tuning are identical to the mixed-precision run.
Maximum absolute error was `1.4901161193847656e-7`. At the former failing
index 77, reference bits remain `3eae7bc1` (0.3407879173755646); GPU bits
are now `3eae7bbe` (0.3407878279685974), error `8.940696716308594e-8`.

Fence and explicit cleanup completed. Wrapped direct-memory peak was 36.625 MiB,
with zero allocation denials/ledger errors. Native close returned zero;
PID-specific NOTE_EXIT, successful shell kill/deletion, completed EndAppMount
and loader runtime release were observed. The process was absent and title
unmounted afterward, with no later ABORT, target fatal signal, crash-report
start or replacement-spawn failure. All services answered; capture was stopped.
Whole-run time was 9.744221 s, including exact-value diagnostic output, not an
inference benchmark or evidence of the relative throughput of the two variants.

The controlled A/B result resolves the measured decode discrepancy through
compute precision, without changing cache values or tolerances. It does not
validate prefill, full model graphs, memory scaling or timeout recovery.
Before adopting a model-wide policy, verify bounded prefill and scope the
precision choice to attention where possible rather than disabling fast FP16
matmul globally. Upstream server/application integration remains separate.

### Attention-scoped FP32 policy and bounded prefill (hardware passed)

`patches/ggml-vulkan/0004-native-f32-attention-intermediates.patch` scopes the
precision choice to **native non-MMQ scalar attention requesting F32
accumulation**. It selects the existing full-FP32 scalar shader instead of its
FP16-intermediate variant. Default/F16 attention, MMQ selection, other platforms
and all matmul code are unchanged. This is the native precision policy justified
by the controlled decode comparison, not a general driver/compiler workaround.
The global `GGML_VK_DISABLE_F16` probe entry is removed, so the device's FP16
matmul paths remain available.

The native workload `"ggml-attn-prefill-scoped-fp32"` uses four causal
queries, two Q heads sharing one KV head, head size 64, 32 KV positions, 512
outputs. Its cache storage/values, scale 1/8, F32 accumulation request,
atol=0.0001 and rtol=0.001 are unchanged from the originally prepared prefill
case. The CPU reference uses F32 accumulation over the same half-rounded cache.
Every output is initialized with a NaN sentinel, read back and compared.

Only the first **16 failing values** are logged as exact float bits; all 512
values are still compared and counted. The result includes total mismatches,
first index, finite maximum error, logged-failure count and log limit. This
keeps diagnostic I/O bounded rather than writing 512 successful-value rows.
No memory, dispatch, submission, fence or watchdog limits were increased.

Host smoke passed all 512 reference outputs against independent stable causal
attention (maximum absolute difference `1.49011612e-7`), nonfinite/last-element
failure checks, exact tolerance boundary, and bounded logging with a fully bad
512-value output. The final comparison counts every failure even after its
16-record diagnostic limit. Native build passed 277 native import bindings,
companion and fSELF/package validation. The existing shader sources/digest are
unchanged; only scalar pipeline selection changes in the generated export.

Deployed eboot SHA-256:
`916e88ca25c4709e60dcae58dcb02e4d51cc0517b95d2b04d869d173aa29074d`.
Staged/final raw readback and unchanged companions were verified. The passing
decode executable/output are preserved, registration/loaders unchanged.
Evidence: `test-results/ps5/native-ggml-prefill-scoped-fp32/20261015-185248/`,
including `result.json`, operator/pipeline/dispatch output and independent
kernel/loader/process/mount/service evidence. The user-selected run passed as
PID 148 / app ID `0x6018`: actual shader `"fp32"` (82,152 bytes) while
**device_fp16=true**, four query tokens, GQA ratio 2, split_k=1, mask enabled.
All 512 outputs were compared, zero mismatches, maximum absolute error
`1.49011612e-7`, no diagnostic failure rows. Tolerances and safety caps are
unchanged. This verifies the scoped scalar precision selection on hardware
without globally disabling the device's FP16 paths; matmul throughput was not
remeasured or inferred from this graph.

Fence and explicit cleanup completed, wrapped direct-memory peak 36.625 MiB,
zero denials/ledger errors. Close returned zero; PID-specific NOTE_EXIT and
successful shell kill/deletion, EndAppMount and loader runtime release were
observed. PID 148 was absent and title unmounted afterward; no later ABORT,
target fatal signal, crash-report start or replacement-spawn failure appeared.
All four services answered and capture was stopped. Reported total 888,960 us
is one bounded correctness run including CPU reference work, not inference speed.

The selected tiny quantized matmul/MoE and attention decode/prefill checks have
now passed across their recorded runs. Full selected-model graph coverage,
resident-model allocation policy, model execution, memory scaling and timeout
recovery remain separate gates. Next establish a small resident dense-model
baseline with the validated native runtime, reusing the upstream application/
server integration rather than importing its loader/closure/build assumptions.

### PS5LM upstream integration decision

Upstream `cobanov/PS5LM` at `f2ee6ba` contains a native llama-server application,
model planner, dashboard and remote-control workflow, but retains our same
`552f18f` llama pin. No upstream merge was performed while isolating attention.
Reuse its server/application work after this gate, rather than rebuilding it,
but retain this project's validated native identities/closure, safety guards and
Mac build. Its LoadExec `"exit"` route failed on our target; its build assumes
Linux/OpenGL dependencies and wildcard SDK stubs, and issue #16 reports storage
failures on 12.70. None is justification to replace the current validated runtime
wholesale or relax correctness checks.

### Native launcher verified (2026-10-15)

`scripts/ps5-native-test.sh` deployed, launched and watched PPSA99041 from the
Mac with no Play by hand: launcher payload `probes/launch` started the title
through `sceLncUtilLaunchApp` in 0.7 s (PID 150, app ID `0x8018`), the deployed
`ggml-attn-prefill-scoped-fp32` workload passed 512/512 again, and the title
closed itself (`ProcessTerm`, `EndAppMount`), unmounted before and after.
Evidence: `test-results/ps5/PPSA99041/20261015-190216/`. On that date the user
approved agent launches of already-registered titles, one run at a time
(`AGENTS.md`); this replaces "native titles are launched by the user selecting Play".

### Dense-model baseline: decisions, pin and budget

**What is ported from upstream.** `cobanov/PS5LM`'s `ps5/app` runs llama-server
inside a native title with a dashboard, a model planner and a LoadExec restart.
For a first model only the model-load and greedy-generate path is needed, so the
new `model` build mode (`bash scripts/build-native-compute.sh model`) calls
libllama directly from `ps5/native-compute/ggml-model.cpp`: load, one context,
decode a fixed prompt, argmax. No server, dashboard, `compat_app.c`, Linux build,
wildcard stubs, `/data/homebrew` route or `sceSystemServiceLoadExec`. It keeps
our native imports, `sceShellCoreUtilExitApp` close, no-follow setup checks,
write-path guards, closed environment, direct-memory ledger and watchdog.
Two upstream settings were taken: no mmap (`-lm none`, here
`LLAMA_LOAD_MODE_NONE`) and small physical batches. Upstream's
`radv_enable_unified_heap_on_apu` was not: a 0.8B model needs no unified heap.
The `abhi245y/PS5LM` fork notes (one model per launch; its 11.44 GiB heap and
~0.35 GiB screen figures) are references, not limits; this run loads exactly one
model and never unloads and reloads within a launch.

**Model.** `unsloth/Qwen3.5-0.8B-GGUF` at revision
`6ab461498e2023f6e3c1baea90a8f0fe38ab64d0`, file `Qwen3.5-0.8B-Q4_K_M.gguf`,
532,517,120 bytes, SHA-256
`bd258782e35f7f458f8aced1adc053e6e92e89bc735ba3be89d38a06121dc517` (equal to the
Hub's LFS object id). Architecture `qwen35`: 24 layers, one in four full
attention (8 Q / 2 KV heads, head size 256), the rest Gated DeltaNet; tied
embeddings. Tensor types F32, Q4_K, Q5_K, Q6_K, Q8_0. Chosen because it is the
handover's default, it is small, the CPU payload already ran it on this console
family, and upstream ran `qwen35` on RADV. The addendum's preferred Llama 3.2 3B
(plain attention) is the fallback if the DeltaNet ops misbehave; Qwen was
already downloaded, referenced and smoke-tested when the addendum arrived.
Q5_K, Q8_0 and the DeltaNet ops had no operator checks of their own; the
model's token comparison is their first check.
On the console it is `/data/PS5LM/models/Qwen3.5-0.8B-Q4_K_M.gguf`, uploaded by
`scripts/ps5-put-file.py` (staged `.part`, raw readback with `SELF`, rename,
final readback: SHA-256 equal). The incomplete `Qwen3.5-2B-Q4_K_M.gguf.part` is untouched.

**Budget** (`ps5/native-compute/ggml-model.h`). Measured on the host with the
same pin: model buffers 497.39 MiB (GPU) + 198.93 MiB (CPU input embedding
copy) = 696 MiB; KV 12.00 MiB (F16, 1024 tokens, 6 attention layers);
recurrent state 19.27 MiB; compute 30.63 MiB GPU + 0.31 MiB CPU; output
0.95 MiB. Caps, each enforced in the title:

| Part | Cap | Enforced by |
|---|---|---|
| Weights: what the model load adds, including any loader read/staging buffer the heap keeps | 1152 MiB | ledger delta across `llama_model_load_from_file`, else close with `model_weights_budget` |
| Context: KV, recurrent state, outputs, compute buffers | 256 MiB | ledger delta across `llama_init_from_model`, else `model_context_budget` |
| One Vulkan allocation / buffer | 256 MiB (ggml told 240 MiB) | dispatch guard; `GGML_VK_FORCE_MAX_ALLOCATION_SIZE` (from the LM-head fix on, `GGML_VK_FORCE_MAX_BUFFER_SIZE` is 512 MiB: it is only an op-size check, never an allocation; see below) |
| All live Vulkan memory | 1408 MiB | dispatch guard, per allocation, exact per-handle frees |
| Total wrapped direct memory (driver ring, heaps, stacks, all of the above) | **1536 MiB** | the existing ledger, per allocation |

1.5 GiB is an eighth of the queried 12 GiB direct budget, which is not a safe
limit. Run settings: context 1024, batch = ubatch = 32 tokens, one sequence,
two CPU threads, flash attention on (F32 precision, hence the validated FP32
scalar shader), F16 KV, `op_offload` off (CPU weights never stream through the
GPU), no CPU weight repacking, no mmap. Other limits for this mode: watchdog
300 s (the test script waits 600 s), fence and semaphore waits still 2 s, idle
waits still refused, up to 8192 queue submissions of at most 16 batches / 64
command buffers, dispatches up to 65535 groups per dimension. Async compute,
cooperative matrices, fusion and graph optimisation stay off. The loader's
async uploads (1 MiB pinned chunks, timeline-semaphore events) are used, as
upstream's no-mmap server does. `posix_madvise` (only llama's unused mmap path
references it) is a local no-op returning `ENOSYS`.

**Pass rule**, fixed before any GPU run: 32 greedy tokens on the GPU against
the host CPU reference (same pin and patches, CPU backend, no Metal/BLAS/
Accelerate; `scripts/build-host-model-reference.sh`, which writes
`build/native-model/gen/model-reference.h`). Pass: the first 8 tokens are
identical, or the first difference is at a position where the CPU's top two
logits are within 0.5, and the text is coherent. Fail: anything else, NaN or
infinite logits, a run of 8 identical tokens, a llama.cpp error log or an
untracked Vulkan free. The title also requires `offloaded 25/25 layers`. The
top-two logits of every position are logged on both sides. The CPU reference
text is "A GPU (Graphics Processing Unit) is specialized hardware designed to
perform parallel calculations, such as rendering 3D models, playing video games,
or processing massive datasets"; its first token is itself a near tie (gap 0.37).
Evidence: `test-results/local/model-reference/20261015-191628/`. A host smoke
ran the title's own comparison branch with Metal standing in for the GPU:
25/25 offloaded, 2 graph splits, first difference at token 22 (CPU gap 0.244),
rule `prefix`, pass (`test-results/local/model-metal-smoke/20261015-1925/`).
That is a check of the runner, not of the PS5 GPU.

**Hardware result.** Run 3 passed (`test-results/ps5/PPSA99041/20261015-193802/`,
eboot SHA-256 `30f4256a74f54bdc50e551d3a7c1918cada4ec506a4226555c60ccf8fd38a855`):
the GPU's 32 greedy tokens equal the CPU reference's through token 21; at token
22 the GPU chose " graphics" over " models", where the CPU's top two logits are
0.244 apart. Same as the Metal stand-in. Details and memory in
[CONSOLE.md](CONSOLE.md).

What the two failed runs changed, each with its reason:

- Run 1 hit the watchdog during tensor creation. Each write from the title to
  `/data` costs tens of milliseconds, and the llama.cpp log forwarded every
  debug line. The model mode now fully buffers stdout/stderr (1 MiB), flushes
  from the main thread at phase ends (never from the watchdog, which could find
  the stream locked), drops debug lines and the metadata dump, turns off the
  per-dispatch attention diagnostics, and writes timed `PS5LM_MODEL_PHASE`
  markers to the kernel log. A fatal path on the main thread flushes first.
- Run 2 stopped at the context cap: 271 MiB against 256. The Vulkan part was
  67 MiB, as predicted. The rest was attributed with a new ledger record of
  every direct allocation of 4 MiB or more (size, time, caller offset from
  `main`, `PS5LM_MODEL_ALLOC` in the kernel log): one 198 MiB heap block during
  `llama_init_from_model`, which lldb on the host traced to
  `ggml_backend_sched_new`'s context buffer, sized from llama's
  `graph_max_nodes` (generous for `qwen35`). The context cap became 384 MiB;
  the 1536 MiB total did not change. Shrinking that buffer is a possible patch,
  not done.
- `ps5-native-test.sh` reported `no_result` for both: under `set -o pipefail`,
  `klog_new | grep -q` lets grep exit at the first match, `tail` then dies of
  SIGPIPE, and the pipeline counts as failed once the log is long. It now uses
  `klog_has`, which reads the whole log, and escapes `ProcessTerm\(\)` for `-E`.

**LM head on the CPU** (the first explanation, kept for the record; it was
wrong, see "LM head: actual cause and fix" below). ggml-vk's `supports_op` rejects a tensor larger than
the device's `maxStorageBufferRange` (unless 64-bit indexing is available). The
tied output matrix is 198.93 MiB, so llama places it in `Vulkan_Host` memory and
the scheduler runs that matmul on the CPU: 24 layers on the GPU, LM head on two
CPU threads, three graph splits. The in-title `offloaded 25/25` check passes
because llama counts the output layer as assigned; the split count is the
evidence. It is probably also why generation (7.2 tok/s) is slower than the CPU-only
payload measured earlier on the Slim (13.1 tok/s, three threads): one 199 MiB
read per token on two threads. Not measured separately. Fixing it (splitting the matmul, or 64-bit indexing) is the next step
for speed, not for correctness.

### LM head: actual cause and fix (2026-10-15, evening)

**Cause.** Not `maxStorageBufferRange`: the pinned RADV reports `UINT32_MAX`
(`.deps/PS5_Mesa/src/amd/vulkan/radv_physical_device.c:1877`; the 128 MiB value
is the older ps5vk driver's) and `maxBufferSize`/`maxMemoryAllocationSize` are
`0xFFFFFFFC`. The output tensor itself (Q6_K, 1024 x 248320, 208,588,800 B) is
under every limit. What fails is the *probe op* llama builds when it picks the
output layer's buffer type: `weight_buft_supported()` in
`src/llama-model-loader.cpp` asks `ggml_backend_dev_supports_op` about
`ggml_mul_mat(w, b)` with `b` = F32 `[1024, 512]`, so the probe's result is
F32 `[248320, 512]` = 508,559,360 B (485 MiB). ggml-vk's
`tensor_size_supported()` checks the destination as well as the sources against
`device->max_buffer_size`, which the title forced to 251,658,240 (240 MiB) with
`GGML_VK_FORCE_MAX_BUFFER_SIZE`. 485 MiB > 240 MiB, so Vulkan0 is refused and
llama takes the next entry of the output layer's list, the device's host buffer
type `Vulkan_Host`; the scheduler then runs that matmul on the CPU. llama logs
this only at debug level ("cannot be used with preferred buffer type"), which
the title dropped. Upstream PS5LM and the abhi245y fork set neither
`GGML_VK_FORCE_*` variable, so their `max_buffer_size` is RADV's ~4 GiB and the
probe passes. The Metal stand-in had 2 splits for the same reason (no such cap).

**Host reproduction** (no console): a small program on the same pin builds the
exact probe from the GGUF's tensor metadata and applies ggml-vk's size test:
src0 208,588,800 B ok, src1 2,097,152 B ok, dst 508,559,360 B rejected at
240 MiB, accepted at 512 MiB; the real runtime matmul at `n_ubatch` 32 is
31,784,960 B (30.31 MiB), which is exactly the 30.57 MiB `Vulkan_Host` compute
buffer seen on the console. Source: kept in the session scratchpad, not in the
repo; the arithmetic is above.

**Fix** (smallest): `GGML_VK_FORCE_MAX_BUFFER_SIZE` 240 MiB -> 512 MiB
(536,870,912) in `ps5/native-compute/main.c`. In this pin `max_buffer_size` is
read in exactly one place, `ggml_backend_vk_device_supports_op`'s size test; it
never sizes an allocation. Allocations stay capped by
`GGML_VK_FORCE_MAX_ALLOCATION_SIZE` (240 MiB, unchanged) and by the dispatch
guard's 256 MiB per allocation and 1408 MiB total. 512 MiB covers a 262,144-token
vocabulary probe at hidden size 1024 or less; a larger probe would fall back to
`Vulkan_Host` again, safely. No llama.cpp source change. Diagnostics added for
the console run: `patches/ggml-vulkan/0005-native-device-limits-log.patch`
prints one `PS5LM_VK_LIMITS` line at device init (ggml's allocation/buffer
limits, the driver's `maxStorageBufferRange`, `maxMemoryAllocationSize`,
`maxBufferSize`, 64-bit indexing, int64, BDA, UMA); the model log filter keeps
llama's "cannot be used with preferred buffer type" line, and the summary
reports `host_model_buffer_mib` and `weights_moved_lines`.

**Budget after the fix**, written before the run. The 198.93 MiB tied output
moves from `Vulkan_Host` (pinned host memory, already a Vulkan allocation in the
ledger) to `Vulkan0`; both are direct memory and both are counted by the
dispatch guard, so the totals do not change: model buffers 497.40 MiB Vulkan0 +
198.93 MiB CPU (input embedding for GET_ROWS) = 696 MiB, in allocations of at
most 240 MiB (the 199 MiB tensor alone in one); the 30.3 MiB logits buffer moves
from the `Vulkan_Host` compute buffer to the `Vulkan0` one. The previous run
showed the weight upload needs no staging buffer (Vulkan live after load equals
the model buffers exactly, so device memory is host-visible here). Caps unchanged:
weights 1152 MiB, context 384 MiB, one allocation 256 MiB, Vulkan 1408 MiB,
total 1536 MiB. Expected: `splits = 2` (CPU GET_ROWS of the input embedding,
then everything on Vulkan0), no `CPU model buffer` line.

**Correction about where the head was.** Both the input and the output layer
fall back through the same list (`Vulkan_Host`, then `CPU`), and llama tests
the `Vulkan_Host` entry with the Vulkan device's `supports_op`. The input
embedding's GET_ROWS probe passes there, so the input sat in `Vulkan_Host`; the
output's MUL_MAT probe failed there too, so the LM head sat in the plain `CPU`
buffer. Same effect (CPU matmul), different buffer from what was written above.
The summary field `host_model_buffer_mib` therefore reports the input
embedding (198.93 MiB, as intended: GET_ROWS stays on the CPU).

**Second limit found on the console** (run `20261015-195314`, safe stop): with
the head on Vulkan0 the title's dispatch guard (`GROUP_DIM_LIMIT` 65535 per
dimension, unchanged) refused a dispatch. RADV reports
`maxComputeWorkGroupCount` = {2^32-1, 65535, 65535}, and ggml-vk only splits a
dispatch when it exceeds that reported limit, so the 248320-row mat-vec went out
in one x dimension. `patches/ggml-vulkan/0006-native-dispatch-group-limit.patch`
lowers ggml's copy of the x limit to 65535 on `__PROSPERO__` only, which turns on
ggml's existing splits (here: 64 z slices of 3880 rows); the guard now also logs
`PS5LM_DISPATCH_LIMIT` with x/y/z if it ever fires. The driver and the guard are
unchanged.

**Result** (`20261015-195528`, repeat `20261015-195622`, eboot
`cd39599d3edb14c77bfb7c10a542a97837dc4f5df6b3b8d1d7544ec3a51a6f82`): pass, the
same 32 tokens as the CPU-head runs, 2 splits, generation 140 tok/s (was 7.2),
prompt 99 tok/s (was 69), memory within every cap (see CONSOLE.md).


### Small MoE: decision, pin and budget (2026-10-15, evening)

Approved by the user through the coordinator this session (one console run and
one repeat after the LM-head fix passed). **Model:**
`bartowski/granite-3.1-1b-a400m-instruct-GGUF` at revision
`940d2e1f9f65330615c7c8e980e6c5ac73d3360c`, file
`granite-3.1-1b-a400m-instruct-Q4_K_M.gguf`, 821,847,360 bytes, SHA-256
`3a2ec1c2a78cb29d901e29bbf5162dcd03381e13803d2cbdcff838d4d08142eb` (equal to the
Hub's LFS id at that revision and to the local file). Upstream publisher
`ibm-granite`; bartowski is a well-known quantizer. Architecture `granitemoe`:
24 layers, 32 experts with 8 active, plain GQA attention (16 Q / 8 KV heads,
head size 64), hidden 1024, expert FFN 512, vocabulary 49,155, tied embeddings
(token_embd Q6_K 39.38 MiB). Expert tensors Q4_K (gate/up) and Q6_K (down).
Chosen because ROADMAP already pinned it, it is the smallest real MoE on the
plan, and its attention is the ordinary kind. On the console:
`/data/PS5LM/models/granite-3.1-1b-a400m-instruct-Q4_K_M.gguf`, uploaded with
`scripts/ps5-put-file.py` (raw `SELF` readback SHA-256 equal;
`test-results/local/model-upload/`).

**No separate MUL_MAT_ID operator run.** ROADMAP suggested one at the real
expert shapes first. Not done, to keep within the run limit: Q4_K `MUL_MAT_ID`
already passed on this GPU at a small shape, the token comparison below
exercises the real shapes in every layer, and a wrong expert result shows up as
a token mismatch (a safe failure). If the model fails the rule, an operator
check is the next step before any second model run.

**Build.** `bash scripts/build-native-compute.sh moe` is the model mode with
`PS5LM_MODEL_MOE` (model file, size check, prompt); everything else (settings,
caps, guards, watchdog, close) is the same code. CPU reference:
`PS5LM_MODEL_VARIANT=moe scripts/build-host-model-reference.sh` writes
`build/native-moe/gen/model-reference.h`. Prompt: the same question in Granite's
chat format, user turn only, no system turn. Reference text (host CPU,
`test-results/local/model-reference/20261015-195935/`): "A GPU (Graphics
Processing Unit) and a CPU (Central Processing Unit) are fundamentally different
in their architecture, purpose, and performance characteristics. GPUs". Its
token 1 is a near tie (CPU gap 0.213). Metal stand-in smoke of the title's
comparison branch: 25/25 offloaded, 2 splits, first difference at token 1 (the
near tie), rule `near_tie`, pass, coherent
(`test-results/local/model-metal-smoke/moe-*/`). A check of the runner, not of
the PS5 GPU.

**Budget**, written before the first load. Host measurements: model buffers
782.12 MiB GPU + 39.38 MiB input embedding (in `Vulkan_Host`, GET_ROWS on the
CPU) = 821.5 MiB; KV 48.00 MiB (F16, 1024 tokens, 24 layers, 8 KV heads x 64);
compute 7.69 MiB GPU + 0.31 MiB CPU; output 0.19 MiB. With the ~68 MiB of
loader transients seen on the dense model, the load should add about 890 MiB;
the context about 56 MiB of Vulkan memory plus llama's graph-scheduler
bookkeeping (198 MiB for `qwen35`, sized by node count; expected smaller here).
Caps are **unchanged** and enforced as for the dense model: weights 1152 MiB,
context 384 MiB, one allocation 256 MiB (ggml 240 MiB), all Vulkan memory
1408 MiB, total wrapped direct memory **1536 MiB** (an eighth of the 12 GiB
direct budget). Same run settings: context 1024, batch = ubatch = 32, one
sequence, two CPU threads, flash attention (FP32), F16 KV, no op offload, no
mmap, watchdog 300 s. Largest dispatch: the 49,155-row LM head, below the
65535-group limit without splitting. Pass rule: identical to the dense model's.

**Hardware result.** A first run failed safely on a build bug (the MoE flag
reached `main.c` but not `ggml-model.cpp`, so the title loaded Qwen and
compared it with the Granite reference); `build-native-compute.sh` now passes
the same flags to both and refuses a package whose objects name different model
files. The fixed build passed (`test-results/ps5/PPSA99041/20261015-200238/`,
repeat `…/20261015-200310/`, eboot
`0c91055b4f425d93dce548dcc850a2273e81afe9596eae7969cb32037996890a`): rule
`near_tie` at token 1 (CPU gap 0.213), coherent text, no NaN, 2 splits, every
expert and the LM head on `Vulkan0`, memory within each cap (load +826 MiB,
context +93 MiB, direct peak 991 MiB, Vulkan peak 876 MiB), 160 tok/s generation.
Because the match is a near tie at token 1 rather than an 8-token prefix, the
CPU reference was rerun with the GPU's token 1 forced: the CPU then produces the
GPU's next 24 tokens exactly, and their next difference is again a near tie
(0.127) (`test-results/local/moe-forced-continuation/20261015-200354/`). So the
expert path (`MUL_MAT_ID`, Q4_K and Q6_K) agrees with the CPU at real shapes.
Granite's graph-scheduler buffer is small (context +93 MiB in total), unlike
`qwen35`'s 198 MiB.

### LAN API: server mode, lifetime and budget (2026-10-15, night)

Written before the first server run.

**What runs.** Build modes `server` (Granite 3.1 1B-A400M) and `server-qwen`
(Qwen3.5 0.8B) of PPSA99041 run llama.cpp's own server, `llama_server()` from
`tools/server/server.cpp` at the same pin, on a 16 MiB-stack thread inside the
title (`ps5/native-compute/ggml-server.cpp`). OpenAI-compatible
`/v1/chat/completions`, `/v1/completions`, `/v1/models`, plus `/health`,
`/completion`, `/tokenize` and `/props`, on port 8081. Everything else is the
model mode: the same model file and size check, `main.c`'s ledger, write-path
guards, closed environment, dispatch guard, watchdog and
`sceShellCoreUtilExitApp` close.

**Taken from upstream, selectively.** From `cobanov/PS5LM`: the idea of calling
`llama_server()` in the title (`ps5/app/main.cpp`), and from its
`compat_app.c` only what our link reported undefined (`ps5/native-compute/net-compat.c`:
a numeric `getaddrinfo`/`freeaddrinfo`, numeric `getnameinfo`, `accept4`,
`isatty`, `getpwuid`, `timegm`, `pathconf`, and refusing `readlink`, `link`,
`symlink`, plus a refusing `remove`), each bound as `ps5lm_NAME` with a local
version-script entry like `mkstemp`, never exported. The SDK's own
`getaddrinfo` binding is dropped for this mode only: it refuses every lookup and
cpp-httplib binds through it. From the `abhi245y/PS5LM` fork:
`patches/server/0002-listener-survives-network-drops.patch` (unchanged). Not
taken: the dashboard, model planner, `/data/homebrew`, `LoadExec`, etaHEN,
the web UI (`--no-webui`, built with `LLAMA_BUILD_UI=OFF`,
`LLAMA_USE_PREBUILT_UI=OFF`, no download at build time), subprocesses
(`LLAMA_SUBPROCESS=OFF`), TLS. The validator still requires every import to come
from the six declared modules; the server adds 35 (sockets from `libkernel`,
a few libc functions), 321 in all.

**Our patch.** `patches/server/0001-ps5lm-server-hooks.patch` (only under
`__PROSPERO__`, or `PS5LM_SERVER_HOOKS` for the Mac rehearsal): every response
through llama's `ex_wrapper` is counted by status, `POST /ps5lm/shutdown` asks
the title to end the window, and the title is told once `shutdown_handler` is
set (after the model loads), the first moment `llama_server_terminate()` works.

**Lifetime, bounded.** The title's main thread owns it (`ggml-server.h`):
the model must be loaded within 120 s; the server then serves for **600 s** or
until `POST /ps5lm/shutdown`, whichever comes first; the main thread then calls
`llama_server_terminate()`, which stops the HTTP listener and the server loop,
frees the context and model, and returns; this must finish within 60 s. Missing
either deadline closes the title through `ps5lm_ops_fatal` (the usual
`PS5LM_COMPUTE_RESULT` with `success:false` and the close). The watchdog is
120 + 600 + 60 + 60 = **840 s**, so `ps5-native-test.sh` gets `-t 960`. After
the stop the title prints `PS5LM_SERVER_SUMMARY` (responses, errors, stop
reason, load/ready/served times, weights/context/peak memory, Vulkan live after
free) and one `PS5LM_COMPUTE_RESULT` line, whose server fields are
`http_responses`, `http_errors`, `served_ms` and `stop_reason`. Success needs
the server to return 0, a stop by window or request, at least one response, no
response with status 400 or above and no untracked Vulkan free. The listener
binds `0.0.0.0:8081` (the console has one LAN interface) only for that window:
it is created after the title starts and closed before the result line. The
API is unauthenticated, so it is never left running: no autoload, no restart.

**Settings** (all passed explicitly and checked by link-time wraps of
`llama_model_load_from_file` and `llama_init_from_model`, which also allow
exactly one model load and one context per launch): `-np 1`, `-c 2048` (the
512-token benchmark prompt plus 128 generated tokens fit with room; at most
4096 was allowed), `-b 32 -ub 32` (the model mode's batches, so every submit
stays as small as in the passed runs), `-t 2 -tb 2`, `-fa on`, F16 K and V,
`--no-op-offload`, `--no-repack`, `-lm none` (no mmap), `-lzm off`, `-fit off`
(no automatic resizing from the queried device memory), `--cache-ram 0` (the
server's default keeps up to 8 GiB of prompt cache in host memory),
`--ctx-checkpoints 0` (32 by default; each would copy Qwen's 19 MiB recurrent
state), `--threads-http 4`, `--no-webui`. Queue submissions: 1,048,576 over the
launch instead of the model mode's 8192 (about 13 per decoded token; still a
hard cap, the window is the liveness rule).

**Budget**, from the model-mode measurements; caps unchanged and enforced as in
the model mode:

| Part | Granite (server) | Qwen3.5 0.8B (server-qwen) | Cap | Enforced by |
|---|---|---|---|---|
| Before the load (driver, heaps, server thread stack, HTTP threads) | ~60 MiB | ~60 MiB | in the total | ledger |
| Weights (what the load adds) | ~826 MiB | ~764 MiB | 1152 MiB | wrap of `llama_model_load_from_file`, else `model_weights_budget` |
| Context (KV at 2048, compute, scheduler) | ~141 MiB (KV 96) | ~283 MiB (KV 24, recurrent 19, scheduler 198) | 384 MiB | wrap of `llama_init_from_model`, else `model_context_budget` |
| Serving scratch (requests, sampler, logs) | < 64 MiB | < 64 MiB | in the total | ledger |
| All Vulkan memory | ~920 MiB | ~800 MiB | 1536 MiB | dispatch guard |
| **Total direct memory** | **~1.1 GiB** | **~1.2 GiB** | **1536 MiB** | ledger, per allocation |

1536 MiB stays an eighth of the 12 GiB direct budget; no reason to change it.

**Checked on the Mac first** (`scripts/host-server-rehearsal.sh`, same pin and
patches, CPU backend, the same `ggml-server.cpp` with `PS5LM_HOST_REHEARSAL`):
the arguments parse into exactly the checked settings, the API reproduces the
CPU reference's 32 tokens (rule `identical`), chat, streaming and a second
request work, and both stops (window and `/ps5lm/shutdown`) end in
`PS5LM_SERVER_SUMMARY` with `pass:true`. That checks the runner, not the
console.

**Correctness rule through the API** (`scripts/ps5-api-check.py`): `/tokenize`
of the model mode's prompt must give the model mode's prompt tokens; a
temperature-0 `/completion` of 32 tokens must equal the model mode's GPU tokens
or pass the model mode's rule against the CPU reference; `/v1/completions` must
give the same text; then a chat request, a streaming chat request, the same
completion again (identical tokens) and `/health`.

**Result.** Run 1 passed on the console (`test-results/ps5/PPSA99041/20261015-203449/`,
eboot `2303fcb0841d24da93b83f714f58a827759d46f87b8cbb826ea334ab6de22eaf`):
the API's completion is identical to the model mode's GPU tokens, all checks
pass, memory as budgeted (load +846 MiB, context +136 MiB, peak 1084 MiB of
1536), Vulkan memory 0 after the server returned, closed through the verified
route after a `/ps5lm/shutdown`. The unchanged repeat matched. Numbers in
CONSOLE.md.

**Placement check added** (after runs 1 and 2). The server's logger hides
llama's load lines at its default verbosity, so the load wrap now puts a tap in
front of it (`llama_log_get`/`llama_log_set`; everything is still forwarded)
that reads `offloaded N/M` and the graph splits as the model mode does, closes
the title with `model_not_fully_offloaded` unless all layers and the output
layer are on the GPU, and reports both in `PS5LM_SERVER_SUMMARY`. Qwen3.5 0.8B
(`server-qwen`, run 3) then passed with 25/25 and 2 splits and tokens identical
to its model mode; Granite with the same build (run 4) too. Qwen's context took
+253 MiB of its 384 MiB cap, as budgeted.

**Both stops verified on the console:** `POST /ps5lm/shutdown` (runs 1-4) and
the 600 s window without any request (run 5: stopped at 600.03 s of serving,
closed 609 s after launch).

**Observation, not yet a finding.** Both models generate at about 210 tok/s
through the server although Qwen3.5 0.8B and Granite's active 400M parameters
differ in shape and size, and prompt processing differs (1.4k vs 1.9k tok/s).
A per-token cost that does not depend on the model (about 13 queue
submissions per token, each waited on; async compute, fusion and graph
optimisation are off in this title) would explain it. Not measured yet.

### Larger MoE models: the climb, pins and budgets (2026-10-15, late)

**Rule** (AGENTS.md): the memory a title holds grows in steps, each at most
double the largest total that passed and closed cleanly, each written here
before its first run, keeping at least 1 GiB of the ~11.4 GiB device memory
RADV reports free (so no total above 10.4 GiB, 10,649 MiB). Any failure, hang
or unclosed title ends the climb at the last passing size. The last passing
total before this climb: 1536 MiB (Granite 3.1 1B-A400M, Qwen3.5 0.8B).

**Ladder**, chosen for architectures the pinned llama.cpp supports and quant
types close to the checked ones (Q4_K, Q5_K, Q6_K, Q8_0):

| Step | Total cap | Model (architecture) | File | Quant types | New on this GPU |
|---|---|---|---|---|---|
| 1 | 3072 MiB | Granite 3.1 3B-A800M Instruct (`granitemoe`, 40 experts / 8 active, 32 layers) | 2,016,888,384 B | Q4_K, Q6_K | only shapes |
| 2 | 6144 MiB | OLMoE-1B-7B-0125-Instruct (`olmoe`, 64 experts / 8 active, 16 layers, Q/K norm, no GQA) | 4,213,512,192 B | Q4_K, Q6_K | the `olmoe` graph |
| 3 | 10560 MiB | Qwen3-30B-A3B-Instruct-2507 (`qwen3moe`, 128 experts / 8 active, 48 layers, GQA 32/4) | 9,870,270,464 B | IQ2_XS, IQ3_XXS, IQ2_S, IQ3_S, Q4_K, Q5_K | IQ2/IQ3 matmul and `MUL_MAT_ID` shaders |

Step 3 is capped by the device memory, not by doubling (2 x 6144 = 12,288).
Untested ops are checked by the token comparison, as for the first models: a
wrong expert or quant kernel shows as a token mismatch, a safe failure. Not
chosen: gpt-oss 20B (10.7 GiB in every quant, leaves under 1 GiB), Qwen3.6
35B-A3B (smallest useful quant ~10 GiB), Granite 4.0 H Tiny and LFM2-8B-A1B
(Mamba/short-conv ops never checked here), DeepSeek-V2-Lite (MLA attention).

**Pins** (sizes and SHA-256 equal to the Hub's LFS ids at these revisions and
to the downloaded files; Mac copies in `models/`, console copies in
`/data/PS5LM/models/`, uploaded with `scripts/ps5-put-file.py`: staged `.part`,
raw `SELF` readback SHA-256, rename, final readback):

| Model | Repo @ revision | File | Bytes | SHA-256 |
|---|---|---|---|---|
| Granite 3.1 3B-A800M | `bartowski/granite-3.1-3b-a800m-instruct-GGUF` @ `be9a36f042806cb586bc65556c527079782b78e0` | `granite-3.1-3b-a800m-instruct-Q4_K_M.gguf` | 2,016,888,384 | `48e0edcd578fd4462f26127f04c651d0e650741110185297741089aea01a82b3` |
| OLMoE 1B-7B 0125 | `allenai/OLMoE-1B-7B-0125-Instruct-GGUF` @ `2ac1d27317927518c9ef7bd99f91f3f1e4ee288d` | `OLMoE-1B-7B-0125-Instruct-Q4_K_M.gguf` | 4,213,512,192 | `4ddc0e53159ed512b8dd67914a66e27bc618f694672ba43a9a0454eabd9c684f` |
| Qwen3 30B-A3B 2507 | `bartowski/Qwen_Qwen3-30B-A3B-Instruct-2507-GGUF` @ `6c6e8692f43e4ca663f7ece8229a1361090d3a4c` | `Qwen_Qwen3-30B-A3B-Instruct-2507-IQ2_M.gguf` | 9,870,270,464 | `f2dc78edd3ec0171904f1945d8c05a948131b1103172b1710b763db2eb65f52a` |

Publishers: `ibm-granite` (quantized by bartowski), `allenai` (its own GGUF),
`Qwen` (quantized by bartowski with an imatrix).

**Build.** Each step has a model mode and a server mode
(`bash scripts/build-native-compute.sh moe3b|olmoe|qwen3moe` and
`server-moe3b|server-olmoe|server-qwen3moe`), selected by one define
(`PS5LM_MODEL_GRANITE3B`, `PS5LM_MODEL_OLMOE`, `PS5LM_MODEL_QWEN3MOE`) in
`ps5/native-compute/ggml-model.h`, which also holds that model's caps, its
queue-submission cap, its model-mode watchdog and its server load deadline.
The build refuses a package whose objects name any other model file. The CPU
reference is `PS5LM_MODEL_VARIANT=<variant> scripts/build-host-model-reference.sh`;
the API check takes `--variant <variant> --model-run <model-mode run>`. Run
settings, the pass rule, the API rule and every other limit are the earlier
models' (context 1024 in model mode, 2048 in server mode, batch = ubatch = 32,
two threads, FP32 flash attention, F16 KV, no op offload, no mmap, one model
load and one context per launch, all layers and the output on the GPU). The
first two models build byte-identically to before (`moe` rebuilt: `0c91055b…890a`).

**What scales with size.** The weights cap, the context cap, the total cap
(the direct-memory ledger, which has one 64 KiB slot per unit of the total)
and the Vulkan cap (weights + context) per model. Queue submissions: the
loader uploads in 1 MiB chunks, about one submission each (Granite 1B: 918
after loading 826 MiB), so the model mode's 8192 becomes about the weights in
MiB plus 8192. Load time: the console read Granite 1B at about 90 MB/s
(9.1 s for 821 MB; Qwen 0.8B came from cache), so deadlines assume 80 MB/s
and double it. One Vulkan allocation stays at most 256 MiB (ggml 240 MiB):
the largest single tensor of all three is Qwen3's 204 MiB Q5_K output.

#### Step 1: Granite 3.1 3B-A800M, total 3072 MiB

Host CPU reference (same pin and patches;
`test-results/local/model-reference/20261015-210244/`): model buffer 1921.79 MiB,
KV 64.00 MiB (1024 cells, 32 layers, F16), compute 6.38 MiB; text "A GPU
(Graphics Processing Unit) and a CPU (Central Processing Unit) are both
essential components in a computer system, but they serve distinct purposes
and have different"; prompt 22 tokens; smallest top-two gap 0.105 at token 20.

| Part | Expected | Cap | Enforced by |
|---|---|---|---|
| Before the load | ~70 MiB | in the total | ledger |
| Weights (what the load adds) | ~1960 MiB | 2304 MiB | ledger delta across the load (model mode) / its wrap (server), else `model_weights_budget` |
| Context, model mode (KV 64 + compute + scheduler) | ~110 MiB | 384 MiB | ledger delta across `llama_init_from_model`, else `model_context_budget` |
| Context, server mode (KV 128 at 2048) | ~180 MiB | 384 MiB | same |
| All Vulkan memory | ~2.1 GiB | 2688 MiB | dispatch guard, per allocation |
| **Total direct memory** | **~2.2 GiB** | **3072 MiB** | ledger, per allocation |

Queue submissions in the model mode: 12,288 (expected ~2,500). Watchdog 300 s
(load expected ~25 s); server load deadline 120 s, window 600 s, watchdog 840 s.

**Result: pass** (console runs 1 and 2 of this climb; details in CONSOLE.md).
Model mode: rule `prefix` (first 20 tokens equal to the CPU, the difference at
the 0.105 near tie), 33/33 offloaded, 2 splits, load 21.3 s, weights +1986 MiB,
context +123 MiB, direct peak 2181 MiB, 2626 submissions. Server mode: API
check 9/9 with tokens identical to the model mode, weights +2005 MiB, context
+184 MiB, peak 2294 MiB; 711/709 tok/s prompt, 149.5/146.4 tok/s generation.
Largest passing total is now **3072 MiB**, so step 2 may be at most 6144 MiB.

#### Step 2: OLMoE-1B-7B-0125-Instruct, total 6144 MiB

Written before its first run. Host CPU reference
(`test-results/local/model-reference/20261015-210636/`): model buffer
4016.62 MiB (untied output 80.60 MiB Q6_K; input embedding 55.27 MiB Q4_K,
expected in `Vulkan_Host`), KV 128.00 MiB (1024 cells, 16 layers, 16 KV heads
x 128, F16), compute 6.64 MiB; text "A GPU (Graphics Processing Unit) and a CPU
(Central Processing Unit) are both essential components in computers, but they
serve different purposes and are designed with distinct"; prompt 27 tokens
(OLMoE's template: `<|endoftext|><|user|>\n…\n<|assistant|>\n`; its `<|user|>`
markers are plain text in this vocabulary); smallest top-two gap 0.241 at
token 25, none under 1.19 in the first 8.

| Part | Expected | Cap | Enforced by |
|---|---|---|---|
| Before the load | ~70 MiB | in the total | ledger |
| Weights | ~4030 MiB | 4608 MiB | as step 1 |
| Context, model mode (KV 128) | ~180 MiB | 512 MiB | as step 1 |
| Context, server mode (KV 256 at 2048) | ~310 MiB | 512 MiB | as step 1 |
| All Vulkan memory | ~4.3 GiB | 5120 MiB | dispatch guard |
| **Total direct memory** | **~4.4 GiB** | **6144 MiB** | ledger |

Queue submissions in the model mode: 16,384 (expected ~4,700). Load at the
measured ~95 MB/s: ~45 s; model-mode watchdog 300 s, server load deadline
180 s (watchdog 900 s, so `ps5-native-test.sh -t 1020`).

**Result: pass** (console runs 3 and 4). Model mode: rule `prefix` (first 25
tokens equal, then the 0.241 near tie), 17/17, 2 splits, load 45.6 s, weights
+4022 MiB, context +166 MiB, direct peak 4257 MiB, 4437 submissions. Server
mode: 9/9, tokens identical to the model mode, weights +4042 MiB, context
+292 MiB, peak 4432 MiB; 614/571 tok/s prompt, 232/223 tok/s generation.
Largest passing total is now **6144 MiB**; step 3 may be at most 12,288 MiB,
and the device-memory rule holds it to 10,649 MiB.

#### Step 3: Qwen3-30B-A3B-Instruct-2507 IQ2_M, total 10560 MiB

Written before its first run. Chosen over the IQ2_S (8.14 GiB) and IQ2_XXS
(7.05 GiB) files of the same repo because it is the largest that keeps the
measured overheads well inside 10,649 MiB; the 10.16 GiB Q2_K would not.
Host CPU reference (`test-results/local/model-reference/20261015-210644/`):
model buffer 9407.33 MiB (untied output 204.02 MiB Q5_K, the largest tensor;
input embedding 127.51 MiB IQ3_S, expected in `Vulkan_Host`; experts IQ2_XS
(6160 MiB) and IQ3_XXS (2426 MiB), attention IQ2_S/Q4_K/Q5_K), KV 96.00 MiB
(1024 cells, 48 layers, 4 KV heads x 128, F16), compute 19.05 MiB, graph 3030
nodes; text "A GPU (Graphics Processing Unit) and a CPU (Central Processing
Unit) differ primarily in their design and intended use. A CPU is optimized
for general-purpose tasks"; prompt 22 tokens; first-8 gaps all above 1.4,
smallest 0.160 at token 27. New on this GPU: the IQ2_XS, IQ2_S, IQ3_XXS and
IQ3_S matmul and `MUL_MAT_ID` shaders (all in the validated SPIR-V set; never
run here), 128 experts, a 151,936-row LM head (split by patch 0006).

Overheads measured in steps 1 and 2: the load adds the model buffers plus
5 MiB (model mode) or 25 MiB (server); the context adds KV plus 36–59 MiB;
about 90 MiB is live before the load; serving adds about 15 MiB.

| Part | Expected | Cap | Enforced by |
|---|---|---|---|
| Before the load | ~90 MiB | in the total | ledger |
| Weights | ~9415 MiB (model), ~9435 (server) | 9728 MiB | as step 1 |
| Context, model mode (KV 96, compute ~19) | ~200 MiB | 768 MiB | as step 1 |
| Context, server mode (KV 192 at 2048) | ~300 MiB | 768 MiB | as step 1 |
| All Vulkan memory | ~9.5 GiB | 10496 MiB | dispatch guard |
| **Total direct memory** | **~9.6 GiB** (9,850 MiB) | **10560 MiB** | ledger |

10,560 MiB leaves 1,114 MiB of RADV's ~11.4 GiB (11,674 MiB) free even at the
cap; the expected peak leaves about 1.8 GiB. Queue submissions in the model
mode: 24,576 (expected ~10,000: one per MiB uploaded). Load at the measured
~92 MB/s: ~107 s; model-mode watchdog 480 s (`-t 600`), server load deadline
300 s (watchdog 1020 s, `-t 1140`). If this step fails in any way the climb
ends at 6144 MiB (OLMoE).

**Result: fail by the pass rule** (console run 5 of this climb). The GPU's
first difference from the CPU is at token 7, where the CPU's top two logits are
1.415 apart (the GPU's 0.154): neither an 8-token prefix nor a near tie. The
text is coherent, and the run was otherwise clean: 49/49 on the GPU, load
106.8 s, weights +9447 MiB, context +205 MiB, direct peak 9723 MiB of 10560,
Vulkan 0 after free, closed. Offline: forcing the GPU's first 8 tokens into the
CPU run reproduces all 24 later GPU tokens; Metal's logits differ from the
CPU's by as much as the PS5's. Most likely IQ2 rounding at a decision boundary,
not a kernel error, but the rule stands, so **the climb ends at 6144 MiB with
OLMoE-1B-7B-0125** (repeat run identical). The model file stays on the console
for the next phase.

**Proposals for the user** (not done): (a) a rule for low-bit quants that also
accepts a first difference where the *GPU's* top two are within 0.5 and the
CPU, forced with the GPU's token, reproduces the GPU's continuation (both true
here); (b) the next step from 6144 MiB with a model at 6–6.5 GiB total, e.g.
Qwen3-30B-A3B IQ2_XXS (7.05 GiB, would need about 7.6 GiB: under the 12,288
doubling, over 6144 so it is a new step) or a 4–5 bit 7–8B MoE, to find a
`qwen3moe` file that passes before returning to ~9.6 GiB; (c) an operator
check of the IQ2_XS/IQ3_XXS `MUL_MAT_ID` and `MUL_MAT` shaders against the CPU
at the model's shapes, to put a number on their error.

**Performance notes for the next phase** (observed, not acted on): generation
falls with the layer count more than with the active parameters (16-layer
OLMoE 232 tok/s, 24-layer models ~212, 32-layer Granite 3B 150, 48-layer
Qwen3-30B 37 on the title's clock), which fits a fixed cost per layer
(submissions and waits; fusion, graph optimisation and async compute are off);
prompt processing at ubatch 32 is 570–710 tok/s for the 2–4 GB models and
22 tok/s for the IQ2 30B (`MUL_MAT_ID` with IQ types at 32 tokens); loading
reads ~92 MB/s from `/data` (107 s for 9.9 GB), so larger or parallel reads
would shorten big loads.

### Performance: measured tuning (2026-10-15, night)

**Baseline** (the builds on record, unchanged since; 128-token prompt, medians of
3 after a warm-up, `scripts/ps5-api-bench.py`): Qwen3.5 0.8B pp 1744 / tg 214.0
tok/s (`test-results/ps5/api-bench/20261015-204241/`), OLMoE-1B-7B pp 614.5 /
tg 232.4 (`…/20261015-212407/`), Granite 3.1 1B-A400M pp 1370 / tg 212.5
(`…/20261015-204434/`), Granite 3.1 3B-A800M pp 711 / tg 149.5
(`…/20261015-211113/`).

**What the earlier logs already say.** The PS5 winsys prints its submission
times every 10 s of a submitting queue (`radv/ps5 submissions: window …` in
the title's output). During the OLMoE benchmark: 5103 submissions in a window,
1467 ms inside `sceAgcDriverSubmitDcb` on the submitting thread (0.29 ms each)
and 1998 ms in `sceAgcSuspendPoint` on the winsys's kick thread (0.39 ms
each); Qwen: 2842 submissions, 302 ms and 581 ms. At about 13 submissions per
decoded token (934-node OLMoE graph: one every 100 nodes, plus the flop-based
split and the almost-ready fence), submissions alone could cost 1.4–3.7 ms of
a 4.3–4.7 ms token. That is the first thing to measure.

**Instrumentation** (no change to what runs): the dispatch guard
(`ggml-dispatch.c`) counts dispatches and times every `vkQueueSubmit`,
fence wait and semaphore wait (one monotonic clock read on each side); patch
`ggml-vulkan/0007` times each `graph_compute` (including its final
synchronise, since async is off). Server mode serves them at
`GET /ps5lm/stats`.

**Runtime knobs, so one serving window measures many settings.** Patch
`ggml-vulkan/0007` latches, at the start of every graph on the computing
thread, `disable_fusion` (with the ADD+RMS_NORM partials), the nodes per
submit and the flop-based submit split (0 = off), reads
`disable_graph_optimize` when the scheduler optimises a graph, and the
mat-vec workgroup size (`dmmv_wg`: 0 subgroup, 1 large; ProsperoAI's
`GGML_VK_PS5_DMMV_WG`) at each pipeline choice. -1 keeps what device creation
chose from the closed environment (fusion and graph optimisation off, 100
nodes, the default split, subgroup workgroups). Patch `server/0003` adds
`POST /ps5lm/knobs` (every name and range checked by the title; a bad one is
a 400, which fails the run) and a prompt-chunk knob (`prompt_batch`, 1–512):
the server's context is now created for 512-token physical batches, and the
chunk it decodes at a time is the knob, 32 by default (the old setting, so the
default computes exactly the old graphs). Not made runtime knobs: ProsperoAI's
`RM_STDQ`/`RM_KQ`/subgroup size (pipeline specialisation, fixed when the
pipeline is defined at device creation), CPU threads (the only CPU node is the
input-embedding `GET_ROWS` from `Vulkan_Host`; nothing else runs on the CPU).
`scripts/ps5-api-tune.py` runs a fixed adaptive plan in one window: baseline;
four submit cadences; the large mat-vec workgroup; prompt chunks 64/128/256/512;
fusion; graph optimisation; both; the best correct combination; the baseline
again. Every configuration first passes the correctness check (32
temperature-0 tokens of the model mode's prompt equal to the model mode's GPU
tokens, or the model mode's rule against the CPU reference) or is never
chosen. The model mode's prompt is shorter than 32 tokens, so prompt chunks
do not change it; for them the 512-token benchmark's 128 generated tokens are
also compared across chunks.

**Fence timeout and watchdog unchanged:** every fence and semaphore wait is
still at most 2 s. A 512-token OLMoE graph is estimated at 0.2–0.9 s (the
512-token prompt took 0.9 s as 16 graphs of 32); the knobs cannot make one
submission larger than a graph. Device-lost watchdog, idle-wait refusal and
the serving window are unchanged.

**Budget for the 512-token context** (written before the first run).
Measured on the Mac with the same pin (Metal stand-in, n_ctx 2048,
n_outputs_max 1 as the server sets it): Qwen3.5 0.8B compute buffers 13.8 →
42.2 MiB (GPU) and 0.4 → 6.0 MiB (CPU); OLMoE 33.5 → 74.7 MiB and 0.6 →
10.0 MiB. ggml-vk's preallocated conversion buffers grow with the batch too
(allocated at the first graph, counted in the total and Vulkan caps, not the
context cap); estimated under 64 MiB.

| Part | Qwen3.5 0.8B (server-qwen) | OLMoE (server-olmoe) | Enforced by |
|---|---|---|---|
| Weights | ~764 MiB, cap 1152 (unchanged) | ~4042 MiB, cap 4608 (unchanged) | wrap of the load |
| Context | ~320 MiB, cap **512** (was 384) | ~340 MiB, cap **768** (was 512) | wrap of the context |
| All Vulkan memory | ~0.85 GiB, cap 1664 MiB | ~4.4 GiB, cap 5376 MiB | dispatch guard |
| **Total direct memory** | **~1.3 GiB, cap 1536 MiB (unchanged)** | **~4.5 GiB, cap 6144 MiB (unchanged)** | ledger |

No total grows, so the stepped rule's largest passing total (6144 MiB) is not
touched. The model modes keep batch 32 and get the same larger context caps
(an upper bound only).

**Storage probe** (for SSD weight streaming), server mode only, after the
server has stopped and freed: the pinned model file, read only, regions of
96 MiB from its start: sequential reads of 1, 4, 16 and 64 MiB, four threads
with their own descriptors (4 MiB reads), then the first region again (cached,
for comparison). One 64 MiB buffer; at most 10 s a test and 30 s in all
(inside the watchdog's 60 s margin). One `PS5LM_STORAGE_PROBE` line. For the
OLMoE file the first regions were read first during the 4 GB load, so they
are the least likely to be cached; the 0.5 GB Qwen file is probably cached
(it loaded at ~650 MB/s before).


**Tuning runs (console runs 1 and 2 of this work).** Both passed and closed;
every configuration's 32 check tokens were identical to the model mode's GPU
tokens, and the 128 tokens generated after each 128- and 512-token benchmark
prompt were identical across all 15 configurations of each model. Medians
(`test-results/ps5/api-tune/20261015-233814/` Qwen, `…/20261015-234228/` OLMoE):

| Configuration | Qwen pp128 / pp512 / tg128 | OLMoE pp128 / pp512 / tg128 | Qwen / OLMoE dispatches per graph |
|---|---|---|---|
| baseline (old settings) | 1731 / 1867 / 212.0 | 615 / 571 / 231.5 | 796 / 582 |
| one submission per graph (1000 nodes, no flop split) | 1731 / 1861 / 191.4 | 619 / 568 / 216.6 | |
| 300 nodes, no flop split | 1782 / 1868 / 204.1 | 623 / 572 / 225.7 | |
| 150 nodes, no flop split | 1792 / 1885 / 209.7 | 624 / 573 / 233.4 | |
| 1000 nodes, flop split kept | 1735 / 1861 / 214.6 | 614 / 572 / 233.5 | |
| + large mat-vec workgroup (ProsperoAI `DMMV_WG=1`) | 1714 / 1855 / 154.6 | 619 / 573 / 185.7 | |
| + prompt chunk 64 / 128 / 256 / 512 | pp512 2783 / 3480 / 4567 / 5163 | pp512 907 / 1441 / 2185 / 2575 | |
| + fusion | 1778 / 1922 / 243.8 | 624 / 577 / 300.0 | 622 / 327 |
| + graph optimisation | 1681 / 1882 / 230.6 | 628 / 584 / 240.7 | |
| + fusion and graph optimisation | 1730 / 1927 / 253.0 | 635 / 590 / 324.3 | 622 / 327 |
| **final: fusion, graph opt., 1000 nodes, chunk 512** | **3153 / 5271 / 251.4** | **1429 / 2656 / 326.7** | 621 / 326 |
| baseline again | 1721 / 1874 / 209.8 | 615 / 570 / 230.3 | |

**What the counters say.** At the baseline a Qwen token's graph took 4.41 ms
of which 2.15 ms was inside `vkQueueSubmit` (13.8 submissions, 156 µs each)
and 1.18 ms in the final fence wait; OLMoE 5.52 ms, 3.69 ms in submit. With
one submission per graph the submit time vanished (74 µs) but the fence wait
grew to 3.8 ms and the token got slower: the submit call blocks while the GPU
is still busy, so its time is mostly GPU time, not driver overhead, and
splitting the graph overlaps recording with execution. What did help is
fewer dispatches: fusion (rms_norm+mul, multi-add, MoE top-k, …) cut 796 → 622
(Qwen) and 582 → 327 (OLMoE) dispatches per token and the token time fell
with them, graph optimisation (fewer barriers between independent nodes)
added a little more. Generation is therefore bound by a fixed cost per
dispatch on the GPU side (dispatch plus barrier, roughly 4–6 µs each at
these sizes), which is why it tracked the layer count. Larger mat-vec
workgroups are slower on this GPU. Prompt processing scales with the chunk
up to 512 (MoE most: each chunk reads all experts once).

**Kept** (one build-time change of the defaults, measured above): for model
and server builds the closed environment no longer disables fusion and graph
optimisation and sets `GGML_VK_MAX_NODES_PER_SUBMIT=1000`; the server's
prompt chunk is 512. The operator probes keep fusion and graph optimisation
off. **Reverted/not kept:** one submission per graph, 150/300-node cadences
without the flop split, the large mat-vec workgroup. **Not tried:** async
compute (single-sequence decoding needs the logits before the next graph, so
there is nothing to overlap; it would change the synchronisation paths the
2 s waits guard), ProsperoAI's rows-per-workgroup and subgroup knobs (fixed
at pipeline definition: build-time runs, left for later), CPU threads
(nothing but the embedding lookup runs on the CPU).

**Budget change for Qwen3.5 0.8B and Granite 3.1 1B** (written before the
run that uses it): the 512-token context measured +506 MiB on Qwen (cap 512)
and a direct peak of 1505 of 1536 MiB, because qwen35's scheduler block is
sized from `n_tokens * 40` nodes (414 MiB at 512, 198 MiB at 32). Context cap
**768 MiB**, total **2048 MiB** (Vulkan cap 1920); expected peak ~1.5 GiB.
2048 MiB is below the largest passing total (6144), so it is not a new step.

**Model load.** The loader read the OLMoE file at ~92 MB/s, but the storage
probe read the same file's first regions (read first during the load) at
1.4 GB/s with 1 MiB `read()` calls and 2.2–2.8 GB/s with 4–64 MiB, 3.2 GB/s
with four threads. llama.cpp's no-mmap loader reads through `fread` on a
`FILE`. Patch `patches/0003-ps5-model-file-reads.patch` (`__PROSPERO__` only)
reads with `read(2)` on a descriptor of its own (the `FILE` keeps the
position). Checked on the Mac against `fread` for 200 random ranges
(byte-identical, positions equal), then on the console: **OLMoE loads in
3.6 s instead of 45.6 s** (about 1.17 GB/s, model mode and server mode), with
the same tokens. The probe's `fread` test confirms the cause: 1 MiB `fread`s of
an uncached region of the OLMoE file ran at 92.6 MB/s (the old load rate),
the same calls on the cached Qwen file at 4.3 GB/s; this libc's `fread`
apparently reaches the file in small requests, which only cost when they go
to the SSD. Qwen's load (file cached) went from 0.82 to 1.26 s, within what a
cached 0.5 GB read varies; not investigated further.

**Results** (kept defaults; console runs 3–6; API medians, 128/512-token
prompts, 128 generated tokens; model-mode tokens identical to the earlier
model-mode runs, rule `prefix` as before):

| Model | Prompt tok/s before → after | Generation tok/s before → after | Load | Per-token graph (dispatches, submits) |
|---|---|---|---|---|
| Qwen3.5 0.8B (24 layers) | 1744 / 1884 → **3147 / 5264** | 214.0 / 210.9 → **254.5 / 252.7** | 0.8 → 1.3 s (cached) | 796 → 621, 13.8 → 11.0 |
| OLMoE-1B-7B (16 layers) | 615 / 571 → **1429 / 2628** | 232.4 / 223.1 → **326.9 / 309.2** | 45.6 → **3.6 s** | 582 → 326, 11.9 → 11.2 |

Granite 3.1 1B-A400M (server run 7): prompt 1370/1396 → **3328/6532**,
generation 212.5 → **344.6**, load 6.0 → 1.0 s, completion identical to its
model mode. Granite 3.1 3B-A800M (run 8): prompt 711/709 → **1762/3321**,
generation 149.5 → **236.7**, load 21.5 → 2.1 s; its completion now equals the
CPU reference for all 32 tokens (the old GPU run split at a 0.105 tie), so
fusion changed its rounding, within the rule. Neither Granite had a
model-mode run with the new defaults (the gate is Qwen and OLMoE).

**MMVQ knob: no effect, nothing changed.** With `disable_mmvq` the dispatch
counts and rates of Granite 1B, Granite 3B and OLMoE stayed the same (within
1 %), so the integer-dot mat-vec path is not used for these models on this
device; the knob stays as tooling with no default change.

**Final code** (runs 9–12: the MMVQ knob added, defaults as kept): OLMoE
server and Qwen server within 1 % of runs 6 and 5; both model modes pass with
tokens, memory and submission counts identical to runs 3 and 4.

**What limits speed now.** Generation: a Qwen token's graph is 3.47 ms (621
dispatches, ~5.6 µs each), an OLMoE token's 3.45 ms (326 dispatches, but MoE
mat-vecs are larger); the GPU time per dispatch, not submission or CPU
recording, sets it. More fusion (fewer dispatches) or fewer barriers is what
would help; ggml-vk's remaining unfused ops are the place to look. Prompt
processing: 512-token chunks are the largest the context allows; larger
chunks need a larger compute buffer and (Qwen) a larger scheduler block.


**Granite 1B and 3B with the kept defaults, and an MMVQ knob** (written before
console runs 7 and 8). One server window each (`server`, `server-moe3b`):
API check against their earlier model-mode runs (tokens identical, or the
model mode's rule against the CPU reference, since fusion may change
rounding), the benchmark, then `--plan mmvq`: a runtime knob
`disable_mmvq` (patch 0007 again, per graph, -1 = device default) turns off
ggml-vk's integer-dot mat-vec path, which on AMD quantises the input to q8_1
in a dispatch of its own for every mat-vec with K ≥ 2048. Context at 512
tokens, measured on the Mac (Metal): Granite 1B compute 9.6 → 38.3 MiB, 3B
11.9 → 56.6 MiB, so server contexts of about 180 and 240 MiB against caps of
768 and 384 MiB; totals about 1.2 and 2.4 GiB against 2048 and 3072 MiB.
No cap changes.


### SSD streaming: feasibility, routes and budgets (2026-10-16)

Goal: run an MoE model whose file is larger than the memory the title holds
for it, with the experts coming from the SSD. Shared tensors (attention,
router, norms, embeddings, LM head) stay resident on the GPU; only expert
weights stream. A well-measured "not worth it" is an acceptable outcome.

**Expert bytes** (from the GGUF tensor tables; sizes are offset differences,
so they include alignment):

| | OLMoE-1B-7B-0125 Q4_K_M | Qwen3-30B-A3B-2507 IQ2_M |
|---|---|---|
| File | 4,213,512,192 B (3.92 GiB) | 9,870,270,464 B (9.19 GiB) |
| Expert tensors (`ffn_{gate,up,down}_exps`) | 3,900,702,720 B (3720 MiB, 92.6 %) | 9,003,073,536 B (8586 MiB, 91.3 %) |
| Everything else | 311,027,712 B (297 MiB) | 861,227,008 B (821 MiB) |
| Layers x experts, used per token | 16 x 64, 8 | 48 x 128, 8 |
| One expert's matrix (contiguous in the file) | Q4_K 1,179,648 B; Q6_K 1,720,320 B | IQ2_XS 454,656 B; IQ3_XXS 602,112 B |
| One expert (gate+up+down), mean | 3.63 MiB | 1.40 MiB |
| Expert bytes one token needs (8 x layers) | **465 MiB** (128 expert-layers) | **537 MiB** (384 expert-layers) |

**Without any cache** every token reads all of it: at the best measured rate
(3.2–3.7 GB/s, four threads, 4 MiB reads) that is at most **7.2 tok/s
(OLMoE) and 6.2 tok/s (Qwen3)**, at the single-thread 1 MiB rate (1.4 GB/s)
2.9 and 2.5 tok/s, before any compute. Fully resident today: OLMoE 327 tok/s
(server), Qwen3 37 tok/s (old defaults, failed the token rule).

**Expert reuse** (host routing trace, same pin, CPU: four prompts of 22–33
tokens, 128 greedy tokens each, 512 generated tokens; per-layer LRU warmed by
the prompt; `ffn_moe_topk-L` captured with an eval callback). A token keeps on
average 39 % (OLMoE) / 44 % (Qwen3) of the previous token's experts per layer.

| Experts cached per layer | OLMoE resident (MiB) | OLMoE hit | OLMoE miss MiB/token | Qwen3 resident (MiB) | Qwen3 hit | Qwen3 miss MiB/token |
|---|---|---|---|---|---|---|
| 1/8 | 762 | 0.27 | 340 | 1895 | 0.56 | 234 |
| 1/4 | 1227 | 0.52 | 222 | 2968 | 0.77 | 122 |
| 3/8 | 1692 | 0.68 | 151 | 4041 | 0.88 | 64 |
| 1/2 | 2157 | 0.79 | 96 | 5114 | 0.93 | 37 |
| 5/8 | 2622 | 0.88 | 57 | 6188 | 0.96 | 24 |
| 3/4 | 3087 | 0.94 | 29 | 7261 | 0.97 | 15 |

Read-bound ceilings from this table (misses / read rate, nothing else):
OLMoE at half its experts (2.1 GiB resident vs 3.9 GiB file) 15–35 tok/s at
1.5–3.5 GB/s; Qwen3 at half (5.0 GiB vs 9.2 GiB) 39–90 tok/s, at 3/8
(3.9 GiB) 22–52 tok/s. The trace is short (512 tokens, four topics); a
static "hottest experts" pin is worse than LRU here (OLMoE 0.72 vs 0.79 at
1/2), so routing is topic-local. Real rates will be lower than the ceilings:
a miss is a 0.45–1.7 MB read on the critical path of its layer (latency, not
just bandwidth), and the experts a layer needs are only known after its
router has run, so every layer needs a GPU→CPU round trip before its reads
(48 per Qwen3 token).

**Route A, no new kernels: experts on the CPU, from the file.** llama.cpp
keeps tensors matching `ffn_.*_exps` in a CPU buffer
(`tensor_buft_overrides`); with mmap that buffer is the file mapping itself,
so expert pages come from the SSD through the page cache and the CPU
computes `MUL_MAT_ID` on the selected experts only; everything else stays on
the GPU. Its cost per token is the CPU's expert compute (465 / 537 MiB of
weights per token) plus a GPU→CPU→GPU hop per layer plus page faults for
misses. Unknowns, all measured by the probe below before any model run:
(1) does mmap of a file work in a title at all (PS5SX2 reports that mmap
fails in its app sandbox), and where are mapped file pages charged (the
title's flexible pool is only 448 MiB; the direct budget is ours; or the
system's page cache)? (2) how fast pages fault in; (3) the CPU's
`MUL_MAT_ID` rate on these quant types with 1–8 threads (13 logical CPUs
allowed, mask `0x1fff`).

**Route B (only if A shows the bottleneck is compute, not reads): a GPU
expert cache**, a bounded patch keeping hot experts in device memory and
uploading misses from large `read()`s. Decided after Route A's numbers.

**Correctness gate**, fixed before any streaming run: the streaming run's
32 greedy tokens equal the fully resident run's of the same model and build
(OLMoE: the model mode's tokens, runs 20261015-212150 / 20261016-001732), or
pass the GPU-vs-CPU rule against the CPU reference. Recorded per run: memory
peaks per part (direct ledger, Vulkan, page-cache residency of the mapping
via `mincore`), tok/s, and bytes read from storage per token (newly resident
pages of the mapping per token, a lower bound).

#### Stream probe (console run 1 of this work): budget and limits

Build `bash scripts/build-native-compute.sh stream-probe`
(`ps5/native-compute/ggml-stream-probe.cpp`); no model load. Reads the
OLMoE and Qwen3 files already on the console, read-only. As run (eboot
`e734fdfc…aaf7`) it began with an mmap section; after the result below that
section was removed (eboot `c2b48c08…713e`, 281 imports, built and
validated, not run).

| Part | Expected | Cap |
|---|---|---|
| Heap (one 120 MiB buffer: expert tensors for the CPU test, pread buffers) | ~125 MiB | in the total |
| Tiny GPU check (one 32-float add) | < 1 MiB | in the total |
| **Total direct memory** | **~200 MiB** | **1024 MiB** (ledger) |
| Mapped file pages touched | ≤ 290 MiB | 224 MiB sequential + 40 + 16 slices; the sequential steps (32 MiB) stop early if the title's flexible pool falls by 64 MiB, the direct budget by 64 MiB, or the system's free + inactive + cache pages below 1 GiB |
| pread through the page cache | ~400 MB | the loader has read 4–10 GB this way |
| CPU threads | 1, 2, 4, 6, 8 | 0.4 s per setting, persistent ggml threadpool, no polling between graphs |

Every test has a time limit (pread 8 s each); expected total ~30 s; watchdog
300 s. Checked on the Mac first with stubs for the PS5 calls (same code,
local model files): all sections complete. New imports, all in `libkernel`:
`mincore`, `madvise`, `pread`, `getrusage`, `sysctlbyname`,
`sceKernelAvailableFlexibleMemorySize` (283 imports, validator passes).

**Result: the probe hung in `mmap()` of the model file; stop condition**
(console run 1, `test-results/ps5/PPSA99041/20261016-003554/`, details in
CONSOLE.md). The title printed its start line (flexible pool 385 MiB
available of 448, direct 12.0 GiB, 16 KiB pages) and never got past mapping
the 3.9 GiB OLMoE file read-only and shared (or the `mincore()` over it
that follows): the kernel's wired pages grew by ~20 MB/s until the whole
~1.2 GiB system pool was wired, system processes were swapped out, `FMEM
allocation timeout` repeated, and after its watchdog's exit request the title
did not close within the script's window. My stop rules sat inside the
touch loop, after the mapping, so they never ran: the danger is the mapping
itself, not the pages touched. Console work stopped there (1 of 10 runs).

What this settles:

- **Route A is not viable in a title.** A file mapping's pages are wired
  out of the small pool the system itself lives on, so expert weights
  cannot come "from the file through the page cache" here; with a 3.6–8.4 GiB
  expert set this freezes the console. The resident model modes were safe
  only because they load with `LLAMA_LOAD_MODE_NONE` (and the server with
  `-lm none`). llama.cpp's default load mode is `AUTO` = mmap: **every model
  build must keep the explicit no-mmap setting**, and the
  `tensor_buft_overrides` + mmap recipe must not be tried in a title. It
  might still work in a payload (8 GiB flexible), but a payload has no
  direct memory for the GPU part, so it is no help here.
- The CPU half of Route A (expert compute on 13 allowed CPUs) was never
  measured on the PS5: the probe's pread and CPU sections come after the
  mapping. A Mac check of the same code is no stand-in for the PS5's Zen 2.
  Without mmap, CPU experts would have to sit in our own direct memory,
  which is residency, not streaming.
- The only safe way to bring weights in from storage is `read()`/`pread()`
  into memory the title allocated itself (the direct-memory ledger), as the
  loader already does at 1.2 GB/s and the storage probe at 1.4–3.7 GB/s.

**Route B, a GPU expert cache fed by `pread()`** (designed, not built; no
console runs left in this session, and building it blind would be a forced
prototype). Shape: per layer, a fixed number of expert slots in host-visible
device memory (one Vulkan buffer per layer and matrix); the router's top-k
ids come back to the CPU after each layer's routing (the graph split per
layer, like a CPU expert pass); misses are read with large parallel `pread()`s
straight into free slots (LRU), the ids are remapped to slot indices, and the
layer's `MUL_MAT_ID` runs on the slot buffer. It needs one ggml-vulkan/llama
patch (slot-indexed expert tensors and an id remap between the router and the
expert matmuls), bounded memory (slots x slot size, written per model), and
the same 2 s fence limits.

Latency model (routing trace above; per layer: one GPU→CPU round trip plus,
if it misses, a read latency plus miss bytes / read rate; GPU compute per
token 3.45 ms OLMoE (measured) and 20–27 ms Qwen3 (27 ms measured with the old
defaults); three cases: round trip 0.15 / 0.3 / 0.5 ms, read 3.5 / 2.0 /
1.4 GB/s, per-layer read latency 0.15 / 0.3 / 0.5 ms;
`test-results/local/ssd-feasibility/20261016-003542/routeb-model.txt`):

| Experts cached per layer | OLMoE resident | OLMoE tok/s | Qwen3 resident | Qwen3 tok/s |
|---|---|---|---|---|
| 3/8 | 1.7 GiB | 8–19 | 3.9 GiB | 9–20 |
| 1/2 | 2.1 GiB | 11–28 | 5.0 GiB | 12–25 |
| 5/8 | 2.6 GiB | 17–42 | 6.0 GiB | 13–28 |
| 3/4 | 3.0 GiB | 28–65 | 7.1 GiB | 15–30 |
| all (resident, measured) | 4.5 GiB peak | 327 | 9.5 GiB peak | 37 (old defaults) |

For Qwen3 the 48 per-layer round trips (7–24 ms) cost as much as the reads,
so even a perfect cache stays near half the resident rate; for OLMoE the
reads dominate and streaming loses 5–30x against residency. Both models fit
fully resident, so streaming either of them only saves memory nobody needs.

**Recommendation.** Not worth pursuing for the models on the console.
Worth a measured attempt only for a model that cannot be resident and whose
quality justifies ~10–20 tok/s, e.g. Qwen3-30B-A3B at Q4_K_M (~17 GiB;
experts ~16 GiB, ~1 GiB per token) with ~8 GiB of expert slots, and only as
Route B (never mmap). Before building it: (1) run the probe as it is now
(mapping removed: pread of expert-sized slices, CPU `MUL_MAT_ID` rates,
`ps5-native-test.sh -t 360` so the 300 s watchdog's close is visible) after
the console is recovered, to replace the assumed read latency and round-trip
times; (2) measure one GPU→CPU→GPU round trip per layer with the resident
OLMoE graph split at the router (an eval callback on `ffn_moe_topk`), which
alone bounds Route B; (3) then the cache patch, gated by tokens identical to
the resident run of the same model and build.

### Vision: Qwen3-VL, pins, budget and checks (2026-10-16)

Written before the first load of a vision model.

**Model.** Qwen3-VL 4B Instruct from the publisher's own GGUF repo: the text
model at Q4_K_M and its mmproj (vision encoder + merger) at F16. F16 rather
than the published Q8_0 mmproj (454 MB): it is the encoder's original
precision, so the GPU-vs-CPU comparison measures the backends and not a
quantised encoder, and the 382 MB it costs fits easily. The pinned mtmd
(`tools/mtmd/models/qwen3vl.cpp`, projector `qwen3vl_merger`) loads both.
Fallback, unused so far: Qwen2.5-VL 3B.

| File | Repo @ revision | Bytes | SHA-256 |
|---|---|---|---|
| `Qwen3VL-4B-Instruct-Q4_K_M.gguf` | `Qwen/Qwen3-VL-4B-Instruct-GGUF` @ `1cd86afb9a95c410a6038ab3b40d8b578c892266` | 2,497,281,664 | `66358cb18bb6b3b1b6675aa412c7a88ef01d228f481184d13668e5201c730a0a` |
| `mmproj-Qwen3VL-4B-Instruct-F16.gguf` | same | 836,180,256 | `256f3a43bd4205ffef48d6b92715e1e70b5b0e9aef06522584967513a9985331` |

Both SHA-256 equal the Hub's LFS ids at that revision and the downloaded
files. Text model: `qwen3vl`, 36 layers, hidden 2560, 32 Q / 8 KV heads of
128, M-RoPE sections 24/20/20, 3 deepstack layers (so `n_embd_inp` = 4 x 2560
= 10240: the image embedding carries the main features and three deepstack
slices), tied embeddings: `token_embd` is one Q6_K tensor of 304.3 MiB
(151,936 x 2560), the largest tensor. Mmproj: ViT with 24 layers, hidden 1024,
16 heads of 64, patch 16, spatial merge 2, learned 48 x 48 position
embeddings (interpolated to the image), deepstack after layers 5, 11 and 17;
787 MiB F16 + 10 MiB F32.

**Test image and prompt.** `testdata/vision/hello-shapes-448.png` (448 x 448,
2558 bytes, SHA-256 `93d3005243aadc3361ec4e1ee3109a929fbfaaf19ed40df40b26a6122d0df774`),
drawn by `scripts/make-vision-test-image.py` (pure Python: block letters from
a 5 x 7 pixel font defined in the script, no font file, so no licence beyond
the repo's): the word HELLO in black, a red circle, a blue square and a green
triangle on white. At patch 16 and merge 2 it is 14 x 14 = **196 image
tokens**. The image tokens are capped: `image_min_tokens` 64,
`image_max_tokens` 196, which also caps the encoder's warm-up graph (the
pin's default for Qwen-VL warms up at 46 x 46 = 2116 tokens, 8464 patches).
Prompt (Qwen chat format, user turn only): `<__media__>Describe this image in
one sentence. Then answer: what word is written in the image?`; mtmd wraps the
image in `<|vision_start|>`/`<|vision_end|>`. 28 text tokens + 196 image
tokens; 64 greedy tokens at most. **Checkable answer:** the generated text must
contain `HELLO` (case-insensitive). The shapes are reported, not gated.

**Runner** (`ps5/native-compute/ggml-vision.cpp`, build mode `vl4b`):
the model mode's runtime (ledger, guards, closed environment, watchdog, close)
with libmtmd linked (`build-native-llama.sh` builds `mtmd` with
`PS5LM_LLAMA_MTMD=1`; the server build already had it). Order: load the text
model (no mmap, all 37 layers on the GPU), `mtmd_init_from_file` (GPU, flash
attention on, warm-up on), one context (1024 tokens, batch = ubatch = 32,
FP32 flash attention, F16 KV), decode the embedded PNG with mtmd's helper
(stb_image, as the server does), tokenize, `mtmd_encode_chunk` (timed),
compare the embedding with the CPU's, then decode text, image embedding (7
batches of 32 with M-RoPE positions) and text, and generate. The mmproj loads
through clip's `std::ifstream` (no mmap anywhere). The CPU reference's
embedding (8,028,160 B) and trace samples (1.8 MB) are embedded in the eboot
with `.incbin`, so the title compares in place and writes only its usual
output file.

**Allocation cap for the tied output.** The 304.3 MiB output matrix does not
fit ggml's 240 MiB allocation cap. For this model only:
`GGML_VK_FORCE_MAX_ALLOCATION_SIZE` 320 MiB and the dispatch guard's
per-allocation cap 336 MiB (`PS5LM_MODEL_GPU_ALLOCATION_MIB`); every other
model keeps 240 / 256 MiB (the OLMoE build is still byte-identical,
`354b6fb4…cc00`). The output layer's load-time probe (F32 151,936 x 512 =
297 MiB) is under the 512 MiB `max_buffer_size`; the 151,936-row LM head is
split by patch 0006 as Qwen3-30B's was.

**Budget** (MiB; host measurements with the same pin: text model 2375.9 in
GPU buffers, KV 144.0 at 1024 tokens, encoder compute 29.9 on the CPU at 196
tokens, context compute 20.1). Written before the first load; caps enforced
in the title:

| Part | Expected | Cap | Enforced by |
|---|---|---|---|
| Before the load | ~90 | in the total | ledger |
| Text weights: GPU buffers 2376 (incl. the tied output's GPU copy) + input embedding in `Vulkan_Host` 304 + loader transients | ~2700 | 3072 | ledger delta across `llama_model_load_from_file`, else `model_weights_budget` |
| Mmproj: weights 797 + encoder compute (~30–60 at 196 tokens) + read buffer (largest tensor 32) | ~880 | 1152 | ledger delta across `mtmd_init_from_file`, else `mmproj_budget` |
| Context: KV 144 + compute + scheduler | ~250 | 1024 | ledger delta across `llama_init_from_model`, else `model_context_budget` |
| Image embedding (host copy) + PNG decode | ~10 | in the total | ledger |
| All Vulkan memory | ~3.8 GiB | 5248 (3072 + 1024 + 1152) | dispatch guard |
| **Total direct memory** | **~3.9 GiB** | **6144** | ledger, per allocation |

6144 MiB is the largest total that has passed (OLMoE), so this is not a new
step. Watchdog 300 s (`-t 600`); loads expected ~2–3 s (text, `read(2)`)
and up to ~10 s (mmproj through `fread` at the uncached 92 MB/s). The
encoder is about 0.5 TFLOP (24 layers x 784 patches); at the 7–8 TFLOP/s
the 512-token prompts reached it is ~70 ms, far inside the 2 s fence limit.
Queue submissions 16,384.

**CPU reference** (`scripts/build-host-vision-reference.sh vl4b`, same pin
and patches, CPU only, writes `build/native-vl4b/gen/`;
`test-results/local/vision-reference/vl4b-20261016-080617/`): "The image
displays the word "HELLO" in bold black letters above three geometric
shapes—a red circle, a blue square, and a green triangle—arranged
horizontally. The word written in the image is "HELLO"." then `<|im_end|>`
(47 tokens). First-8 gaps all above 0.56; the smallest gap is 0.018 at token
10. Answer right, shapes right.

**Image embedding tolerance**, fixed before the first console run. The Mac's
Metal stand-in of the same runner (`PS5LM_VISION_METAL=1`;
`test-results/local/vision-metal-smoke/vl4b-20261016-080628/`) against this
CPU reference: max abs 0.874 (5.8 % of the largest reference value 15.08),
relative L2 0.0106, cosine 0.99994; per token, 195 of 196 tokens above
0.9987 and one (token 189, a white-background patch in the bottom row) at
0.957, its deepstack slice from layer 11 at 0.88 (a low-norm token, where
LayerNorm amplifies rounding); 47 tokens identical to the CPU's. Per-layer
samples: relative error ~0.001 through layer 9, rising to ~0.02 by layer 22
as activations grow. So per-token minimum cosine is not a usable rule.
**Tolerance:** relative L2 over the whole embedding ≤ 0.03, cosine ≥ 0.9995,
and at most 2 of 196 image tokens (1 %) with their own cosine below 0.995.
Reported as well: max abs error, max abs relative to the largest reference
value, per-slice relative L2 (main, deepstack 5, 11, 17), worst token.

**Pass rule** (all of): the embedding within the tolerance; the text rule of
the text models (first 8 tokens identical to the CPU's, or the first
difference at a token whose CPU top-two gap is < 0.5; no NaN, no run of 8
equal tokens, no llama.cpp error line, no untracked Vulkan free); the answer
contains HELLO; all layers and the output on the GPU.

**If the embedding is outside the tolerance**, the title encodes once more
with an eval callback that copies strided samples (every 61st value) of the
encoder's tensors: `patch_bias` (patch-embedding conv2d/im2col + bias),
`inp_pos_emb` (+ interpolated position embeddings), layer 0's
`Qcur_rope`/`Kcur_rope` (image M-RoPE), `kqv_out` (non-causal attention),
`attn_out`, `ffn_inp`, `ffn_out`, and every `layer_out-N`; each is compared
with the CPU's samples (`PS5LM_VISION_TRACE`, first tensor over 0.03) to find
the op before any fix in `patches/`.

**Result, model mode** (console runs 1 and 2 of this work; CONSOLE.md).
Run 1 failed the embedding tolerance safely (relative L2 0.070, cosine
0.99766, 31 tokens below 0.995; text `prefix`, answer right). The trace put
the first tensor over tolerance at layer 0's FFN output, every matmul output
5–30x worse than Metal's. Cause, read from ggml-vk at the pin:
`ggml_vk_get_mul_mat_mat_f16acc` returns true for F16 weights at
`GGML_PREC_DEFAULT` on an fp16 device without coopmat (the title disables
coopmat), so the encoder's F16 x F32 matmuls ran the FP16-accumulating
`matmul_f16_f32_f16acc` pipelines over K up to 4096 (the FFN down
projection). Fix, `patches/0004-ps5-clip-f32-matmul-accumulation.patch`
(`__PROSPERO__` only): after clip builds its graph (the reserve and the
encode), every `GGML_OP_MUL_MAT` node requests `GGML_PREC_F32`
(`ggml_prec_set_acc`), which ggml-vk honours with its `matmul_f16_f32` / dot2
f32acc pipelines; this covers the patch-embedding conv's im2col matmul too.
Text-model graphs and other platforms are unchanged (the CPU reference's
embedding is byte-identical with the patch applied). Run 2 passed: relative
L2 0.0082, cosine 0.999967, one token below 0.995 (the white-background
token 189 at 0.976), all 47 tokens identical to the CPU's, answer right,
memory as budgeted (text +2716, mmproj +1000, context +227 MiB, peak
4027 MiB of 6144), encode 231 ms for 196 image tokens, 114.8 tok/s
generation. Not done: an op-level check of conv2d/im2col, the image RoPE and
non-causal attention by themselves; after the fix every traced tensor is
inside tolerance in the one encode, so there was no op left to isolate.

**Server mode** (`server-vl4b`; written before its first run). llama.cpp's
server as for the text models (one slot, context 2048, physical batches of
512, FP32 flash attention, F16 KV, no mmap, the 600 s window or
`POST /ps5lm/shutdown`), plus `--mmproj` with the model mode's encoder
settings (`--image-min-tokens 64 --image-max-tokens 196`, GPU, flash
attention, warm-up; any image a client sends is resized to at most 196
tokens). A link-time wrap of `mtmd_init_from_file` allows exactly one mmproj
load, checks the file and these settings, and enforces the mmproj cap as
the model mode does; patch 0004 is in the server build too. The image
embedding is decoded in one 512-token batch (the model mode: 7 batches of
32), so the API's tokens are checked against the model mode's GPU tokens or,
failing that, by the model mode's rule against the CPU reference on the
token ids the API returns (`logprobs`); the Mac's CPU rehearsal of this
server already differs from the CPU model mode at token 10, the 0.0175 tie
(rule `prefix`, `test-results/ps5/api-vision/20261016-083119/`).
Budget (caps unchanged from the model mode, enforced the same way):

| Part | Expected | Cap |
|---|---|---|
| Before the load, server threads | ~110 MiB | in the total |
| Text weights | ~2720 MiB (model mode +2716) | 3072 |
| Context: KV 288 MiB at 2048 + compute at 512-token batches + scheduler | ~550 MiB | 1024 |
| Mmproj (after the context in the server) | ~1000 MiB (model mode +1000) | 1152 |
| Serving scratch (image decode, requests) | < 64 MiB | in the total |
| **Total direct memory** | **~4.4 GiB** | **6144 MiB** |

Watchdog 180 + 600 + 60 + 60 = 900 s (`-t 1020`). Requests in the window:
`scripts/ps5-api-vision.py` (one check with token ids, then one warm-up and
three timed image requests of 64 tokens, prompt cache off), then the
unchanged text benchmark `scripts/ps5-api-bench.py`, then the shutdown.

**Result, server mode** (console run 3): pass. Through the OpenAI API the
image request's 44 token ids equal the model mode's (and the CPU's), answer
right; image prompt 397 ms (encode + decode of 224 prompt tokens),
generation 113 tok/s; text benchmark 913 / 1171 tok/s prompt, 116 / 112
tok/s generation; context +466 MiB, mmproj +990 MiB, peak 4382 MiB of 6144.

#### Qwen3-VL 8B: pin, new step 8192 MiB, budget (written before its first load)

| File | Repo @ revision | Bytes | SHA-256 |
|---|---|---|---|
| `Qwen3VL-8B-Instruct-Q4_K_M.gguf` | `Qwen/Qwen3-VL-8B-Instruct-GGUF` @ `f982a07559d4a2f6c8744d840bf6fccab30eea96` | 5,027,784,800 | `67d1659bfe71b89d50b45a4ad1a9e5b997e5bb16ce5da66a6a6167abd569e9e2` |
| `mmproj-Qwen3VL-8B-Instruct-F16.gguf` | same | 1,159,029,824 | `ca524100ebf825c9a870db1c580d03879e0da0ab2541697e2458e64891cf9d38` |

SHA-256 equal to the Hub's LFS ids and the downloaded files. Text model: 36
layers, hidden 4096, FFN 12288, 32 Q / 8 KV heads of 128, 3 deepstack layers
(`n_embd_inp` 16384), **untied**: `output.weight` Q6_K 486.9 MiB (one
tensor), `token_embd` Q4_K 333.8 MiB; 4789 MiB of tensors. Mmproj: 27-layer
ViT, hidden 1152, deepstack after layers 8, 16, 24; 1105 MiB.

**Allocation cap**: ggml 512 MiB (`GGML_VK_FORCE_MAX_ALLOCATION_SIZE`
536,870,912), dispatch guard 528 MiB, for the 486.9 MiB output; the load-time
probe (297 MiB) is under the 512 MiB `max_buffer_size`.

**Step.** The largest passing total is 6144 MiB (OLMoE, and the 4B here);
this step is **8192 MiB** (at most 2 x 6144 = 12,288 and under the
device-memory rule's 10,649), leaving 3.4 GiB of RADV's ~11.4 GiB free at the
cap. Any failure, hang or unclosed title ends it at 6144 MiB.

| Part | Expected (model / server) | Cap | Enforced by |
|---|---|---|---|
| Before the load | ~90 / ~110 | in the total | ledger |
| Text weights: GPU buffers 4455 (incl. output 487) + input embedding in `Vulkan_Host` 334 + transients | ~4830 | 5376 | as the 4B |
| Mmproj: 1105 weights + encoder compute at 196 tokens (~40) + read buffer (40.5) | ~1180 | 1408 | as the 4B |
| Context: KV 144 (model, 1024) / 288 (server, 2048) + compute + scheduler | ~230 / ~550 | 1024 | as the 4B |
| Serving scratch | – / < 64 | in the total | ledger |
| All Vulkan memory | ~6.0 / ~6.4 GiB | 7808 | dispatch guard |
| **Total direct memory** | **~6.2 / ~6.7 GiB** | **8192** | ledger |

Queue submissions in the model mode 24,576 (the 4B used 3339). Watchdog
300 s (model, `-t 600`), server 900 s (`-t 1020`); loads expected ~5 s (text)
and ~13 s (mmproj through `fread`). CPU reference
(`test-results/local/vision-reference/vl8b-20261016-083850/`): "The image
displays the word "HELLO" in bold black letters above a row of three colored
shapes: a red circle, a blue square, and a green triangle. The word written
in the image is "HELLO"." (47 tokens; smallest top-two gap 0.69 at token 19,
none below 0.5). Metal stand-in (`…/vision-metal-smoke/vl8b-20261016-083916/`):
relative L2 0.0058, cosine 0.99998, no token below 0.995, tokens identical.
Tolerance and pass rule unchanged.

**Result, 8B** (console runs 4 and 5): pass in both modes. Model mode:
embedding relative L2 0.0056, cosine 0.999984, no token below 0.995; all 47
tokens identical to the CPU's; weights +4826, mmproj +1312, context +228 MiB,
peak 6458 MiB; encode 291 ms; 78.7 tok/s. Server: the API's tokens equal the
model mode's; image prompt 624 ms, 79.2 tok/s; context +514 MiB, peak
6875 MiB of 8192. **The largest passing total is now 8192 MiB.** The mmproj
added 1312 MiB against a 1408 MiB cap (weights 1105 + ~207, as the 4B's
~203 over its weights): keep that margin in mind for a larger encoder.
Not done: clip loads the mmproj through `std::ifstream`, which is `fread`
here (92.5 MB/s uncached, measured again by the storage probe): 9.0 s (4B)
and 12.5 s (8B) of each launch. A `read(2)` patch like
`patches/0003-ps5-model-file-reads.patch` for clip's tensor loop would cut
that to about a second; proposed, not made.


### Fixes from upstream v0.9.0 (2026-10-16)

Upstream `cobanov/PS5LM` released v0.9.0 (`6323ecb`). It was reviewed, not merged, and four
pieces were taken. The llama.cpp pin stays `552f18f`. Upstream moved to `04f7226` for MiniCPM-V
and `/v1/systemone` decision models, which this work doesn't need.

- **Floats in requests (`net-compat.c`, `ps5lm_localeconv`).** The console's `localeconv()`
  reports an empty decimal point while its `strtod` reads `.`. nlohmann::json puts the reported
  point into a number before `strtod`, so any request with a float in it (`"temperature": 0.7`
  from an ordinary OpenAI client) failed an assertion and took the server down. Our API checks
  only ever sent `temperature: 0`, an integer, so they never hit it. Server builds now bind
  `localeconv` to a C-locale copy (`--defsym`, kept local like the other `net_compat` names),
  and the title no longer imports libc's. From upstream `4f57e0e`.
- **Freed GPU buffers (`patches/mesa/0001-radv-ps5-destroyed-buffer-waits-for-gpu.patch`).**
  The PS5 winsys released a destroyed buffer's direct memory at once, so work already submitted
  could reach unmapped memory, and a GPU fault on the PS5 hangs the console. The patch makes the
  memory wait for the queue to pass the newest submission. It comes from PS5CEMU-HAR (GPL-3.0)
  by way of upstream's `patches/mesa/0007`, unchanged.
  - `scripts/build-native-radv.sh` now builds from an export of the pinned Mesa with
    `patches/mesa` applied (`build/radv-preflight/mesa-source`, build dir `radv-ps5lm`), never
    from `.deps/PS5_Mesa`. `provenance.json` lists the patches.
  - `build-native-compute.sh` links that archive by default
    (`885efdeb…8532`). The old `radv-release` archive stays on disk for comparison.
  - Upstream's `0008`/`0009` (submit thread, wait spin) are not taken: upstream measured no gain.
- **Anthropic API (`patches/server/0004-anthropic-system-messages.patch`).** Later system
  messages are folded into the leading one, so templates such as Qwen's accept Claude Code's
  `/v1/messages` requests. From upstream, unchanged.
- **Chat-template floats (`patches/server/0005-jinja-tojson-floats-as-python.patch`).** `tojson`
  writes floats as Python's `json.dumps` does (`1.0`, not `1`). From upstream, unchanged.

`scripts/ps5-api-check.py` gained four checks:

- a chat request with float sampling settings, followed by `/health`
- `/v1/messages` with a system message inside `messages`
- `/apply-template` with a float in a tool schema
- `--soak N`: N short completions in a row (upstream saw a decision model stop after ~55)

**Checks.**

- **Host rehearsal (CPU, Qwen3.5 0.8B):** 14/14 checks, soak 30/30.
- **Console run (`server-qwen`):** eboot `bc8f8c3a…67fa` linked the patched RADV, with 320
  imports and no `localeconv` import. Result: PASS, exit 0, closed:true, PID 113, 127 responses,
  0 errors.
  - api-check 13/13: completions identical to the model mode's GPU tokens; the float request
    answered and `/health` stayed ok; the Anthropic request answered; the template wrote `1.0`;
    soak 100/100, 62 ms median, 67 ms max.
  - Benchmark: pp 3127.4/5254.6, tg 252.2/250.5 tok/s at 128/512 prompt tokens. Run 10 before
    these changes measured 3156.5/5271.2 and 253.5/251.3, so within 1%.
  - Memory: peak 1409.6 MiB of 2048 (1407 before), Vulkan live after free 0, so the deferred
    frees didn't raise the peak.
  - Evidence: `test-results/ps5/PPSA99041/20261016-090943/`, `api-check/20261016-091003/`,
    `api-bench/20261016-091017/`.

**Not yet rerun with the patched driver:** the other build modes (MoE, vision). Their earlier
passes used the unpatched `radv-release` archive, and their next builds link the patched one.

## On the console

Runs: see [CONSOLE.md](CONSOLE.md). mmap of the model works in a payload (in a native title it wires the file into the system pool and starved the console: see "SSD streaming"); one block of 6 GiB is the ceiling; `std::thread::hardware_concurrency()` reports 16 while only 5 CPUs are allowed, hence the patch. Open: whether leaving through `_Exit` ends the `[SceLibc] A heap error is detected` message (#3).
