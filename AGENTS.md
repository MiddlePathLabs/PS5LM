# Agent rules for PS5LM

The console is a jailbroken PS5 used as a test target. Test changes on it with:

```sh
source scripts/env.sh
scripts/ps5-test.sh TARGET          # one build and run; exit code and test-results/ps5/TARGET/latest/result.json
```

Read `docs/MAC_DEV_LOOP.md` before the first run.

- Run one `ps5-test.sh` cycle at a time. Read its result before the next edit; never loop it unattended.
- Exit 4 (console unreachable or service down), or a console that no longer answers: stop and tell the user. Only they can reboot or re-jailbreak it.
- Never run `scripts/host-relapse.sh` (it serves the jailbreak exploit).
- Never touch firmware, updates, etaHEN, kstuff, Payload Manager's settings or autoload, or anything outside `/data/PS5LM` and `/data/pldmgr/payloads/ps5test` on the console.
- Payloads that lock or allocate large amounts of memory can freeze the console (`docs/CONSOLE.md`). Start small.
- llama.cpp changes go in `patches/`, not in `third_party/llama.cpp`.
- Don't commit test results. Don't commit, push or rewrite git history unless asked.
