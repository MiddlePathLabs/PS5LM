#!/usr/bin/env python3
"""Generate the long-context comprehension corpus (version-controlled fixture).

Writes testdata/lc-comprehension/corpus.json: deterministic document-like corpora for
~16k/32k/64k-token prompts, with hand-authored critical documents (facts, distractors,
revisions, cross-references), seeded coherent filler, questions, exact reference answers
and citation requirements. The same corpus text is used at every length and for every
model; only the amount of filler grows. ps5-lc-comprehension.py consumes the JSON.

Content rule (as for the needle tests): entirely invented places, people, equipment and
values; no outside text.
"""
import json
import pathlib
import random

ROOT = pathlib.Path(__file__).resolve().parent.parent
SEED = 20261016

# ---- filler vocabulary: disjoint topics from every critical value below.
SITES = ['Harlington', 'Marsh Dean', 'Osmund', 'Ashby Fold', 'Pennock Ridge', 'Kestrel Bay',
         'Calver', 'Wrenford', 'Tilmore', 'Saltwick', 'Brackenhill', 'Low Fen']
STAFF = ['R. Aldergate', 'M. Peverell', 'T. Kesson', 'H. Ryland', 'S. Corless', 'J. Marbeck',
         'A. Thorne', 'P. Vandeleur', 'E. Brantwood', 'C. Ogilvie', 'F. Sarson', 'D. Wickenden']
CREWS = ['track crew 2', 'track crew 5', 'sleeper gang', 'point crew', 'drainage gang',
         'welding team', 'signalling fit-out crew', 'cable jointing pair']
ASSETS = ['turnout T-14', 'turnout T-22', 'level crossing LX-3', 'level crossing LX-8',
          'culvert C-19', 'retaining wall RW-4', 'bridge deck B-31', 'fence line F-77',
          'cable route CR-6', 'signal S-40', 'signal S-55', 'track circuit TC-212']
WORK = ['re-sleepering', 'ballast renewal', 'drainage clearing', 'point grinding', 'weld repairs',
        'painting', 'vegetation cutting', 'cable pulling', 'gauge measurement', 'tamping']
MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August',
          'September', 'October', 'November', 'December']

FILLER_TEMPLATES = [
    'Work order {order}: {work} at {asset} near {site}. Supervisor {staff}. Planned for the week of '
    '{day} {month}; the possession runs {n1} hours with {n2} staff on site.',
    'Inspection note: {asset} at {site} was examined by {staff}. Surface wear measured {mm} mm, '
    'within the maintenance band. Next inspection due {month}.',
    'Timesheet summary for {staff}: {n1} hours on {work} at {site}, {n2} hours on administrative '
    'tasks. Approved by the programme office.',
    'Logistics memo: {n1} tonnes of ballast delivered to {site} siding by train {train}. Unloading '
    'took {n2} minutes; the wagon brakes were checked before departure.',
    'Safety briefing record: {crew} briefed on {work} at {site}. Topics: track safety, tool checks, '
    'weather limits. Held by {staff}; attendance {n1}.',
    'Plant note: the {plant} used at {site} was returned to the depot with {hmk} hours on the '
    'clock. Fuel used {n1} litres. Booked in by {staff}.',
    'Environment spot check at {site}: noise {db} dB(A) at the nearest boundary during {work}, '
    'below the consent limit. Reptile fencing intact along fence line. Recorded by {staff}.',
    'Progress report: {work} at {asset} is {pct} % complete. Remaining: {n1} shifts. Constraint: '
    'no work within the {month} possession-free period. Owner {staff}.',
]
PLANTS = ['vibrator plate VP-2', 'road-railer RR-7', 'excavator EX-11', 'ballast regulator BR-4',
          'tamper TAM-3', 'mobile crane MC-8']


def filler_document(rng, index):
    t = rng.choice(FILLER_TEMPLATES)
    body = t.format(order=f'CFR-W-{1000 + rng.randrange(9000):04d}',
                    work=rng.choice(WORK), asset=rng.choice(ASSETS), site=rng.choice(SITES),
                    staff=rng.choice(STAFF), crew=rng.choice(CREWS), day=rng.randint(2, 27),
                    month=rng.choice(MONTHS), n1=rng.randint(3, 40), n2=rng.randint(2, 18),
                    mm=rng.randint(2, 24), train=f'{rng.randint(6, 9)}V{rng.randint(10, 99)}',
                    hmk=rng.randint(120, 9800), db=rng.randint(52, 74), pct=rng.choice([10, 25, 40, 60, 75, 90]),
                    plant=rng.choice(PLANTS))
    year = rng.choice([2025, 2026])
    month = rng.choice(MONTHS)
    day = rng.randint(2, 27)
    return f'CFR-F-{index:04d} ({year} {month} {day}): {body}'


# ---- hand-authored critical documents. Values here are the graded ground truth.
CRITICAL_DOCS = {
    'transformer-harlington': 'CFR-D-0201 (2026 March 3): Substation upgrade record, Harlington. '
                              'The auxiliary transformer T-3 at Harlington substation is rated 415 kVA, '
                              'fed from the local distribution board DB-H2.',
    'transformer-marshdean':   'CFR-D-0233 (2026 March 25): Substation upgrade record, Marsh Dean. '
                              'The auxiliary transformer T-1 at Marsh Dean substation is rated 630 kVA '
                              'and feeds the signalling supply panel SP-M1.',
    'transformer-osmund':      'CFR-D-0287 (2026 April 18): Substation upgrade record, Osmund. '
                              'The auxiliary transformer T-2 at Osmund substation is rated 290 kVA. '
                              'Cooling is ONAN; the silencer was replaced in the same visit.',
    'pump-rev-b':              'CFR-D-0114 Rev B (2026 February 10): Pump commissioning sheet, Osmund '
                              'depot. The MERIDIAN-7 booster pump discharge setpoint is 4.8 bar at '
                              'nominal duty. Impeller trim 172 mm.',
    'pump-rev-c':              'CFR-D-0114 Rev C (2026 March 12): Pump commissioning sheet, Osmund '
                              'depot. This revision supersedes Rev B after the dampener retrofit. '
                              'The MERIDIAN-7 booster pump discharge setpoint is 4.2 bar at nominal '
                              'duty. Impeller trim 172 mm unchanged.',
    'pump-7b':                 'CFR-D-0119 (2026 March 2): Pump commissioning sheet, Wrenford depot. '
                              'The MERIDIAN-7B booster pump (a later build of the same frame) '
                              'discharge setpoint is 5.1 bar. It shares the 172 mm impeller trim.',
    'interlock-ref':           'CFR-T-0221 (2026 April 2): Brake design note. The brake interlock '
                              'described here trips when the disc temperature exceeds the Class H '
                              'limit stated for the Osmund incline in CFR-T-0189. Reset is manual.',
    'class-h-limit':           'CFR-T-0189 (2026 March 21): Thermal assessment, Osmund incline. '
                              'The Class H limit for brake discs on the Osmund incline is 620 degrees '
                              'Celsius. Above this the disc coating degrades.',
    'siding-ashby':            'CFR-L-0301 (2026 April 7): Siding capacity survey. Ashby Fold siding '
                              'holds 14 wagons when loaded to the mark.',
    'siding-pennock':          'CFR-L-0318 (2026 April 9): Siding capacity survey. Pennock Ridge '
                              'siding holds 9 wagons; the foul-check board is 3 m past the buffer stop.',
    'siding-kestrel':          'CFR-L-0342 (2026 April 14): Siding capacity survey. Kestrel Bay '
                              'siding holds 17 wagons after the April extension.',
    'toll-early':              'CFR-A-0051 (2026 February 20): Tolling register. The Calver toll '
                              'exemption code for programme vehicles is TOL-7749.',
    'toll-late':               'CFR-A-0093 (2026 April 27): Tolling register update. The Calver toll '
                              'exemption code for programme vehicles is TOL-3316.',
    'repeat-rating-1':         'CFR-D-0356 (2026 May 6): Transformer load note. Under winter peak the '
                              'Marsh Dean transformer T-1 was recorded at 71 % of rating.',
    'repeat-rating-2':         'CFR-D-0361 (2026 May 11): Transformer load note. Under summer peak the '
                              'Marsh Dean transformer T-1 was recorded at 58 % of rating.',
}

# A distractor that quotes the superseded value (tests revision awareness).
CRITICAL_DOCS['pump-distractor'] = ('CFR-Q-0007 (2026 March 30): Quality audit trail. The audit '
                                    'checklist still cites the MERIDIAN-7 discharge setpoint of '
                                    '4.8 bar from CFR-D-0114 Rev B, pending paperwork update.')

QUESTIONS = [
    {
        'id': 'sum-three-depths',
        'documents': [('transformer-harlington', 0.15), ('transformer-marshdean', 0.50),
                      ('transformer-osmund', 0.85)],
        'question': 'What is the total rated capacity, in kVA, of the three auxiliary transformers '
                    'at Harlington, Marsh Dean and Osmund substations?',
        'reference': {'answer': '1335 kVA', 'compute': '415 + 630 + 290',
                      'citations': ['CFR-D-0201', 'CFR-D-0233', 'CFR-D-0287']},
        'grader': {'type': 'numeric', 'value': 1335, 'unit': 'kVA', 'tolerance': 0,
                   'forbid_values': [1205, 1465]},
    },
    {
        'id': 'revision-supersedes',
        'documents': [('pump-rev-b', 0.30), ('pump-rev-c', 0.60), ('pump-distractor', 0.62)],
        'question': 'According to the current revision of CFR-D-0114, what is the MERIDIAN-7 booster '
                    'pump discharge setpoint?',
        'reference': {'answer': '4.2 bar (Rev C, 2026 March 12)',
                      'citations': ['CFR-D-0114']},
        'grader': {'type': 'numeric', 'value': 4.2, 'unit': 'bar', 'tolerance': 0.,
                   'forbid_values': [], 'require': ['4.2'], 'disallow_answer': ['4.8']},
    },
    {
        'id': 'cross-reference-2step',
        'documents': [('interlock-ref', 0.55), ('class-h-limit', 0.20)],
        'question': 'At what brake disc temperature does the interlock described in CFR-T-0221 trip?',
        'reference': {'answer': '620 degrees Celsius (the Class H limit from CFR-T-0189)',
                      'compute': 'CFR-T-0221 points at CFR-T-0189, which states 620 C',
                      'citations': ['CFR-T-0221', 'CFR-T-0189']},
        'grader': {'type': 'numeric', 'value': 620, 'unit': 'celsius', 'tolerance': 0,
                   'forbid_values': [], 'require': ['620']},
    },
    {
        'id': 'synthesis-sidings',
        'documents': [('siding-ashby', 0.25), ('siding-pennock', 0.50), ('siding-kestrel', 0.75)],
        'question': 'How many wagons in total can the three sidings at Ashby Fold, Pennock Ridge and '
                    'Kestrel Bay hold together? Cite the survey document for each siding.',
        'reference': {'answer': '40 wagons (14 + 9 + 17)', 'compute': '14 + 9 + 17',
                      'citations': ['CFR-L-0301', 'CFR-L-0318', 'CFR-L-0342']},
        'grader': {'type': 'numeric', 'value': 40, 'unit': 'wagons', 'tolerance': 0,
                   'forbid_values': [23, 31, 26], 'require_citations': True},
    },
    {
        'id': 'refusal-warranty',
        'documents': [('pump-7b', 0.45), ('pump-rev-c', 0.46)],
        'question': 'What is the warranty period of the MERIDIAN-7 booster pump?',
        'reference': {'answer': 'not enough information (no document states a warranty period)',
                      'citations': []},
        'grader': {'type': 'refusal'},
    },
    {
        'id': 'near-identical-ids',
        'documents': [('pump-rev-c', 0.35), ('pump-7b', 0.40), ('pump-rev-b', 0.33)],
        'question': 'What is the discharge setpoint of the MERIDIAN-7B booster pump?',
        'reference': {'answer': '5.1 bar (CFR-D-0119)', 'citations': ['CFR-D-0119']},
        'grader': {'type': 'numeric', 'value': 5.1, 'unit': 'bar', 'tolerance': 0.,
                   'forbid_values': [], 'require': ['5.1'], 'disallow_answer': ['4.8']},
    },
    {
        'id': 'repeated-entity-values',
        'documents': [('repeat-rating-1', 0.30), ('repeat-rating-2', 0.70)],
        'question': 'What load, as a percentage of rating, was the Marsh Dean transformer T-1 '
                    'recorded at under summer peak?',
        'reference': {'answer': '58 % (CFR-D-0361; the 71 % figure is the winter peak, CFR-D-0356)',
                      'citations': ['CFR-D-0361']},
        'grader': {'type': 'numeric', 'value': 58, 'unit': 'percent', 'tolerance': 0,
                   'forbid_values': [], 'require': ['58'], 'disallow_answer': ['71']},
    },
    {
        'id': 'position-early',
        'documents': [('toll-early', 0.15)],
        'question': 'What is the Calver toll exemption code for programme vehicles?',
        'reference': {'answer': 'TOL-7749 (CFR-A-0051)', 'citations': ['CFR-A-0051']},
        'grader': {'type': 'code', 'value': 'TOL-7749', 'forbid': ['TOL-3316']},
    },
    {
        'id': 'position-late',
        'documents': [('toll-late', 0.85)],
        'question': 'What is the Calver toll exemption code for programme vehicles?',
        'reference': {'answer': 'TOL-3316 (CFR-A-0093)', 'citations': ['CFR-A-0093']},
        'grader': {'type': 'code', 'value': 'TOL-3316', 'forbid': ['TOL-7749']},
    },
]
INSTRUCTIONS = ('Answer using only the documents above. Keep the answer short. Cite the document '
                'IDs (e.g. CFR-D-0107) that support it. End your reply with a line of the form '
                '"Answer: <short answer>". If the documents do not state the information, end with '
                '"Answer: not enough information".')


def build_corpus_text(n_filler, case):
    """Documents: filler list with this case's critical documents inserted at their depths."""
    rng = random.Random(SEED + 17)
    docs = [filler_document(rng, i) for i in range(n_filler)]
    for key, depth in case['documents']:
        docs.insert(min(round(depth * len(docs)), len(docs)), CRITICAL_DOCS[key])
    body = '\n\n'.join(docs)
    return ('You are reviewing maintenance and engineering documents from the Calder Freight '
            'Railway upgrade programme.\n\n' + body + '\n\nQuestion: ' + case['question'] +
            '\n' + INSTRUCTIONS)


def main():
    out = {'seed': SEED, 'instructions': INSTRUCTIONS,
           'critical_documents': CRITICAL_DOCS, 'questions': QUESTIONS,
           'generator': {'module': 'scripts/ps5-lc-fixtures.py',
                         'note': 'filler_document(random.Random(seed + 17), i) for i in range(n), '
                                 'critical docs inserted at round(depth * len(docs)); deterministic'}}
    path = ROOT / 'testdata/lc-comprehension/corpus.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(out, indent=1) + '\n')
    # self-check: rebuild a corpus twice and confirm determinism
    assert build_corpus_text(50, QUESTIONS[0]) == build_corpus_text(50, QUESTIONS[0])
    print(path, len(QUESTIONS), 'questions,', len(CRITICAL_DOCS), 'critical docs')


if __name__ == '__main__':
    main()
