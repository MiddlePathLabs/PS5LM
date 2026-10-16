#!/usr/bin/env python3
"""Repeatable prompt-processing and generation benchmark through the title's LAN API.

    scripts/ps5-api-bench.py                     # http://$PS5_HOST:8081
    scripts/ps5-api-bench.py --host 127.0.0.1    # the host rehearsal
    scripts/ps5-api-bench.py --depth 7936 --warmup 0 --repeats 1   # long context (docs/PORTING.md
                                                 # "Long context"): prompt of N tokens, 128 generated

For each prompt size (128 and 512 tokens): one warm-up request, then 3
measured repetitions; each request is a temperature-0 /completion that
generates exactly 128 tokens (ignore_eos) with the prompt cache off, so every
repetition processes the whole prompt. Prompts are the first N tokens of a
fixed passage (below), tokenized by the server's own model, sent as token ids.
Reads llama-server's own `timings` (prompt_per_second, predicted_per_second)
and the wall time of the HTTP request. With --depth N[,N...] the prompts are the
first N tokens of the passage continued with more sections of the same text
(the first 512 tokens are the default prompts' tokens), and the title's longest
fence wait and graph per request are recorded too (GET /ps5lm/stats after
POST /ps5lm/knobs reset_max); the defaults are unchanged. Writes bench.json and table.md to
test-results/ps5/api-bench/<time>/ and prints the table (median, min, max).
"""
import argparse
import datetime
import json
import os
import pathlib
import statistics
import sys
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
SIZES = (128, 512)
GENERATE = 128
REPEATS = 3
WARMUP = 1
# A fixed passage, long enough for 512 tokens in any of our tokenizers. Never
# change it between runs that are compared.
PASSAGE = ' '.join(
    f'Section {i}. A graphics processor runs thousands of small threads at once, grouped into waves '
    f'that execute the same instruction on different data, while a central processor runs a few '
    f'threads very quickly with deep caches, branch prediction and out-of-order execution. Memory '
    f'bandwidth, not arithmetic, usually limits the speed of generating text with a language model, '
    f'because every generated token reads all of the active weights once. Processing a prompt is '
    f'different: many tokens share each weight read, so it is limited by arithmetic instead.'
    for i in range(1, 13))


def long_passage(sections):
    """PASSAGE continued with more sections of the same text (its first 12 sections are PASSAGE)."""
    return ' '.join(
        f'Section {i}. A graphics processor runs thousands of small threads at once, grouped into waves '
        f'that execute the same instruction on different data, while a central processor runs a few '
        f'threads very quickly with deep caches, branch prediction and out-of-order execution. Memory '
        f'bandwidth, not arithmetic, usually limits the speed of generating text with a language model, '
        f'because every generated token reads all of the active weights once. Processing a prompt is '
        f'different: many tokens share each weight read, so it is limited by arithmetic instead.'
        for i in range(1, sections + 1))


def post(base, path, body, timeout=600):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'})
    t = time.monotonic()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read()), time.monotonic() - t


def get(base, path, timeout=30):
    with urllib.request.urlopen(base + path, timeout=timeout) as r:
        return json.loads(r.read())


def summary(values):
    return {'median': statistics.median(values), 'min': min(values), 'max': max(values), 'n': len(values)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default=os.environ.get('PS5_HOST'))
    ap.add_argument('--port', type=int, default=8081)
    ap.add_argument('--label', default='')
    ap.add_argument('--out', default=None)
    ap.add_argument('--depth', default='', help='comma-separated prompt lengths in tokens (long context)')
    ap.add_argument('--warmup', type=int, default=WARMUP)
    ap.add_argument('--repeats', type=int, default=REPEATS)
    ap.add_argument('--timeout', type=int, default=600, help='seconds per request')
    args = ap.parse_args()
    sizes = tuple(int(x) for x in args.depth.split(',') if x) or SIZES
    deep = bool(args.depth)
    if not args.host:
        raise SystemExit('set PS5_HOST or pass --host')
    base = f'http://{args.host}:{args.port}'
    health = get(base, '/health')
    models = get(base, '/v1/models')
    props = get(base, '/props')
    passage = PASSAGE
    if deep:
        sections = 12
        while True:
            passage = long_passage(sections)
            tokens = post(base, '/tokenize', {'content': passage, 'add_special': True, 'parse_special': False},
                          timeout=300)[0]['tokens']
            if len(tokens) >= max(sizes):
                break
            sections = sections * 2
    tokens = post(base, '/tokenize', {'content': passage, 'add_special': True, 'parse_special': False},
                  timeout=300)[0]['tokens']
    if len(tokens) < max(sizes):
        raise SystemExit(f'passage is only {len(tokens)} tokens')
    out = pathlib.Path(args.out) if args.out else (
        ROOT / 'test-results/ps5/api-bench' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True, exist_ok=True)
    result = {'base': 'http://<console>:%d' % args.port if args.host not in ('127.0.0.1', 'localhost') else base,
              'label': args.label, 'started': datetime.datetime.now().isoformat(timespec='seconds'),
              'health': health, 'model': [m.get('id') for m in models.get('data', [])],
              'n_ctx': (props.get('default_generation_settings') or {}).get('n_ctx'),
              'settings': {'sizes': sizes, 'generate': GENERATE, 'repeats': args.repeats, 'warmup': args.warmup,
                           'temperature': 0, 'cache_prompt': False, 'ignore_eos': True},
              'passage_tokens': len(tokens), 'sizes': {}}
    for size in sizes:
        prompt = tokens[:size]
        rows = []
        for i in range(args.warmup + args.repeats):
            body = {'prompt': prompt, 'n_predict': GENERATE, 'temperature': 0, 'cache_prompt': False,
                    'ignore_eos': True, 'stream': False}
            if deep:
                try:
                    post(base, '/ps5lm/knobs', {'reset_max': 1})
                except OSError:
                    pass
            resp, wall = post(base, '/completion', body, timeout=args.timeout)
            t = resp['timings']
            row = {'warmup': i < args.warmup, 'wall_s': wall, 'prompt_n': t['prompt_n'], 'prompt_ms': t['prompt_ms'],
                   'prompt_per_second': t['prompt_per_second'], 'predicted_n': t['predicted_n'],
                   'predicted_ms': t['predicted_ms'], 'predicted_per_second': t['predicted_per_second'],
                   'first_tokens': resp.get('tokens', [])[:8] or None, 'text_head': resp.get('content', '')[:80]}
            if t['prompt_n'] != size or t['predicted_n'] != GENERATE:
                raise SystemExit(f'unexpected counts at size {size}: {t}')
            if deep:
                try:
                    vk = (get(base, '/ps5lm/stats') or {}).get('vk') or {}
                    row['max'] = {k: vk.get(k) for k in ('max_fence_wait_ns', 'max_graph_ns', 'max_submit_ns')}
                except OSError:
                    pass
            rows.append(row)
            print(f"size {size} {'warm-up' if row['warmup'] else 'rep'} {i}: pp {t['prompt_per_second']:.1f} tok/s, "
                  f"tg {t['predicted_per_second']:.1f} tok/s, wall {wall:.2f} s", flush=True)
        measured = [r for r in rows if not r['warmup']]
        result['sizes'][str(size)] = {
            'runs': rows,
            'prompt_per_second': summary([r['prompt_per_second'] for r in measured]),
            'predicted_per_second': summary([r['predicted_per_second'] for r in measured]),
            'wall_s': summary([r['wall_s'] for r in measured]),
        }
    result['health_after'] = get(base, '/health')
    lines = [f"Model {', '.join(result['model'])}, n_ctx {result['n_ctx']}, {GENERATE} generated tokens, "
             f"temperature 0, {args.warmup} warm-up + {args.repeats} repetitions {args.label}".rstrip(), '',
             '| prompt tokens | prompt tok/s median (min-max) | generation tok/s median (min-max) | wall s median (min-max) |',
             '|---|---|---|---|']
    for size in sizes:
        s = result['sizes'][str(size)]
        f = lambda d, p: f"{d['median']:.{p}f} ({d['min']:.{p}f}-{d['max']:.{p}f})"
        lines.append(f"| {size} | {f(s['prompt_per_second'], 1)} | {f(s['predicted_per_second'], 1)} | "
                     f"{f(s['wall_s'], 2)} |")
    table = '\n'.join(lines) + '\n'
    (out / 'bench.json').write_text(json.dumps(result, indent=2) + '\n')
    (out / 'table.md').write_text(table)
    print(table + f'{out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
