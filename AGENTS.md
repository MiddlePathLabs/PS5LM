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
- Native titles (PPSA99040, PPSA99041) follow the same one-run rule. An agent may launch an already-registered title itself with `scripts/ps5-native-test.sh TITLE`, once per run, and must read `test-results/ps5/TITLE/latest/result.json` and confirm `"closed": true` before the next edit. Exit 5 or 7 (title still running or not closed), a hang, or `VK_ERROR_DEVICE_LOST`: stop and tell the user. Registering new titles and changing ShadowMountPlus's configuration still need the user's approval (`docs/MAC_DEV_LOOP.md`).
- Models: an agent may download GGUF files to the Mac (`models/`, ignored by git), upload them to the console and load them in a test without asking, approved by the user on 2026-10-15, within these limits:
  - Pin every file (Hugging Face repo, revision, file name, size, SHA-256) in `docs/PORTING.md`, from the original publisher or a well-known quantizer. Verify the upload by a SHA-256 of a raw readback (`SELF` on the FTP connection) before loading it.
  - Store models only in `/data/PS5LM/models/`. Delete or replace only files an agent uploaded; leave everything else there alone.
  - Model size grows in steps, approved by the user on 2026-10-15 for agents to raise themselves: start at 2 GiB per model file; each step may at most double the largest model that has passed and closed cleanly (2 → 4 → 8 GiB), and the title's total memory budget may grow with it. Write the new step and its budget in `docs/PORTING.md` before the first run at that size. A failed run, a hang, an unclosed title or a console that stops answering ends the climb: go back to the last size that passed. Hard ceiling 10 GiB per file and 10.5 GiB total budget (RADV reports about 11.4 GiB of device memory); above that is the user's call.
  - Write the memory budget (weights, context, scratch, total, each with a cap enforced in the title) in `docs/PORTING.md` before the first load of a model or a larger setting.
  - One model per title launch: never unload and load another model in the same run.
- Payloads that lock or allocate large amounts of memory can freeze the console (`docs/CONSOLE.md`). Start small.
- llama.cpp changes go in `patches/`, not in `third_party/llama.cpp`.
- Don't commit test results. Don't commit, push or rewrite git history unless asked.
