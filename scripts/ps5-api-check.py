#!/usr/bin/env python3
"""Correctness and health checks of the title's LAN API (server mode).

Run from the Mac while a server window is open (docs/MAC_DEV_LOOP.md):

    scripts/ps5-api-check.py --variant moe            # Granite, against the MoE model-mode run
    scripts/ps5-api-check.py --variant dense          # Qwen3.5 0.8B
    scripts/ps5-api-check.py --variant moe --host 127.0.0.1   # the host rehearsal

Checks, in order: /health and /v1/models; /tokenize of the model mode's prompt
gives the model mode's prompt tokens; a temperature-0 /completion of that prompt
(32 tokens) reproduces the model mode's GPU tokens, or passes the model mode's
GPU-vs-CPU rule against the CPU reference (first 8 tokens identical, or the
first difference where the CPU's top two logits are within 0.5, no run of 8
identical tokens); /v1/completions gives the same text; a short chat request;
a streaming chat request; the same completion again (identical tokens); a chat
request with float sampling settings; an Anthropic /v1/messages request with a
system message inside messages; /apply-template with a float in a tool schema;
optionally --soak N short completions in a row; and /health afterwards. Chat requests switch thinking off (Qwen3.5 would otherwise
spend the 32 tokens reasoning; Granite's template ignores the switch). Writes check.json to test-results/ps5/api-check/<time>/.
Only reads from the server, apart from optional --shutdown, which ends the
title's serving window (POST /ps5lm/shutdown).
"""
import argparse
import datetime
import json
import os
import pathlib
import re
import sys
import time
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
# The model mode's runs whose GPU tokens the API must reproduce (docs/CONSOLE.md).
VARIANTS = {
    'moe': {
        'model_run': 'test-results/ps5/PPSA99041/20261015-200238',
        'reference': 'build/native-moe/gen/model-reference.h',
        'prompt': '<|start_of_role|>user<|end_of_role|>In a short paragraph, explain how '
                  'a GPU differs from a CPU.<|end_of_text|>\n<|start_of_role|>assistant<|end_of_role|>',
    },
    # The MoE climb (docs/PORTING.md "Larger MoE models"): pass --model-run with the
    # model-mode run of the same model.
    'moe3b': {
        'model_run': None,
        'reference': 'build/native-moe3b/gen/model-reference.h',
        'prompt': '<|start_of_role|>user<|end_of_role|>In a short paragraph, explain how '
                  'a GPU differs from a CPU.<|end_of_text|>\n<|start_of_role|>assistant<|end_of_role|>',
    },
    'olmoe': {
        'model_run': None,
        'reference': 'build/native-olmoe/gen/model-reference.h',
        'prompt': '<|endoftext|><|user|>\nIn a short paragraph, explain how a GPU differs '
                  'from a CPU.\n<|assistant|>\n',
    },
    'qwen3moe': {
        'model_run': None,
        'reference': 'build/native-qwen3moe/gen/model-reference.h',
        'prompt': '<|im_start|>user\nIn a short paragraph, explain how a GPU differs '
                  'from a CPU.<|im_end|>\n<|im_start|>assistant\n',
    },
    'dense': {
        'model_run': 'test-results/ps5/PPSA99041/20261015-195528',
        'reference': 'build/native-model/gen/model-reference.h',
        'prompt': '<|im_start|>user\nIn a short paragraph, explain how a GPU differs '
                  'from a CPU.<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n',
    },
}
PREDICT = 32
# The mixed soak's medium prompt (~600 tokens): invented text, the benchmark passage's style.
MEDIUM_TEXT = ' '.join(
    f'Note {i}. A graphics processor runs thousands of small threads at once, grouped into waves that execute '
    f'the same instruction on different data, while a central processor runs a few threads very quickly. '
    f'Generating text reads all active weights once per token, so memory bandwidth limits it.'
    for i in range(1, 15))
PREFIX = 8
TIE_GAP = 0.5


def request(base, path, body=None, timeout=120, method=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data, method=method or ('POST' if data else 'GET'),
                                 headers={'Content-Type': 'application/json'})
    t = time.monotonic()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read()
        return r.status, json.loads(raw) if raw else None, time.monotonic() - t


def stream_chat(base, body, timeout=120):
    req = urllib.request.Request(base + '/v1/chat/completions', data=json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json'})
    chunks, text, done = 0, '', False
    with urllib.request.urlopen(req, timeout=timeout) as r:
        status = r.status
        for line in r:
            line = line.decode().strip()
            if not line.startswith('data:'):
                continue
            payload = line[5:].strip()
            if payload == '[DONE]':
                done = True
                break
            chunks += 1
            for choice in json.loads(payload).get('choices', []):
                text += (choice.get('delta') or {}).get('content') or ''
    return status, chunks, text, done


def wait_ready(base, seconds):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            status, body, _ = request(base, '/health', timeout=5)
            if status == 200:
                return body
        except (urllib.error.URLError, OSError, ValueError) as e:
            last = e
        time.sleep(2)
    raise SystemExit(f'server not ready after {seconds} s: {last}')


def model_run_tokens(run):
    out = (ROOT / run / 'native-model.out').read_text(errors='replace')
    summary = json.loads(re.findall(r'PS5LM_MODEL_SUMMARY (\{.*\})', out)[-1])
    prompt = json.loads(re.findall(r'PS5LM_MODEL_PROMPT (\{.*\})', out)[-1])
    return summary['tokens'], prompt['ids']


def reference_prompt_ids(header):
    """Without a model-mode run: the prompt ids the host CPU reference tokenized, from the
    reference run whose model-reference.h is this header (scripts/build-host-model-reference.sh)."""
    text = (ROOT / header).read_text()
    for run in sorted((ROOT / 'test-results/local/model-reference').glob('*/'), reverse=True):
        h = run / 'model-reference.h'
        if h.exists() and h.read_text() == text:
            out = (run / 'output.log').read_text(errors='replace')
            return json.loads(re.findall(r'PS5LM_MODEL_PROMPT (\{.*\})', out)[-1])['ids'], str(run.relative_to(ROOT))
    raise SystemExit(f'no reference run matches {header}')


def reference(header):
    text = (ROOT / header).read_text()
    tokens = [int(x) for x in re.search(r'ps5lm_reference_tokens\[\d+\] = \{([^}]*)\}', text).group(1).split(',')]
    gaps = [float(x.rstrip('f')) for x in re.search(r'ps5lm_reference_gaps\[\d+\] = \{([^}]*)\}', text).group(1).split(',')]
    return tokens, gaps


def longest_repeat(tokens):
    best = run = 0
    for i, t in enumerate(tokens):
        run = run + 1 if i and t == tokens[i - 1] else 1
        best = max(best, run)
    return best


def rule(tokens, cpu, gaps):
    first = next((i for i, (a, b) in enumerate(zip(tokens, cpu)) if a != b), None)
    if first is None and len(tokens) < len(cpu):
        first = len(tokens)
    if first is None:
        return 'identical', first
    if first >= PREFIX:
        return 'prefix', first
    if first < len(gaps) and gaps[first] <= TIE_GAP:
        return 'near_tie', first
    return 'fail', first


def measured_variant(name):
    """A measured model (docs/PORTING.md "Measured models"): its prompt from
    scripts/model-variants.sh, its CPU reference from build/native-m-NAME/gen/."""
    if not name.startswith('m-'):
        raise SystemExit(f'unknown variant {name}')
    import subprocess
    prompt = subprocess.run(['bash', '-c', 'source "$1"; ps5lm_model_variant "$2" >/dev/null || exit 2; '
                             'for f in "${mv_flags[@]}"; do case $f in -DPS5LM_MODEL_PROMPT=*) printf %s "$f";; esac; done',
                             '_', str(ROOT / 'scripts/model-variants.sh'), name[2:]],
                            check=True, capture_output=True, text=True).stdout
    prompt = prompt.removeprefix('-DPS5LM_MODEL_PROMPT="').removesuffix('"').replace('\\n', '\n')
    # A long-context entry (NAME-cNk[-kvqN], docs/PORTING.md "Long context") checks against its base
    # entry's CPU reference: the short prompt does not depend on the context size.
    base = re.sub(r'-c\d+k(-kvq[48])?$', '', name)
    return {'model_run': None, 'reference': f'build/native-{base}/gen/model-reference.h', 'prompt': prompt}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant', required=True,
                    help=f"{', '.join(VARIANTS)}, or m-NAME for an entry of scripts/model-variants.sh")
    ap.add_argument('--host', default=os.environ.get('PS5_HOST'))
    ap.add_argument('--port', type=int, default=8081)
    ap.add_argument('--wait', type=int, default=180, help='seconds to wait for /health')
    ap.add_argument('--shutdown', action='store_true', help='POST /ps5lm/shutdown at the end')
    ap.add_argument('--out', default=None)
    ap.add_argument('--model-run', default=None, help='model-mode run directory (test-results/ps5/PPSA99041/<time>)')
    ap.add_argument('--soak', type=int, default=0, help='N short completions in a row at the end (0: none)')
    ap.add_argument('--soak-mix', action='store_true',
                    help='soak with short and medium prompts alternating and sampled (float) settings, every '
                         'request timed with its server timings and wall-clock time')
    args = ap.parse_args()
    if not args.host:
        raise SystemExit('set PS5_HOST or pass --host')
    base = f'http://{args.host}:{args.port}'
    v = dict(VARIANTS[args.variant]) if args.variant in VARIANTS else measured_variant(args.variant)
    if args.model_run:
        v['model_run'] = args.model_run
    if v['model_run']:
        gpu_tokens, prompt_ids = model_run_tokens(v['model_run'])
    elif args.variant.startswith('m-'):
        # Measured models without a model-mode run (docs/PORTING.md "Measured models"): the
        # completions must pass the model mode's rule against the CPU reference by themselves.
        gpu_tokens = None
        prompt_ids, v['model_run'] = reference_prompt_ids(v['reference'])
    else:
        raise SystemExit('pass --model-run for this variant')
    cpu_tokens, gaps = reference(v['reference'])
    out = pathlib.Path(args.out) if args.out else (
        ROOT / 'test-results/ps5/api-check' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True, exist_ok=True)
    checks, result = [], {'variant': args.variant, 'model_run': v['model_run'], 'reference': v['reference']}

    def check(name, ok, **detail):
        checks.append({'check': name, 'pass': bool(ok), **detail})
        print(f"{'PASS' if ok else 'FAIL'} {name} {json.dumps(detail)[:300]}")

    result['health_initial'] = wait_ready(base, args.wait)
    status, models, _ = request(base, '/v1/models')
    ids = [m.get('id') for m in (models or {}).get('data', [])]
    check('v1_models', status == 200 and len(ids) == 1, ids=ids)

    status, tok, _ = request(base, '/tokenize', {'content': v['prompt'], 'add_special': True, 'parse_special': True})
    api_ids = (tok or {}).get('tokens')
    check('tokenize_prompt', api_ids == prompt_ids, api=api_ids, model_mode=prompt_ids)

    body = {'prompt': v['prompt'], 'n_predict': PREDICT, 'temperature': 0, 'cache_prompt': False,
            'return_tokens': True, 'n_probs': 2, 'stream': False}
    runs = []
    for attempt in (1, 2):
        status, comp, wall = request(base, '/completion', body)
        tokens = comp.get('tokens', [])
        r, first = rule(tokens, cpu_tokens, gaps)
        runs.append({'status': status, 'tokens': tokens, 'text': comp.get('content'), 'wall_s': wall,
                     'timings': comp.get('timings'), 'tokens_evaluated': comp.get('tokens_evaluated'),
                     'rule_vs_cpu': r, 'first_diff_vs_cpu': first,
                     'identical_to_model_mode_gpu': tokens == gpu_tokens,
                     'longest_repeat': longest_repeat(tokens),
                     'top2': [[(p.get('id'), p.get('logprob', p.get('prob'))) for p in step.get('top_logprobs', step.get('top_probs', []))]
                              for step in comp.get('completion_probabilities', [])]})
        ok = status == 200 and len(tokens) == PREDICT and (tokens == gpu_tokens or r != 'fail') \
            and longest_repeat(tokens) < 8
        check(f'completion_{attempt}', ok, identical_to_model_mode_gpu=tokens == gpu_tokens, rule_vs_cpu=r,
              first_diff_vs_cpu=first, prompt_n=(comp.get('timings') or {}).get('prompt_n'),
              text=comp.get('content'))
        if attempt == 1:
            status, oai, _ = request(base, '/v1/completions', {'prompt': v['prompt'], 'max_tokens': PREDICT,
                                                              'temperature': 0, 'cache_prompt': False})
            text = ((oai or {}).get('choices') or [{}])[0].get('text')
            check('v1_completions_same_text', status == 200 and text == comp.get('content'), text=text)
            status, chat, _ = request(base, '/v1/chat/completions', {
                'messages': [{'role': 'user', 'content': 'Name three primary colors. Answer in one short line.'}],
                'max_tokens': 32, 'temperature': 0, 'chat_template_kwargs': {'enable_thinking': False}})
            content = (((chat or {}).get('choices') or [{}])[0].get('message') or {}).get('content') or ''
            result['chat'] = chat
            check('v1_chat', status == 200 and len(content.strip()) > 0, content=content)
            status, chunks, text, done = stream_chat(base, {
                'messages': [{'role': 'user', 'content': 'Count from one to five in words.'}],
                'max_tokens': 32, 'temperature': 0, 'stream': True,
                'chat_template_kwargs': {'enable_thinking': False}})
            check('v1_chat_stream', status == 200 and done and chunks >= 2 and text.strip(), chunks=chunks,
                  done=done, text=text)
    result['completions'] = runs
    check('second_completion_identical', runs[0]['tokens'] == runs[1]['tokens'])

    # Floats in a request: the console's localeconv() once took the server down
    # on "temperature": 0.7 (net-compat.c, ps5lm_localeconv). Sampled, so only
    # the answer and the server's health afterwards are checked, not the text.
    status, chat, _ = request(base, '/v1/chat/completions', {
        'messages': [{'role': 'user', 'content': 'Name a fruit. One word.'}],
        'max_tokens': 16, 'temperature': 0.7, 'top_p': 0.9, 'min_p': 0.05, 'repeat_penalty': 1.1,
        'seed': 1, 'chat_template_kwargs': {'enable_thinking': False}})
    content = (((chat or {}).get('choices') or [{}])[0].get('message') or {}).get('content') or ''
    status_h, _, _ = request(base, '/health')
    check('v1_chat_float_params', status == 200 and content.strip() and status_h == 200, content=content)

    # Anthropic API with a system message inside messages, as Claude Code sends
    # (patches/server/0004): templates such as Qwen's refuse it unless folded.
    status, msg, _ = request(base, '/v1/messages', {
        'model': 'ps5lm', 'max_tokens': 48, 'temperature': 0, 'system': 'You answer briefly.',
        'messages': [{'role': 'user', 'content': 'Say hello.'},
                     {'role': 'assistant', 'content': 'Hello.'},
                     {'role': 'system', 'content': 'Answer in English.'},
                     {'role': 'user', 'content': 'Name a color.'}]})
    blocks = (msg or {}).get('content') or []
    check('anthropic_messages_mid_system', status == 200 and (msg or {}).get('type') == 'message' and len(blocks) > 0,
          blocks=[b.get('type') for b in blocks], stop_reason=(msg or {}).get('stop_reason'))

    # tojson of a float in a chat template (patches/server/0005): recorded, not
    # failed, since only templates that print tool schemas show it.
    status, applied, _ = request(base, '/apply-template', {
        'messages': [{'role': 'user', 'content': 'Weather?'}],
        'tools': [{'type': 'function', 'function': {'name': 'weather', 'description': 'Weather',
                   'parameters': {'type': 'object', 'properties': {'scale': {'type': 'number', 'default': 1.0}}}}}]})
    prompt = (applied or {}).get('prompt') or ''
    shown = '1.0' in prompt
    check('apply_template_tools', status == 200, float_written_as_python=shown if 'scale' in prompt else None)

    # Many requests in a row (upstream saw a decision model stop after ~55):
    # one bounded series, every answer and its time recorded.
    if args.soak and args.soak_mix:
        # Mixed soak (docs/PORTING.md "Long context", the 9B stall): even requests are the
        # short prompt (8 tokens), odd ones a medium prompt (~600 tokens of the benchmark
        # passage plus a question, 64 tokens); all sampled with float settings and a fixed
        # seed per request. A request over 10x the median is listed with its time.
        medium = v['prompt'].replace('In a short paragraph, explain how a GPU differs from a CPU.',
                                     MEDIUM_TEXT + '\n\nIn two sentences, what limits text generation speed?') \
            if 'In a short paragraph, explain how a GPU differs from a CPU.' in v['prompt'] else MEDIUM_TEXT
        rows, failures = [], []
        for i in range(args.soak):
            body = {'prompt': v['prompt'] if i % 2 == 0 else medium, 'n_predict': 8 if i % 2 == 0 else 64,
                    'temperature': 0.7, 'top_p': 0.9, 'min_p': 0.05, 'repeat_penalty': 1.05, 'seed': 1000 + i,
                    'cache_prompt': False}
            at = datetime.datetime.now().isoformat(timespec='milliseconds')
            try:
                status, comp, wall = request(base, '/completion', body, timeout=120)
                t = (comp or {}).get('timings') or {}
                rows.append({'i': i, 'kind': 'short' if i % 2 == 0 else 'medium', 'at': at, 'status': status,
                             'wall_s': round(wall, 3), 'prompt_n': t.get('prompt_n'), 'prompt_ms': t.get('prompt_ms'),
                             'predicted_n': t.get('predicted_n'), 'predicted_ms': t.get('predicted_ms')})
                if status != 200 or not (comp or {}).get('content'):
                    failures.append({'request': i, 'status': status, 'at': at})
            except (urllib.error.URLError, OSError, ValueError) as e:
                failures.append({'request': i, 'error': str(e), 'at': at})
                break
        walls = [r['wall_s'] for r in rows]
        med = {k: sorted(r['wall_s'] for r in rows if r['kind'] == k)[len([r for r in rows if r['kind'] == k]) // 2]
               for k in ('short', 'medium') if any(r['kind'] == k for r in rows)}
        slow = [r for r in rows if r['wall_s'] > 10 * med[r['kind']]]
        result['soak'] = {'requested': args.soak, 'completed': len(rows), 'failures': failures, 'mix': True,
                          'median_wall_s': med, 'over_10x_median': slow, 'max_wall_s': max(walls) if walls else None,
                          'requests': rows}
        check('soak_mix', not failures and len(rows) == args.soak, completed=len(rows), failures=failures[:3],
              median_wall_s=med, max_wall_s=max(walls) if walls else None, over_10x_median=len(slow))
    elif args.soak:
        times, failures = [], []
        for i in range(args.soak):
            try:
                status, comp, wall = request(base, '/completion', {
                    'prompt': v['prompt'], 'n_predict': 8, 'temperature': 0, 'cache_prompt': False}, timeout=60)
                times.append(round(wall, 3))
                if status != 200 or not (comp or {}).get('content'):
                    failures.append({'request': i, 'status': status})
            except (urllib.error.URLError, OSError, ValueError) as e:
                failures.append({'request': i, 'error': str(e)})
                break
        result['soak'] = {'requested': args.soak, 'completed': len(times), 'failures': failures, 'wall_s': times}
        check('soak', not failures and len(times) == args.soak, completed=len(times), failures=failures[:3],
              max_wall_s=max(times) if times else None,
              median_wall_s=sorted(times)[len(times) // 2] if times else None)
    status, health, _ = request(base, '/health')
    check('health_after', status == 200, health=health)
    if args.shutdown:
        status, body, _ = request(base, '/ps5lm/shutdown', {})
        check('shutdown_request', status == 200, response=body)
    result['checks'] = checks
    result['pass'] = all(c['pass'] for c in checks)
    (out / 'check.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"{'PASS' if result['pass'] else 'FAIL'}: {sum(c['pass'] for c in checks)}/{len(checks)} checks; {out}/check.json")
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    sys.exit(main())
