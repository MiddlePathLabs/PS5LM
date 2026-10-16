#!/usr/bin/env python3
"""Measured tuning through the title's LAN API, in one serving window.

    scripts/ps5-api-tune.py --variant dense                      # Qwen3.5 0.8B
    scripts/ps5-api-tune.py --variant olmoe --model-run test-results/ps5/PPSA99041/<time>
    scripts/ps5-api-tune.py --variant dense --host 127.0.0.1     # the host rehearsal

Needs a server build with patches/server/0003 (GET /ps5lm/stats, POST
/ps5lm/knobs). For each configuration of the runtime knobs (all knobs are sent
every time, so nothing carries over): set them; check correctness with the
model mode's prompt (a temperature-0 /completion of 32 tokens must equal the
model mode's GPU tokens, or pass the model mode's rule against the CPU
reference: first 8 identical, or the first difference at a CPU top-two gap of
at most 0.5, no run of 8 identical tokens); then run ps5-api-bench.py's
benchmark (128- and 512-token prompts, 128 generated tokens, 1 warm-up + 3)
with the counters read before and after every measured request.

The plan is adaptive but fixed in advance: baseline; submit cadences; the
fastest correct cadence with the large mat-vec workgroup; prompt chunks of
64-512 tokens; fusion; graph optimisation; both; the best combination; the
baseline again. A configuration that fails the check is never chosen.
Writes tune.json and table.md to test-results/ps5/api-tune/<time>/. With
--shutdown it ends the serving window afterwards (POST /ps5lm/shutdown).
"""
import argparse
import datetime
import importlib.util
import json
import os
import pathlib
import statistics
import sys
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


check = load('ps5_api_check', 'ps5-api-check.py')
bench = load('ps5_api_bench', 'ps5-api-bench.py')

KNOBS = {  # name: (allowed range besides -1); the title checks them again
    'disable_fusion': (0, 1), 'disable_graph_optimize': (0, 1), 'nodes_per_submit': (1, 100000),
    'submit_mflop': (0, 10000000), 'dmmv_wg': (0, 1), 'disable_mmvq': (0, 1), 'prompt_batch': (1, 512),
}
BASE = {k: -1 for k in KNOBS}  # -1: the build's defaults (prompt_batch -1 = the build's chunk)
SIZES = (128, 512)
GENERATE, REPEATS, WARMUP = 128, 3, 1
COUNTERS = ('submissions', 'submit_ns', 'dispatches', 'fence_waits', 'fence_wait_ns', 'semaphore_waits',
            'semaphore_wait_ns', 'graphs', 'graph_ns')


def post(base, path, body, timeout=180):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'})
    t = time.monotonic()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read()), time.monotonic() - t


def get(base, path, timeout=30):
    with urllib.request.urlopen(base + path, timeout=timeout) as r:
        return json.loads(r.read())


def stats(base):
    return get(base, '/ps5lm/stats')


def delta(before, after):
    b, a = before.get('vk', {}), after.get('vk', {})
    return {k: a.get(k, 0) - b.get(k, 0) for k in COUNTERS if k in a}


def validate(knobs):
    for k, v in knobs.items():
        lo, hi = KNOBS[k]
        if v != -1 and not lo <= v <= hi:
            raise SystemExit(f'knob {k}={v} out of range')


def run_config(base, name, knobs, v, model_tokens, prompt_ids, cpu, gaps, passage):
    knobs = {**BASE, **knobs}
    validate(knobs)
    applied = post(base, '/ps5lm/knobs', knobs)[0]
    result = {'name': name, 'knobs': knobs, 'applied': applied}
    # Correctness: the model mode's prompt, 32 tokens at temperature 0.
    resp, _ = post(base, '/completion', {'prompt': v['prompt'], 'n_predict': check.PREDICT, 'temperature': 0,
                                         'cache_prompt': False, 'return_tokens': True, 'stream': False})
    tokens = resp.get('tokens') or []
    rule, first = check.rule(tokens, cpu, gaps)
    identical = tokens == model_tokens
    ok = (identical or rule != 'fail') and check.longest_repeat(tokens) < 8 and len(tokens) == check.PREDICT
    result['check'] = {'pass': ok, 'identical_to_model_mode': identical, 'rule_vs_cpu': rule,
                       'first_diff_vs_cpu': first, 'tokens': tokens,
                       'first_diff_vs_model_mode': next((i for i, (a, b) in enumerate(zip(tokens, model_tokens))
                                                          if a != b), None)}
    print(f"[{name}] check {'pass' if ok else 'FAIL'} ({'identical' if identical else rule}, "
          f"first diff vs CPU {first})", flush=True)
    result['sizes'] = {}
    for size in SIZES:
        prompt = passage[:size]
        rows = []
        for i in range(WARMUP + REPEATS):
            body = {'prompt': prompt, 'n_predict': GENERATE, 'temperature': 0, 'cache_prompt': False,
                    'ignore_eos': True, 'stream': False, 'return_tokens': True}
            s0 = stats(base)
            r, wall = post(base, '/completion', body)
            s1 = stats(base)
            t = r['timings']
            if t['prompt_n'] != size or t['predicted_n'] != GENERATE:
                raise SystemExit(f'unexpected counts at size {size}: {t}')
            rows.append({'warmup': i < WARMUP, 'wall_s': wall, 'prompt_per_second': t['prompt_per_second'],
                         'predicted_per_second': t['predicted_per_second'], 'prompt_ms': t['prompt_ms'],
                         'predicted_ms': t['predicted_ms'], 'counters': delta(s0, s1),
                         'tokens': r.get('tokens') or []})
        m = [r for r in rows if not r['warmup']]
        med = lambda key: statistics.median(r[key] for r in m)
        per_graph = {}
        graphs = sum(r['counters'].get('graphs', 0) for r in m)
        if graphs:
            for k in COUNTERS:
                if k != 'graphs':
                    per_graph[k] = sum(r['counters'].get(k, 0) for r in m) / graphs
        result['sizes'][str(size)] = {
            'runs': rows, 'prompt_per_second': bench.summary([r['prompt_per_second'] for r in m]),
            'predicted_per_second': bench.summary([r['predicted_per_second'] for r in m]),
            'wall_s': bench.summary([r['wall_s'] for r in m]), 'per_graph': per_graph,
            'graphs_per_request': graphs / len(m) if m else 0,
            'tokens_stable': all(r['tokens'] == m[0]['tokens'] for r in m)}
        pg = per_graph
        print(f"[{name}] size {size}: pp {med('prompt_per_second'):.1f} tg {med('predicted_per_second'):.1f} tok/s"
              f" | per graph: {pg.get('submissions', 0):.1f} submits, {pg.get('dispatches', 0):.0f} dispatches,"
              f" graph {pg.get('graph_ns', 0) / 1e3:.0f} us, submit {pg.get('submit_ns', 0) / 1e3:.0f} us,"
              f" fence wait {pg.get('fence_wait_ns', 0) / 1e3:.0f} us", flush=True)
    return result


def tg(r, size=128):
    return r['sizes'][str(size)]['predicted_per_second']['median']


def pp(r, size=512):
    return r['sizes'][str(size)]['prompt_per_second']['median']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant', choices=check.VARIANTS, required=True)
    ap.add_argument('--model-run', default=None)
    ap.add_argument('--host', default=os.environ.get('PS5_HOST'))
    ap.add_argument('--port', type=int, default=8081)
    ap.add_argument('--label', default='')
    ap.add_argument('--wait', type=int, default=300, help='seconds to wait for /health')
    ap.add_argument('--budget', type=int, default=540, help='stop starting configurations after this many seconds')
    ap.add_argument('--shutdown', action='store_true')
    ap.add_argument('--plan', choices=('full', 'cadence', 'mmvq'), default='full')
    args = ap.parse_args()
    if not args.host:
        raise SystemExit('set PS5_HOST or pass --host')
    base = f'http://{args.host}:{args.port}'
    v = dict(check.VARIANTS[args.variant])
    if args.model_run:
        v['model_run'] = args.model_run
    if not v['model_run']:
        raise SystemExit('pass --model-run for this variant')
    model_tokens, prompt_ids = check.model_run_tokens(v['model_run'])
    cpu, gaps = check.reference(v['reference'])
    check.wait_ready(base, args.wait)
    started = time.monotonic()
    passage = post(base, '/tokenize', {'content': bench.PASSAGE, 'add_special': True, 'parse_special': False})[0]['tokens']
    out = ROOT / 'test-results/ps5/api-tune' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True, exist_ok=True)
    report = {'variant': args.variant, 'model_run': v['model_run'], 'label': args.label,
              'base': 'http://<console>:%d' % args.port if args.host not in ('127.0.0.1', 'localhost') else base,
              'started': datetime.datetime.now().isoformat(timespec='seconds'), 'stats_start': stats(base),
              'configs': [], 'skipped': []}

    def save():
        (out / 'tune.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(name, knobs):
        if time.monotonic() - started > args.budget:
            report['skipped'].append(name)
            print(f'[{name}] skipped: time budget', flush=True)
            return None
        r = run_config(base, name, knobs, v, model_tokens, prompt_ids, cpu, gaps, passage)
        report['configs'].append(r)
        save()
        return r

    def best(results, key):
        good = [r for r in results if r and r['check']['pass']]
        return max(good, key=key) if good else None

    if args.plan == 'mmvq':
        # Integer-dot mat-vec off and on, alternated so drift shows.
        for n, k in (('defaults', {}), ('no_mmvq', {'disable_mmvq': 1}), ('defaults_again', {}),
                     ('no_mmvq_again', {'disable_mmvq': 1})):
            run(n, k)
        report['chosen'] = None
        report['stats_end'] = stats(base)
        report['elapsed_s'] = time.monotonic() - started
        save()
        return finish(args, base, report, out, {})
    if args.plan == 'cadence':
        # After the defaults changed: the build's defaults, then submit cadences around them.
        for n, k in (('defaults', {}), ('submit_25mflop', {'submit_mflop': 25}),
                     ('submit_100mflop', {'submit_mflop': 100}), ('submit_500n', {'nodes_per_submit': 500}),
                     ('submit_2000n', {'nodes_per_submit': 2000}), ('defaults_again', {})):
            run(n, k)
        report['chosen'] = None
        report['stats_end'] = stats(base)
        report['elapsed_s'] = time.monotonic() - started
        save()
        return finish(args, base, report, out, {})
    baseline = run('baseline', {})
    cadences = [baseline] + [run(n, k) for n, k in (
        ('submit_1000n_noflop', {'nodes_per_submit': 1000, 'submit_mflop': 0}),
        ('submit_300n_noflop', {'nodes_per_submit': 300, 'submit_mflop': 0}),
        ('submit_150n_noflop', {'nodes_per_submit': 150, 'submit_mflop': 0}),
        ('submit_1000n_autoflop', {'nodes_per_submit': 1000}))]
    cadence = best(cadences, tg)
    ck = {k: cadence['knobs'][k] for k in ('nodes_per_submit', 'submit_mflop')} if cadence else {}
    dmmv = run('cadence_dmmv_large', {**ck, 'dmmv_wg': 1})
    if dmmv and cadence and dmmv['check']['pass'] and tg(dmmv) > tg(cadence) * 1.01:
        ck['dmmv_wg'] = 1
    chunks = [run(f'prompt_batch_{n}', {**ck, 'prompt_batch': n}) for n in (64, 128, 256, 512)]
    chunk = best([r for r in chunks if r] + ([cadence] if cadence else []), pp)
    pk = {'prompt_batch': chunk['knobs']['prompt_batch']} if chunk and chunk['knobs']['prompt_batch'] != -1 else {}
    combos = [cadence]
    combos.append(run('fusion', {**ck, 'disable_fusion': 0}))
    combos.append(run('graph_optimize', {**ck, 'disable_graph_optimize': 0}))
    combos.append(run('fusion_graph_optimize', {**ck, 'disable_fusion': 0, 'disable_graph_optimize': 0}))
    top = best(combos, tg)
    final = dict(top['knobs']) if top else dict(BASE)
    final.update(pk)
    final_result = run('final_candidate', final)
    run('baseline_again', {})
    report['chosen'] = {'knobs': final, 'final_pass': bool(final_result and final_result['check']['pass'])}
    report['stats_end'] = stats(base)
    report['elapsed_s'] = time.monotonic() - started
    save()
    return finish(args, base, report, out, final)


def finish(args, base, report, out, final):
    lines = [f"{args.variant} {args.label}".strip(), '',
             '| config | check | pp 128 | pp 512 | tg 128 | tg 512 | submits/graph | dispatches/graph | graph us | submit us | fence us |',
             '|---|---|---|---|---|---|---|---|---|---|---|']
    for r in report['configs']:
        s128, s512 = r['sizes']['128'], r['sizes']['512']
        pg = s128['per_graph']
        c = r['check']
        lines.append(f"| {r['name']} | {'identical' if c['identical_to_model_mode'] else c['rule_vs_cpu']}"
                     f"{'' if c['pass'] else ' FAIL'} | {s128['prompt_per_second']['median']:.1f} | "
                     f"{s512['prompt_per_second']['median']:.1f} | {s128['predicted_per_second']['median']:.1f} | "
                     f"{s512['predicted_per_second']['median']:.1f} | {pg.get('submissions', 0):.1f} | "
                     f"{pg.get('dispatches', 0):.0f} | {pg.get('graph_ns', 0) / 1e3:.0f} | "
                     f"{pg.get('submit_ns', 0) / 1e3:.0f} | {pg.get('fence_wait_ns', 0) / 1e3:.0f} |")
    lines += ['', f"chosen: {json.dumps(final)}"]
    (out / 'table.md').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines) + f'\n{out}', flush=True)
    if args.shutdown:
        post(base, '/ps5lm/shutdown', {})
    return 0


if __name__ == '__main__':
    sys.exit(main())
