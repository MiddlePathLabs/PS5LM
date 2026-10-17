#!/usr/bin/env python3
"""Long-context comprehension benchmark (docs/PORTING.md "Evaluation builds").

Phase B of the long-context evaluation: deterministic, version-controlled fixtures
(testdata/lc-comprehension/corpus.json, built by scripts/ps5-lc-fixtures.py) asked at
~16k/32k/64k input tokens. Two modes, like ps5-api-longctx.py:

  host    build the prompts against the CPU rehearsal of the same variant (binary search
          over the filler count to hit the rung's target length) and write a manifest
          with the exact token ids:
            scripts/ps5-lc-comprehension.py --variant m-qwen35-9b-c32k-q --host 127.0.0.1 \
                --manifest-out test-results/local/lc-comprehension/m-qwen35-9b-c32k-q.json
  console verify the ids against the manifest, run every case, grade deterministically,
          write comprehension.json + table.md:
            scripts/ps5-lc-comprehension.py --variant m-qwen35-9b-c32k-q \
                --manifest test-results/local/lc-comprehension/m-qwen35-9b-c32k-q.json

Scoring (deterministic graders; no LLM judge): the answer line is the text after the last
"Answer:"; numeric graders extract numbers from it, exact value within tolerance, wrong
totals must be absent; citation graders require the supporting document IDs in the reply
and report wrongly-cited critical IDs; refusal graders require a refusal marker and no
invented duration. Every reply is preserved in the JSON for manual inspection.
Per-request wall times and the title's counters (GET /ps5lm/stats) are recorded, and any
request over 10x the median wall is flagged (the 9B stall watch, Phase D).
"""
import argparse
import datetime
import importlib.util
import json
import os
import pathlib
import re
import statistics
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORPUS = ROOT / 'testdata/lc-comprehension/corpus.json'
RESERVE = 320            # tokens left for the question block and the answer
ALL_CRITICAL_IDS = [f'CFR-{p}-{n:04d}' for p, n in
                    [('D', 201), ('D', 233), ('D', 287), ('D', 114), ('D', 119), ('T', 221),
                     ('T', 189), ('L', 301), ('L', 318), ('L', 342), ('A', 51), ('A', 93),
                     ('D', 356), ('D', 361), ('Q', 7)]]


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


lc = _load('lc', ROOT / 'scripts/ps5-api-longctx.py')


def load_fixture_builder():
    return _load('fx', ROOT / 'scripts/ps5-lc-fixtures.py')


# ---- graders
def answer_line(text):
    m = list(re.finditer(r'(?im)^\s*\*{0,2}answer\*{0,2}\s*:\s*(.*)$', text))
    if m:
        return m[-1].group(1).strip()
    lines = [l for l in text.strip().splitlines() if l.strip()]
    return lines[-1].strip() if lines else ''


def numbers_in(s):
    out = []
    for tok in re.findall(r'-?\d+(?:\.\d+)?', s.replace(',', '')):
        v = float(tok)
        out.append(int(v) if v.is_integer() else v)
    return out


def grade(case, reply):
    g = case['grader']
    line = answer_line(reply)
    r = {'answer_line': line, 'checks': {}, 'pass': False}
    if g['type'] == 'numeric':
        nums = numbers_in(line)
        want = g['value']
        r['checks']['value'] = any(abs(n - want) <= g.get('tolerance', 0) for n in nums)
        r['checks']['no_wrong_total'] = not any(n in (g.get('forbid_values') or []) for n in nums)
        if g.get('require'):
            r['checks']['required_substrings'] = all(s in reply for s in g['require'])
        r['checks']['answer_line_clean'] = not any(s in line for s in (g.get('disallow_answer') or []))
        r['pass'] = all(r['checks'].values())
    elif g['type'] == 'code':
        v = g['value']
        r['checks']['value'] = v in reply
        r['checks']['forbidden_absent'] = not any(f in reply for f in (g.get('forbid') or []))
        r['pass'] = r['checks']['value'] and r['checks']['forbidden_absent']
    elif g['type'] == 'refusal':
        markers = ('not enough information', 'not specified', 'not stated', 'no information',
                   'not provided', 'cannot determine', 'not documented', 'unknown', 'not given')
        r['checks']['refusal'] = any(m in line.lower() for m in markers)
        r['checks']['no_invented_value'] = not re.search(
            r'\b\d+\s*(year|month|week|day)s?\b', line.lower())
        r['pass'] = r['checks']['refusal'] and r['checks']['no_invented_value']
    want_ids = case['reference'].get('citations') or []
    if want_ids or g.get('require_citations'):
        r['checks']['citations'] = all(i in reply for i in want_ids)
        r['pass'] = r['pass'] and r['checks']['citations']
    cited = sorted({i for i in ALL_CRITICAL_IDS if i in reply})
    r['cited_ids'] = cited
    r['wrong_citations'] = [i for i in cited if i not in want_ids]
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant', required=True, help='m-NAME-cNk-q (scripts/model-variants.sh)')
    ap.add_argument('--host', default=os.environ.get('PS5_HOST'))
    ap.add_argument('--port', type=int, default=8081)
    ap.add_argument('--wait', type=int, default=600)
    ap.add_argument('--manifest', help='host manifest (token ids) to verify and use')
    ap.add_argument('--manifest-out', help='host: write the manifest here')
    ap.add_argument('--corpus', default=str(CORPUS))
    ap.add_argument('--only', default='', help='comma-separated case ids to run')
    ap.add_argument('--n-predict', type=int, default=128)
    ap.add_argument('--prompts-only', action='store_true',
                    help='host: build the prompts and write the manifest, send no completions')
    ap.add_argument('--shutdown', action='store_true')
    ap.add_argument('--out')
    args = ap.parse_args()
    if not args.host:
        raise SystemExit('set PS5_HOST or pass --host')
    base = f'http://{args.host}:{args.port}'
    fx = load_fixture_builder()
    corpus = json.loads(pathlib.Path(args.corpus).read_text())
    template = lc.variant_prompt(args.variant)

    host_run = bool(args.manifest_out)
    manifest = json.loads(pathlib.Path(args.manifest).read_text()) if args.manifest else None
    import hashlib
    result = {'variant': args.variant, 'started': datetime.datetime.now().isoformat(timespec='seconds'),
              'corpus_sha': hashlib.sha256(pathlib.Path(args.corpus).read_bytes()).hexdigest()[:16],
              'cases': {}, 'events': []}
    result['health'] = lc.wait_ready(base, args.wait)
    title = lc.Title(base)   # after wait_ready: /ps5lm/stats must not be probed before the server exists
    props = lc.request(base, '/props')[0]
    n_ctx = (props.get('default_generation_settings') or {}).get('n_ctx')
    result['n_ctx'] = n_ctx
    target = n_ctx - RESERVE

    def build(n_filler, case):
        return template.replace(lc.QUESTION, fx.build_corpus_text(n_filler, case))

    only = {x for x in args.only.split(',') if x}
    cases = [q for q in corpus['questions'] if not only or q['id'] in only]

    # ---- prompts: binary-search the filler count per case against this server's tokenizer
    prompts, fillers = {}, {}
    for case in cases:
        cid = case['id']
        if manifest and cid in manifest['fillers']:
            n = manifest['fillers'][cid]
        else:
            lo, hi = 1, 64
            while len(lc.tokenize(base, build(hi, case))) < target:
                lo, hi = hi, hi * 2
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if len(lc.tokenize(base, build(mid, case))) <= target:
                    lo = mid
                else:
                    hi = mid - 1
            n = lo
        ids = lc.tokenize(base, build(n, case))
        if len(ids) > n_ctx - 200:
            raise SystemExit(f'{cid}: prompt {len(ids)} exceeds n_ctx {n_ctx}')
        prompts[cid], fillers[cid] = ids, n
    result['fillers'] = fillers
    result['prompt_tokens'] = {k: len(v) for k, v in prompts.items()}
    if manifest:
        same = {k: prompts[k] == manifest['prompts'][k] for k in prompts}
        result['prompt_ids_equal_manifest'] = same
        if not all(same.values()):
            raise SystemExit(f'prompt ids differ from the manifest: {same}')
    print(f"n_ctx {n_ctx}: filler counts {fillers}, prompt tokens {result['prompt_tokens']}", flush=True)
    if args.prompts_only:
        pathlib.Path(args.manifest_out).parent.mkdir(parents=True, exist_ok=True)
        pathlib.Path(args.manifest_out).write_text(json.dumps(
            {'variant': args.variant, 'fillers': fillers, 'prompts': prompts,
             'prompt_tokens': result['prompt_tokens'], 'corpus_sha': result['corpus_sha']}) + '\n')
        print(args.manifest_out)
        return 0

    # ---- run and grade
    walls = []
    stop = None
    for case in cases:
        cid = case['id']
        row = lc.completion(base, title, prompts[cid], args.n_predict)
        row['case'] = grade(case, row.get('content', ''))
        row.pop('tokens', None)
        walls.append(row['wall_s'])
        result['cases'][cid] = row
        c = row['case']
        print(f"{cid}: {'PASS' if c['pass'] else 'FAIL'} wall {row['wall_s']:.1f} s "
              f"pp {row['prompt_per_second'] or 0:.0f} tg {row['predicted_per_second'] or 0:.1f} "
              f"answer {c['answer_line'][:60]!r}", flush=True)
        f = lc.fence_s(row)
        if f is not None and f > 1.5:
            stop = f'{cid}: longest wait {f:.3f} s > 1.5 s'
            break
    if stop:
        result['events'].append(
            {'t': datetime.datetime.now().isoformat(timespec='seconds'), 'event': stop})
        print(f'STOPPED: {stop}', flush=True)
    result['stopped'] = stop

    # ---- summary (Phase D: stall watch over the walls we just measured)
    med = statistics.median(walls) if walls else None
    summary = {'n_ctx': n_ctx, 'stopped': stop,
               'passed': sum(1 for c in result['cases'].values() if c['case']['pass']),
               'total': len(result['cases'])}
    if med:
        summary['median_wall_s'] = round(med, 2)
        summary['slow_requests'] = [cid for cid, c in result['cases'].items() if c['wall_s'] > 10 * med]
    summary['by_case'] = {cid: {'pass': c['case']['pass'], 'answer_line': c['case']['answer_line'],
                                'cited_ids': c['case']['cited_ids'],
                                'wrong_citations': c['case']['wrong_citations']}
                          for cid, c in result['cases'].items()}
    result['summary'] = summary
    result['stats_end'] = title.stats()
    if args.shutdown and title.available:
        try:
            lc.request(base, '/ps5lm/shutdown', {})
            result['shutdown'] = True
        except Exception as e:  # noqa: BLE001 - diagnostics only
            result['shutdown'] = str(e)
    print('SUMMARY ' + json.dumps(summary), flush=True)
    if host_run:
        pathlib.Path(args.manifest_out).parent.mkdir(parents=True, exist_ok=True)
        pathlib.Path(args.manifest_out).write_text(json.dumps(
            {'variant': args.variant, 'fillers': fillers, 'prompts': prompts,
             'prompt_tokens': result['prompt_tokens'], 'corpus_sha': result['corpus_sha']}) + '\n')
        print(args.manifest_out)
        return 0
    out = pathlib.Path(args.out) if args.out else (
        ROOT / 'test-results/ps5/lc-comprehension' /
        datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True, exist_ok=True)
    (out / 'comprehension.json').write_text(json.dumps(result, indent=1) + '\n')
    lines = ['# Long-context comprehension', '',
             f'variant {args.variant}, n_ctx {n_ctx}, corpus {result["corpus_sha"]}', '',
             '| case | pass | wall s | pp tok/s | tg tok/s | answer | cited |',
             '|---|---|---|---|---|---|---|']
    for cid, c in result['cases'].items():
        lines.append(f"| {cid} | {'PASS' if c['case']['pass'] else 'FAIL'} | {c['wall_s']:.1f} | "
                     f"{c['prompt_per_second'] or 0:.0f} | {c['predicted_per_second'] or 0:.1f} | "
                     f"{c['case']['answer_line'][:48]} | {', '.join(c['case']['cited_ids']) or '-'} |")
    (out / 'table.md').write_text('\n'.join(lines) + '\n')
    print(out)
    return 0 if not stop else 1


if __name__ == '__main__':
    sys.exit(main())
