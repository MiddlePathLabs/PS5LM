#!/usr/bin/env python3
"""Long-context checks of the title's LAN API (docs/PORTING.md "Long context").

One rung (a server build with a larger context, e.g. server-m-qwen35-9b-c8k), one window:

    # on the Mac first, against the CPU rehearsal of the same entry (same pin, patches, prompts):
    scripts/ps5-api-longctx.py --variant m-qwen35-9b-c8k --host 127.0.0.1 --reference-out REF.json [--logits]
    # then on the console, comparing with it:
    scripts/ps5-api-longctx.py --variant m-qwen35-9b-c8k --reference REF.json

Requests, in order (all temperature 0, prompt cache off):
 1. depth 0: the benchmark passage's first 128 tokens, 128 tokens generated (warm-up + 1);
    then its first 512 tokens with 32 tokens and the top-10 log-probabilities (a short
    logit check against the CPU reference);
 2. a timing probe: filler of half the rung's length, 1 token generated. The longest fence
    wait (the title's /ps5lm/stats, reset before each request) must stay far below the
    2 s bound: the full length's is predicted linearly in depth from the depth-0 and
    half-length probes, and if it would exceed --wait-limit the prompt chunk is lowered
    (POST /ps5lm/knobs prompt_batch) in proportion; below 64 tokens, or any measured wait
    above --wait-abort, the long requests are not sent;
 3. needle in a haystack at depths 10 %, 50 % and 90 % of a deterministic filler of the
    rung's length (ctx - 192 tokens, generated here, no outside text): one unique passcode
    per depth, asked for at the end. 10 %: 32 tokens. 50 %: 32 tokens with the top-10
    log-probabilities (the long logit check against the CPU, with --logits). 90 %: 128
    tokens with ignore_eos, which is also the depth benchmark (prompt tok/s at full length,
    generation tok/s at depth); its answer is judged on the tokens before the first
    end-of-generation token.
Every request records the server's timings, the wall time, and the title's counters
before and after (longest submit, fence wait, semaphore wait, graph).

Logit check (tolerances in docs/PORTING.md "Long context", fixed before the console run):
top-1 equal (or the CPU's top two within 0.5), the largest |GPU - CPU| log-probability over
the CPU's top 10 (matched by token id; at least 8 of them in the GPU's top 10), and the
greedy continuation by the model mode's rule (first 8 identical, or the first difference
where the CPU's top two are within 0.5). --tolerance and --min-common set the numbers.

Writes longctx.json and table.md to test-results/ps5/api-longctx/<time>/ (console) or the
--reference-out file (host). Only reads from the server, apart from the prompt_batch and
reset_max knobs and the optional --shutdown.
"""
import argparse
import datetime
import json
import math
import os
import pathlib
import random
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
QUESTION = 'In a short paragraph, explain how a GPU differs from a CPU.'
RESERVE = 192           # tokens of the context left for the question's answer and the benchmark's 128
DEPTHS = (0.10, 0.50, 0.90)
NEEDLES = {0.10: ('Halloway', 'AMBER-4821-FALCON'),
           0.50: ('Brightwater', 'COBALT-9306-LANTERN'),
           0.90: ('Thornbury', 'JUNIPER-5174-ORCHID')}
EOG_TEXT = ['<|im_end|>', '<|endoftext|>', '<|eot_id|>', '<|end_of_text|>', '<|eom_id|>']
PREFIX, TIE_GAP = 8, 0.5

REQ_TIMEOUT = 3600      # one request's HTTP timeout; the host CPU 27B needs hours per needle

# ---- deterministic filler: invented places, people and numbers; no passcodes, vaults or secrets.
PLACES = ['Marrowfield', 'Kestrel Bay', 'Old Tannery Lane', 'Upper Wexham', 'the Calder valley', 'Fenwick Cross',
          'Saltmarsh Point', 'Brackenridge', 'the Ivel estuary', 'Copperhill', 'Lowmoor', 'Ashby Fold',
          'Gullhaven', 'Pennock Ridge', 'the Mirren flats', 'Stonecroft']
NAMES = ['Ada Penhallow', 'Tobias Venn', 'Mirela Osk', 'Harlan Dree', 'Lucia Farrant', 'Owen Tilsley',
         'Rosalind Quaye', 'Emeric Holt', 'Nadia Corwen', 'Silas Ambry', 'Petra Lunde', 'Joachim Rell']
ADJS = ['northern', 'old', 'narrow', 'stone', 'flooded', 'quiet', 'restored', 'eastern', 'timbered', 'walled']
NOUNS = ['mill', 'bridge', 'orchard', 'granary', 'lighthouse', 'ferry landing', 'market hall', 'chapel',
         'weir', 'boathouse', 'kiln', 'signal tower']
PLURALS = ['pear trees', 'fishing boats', 'beehives', 'sheep', 'lanterns', 'barrels of cider', 'oak beams',
           'apprentices', 'cart wheels', 'herons', 'clay pots', 'bales of wool']
QUALITIES = ['sturdiness', 'bright colours', 'early harvests', 'careful records', 'strong tides', 'slow decline',
             'unusual quiet', 'busy fairs']
DOCS = ['parish ledger', 'surveyor\'s notebook', 'harbour log', 'county report', 'letter', 'tax roll', 'diary']
SEASONS = ['spring', 'summer', 'autumn', 'winter']
VERBS = ['repaired', 'flooded', 'repainted', 'inspected', 'rebuilt', 'measured']
VERBS2 = ['restored', 'mapped', 'sold', 'described', 'enlarged', 'abandoned']
TEMPLATES = [
    'In {year}, the {adj} {noun} near {place} was surveyed by {name}, who counted {num} {plural}.',
    '{name} wrote that the {noun} at {place} had been {verb} {small} times since {year}.',
    'The {plural} of {place} were known for their {quality}, according to a {doc} from {year}.',
    'A {doc} kept in {place} describes how {name} {verb2} the {adj} {noun} during the {season} of {year}.',
    'Travellers reaching {place} in the {season} often remarked on the {quality} of its {plural}.',
    'By {year} the {noun} at {place} employed {small} people and kept {num} {plural}.',
    'According to {name}, the road from {place} to {place2} took {small} hours on foot in {year}.',
]


def filler_sentences(count, seed=20261016):
    rng = random.Random(seed)
    out = []
    for _ in range(count):
        t = rng.choice(TEMPLATES)
        place = rng.choice(PLACES)
        out.append(t.format(year=rng.randint(1702, 1938), adj=rng.choice(ADJS), noun=rng.choice(NOUNS),
                            place=place, place2=rng.choice([p for p in PLACES if p != place]),
                            name=rng.choice(NAMES), num=rng.randint(12, 980), small=rng.randint(2, 19),
                            plural=rng.choice(PLURALS), quality=rng.choice(QUALITIES), doc=rng.choice(DOCS),
                            season=rng.choice(SEASONS), verb=rng.choice(VERBS), verb2=rng.choice(VERBS2)))
    return out


def needle_user_text(sentences, depth):
    vault, code = NEEDLES[depth]
    at = round(depth * len(sentences))
    body = sentences[:at] + [f'The secret passcode for the {vault} vault is {code}.'] + sentences[at:]
    return (' '.join(body) + f'\n\nWhat is the secret passcode for the {vault} vault? '
            'Answer with the passcode only.')


def plain_user_text(sentences):
    return ' '.join(sentences) + '\n\nSummarise the text above in one sentence.'


# ---- the model's chat format: the variant's prompt with the question replaced.
def variant_prompt(variant):
    name = variant[2:] if variant.startswith('m-') else variant
    out = subprocess.run(['bash', '-c', 'source "$1"; ps5lm_model_variant "$2" >/dev/null || exit 2; '
                          'for f in "${mv_flags[@]}"; do case $f in -DPS5LM_MODEL_PROMPT=*) printf %s "$f";; esac; done',
                          '_', str(ROOT / 'scripts/model-variants.sh'), name],
                         check=True, capture_output=True, text=True).stdout
    prompt = out.removeprefix('-DPS5LM_MODEL_PROMPT="').removesuffix('"').replace('\\n', '\n')
    if QUESTION not in prompt:
        raise SystemExit(f'{variant}: prompt has no question to replace')
    return prompt


# ---- HTTP
def request(base, path, body=None, timeout=60):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data=data, headers={'Content-Type': 'application/json'})
    t = time.monotonic()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read() or b'null'), time.monotonic() - t


def wait_ready(base, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            h, _ = request(base, '/health', timeout=5)
            if (h or {}).get('status') == 'ok':
                return h
        except (urllib.error.URLError, OSError, ValueError):
            pass
        time.sleep(2)
    raise SystemExit('server not ready')


def tokenize(base, text, add_special=True):
    return request(base, '/tokenize', {'content': text, 'add_special': add_special, 'parse_special': True},
                   timeout=300)[0]['tokens']


class Title:
    """The title's counters (GET /ps5lm/stats) and knobs; absent on a plain llama-server."""
    def __init__(self, base):
        self.base = base
        try:
            self.available = request(base, '/ps5lm/stats')[0] is not None
        except (urllib.error.URLError, OSError, ValueError):
            self.available = False

    def stats(self):
        return request(self.base, '/ps5lm/stats')[0] if self.available else None

    def knob(self, name, value):
        if self.available:
            return request(self.base, '/ps5lm/knobs', {name: value})[0]


def vk_max(stats):
    vk = (stats or {}).get('vk') or {}
    return {k: vk.get(k) for k in ('max_submit_ns', 'max_fence_wait_ns', 'max_semaphore_wait_ns', 'max_graph_ns')}


def completion(base, title, prompt_ids, n_predict, n_probs=0, ignore_eos=False, timeout=None):
    timeout = timeout or REQ_TIMEOUT
    title.knob('reset_max', 1)
    before = title.stats()
    started = datetime.datetime.now().isoformat(timespec='seconds')
    body = {'prompt': prompt_ids, 'n_predict': n_predict, 'temperature': 0, 'cache_prompt': False,
            'return_tokens': True, 'stream': False, 'ignore_eos': ignore_eos}
    if n_probs:
        body['n_probs'] = n_probs
    resp, wall = request(base, '/completion', body, timeout=timeout)
    after = title.stats()
    t = resp.get('timings') or {}
    row = {'started': started, 'wall_s': round(wall, 3), 'prompt_n': t.get('prompt_n'),
           'prompt_ms': t.get('prompt_ms'), 'prompt_per_second': t.get('prompt_per_second'),
           'predicted_n': t.get('predicted_n'), 'predicted_ms': t.get('predicted_ms'),
           'predicted_per_second': t.get('predicted_per_second'), 'tokens': resp.get('tokens', []),
           'content': resp.get('content', ''), 'max': vk_max(after)}
    if before and after and 'submissions' in (after.get('vk') or {}):
        b, a = before['vk'], after['vk']
        row['counters'] = {k: a[k] - b[k] for k in ('submissions', 'fence_waits', 'fence_wait_ns', 'graphs',
                                                     'graph_ns', 'dispatches')}
        row['prompt_batch'] = after.get('prompt_batch')
        row['direct_peak'] = after.get('direct_peak')
    if n_probs:
        row['probs'] = [[(p['id'], p['logprob']) for p in step.get('top_logprobs', [])]
                        for step in resp.get('completion_probabilities', [])]
    return row


def fence_s(row):
    """The longest single blocking wait of a request: fence or semaphore wait, or a submit that
    blocked (vkQueueSubmit blocks while earlier chunks still run on this driver)."""
    m = row.get('max') or {}
    v = [m.get(k) for k in ('max_fence_wait_ns', 'max_semaphore_wait_ns', 'max_submit_ns') if m.get(k) is not None]
    return max(v) / 1e9 if v else None


def graph_s(row):
    v = (row.get('max') or {}).get('max_graph_ns')
    return None if v is None else v / 1e9


def show(name, row):
    print(f"{name}: prompt {row['prompt_n']} tok at {row['prompt_per_second'] or 0:.1f} tok/s, "
          f"gen {row['predicted_n']} at {row['predicted_per_second'] or 0:.1f} tok/s, wall {row['wall_s']:.1f} s, "
          f"max fence {fence_s(row)} s, max graph {((row.get('max') or {}).get('max_graph_ns') or 0) / 1e9:.3f} s",
          flush=True)


# ---- comparisons
def gaps_from_probs(probs):
    return [(s[0][1] - s[1][1]) if len(s) >= 2 else 99.0 for s in probs]


def token_rule(tokens, cpu_tokens, cpu_gaps):
    first = next((i for i, (a, b) in enumerate(zip(tokens, cpu_tokens)) if a != b), None)
    if first is None:
        return ('identical' if len(tokens) >= len(cpu_tokens) else 'short'), first
    if first >= PREFIX:
        return 'prefix', first
    if first < len(cpu_gaps) and cpu_gaps[first] <= TIE_GAP:
        return 'near_tie', first
    return 'fail', first


def logit_compare(gpu, cpu):
    """gpu, cpu: rows with 'tokens' and 'probs' (top-10 per generated token)."""
    g0, c0 = dict(gpu['probs'][0]), cpu['probs'][0]
    cpu_top = [i for i, _ in c0]
    common = [i for i in cpu_top if i in g0]
    diffs = [abs(g0[i] - lp) for i, lp in c0 if i in g0]
    cgaps = gaps_from_probs(cpu['probs'])
    ggaps = gaps_from_probs(gpu['probs'])
    rule, first = token_rule(gpu['tokens'], cpu['tokens'], cgaps)
    top1_equal = gpu['probs'][0][0][0] == c0[0][0]
    nonfinite = sum(1 for step in (gpu['probs'] + cpu['probs']) for _, lp in step if not math.isfinite(lp))
    cvals = [lp for _, lp in c0]
    spread = (max(cvals) - min(cvals)) if cvals else 0.0
    # Per position while the tokens agree: largest |diff| over the CPU's top 10.
    per_pos = []
    rel_pos = []
    for gs, cs in zip(gpu['probs'], cpu['probs']):
        gd = dict(gs)
        d = [abs(gd[i] - lp) for i, lp in cs if i in gd]
        per_pos.append(round(max(d), 4) if d else None)
        rel_pos.append(round(max(d) / spread, 4) if d and spread > 1e-9 else None)
        if gs[0][0] != cs[0][0]:
            break
    return {'top1_equal': top1_equal, 'cpu_top2_gap': round(cgaps[0], 4), 'gpu_top2_gap': round(ggaps[0], 4),
            'common_of_10': len(common), 'nonfinite_logprobs': nonfinite,
            'max_abs_logprob_diff_top10': round(max(diffs), 4) if diffs else None,
            'mean_abs_logprob_diff_top10': round(sum(diffs) / len(diffs), 4) if diffs else None,
            'max_rel_logprob_diff_top10': round(max(diffs) / spread, 4) if diffs and spread > 1e-9 else None,
            'rule': rule, 'first_diff': first, 'per_position_max_diff': per_pos,
            'per_position_max_rel_diff': rel_pos}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant', required=True, help='m-NAME-cNk (scripts/model-variants.sh)')
    ap.add_argument('--host', default=os.environ.get('PS5_HOST'))
    ap.add_argument('--port', type=int, default=8081)
    ap.add_argument('--wait', type=int, default=360, help='seconds to wait for /health')
    ap.add_argument('--reference', help='host CPU reference JSON (written by --reference-out)')
    ap.add_argument('--gate-reference', help='separate short-context CPU reference for --gate-depth0')
    ap.add_argument('--reference-out', help='write the host reference here (run against the rehearsal)')
    ap.add_argument('--logits', action='store_true', help='record top-10 log-probabilities on the 50%% needle')
    ap.add_argument('--logits-all', action='store_true',
                    help='record top-10 log-probabilities on all three needles (early/middle/late positions)')
    ap.add_argument('--wait-limit', type=float, default=1.0, help='seconds: predicted longest fence wait allowed')
    ap.add_argument('--wait-abort', type=float, default=1.5, help='seconds: a measured wait above this stops the long requests')
    ap.add_argument('--graph-limit', type=float, default=1.6, help='seconds: predicted longest graph (one prompt chunk) allowed')
    ap.add_argument('--graph-abort', type=float, default=1.8, help='seconds: a measured graph above this stops the long requests')
    ap.add_argument('--prev', default=None, help='DEPTH:GRAPH_S, the previous rung\'s full length and longest graph')
    ap.add_argument('--slope-us', type=float, default=None,
                    help='graph seconds per token of depth (x1e-6) beyond half length, from the previous rungs')
    ap.add_argument('--tolerance', type=float, default=None, help='max |logprob diff| over the CPU top 10 (from PORTING.md)')
    ap.add_argument('--min-common', type=int, default=8,
                    help='of the CPU top 10, at least this many in the GPU top 10 (from PORTING.md)')
    ap.add_argument('--gate-depth0', action='store_true',
                    help='stop before any long request unless the 512-token prompt passes the token rule against '
                         'the reference (a quantized KV cache is checked at a short context first)')
    ap.add_argument('--skip-probe', action='store_true')
    ap.add_argument('--prompts-only', action='store_true', help='host: write the prompts, send no completions')
    ap.add_argument('--only', default='', help='host: comma-separated requests to run (e.g. depth0_512,needle_50)')
    ap.add_argument('--req-timeout', type=int, default=None,
                    help='seconds per HTTP request (default 3600; the host CPU 27B needs more)')
    ap.add_argument('--shutdown', action='store_true')
    ap.add_argument('--out')
    args = ap.parse_args()
    if not args.host:
        raise SystemExit('set PS5_HOST or pass --host')
    base = f'http://{args.host}:{args.port}'
    global REQ_TIMEOUT
    if args.req_timeout:
        REQ_TIMEOUT = args.req_timeout
    host_run = bool(args.reference_out)
    ref = json.loads(pathlib.Path(args.reference).read_text()) if args.reference else None
    gate_ref = json.loads(pathlib.Path(args.gate_reference).read_text()) if args.gate_reference else ref
    template = variant_prompt(args.variant)
    result = {'variant': args.variant, 'started': datetime.datetime.now().isoformat(timespec='seconds'),
              'base': base if args.host in ('127.0.0.1', 'localhost') else 'http://<console>:%d' % args.port,
              'reference': args.reference, 'wait_limit_s': args.wait_limit, 'wait_abort_s': args.wait_abort,
              'tolerance': args.tolerance, 'requests': {}, 'events': []}
    result['health'] = wait_ready(base, args.wait)
    props = request(base, '/props')[0]
    n_ctx = (props.get('default_generation_settings') or {}).get('n_ctx')
    result['n_ctx'] = n_ctx
    title = Title(base)
    result['stats_start'] = title.stats()
    target = n_ctx - RESERVE
    eog = []
    for text in EOG_TEXT:   # the end-of-generation tokens this vocabulary has (single special tokens)
        ids = tokenize(base, text, add_special=False)
        if len(ids) == 1:
            eog.append(ids[0])
    result['eog_ids'] = eog

    # ---- prompts (identical on the host and the console; the console checks the ids)
    def build(user):
        return template.replace(QUESTION, user)
    if ref:
        n_sent = ref['sentences']
    else:
        lo, hi = 1, max(64, target // 8)
        while len(tokenize(base, build(needle_user_text(filler_sentences(hi), 0.5)))) < target:
            hi *= 2
        while lo < hi:   # largest count whose 50 % needle prompt fits the target
            mid = (lo + hi + 1) // 2
            if len(tokenize(base, build(needle_user_text(filler_sentences(mid), 0.5)))) <= target:
                lo = mid
            else:
                hi = mid - 1
        n_sent = lo
    sentences = filler_sentences(n_sent)
    prompts = {f'needle_{int(d * 100)}': tokenize(base, build(needle_user_text(sentences, d))) for d in DEPTHS}
    half = filler_sentences(max(1, n_sent // 2))
    prompts['probe_half'] = tokenize(base, build(plain_user_text(half)))
    bench_ids = tokenize(base, BENCH_PASSAGE)
    prompts['depth0_128'], prompts['depth0_512'] = bench_ids[:128], bench_ids[:512]
    result['sentences'] = n_sent
    result['prompt_tokens'] = {k: len(v) for k, v in prompts.items()}
    if any(len(v) > n_ctx - 140 for v in prompts.values()):
        raise SystemExit(f'prompt too long for n_ctx {n_ctx}: {result["prompt_tokens"]}')
    if ref:
        same = {k: prompts[k] == ref['prompts'][k] for k in prompts}
        result['prompt_ids_equal_reference'] = same
        if not all(same.values()):
            raise SystemExit(f'prompt ids differ from the reference: {same}')
    print(f"n_ctx {n_ctx}: {n_sent} sentences, prompt tokens {result['prompt_tokens']}", flush=True)

    reqs = result['requests']
    stop = None

    def note(text):
        result['events'].append({'t': datetime.datetime.now().isoformat(timespec='seconds'), 'event': text})
        print(text, flush=True)

    def guard(name):
        f, g = fence_s(reqs[name]), graph_s(reqs[name])
        if f is not None and f > args.wait_abort:
            return f'{name}: longest wait {f:.3f} s > {args.wait_abort} s'
        if g is not None and g > args.graph_abort:
            return f'{name}: longest graph {g:.3f} s > {args.graph_abort} s'
        return None

    only = {x for x in args.only.split(',') if x}
    want = lambda name: not only or name in only
    if args.prompts_only:
        only = {'none'}

    # ---- 1. depth 0
    if want('depth0_128'):
        reqs['depth0_warmup'] = completion(base, title, prompts['depth0_128'], 128, ignore_eos=True)
        reqs['depth0_128'] = completion(base, title, prompts['depth0_128'], 128, ignore_eos=True)
        show('depth 0 (128-token prompt)', reqs['depth0_128'])
    if want('depth0_512'):
        reqs['depth0_512'] = completion(base, title, prompts['depth0_512'], 32, n_probs=10)
        show('depth 0 (512-token prompt, top-10)', reqs['depth0_512'])
        stop = guard('depth0_512')
        if not stop and args.gate_depth0:
            cpu = (gate_ref or {}).get('requests', {}).get('depth0_512')
            if not cpu or not cpu.get('probs'):
                stop = 'gate: no short-context reference'
            else:
                c = logit_compare(reqs['depth0_512'], cpu)
                result['gate_depth0'] = c
                note(f"short-context check: rule {c['rule']}, top-1 equal {c['top1_equal']}, "
                     f"max diff {c['max_abs_logprob_diff_top10']}")
                if c['rule'] == 'fail' or not (c['top1_equal'] or c['cpu_top2_gap'] <= TIE_GAP):
                    stop = f"gate: the 512-token prompt fails the token rule ({c['rule']}, first diff {c['first_diff']})"

    # ---- 2. timing probe at half length
    if not stop and not args.skip_probe and want('probe_half') and 'depth0_512' in reqs:
        reqs['probe_half'] = completion(base, title, prompts['probe_half'], 1)
        show('probe at half length', reqs['probe_half'])
        stop = guard('probe_half')
        w0, wh = fence_s(reqs['depth0_512']), fence_s(reqs['probe_half'])
        g0, gh = graph_s(reqs['depth0_512']), graph_s(reqs['probe_half'])
        if not stop and w0 is not None and wh is not None:
            d_half, d_full = len(prompts['probe_half']), len(prompts['needle_90'])
            predicted = w0 + max(0.0, wh - w0) * d_full / max(1, d_half)
            # The graph (one prompt chunk's wall time) from depth 0 and the larger of the slopes
            # seen at half length and over the previous rung (--prev DEPTH:GRAPH_S).
            slope = max(0.0, gh - g0) / max(1, d_half)
            if args.prev:
                pd, pg = (float(x) for x in args.prev.split(':'))
                slope = max(slope, max(0.0, pg - g0) / max(1.0, pd))
            predicted_g = g0 + slope * d_full
            if args.slope_us is not None:   # the marginal slope measured over the previous rungs
                predicted_g = max(predicted_g, gh + args.slope_us * 1e-6 * (d_full - d_half))
            result['predicted_full_wait_s'] = round(predicted, 4)
            result['predicted_full_graph_s'] = round(predicted_g, 4)
            note(f'longest wait: depth 0 {w0:.3f} s, half length {wh:.3f} s -> full length predicted {predicted:.3f} s; '
                 f'longest graph: depth 0 {g0:.3f} s, half {gh:.3f} s -> predicted {predicted_g:.3f} s')
            chunk = reqs['probe_half'].get('prompt_batch') or 512
            scale = max(predicted / args.wait_limit, predicted_g / args.graph_limit)
            if scale > 1:
                new = int(chunk / scale) // 64 * 64
                if new < 64:
                    stop = f'predicted wait {predicted:.3f} s / graph {predicted_g:.3f} s need a prompt chunk below 64'
                else:
                    title.knob('prompt_batch', new)
                    result['prompt_batch_lowered'] = {'from': chunk, 'to': new, 'predicted_wait_s': predicted,
                                                      'predicted_graph_s': predicted_g}
                    note(f'prompt chunk lowered {chunk} -> {new}')

    # ---- 3. needles (full length)
    if not stop:
        for d in DEPTHS:
            name = f'needle_{int(d * 100)}'
            if not want(name):
                continue
            probs_here = (d == 0.50 and args.logits) or args.logits_all
            if d == 0.90:
                row = completion(base, title, prompts[name], 128, n_probs=10 if probs_here else 0, ignore_eos=True)
            else:
                row = completion(base, title, prompts[name], 32, n_probs=10 if probs_here else 0)
            reqs[name] = row
            show(f'needle at {int(d * 100)} %', row)
            toks = row['tokens']
            # The answer: up to the first end-of-generation token, at most 32 tokens (the 90 %
            # request ignores EOS for the benchmark, so the model writes on after its answer).
            cut = min(next((i for i, t in enumerate(toks) if t in eog), len(toks)), 32)
            answer = request(base, '/detokenize', {'tokens': toks[:cut]})[0].get('content', '') if toks else ''
            code = NEEDLES[d][1]
            norm = lambda s: ''.join(ch for ch in s.upper() if ch.isalnum())
            row['answer'] = answer
            row['retrieved'] = norm(code) in norm(answer)
            print(f"  answer {answer[:80]!r}: {'retrieved' if row['retrieved'] else 'NOT retrieved'}", flush=True)
            stop = guard(name)
            if stop:
                break
    if stop:
        note(f'STOPPED: {stop}')
    result['stopped'] = stop

    # ---- summary
    summary = {'n_ctx': n_ctx, 'needle': {}, 'stopped': stop}
    for d in DEPTHS:
        r = reqs.get(f'needle_{int(d * 100)}')
        summary['needle'][f'{int(d * 100)}'] = None if r is None else r['retrieved']
    full = [reqs[k]['prompt_per_second'] for k in ('needle_10', 'needle_50', 'needle_90') if k in reqs]
    summary['pp_full_tok_s'] = round(statistics.median(full), 1) if full else None
    summary['pp_full_tokens'] = result['prompt_tokens']['needle_90']
    summary['tg_depth_tok_s'] = round(reqs['needle_90']['predicted_per_second'], 1) if 'needle_90' in reqs else None
    r = lambda k, f: round(reqs[k][f], 1) if k in reqs and reqs[k].get(f) is not None else None
    summary['tg_depth0_tok_s'] = r('depth0_128', 'predicted_per_second')
    summary['pp_depth0_128_tok_s'] = r('depth0_128', 'prompt_per_second')
    summary['pp_depth0_512_tok_s'] = r('depth0_512', 'prompt_per_second')
    summary['max_wait_s'] = max([fence_s(x) or 0 for x in reqs.values()] or [0])
    summary['max_graph_s'] = max([(x.get('max') or {}).get('max_graph_ns') or 0 for x in reqs.values()] or [0]) / 1e9
    summary['max_submit_s'] = max([(x.get('max') or {}).get('max_submit_ns') or 0 for x in reqs.values()] or [0]) / 1e9
    summary['prompt_batch_lowered'] = result.get('prompt_batch_lowered')
    if ref:
        lc = {}
        for k in ('depth0_512', 'needle_10', 'needle_50', 'needle_90'):
            if k in reqs and reqs[k].get('probs') and ref['requests'].get(k, {}).get('probs'):
                lc[k] = logit_compare(reqs[k], ref['requests'][k])
                if args.tolerance is not None:
                    c = lc[k]
                    c['pass'] = ((c['top1_equal'] or c['cpu_top2_gap'] <= TIE_GAP) and c['common_of_10'] >= args.min_common and
                                 c['max_abs_logprob_diff_top10'] is not None and
                                 c['max_abs_logprob_diff_top10'] <= args.tolerance and c['rule'] != 'fail')
                print(f'logit check {k}: {json.dumps(lc[k])}', flush=True)
        summary['logit_check'] = lc
        for k in ('needle_10', 'needle_50', 'needle_90'):
            if k in reqs and k in ref['requests']:
                summary.setdefault('tokens_vs_cpu', {})[k] = token_rule(
                    reqs[k]['tokens'], ref['requests'][k]['tokens'], gaps_from_probs(ref['requests'][k].get('probs') or []))
    result['summary'] = summary
    result['stats_end'] = title.stats()
    if args.shutdown and title.available:
        try:
            request(base, '/ps5lm/shutdown', {})
            result['shutdown'] = True
        except (urllib.error.URLError, OSError, ValueError) as e:
            result['shutdown'] = str(e)
    print('SUMMARY ' + json.dumps(summary), flush=True)
    if host_run:
        result['prompts'] = prompts
        pathlib.Path(args.reference_out).parent.mkdir(parents=True, exist_ok=True)
        pathlib.Path(args.reference_out).write_text(json.dumps(result) + '\n')
        print(args.reference_out)
        return 0
    out = pathlib.Path(args.out) if args.out else (
        ROOT / 'test-results/ps5/api-longctx' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True, exist_ok=True)
    (out / 'longctx.json').write_text(json.dumps(result, indent=1) + '\n')
    print(out)
    return 0 if not stop else 1


# The benchmark passage of scripts/ps5-api-bench.py (same text, so depth 0 compares with it).
def _bench_passage():
    import importlib.util
    spec = importlib.util.spec_from_file_location('bench', ROOT / 'scripts/ps5-api-bench.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.PASSAGE


BENCH_PASSAGE = _bench_passage()

if __name__ == '__main__':
    sys.exit(main())
