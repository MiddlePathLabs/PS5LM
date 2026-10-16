#!/usr/bin/env python3
"""One-image check and benchmark through the title's OpenAI API (vision builds).

    scripts/ps5-api-vision.py --model-run test-results/ps5/PPSA99041/<model-mode run>
    scripts/ps5-api-vision.py --host 127.0.0.1 --model-run ...   # the host rehearsal

Sends the fixed test image (testdata/vision/hello-shapes-448.png) as a base64
data URL in an OpenAI /v1/chat/completions request with the model mode's
prompt, temperature 0, the prompt cache off (so every request encodes the
image again), and:

1. check: up to 64 tokens with their ids (logprobs); the reply must contain
   HELLO and use 224 prompt tokens (28 text + 196 image), and its tokens must
   equal the model mode's GPU tokens (its native-model.out) or pass the model
   mode's rule against the CPU reference (--reference, reference.json of
   scripts/build-host-vision-reference.sh): first 8 tokens equal, or the first
   difference where the CPU's top two logits are within 0.5. The server
   decodes the image in one 512-token batch, the model mode in batches of 32,
   so a near tie may go either way;
2. benchmark: one warm-up and three repetitions generating exactly 64 tokens
   (ignore_eos): llama-server's timings (prompt_ms covers the image encode and
   the image/text prompt decode; predicted_per_second) and the wall time.

Writes vision.json and table.md to test-results/ps5/api-vision/<time>/.
Exits 1 if the check fails. The text benchmarks (ps5-api-bench.py) are separate
and unchanged.
"""
import argparse
import base64
import datetime
import json
import os
import pathlib
import re
import statistics
import sys
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
IMAGE = ROOT / 'testdata/vision/hello-shapes-448.png'
QUESTION = 'Describe this image in one sentence. Then answer: what word is written in the image?'
ANSWER = 'HELLO'
PROMPT_TOKENS = 224
CHECK_TOKENS = 64
BENCH_TOKENS = 64
REPEATS = 3
WARMUP = 1


def post(base, path, body, timeout=600):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'})
    t = time.monotonic()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read()), time.monotonic() - t


def get(base, path, timeout=10):
    with urllib.request.urlopen(base + path, timeout=timeout) as r:
        return json.loads(r.read())


def wait_health(base, limit_s):
    end = time.monotonic() + limit_s
    while time.monotonic() < end:
        try:
            if get(base, '/health').get('status') == 'ok':
                return True
        except Exception:
            pass
        time.sleep(2)
    return False


def model_mode(run):
    """The model mode's GPU text (without the end-of-turn token) and token ids."""
    text = tokens = None
    for line in (pathlib.Path(run) / 'native-model.out').read_text(errors='replace').splitlines():
        if line.startswith('PS5LM_MODEL_TEXT '):
            text = json.loads(line.split(' ', 1)[1])['text']
        if line.startswith('PS5LM_MODEL_SUMMARY '):
            tokens = json.loads(line.split(' ', 1)[1])['tokens']
    if text is None or tokens is None:
        raise SystemExit(f'no PS5LM_MODEL_TEXT/SUMMARY in {run}/native-model.out')
    return re.sub(r'<\|im_end\|>$', '', text), tokens


def rule(ids, ref_tokens, ref_gaps):
    """The text models' rule against the CPU reference, on the API's token ids."""
    n = min(len(ids), len(ref_tokens))
    diff = next((i for i in range(n) if ids[i] != ref_tokens[i]), None)
    if diff is None:
        return 'identical' if len(ids) >= len(ref_tokens) - 1 else 'fail', diff
    if diff >= 8:
        return 'prefix', diff
    return ('near_tie' if ref_gaps[diff] <= 0.5 else 'fail'), diff


def request(base, max_tokens, ignore_eos, logprobs=False):
    url = 'data:image/png;base64,' + base64.b64encode(IMAGE.read_bytes()).decode()
    body = {'messages': [{'role': 'user', 'content': [
                {'type': 'image_url', 'image_url': {'url': url}},
                {'type': 'text', 'text': QUESTION}]}],
            'temperature': 0, 'max_tokens': max_tokens, 'cache_prompt': False, 'stream': False}
    if ignore_eos:
        body['ignore_eos'] = True
    if logprobs:
        body['logprobs'] = True
        body['top_logprobs'] = 2
    resp, wall = post(base, '/v1/chat/completions', body)
    return resp, wall


def summary(values):
    return {'median': statistics.median(values), 'min': min(values), 'max': max(values), 'n': len(values)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default=os.environ.get('PS5_HOST'))
    ap.add_argument('--port', type=int, default=8081)
    ap.add_argument('--model-run', required=True, help='the model mode run whose tokens the API must reproduce')
    ap.add_argument('--reference', required=True, help='reference.json of the CPU reference (tokens and gaps)')
    ap.add_argument('--wait', type=int, default=240, help='seconds to wait for /health')
    ap.add_argument('--label', default='')
    args = ap.parse_args()
    if not args.host:
        raise SystemExit('set PS5_HOST or pass --host')
    base = f'http://{args.host}:{args.port}'
    if not wait_health(base, args.wait):
        raise SystemExit('server not healthy')
    expected, gpu_tokens = model_mode(args.model_run)
    ref = json.loads(pathlib.Path(args.reference).read_text())
    ref_tokens, ref_gaps = ref['reference']['tokens'], ref['gaps']
    out = ROOT / 'test-results/ps5/api-vision' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True, exist_ok=True)
    result = {'base': 'http://<console>:%d' % args.port if args.host not in ('127.0.0.1', 'localhost') else base,
              'label': args.label, 'started': datetime.datetime.now().isoformat(timespec='seconds'),
              'model': [m.get('id') for m in get(base, '/v1/models').get('data', [])],
              'image': str(IMAGE.relative_to(ROOT)), 'question': QUESTION, 'model_run': str(args.model_run),
              'settings': {'temperature': 0, 'cache_prompt': False, 'check_tokens': CHECK_TOKENS,
                           'bench_tokens': BENCH_TOKENS, 'warmup': WARMUP, 'repeats': REPEATS}}
    # 1. Check.
    resp, wall = request(base, CHECK_TOKENS, False, logprobs=True)
    text = resp['choices'][0]['message']['content']
    t = resp.get('timings', {})
    usage = resp.get('usage', {})
    ids = [c.get('id') for c in ((resp['choices'][0].get('logprobs') or {}).get('content') or [])]
    gaps = []
    for c in ((resp['choices'][0].get('logprobs') or {}).get('content') or []):
        top = c.get('top_logprobs') or []
        gaps.append(top[0]['logprob'] - top[1]['logprob'] if len(top) >= 2 else None)
    cpu_rule, cpu_diff = rule(ids, ref_tokens, ref_gaps) if ids and None not in ids else ('no_ids', None)
    same_as_model = bool(ids) and ids == gpu_tokens[:len(ids)] and len(ids) >= len(gpu_tokens) - 1
    check = {'text': text, 'model_mode_text': expected, 'text_identical_to_model_mode': text == expected,
             'ids': ids, 'tokens_identical_to_model_mode': same_as_model, 'rule_vs_cpu': cpu_rule,
             'first_diff_vs_cpu': cpu_diff,
             'cpu_gap_at_diff': ref_gaps[cpu_diff] if cpu_diff is not None and cpu_diff < len(ref_gaps) else None,
             'api_logprob_gap_at_diff': gaps[cpu_diff] if cpu_diff is not None and cpu_diff < len(gaps) else None,
             'answer_ok': ANSWER in text.upper(), 'prompt_tokens': usage.get('prompt_tokens'),
             'completion_tokens': usage.get('completion_tokens'), 'finish_reason': resp['choices'][0].get('finish_reason'),
             'timings': t, 'wall_s': wall}
    check['pass'] = ((same_as_model or cpu_rule in ('identical', 'prefix', 'near_tie')) and check['answer_ok']
                     and check['prompt_tokens'] == PROMPT_TOKENS)
    result['check'] = check
    print(f"check: pass={check['pass']} same_as_model_mode={same_as_model} rule_vs_cpu={cpu_rule} "
          f"(first diff {cpu_diff}, cpu gap {check['cpu_gap_at_diff']}) answer_ok={check['answer_ok']} "
          f"prompt_tokens={check['prompt_tokens']} wall {wall:.2f} s\n  {text!r}", flush=True)
    # 2. Benchmark.
    rows = []
    for i in range(WARMUP + REPEATS):
        resp, wall = request(base, BENCH_TOKENS, True)
        t = resp['timings']
        row = {'warmup': i < WARMUP, 'wall_s': wall, 'prompt_n': t['prompt_n'], 'prompt_ms': t['prompt_ms'],
               'predicted_n': t['predicted_n'], 'predicted_per_second': t['predicted_per_second'],
               'prompt_tokens': resp.get('usage', {}).get('prompt_tokens')}
        rows.append(row)
        print(f"{'warm-up' if row['warmup'] else 'rep'} {i}: image prompt {t['prompt_ms']:.1f} ms "
              f"({t['prompt_n']} tokens processed), tg {t['predicted_per_second']:.1f} tok/s, wall {wall:.2f} s",
              flush=True)
        if t['predicted_n'] != BENCH_TOKENS:
            raise SystemExit(f'unexpected generated count: {t}')
    measured = [r for r in rows if not r['warmup']]
    result['bench'] = {'runs': rows, 'prompt_ms': summary([r['prompt_ms'] for r in measured]),
                       'predicted_per_second': summary([r['predicted_per_second'] for r in measured]),
                       'wall_s': summary([r['wall_s'] for r in measured])}
    result['health_after'] = get(base, '/health')
    b = result['bench']
    f = lambda d, p: f"{d['median']:.{p}f} ({d['min']:.{p}f}-{d['max']:.{p}f})"
    table = (f"Model {', '.join(result['model'])}, one 448x448 image ({PROMPT_TOKENS} prompt tokens), "
             f"temperature 0, {WARMUP} warm-up + {REPEATS} repetitions {args.label}".rstrip() + '\n\n'
             '| check | image prompt ms median (min-max) | generation tok/s median (min-max) | wall s median (min-max) |\n'
             '|---|---|---|---|\n'
             f"| {'pass' if check['pass'] else 'FAIL'} | {f(b['prompt_ms'], 1)} | {f(b['predicted_per_second'], 1)} | "
             f"{f(b['wall_s'], 2)} |\n")
    (out / 'vision.json').write_text(json.dumps(result, indent=2) + '\n')
    (out / 'table.md').write_text(table)
    print(table + str(out))
    return 0 if check['pass'] else 1


if __name__ == '__main__':
    sys.exit(main())
