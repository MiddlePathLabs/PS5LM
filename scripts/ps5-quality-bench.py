#!/usr/bin/env python3
"""Model-quality benchmark (Phase C; docs/PORTING.md "Evaluation builds").

Runs the fixed task set in testdata/quality-tasks.json against one server window
(the c2k-q entries), temperature 0, the variant's own chat template, fixed output
limits per task. The same tasks and graders are used for every model.

Graders are deterministic: coding tasks execute the model's final ```python code
block against pinned asserts in a subprocess; answer-line graders read the text
after the last 'Answer:'; JSON graders parse the first balanced object/array.
No LLM judge. Every reply, latency and the title's counters are recorded; any
request over 10x the median wall is flagged (the 9B stall watch, Phase D).

  scripts/ps5-quality-bench.py --variant m-qwen35-9b-c2k-q            # console
  scripts/ps5-quality-bench.py --variant m-qwen35-9b-c2k-q --host 127.0.0.1 --limit 4 \
      --shutdown                                                     # rehearsal shakeout

Writes quality.json, tasks.md and table.md under test-results/ps5/quality/<time>/
(or --out). Exit 1 only on a stop condition (stall watch), not on task failures.
"""
import argparse
import datetime
import importlib.util
import json
import os
import pathlib
import re
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


lc = _load('lc', ROOT / 'scripts/ps5-api-longctx.py')
comp = _load('comp', ROOT / 'scripts/ps5-lc-comprehension.py')
answer_line, numbers_in = comp.answer_line, comp.numbers_in


def extract_python(reply):
    blocks = re.findall(r'```(?:python|py)\s*\n(.*?)```', reply, re.S)
    return blocks[-1].strip() if blocks else None


def first_json(reply, opening='{'):
    """First balanced JSON object (or array when opening='[') in the reply."""
    start = reply.find(opening)
    while start != -1:
        depth, in_str, esc = 0, False, False
        for i in range(start, len(reply)):
            c = reply[i]
            if in_str:
                if esc:
                    esc = False
                elif c == '\\':
                    esc = True
                elif c == '"':
                    in_str = False
                continue
            if c == '"':
                in_str = True
            elif c in '{[':
                depth += 1
            elif c in '}]':
                depth -= 1
                if depth == 0:
                    try:
                        return json.loads(reply[start:i + 1])
                    except json.JSONDecodeError:
                        break
        start = reply.find(opening, start + 1)
    return None


def grade(task, reply):
    g, out = task['grader'], {'checks': {}, 'pass': False}
    t = g['type']
    if t == 'python_tests':
        code = extract_python(reply)
        if not code:
            out['error'] = 'no ```python block found'
            return out
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / 'sol.py'
            p.write_text(code + '\n\n' + '\n'.join(g['tests']) + '\n')
            try:
                r = subprocess.run([sys.executable, '-B', str(p)], capture_output=True,
                                   text=True, timeout=30)
                out['checks']['tests_exit_zero'] = r.returncode == 0
                out['pass'] = r.returncode == 0
                if r.returncode != 0:
                    out['error'] = (r.stderr or r.stdout)[-600:]
            except subprocess.TimeoutExpired:
                out['error'] = 'tests timed out'
    elif t == 'answer_numeric':
        line = answer_line(reply)
        nums = numbers_in(line)
        out['answer_line'] = line
        out['checks']['value'] = any(abs(n - g['value']) <= g.get('tolerance', 0) for n in nums)
        out['checks']['no_wrong_value'] = not any(
            v in nums for v in (g.get('forbid_values') or []))
        out['pass'] = all(out['checks'].values())
    elif t == 'answer_contains':
        line = answer_line(reply)
        out['answer_line'] = line

        def has(s):
            # "Answer: X" constrains the answer line; bare strings may appear anywhere.
            return (s[len('Answer: '):].strip() in line) if s.startswith('Answer: ') else (s in reply)

        out['checks']['contains'] = all(has(s) for s in g['all_of'])
        out['pass'] = out['checks']['contains']
    elif t == 'regex':
        text = reply.strip()
        pat = re.compile(g['pattern'])
        out['checks']['match'] = bool(pat.fullmatch(text) if g.get('mode') == 'fullmatch' else pat.search(text))
        out['pass'] = out['checks']['match']
    elif t == 'sentences':
        body = reply.strip()
        sents = [s for s in re.split(r'(?<=[.!?])\s+', body) if s.strip()]
        out['checks']['sentence_count'] = len(sents) == g['count']
        out['checks']['each_starts'] = all(s.strip().startswith(g['each_starts']) for s in sents)
        out['pass'] = all(out['checks'].values())
        out['counts'] = len(sents)
    elif t == 'bullets':
        lines = [l for l in reply.strip().splitlines() if l.strip()]
        out['checks']['line_count'] = len(lines) == g['count']
        out['checks']['bullet_form'] = all(l.strip().startswith('- ') and len(l.strip()) > 2
                                           for l in lines)
        out['pass'] = all(out['checks'].values())
    elif t == 'json_fields':
        obj = first_json(reply, '{')
        out['json_parsed'] = obj is not None
        if not isinstance(obj, dict):
            out['error'] = 'no JSON object parsed'
            return out
        ok = True
        for k, v in g['fields'].items():
            got = obj.get(k)
            good = got == v and type(got) is type(v) if isinstance(v, bool) else got == v
            if g.get('types', {}).get(k) == 'int':
                good = isinstance(got, int) and not isinstance(got, bool) and got == v
            elif g.get('types', {}).get(k) == 'bool':
                good = isinstance(got, bool) and got == v
            elif g.get('types', {}).get(k) == 'str':
                good = isinstance(got, str) and got == v
            out['checks'][f'field:{k}'] = good
            ok = ok and good
        for k in g.get('required_keys', []):
            out['checks'][f'key:{k}'] = k in obj
            ok = ok and k in obj
        for k in g.get('forbidden_keys', []):
            out['checks'][f'absent:{k}'] = k not in obj
            ok = ok and k not in obj
        if 'array_items' in g:
            for k, n in g['array_items'].items():
                good = isinstance(obj.get(k), list) and len(obj[k]) == n
                out['checks'][f'array:{k}'] = good
                ok = ok and good
        out['pass'] = bool(ok)
    elif t == 'json_array':
        arr = first_json(reply, '[')
        out['json_parsed'] = arr is not None
        if not isinstance(arr, list):
            out['error'] = 'no JSON array parsed'
            return out
        got = {str(e.get(g['key'])): e.get(g['value_key']) for e in arr if isinstance(e, dict)}
        want = {str(e[g['key']]): e[g['value_key']] for e in g['expect']}
        out['checks']['matches'] = got == want
        out['pass'] = got == want
    elif t == 'refusal':
        line = answer_line(reply)
        out['answer_line'] = line
        markers = ('not enough information', 'not specified', 'not stated', 'no information',
                   'not provided', 'cannot determine', 'not documented', 'unknown', 'not given')
        out['checks']['refusal'] = any(m in line.lower() for m in markers)
        out['checks']['no_invented_value'] = not re.search(r'\b\d+\s*[a-z%]*', line)
        out['pass'] = all(out['checks'].values())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant', required=True)
    ap.add_argument('--tasks', default=str(ROOT / 'testdata/quality-tasks.json'))
    ap.add_argument('--host', default=os.environ.get('PS5_HOST'))
    ap.add_argument('--port', type=int, default=8081)
    ap.add_argument('--wait', type=int, default=600)
    ap.add_argument('--limit', type=int, default=None, help='run only the first N tasks (shakeout)')
    ap.add_argument('--only', default='', help='comma-separated task ids')
    ap.add_argument('--shutdown', action='store_true')
    ap.add_argument('--out')
    args = ap.parse_args()
    if not args.host:
        raise SystemExit('set PS5_HOST or pass --host')
    base = f'http://{args.host}:{args.port}'
    tasks = json.loads(pathlib.Path(args.tasks).read_text())['tasks']
    if args.only:
        want = {x for x in args.only.split(',') if x}
        tasks = [t for t in tasks if t['id'] in want]
    if args.limit:
        tasks = tasks[:args.limit]
    template = lc.variant_prompt(args.variant)
    result = {'variant': args.variant, 'n_tasks': len(tasks), 'tasks': {}, 'events': []}
    result['health'] = lc.wait_ready(base, args.wait)
    title = lc.Title(base)   # after wait_ready: /ps5lm/stats must not be probed before the server exists
    result['started'] = datetime.datetime.now().isoformat(timespec='seconds')
    result['stats_start'] = title.stats()

    stop = None
    for task in tasks:
        tid = task['id']
        prompt_ids = lc.tokenize(base, template.replace(lc.QUESTION, task['prompt']))
        row = lc.completion(base, title, prompt_ids, task['n_predict'])
        row.pop('tokens', None)
        row['category'] = task['category']
        row['graded'] = grade(task, row.get('content', ''))
        result['tasks'][tid] = row
        g = row['graded']
        extra = f" err={g.get('error', '')[:80]}" if g.get('error') else ''
        print(f"{tid} [{task['category']}]: {'PASS' if g['pass'] else 'FAIL'} "
              f"wall {row['wall_s']:.1f} s, {row['predicted_n']} tok{extra}", flush=True)
        f = lc.fence_s(row)
        if f is not None and f > 1.5:
            stop = f'{tid}: longest wait {f:.3f} s > 1.5 s'
            break
    if stop:
        result['events'].append({'t': datetime.datetime.now().isoformat(timespec='seconds'),
                                 'event': stop})
        print(f'STOPPED: {stop}', flush=True)
    result['stopped'] = stop
    result['stats_end'] = title.stats()

    # ---- summary
    cats, walls, pps, tgs = {}, [], [], []
    passed = 0
    for tid, r in result['tasks'].items():
        c = r['category']
        cats.setdefault(c, {'passed': 0, 'total': 0})
        cats[c]['total'] += 1
        cats[c]['passed'] += int(r['graded']['pass'])
        passed += int(r['graded']['pass'])
        walls.append(r['wall_s'])
        if r.get('prompt_per_second'):
            pps.append(r['prompt_per_second'])
        if r.get('predicted_per_second'):
            tgs.append(r['predicted_per_second'])

    def p95(v):
        return round(sorted(v)[max(0, int(0.95 * len(v)) - 1)], 2) if v else None

    summary = {'variant': args.variant, 'passed': passed, 'total': len(result['tasks']),
               'pass_rate': round(passed / len(result['tasks']), 3) if result['tasks'] else 0,
               'by_category': cats,
               'invalid_json': sum(1 for r in result['tasks'].values()
                                   if r['category'] == 'structured' and not r['graded'].get('json_parsed', True)),
               'median_wall_s': round(statistics.median(walls), 2) if walls else None,
               'p95_wall_s': p95(walls),
               'median_pp_tok_s': round(statistics.median(pps), 1) if pps else None,
               'median_tg_tok_s': round(statistics.median(tgs), 1) if tgs else None,
               'total_output_tokens': sum(r.get('predicted_n') or 0 for r in result['tasks'].values()),
               'total_prompt_tokens': sum(r.get('prompt_n') or 0 for r in result['tasks'].values()),
               'total_wall_s': round(sum(walls), 1) if walls else None,
               'stopped': stop}
    med = statistics.median(walls) if walls else None
    if med:
        summary['slow_requests'] = [tid for tid, r in result['tasks'].items() if r['wall_s'] > 10 * med]
    result['summary'] = summary
    if args.shutdown and title.available:
        try:
            lc.request(base, '/ps5lm/shutdown', {})
            result['shutdown'] = True
        except Exception as e:  # noqa: BLE001 - diagnostics only
            result['shutdown'] = str(e)
    print('SUMMARY ' + json.dumps(summary), flush=True)
    out = pathlib.Path(args.out) if args.out else (
        ROOT / 'test-results/ps5/quality' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True, exist_ok=True)
    (out / 'quality.json').write_text(json.dumps(result, indent=1) + '\n')
    lines = ['# Quality benchmark', '', f'variant {args.variant}: {passed}/{len(result["tasks"])} passed', '',
             '| category | passed | total |', '|---|---|---|']
    for c, v in cats.items():
        lines.append(f'| {c} | {v["passed"]} | {v["total"]} |')
    lines += ['', '| id | category | pass | wall s | prompt tok | out tok | note |', '|---|---|---|---|---|---|---|']
    for tid, r in result['tasks'].items():
        note = (r['graded'].get('error') or r['graded'].get('answer_line') or '')[:60].replace('|', '\\|')
        lines.append(f"| {tid} | {r['category']} | {'PASS' if r['graded']['pass'] else 'FAIL'} | "
                     f"{r['wall_s']:.1f} | {r.get('prompt_n')} | {r.get('predicted_n')} | {note} |")
    (out / 'table.md').write_text('\n'.join(lines) + '\n')
    print(out)
    return 1 if stop else 0


if __name__ == '__main__':
    sys.exit(main())
