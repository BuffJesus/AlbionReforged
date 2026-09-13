"""Native selection contract; optional differential execution of original TU1 PPC."""
import argparse
import bisect
import hashlib
import json
from pathlib import Path
import random
import subprocess

from original_ppc_index import OriginalIndex


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--probe', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--original-text', type=Path)
    args = ap.parse_args()
    oracle = OriginalIndex(args.original_text.read_bytes()) if args.original_text else None
    rng = random.Random(0x82B47240)
    cases = []

    def add(name, mounts, queries, budget=1000000):
        cases.append((name, mounts, queries, budget))

    edges = [0, 1, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff]
    add('empty', [], edges, 0)
    add('empty_mounts', [(0, []), (-1, [])], edges, 0)
    add('unsigned_keys', [(0, [(h, i) for i, h in enumerate(reversed(edges))])], edges + [2])
    priorities = [-0x80000000, -1, 0, 1, 0x7fffffff]
    for a in priorities:
        for b in priorities:
            add(f'priority_{a}_{b}', [(a, [(1, 7)]), (b, [(1, 11)])], [0, 1, 2])
    add('equal_priority_later_mount', [(5, [(3, 10)]), (5, []), (5, [(3, 20)])], [3])
    add('same_hash_different_resources', [(0, [(42, 2)]), (1, [(42, 19)])], [42])
    add('budget_exact', [(0, [(1, 1)]), (0, [(1, 2)])], [1], 2)
    add('budget_counts_overwritten_inputs', [(0, [(1, 1)]), (0, [(1, 2)])], [1], 1)
    add('budget_zero_nonempty', [(0, [(0, 0)])], [0], 0)
    add('duplicate_distinct_payload', [(0, [(9, 28), (9, 44)])], [9])
    add('duplicate_same_payload', [(0, [(9, 28), (9, 28)])], [9])
    add('duplicate_lower_priority_still_unsupported', [(9, [(9, 1)]), (-1, [(9, 2), (9, 3)])], [9])
    add('max_entry_index', [(0, [(0xffffffff, 0xffffffff)])], [0xffffffff, 0])
    for n in range(240):
        pool = sorted(set(edges + [rng.getrandbits(32) for _ in range(rng.randrange(1, 80))]))
        mounts = []
        for _ in range(rng.randrange(1, 12)):
            entries = [(h, rng.getrandbits(32)) for h in rng.sample(pool, rng.randrange(len(pool)+1))]
            mounts.append((rng.choice(priorities), entries))
        add(f'random_{n}', mounts, pool + [rng.getrandbits(32) for _ in range(20)])

    chunks, expected, query_total, differential = [], [], 0, 0
    for name, mounts, queries, budget in cases:
        chunks.append(f'{len(mounts)} {budget} {len(queries)}')
        for priority, entries in mounts:
            chunks.append(f'{priority} {len(entries)}')
            chunks.extend(f'{h} {entry}' for h, entry in entries)
        chunks.append(' '.join(map(str, queries)))
        count = sum(len(entries) for _, entries in mounts)
        duplicate = any(len({h for h, _ in entries}) != len(entries) for _, entries in mounts)
        if count > budget or duplicate:
            expected.append((name, [4 if count > budget else 8, 0]))
            continue
        candidates = {}
        for m, (priority, entries) in enumerate(mounts):
            for h, entry in entries:
                candidates.setdefault(h, []).append((priority, m, entry))
        winners = {h: max(options, key=lambda x: (x[0], x[1])) for h, options in candidates.items()}
        wanted = [0, len(winners)]
        keys = sorted(winners)
        for q in queries:
            hit = winners.get(q)
            wanted.append((hit[1] << 32) | hit[2] if hit else (1 << 64) - 1)
            if oracle:
                position = oracle.lower_bound(keys, q)
                assert position == bisect.bisect_left(keys, q), (name, q, position)
                differential += 1
        query_total += len(queries)
        expected.append((name, wanted))
    if oracle:
        # Repeated hashes in an already sorted table also return the first equal
        # record. This does not settle the earlier batch sort's tie ordering.
        for keys in [[], [0], [1, 1, 1], edges, [0, 0, 1, 0x80000000, 0x80000000, 0xffffffff]]:
            for q in edges:
                assert oracle.lower_bound(keys, q) == bisect.bisect_left(keys, q)
                differential += 1
        for a in priorities:
            for b in priorities:
                assert oracle.replaces(a, b) == (a >= b), (a, b)
                differential += 1
        try:
            OriginalIndex(b'wrong executable')
            raise AssertionError('identity check accepted wrong bytes')
        except ValueError:
            pass
    run = subprocess.run([str(args.probe.resolve())], input='\n'.join(chunks)+'\n',
                         capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, (run.returncode, run.stderr)
    actual = run.stdout.splitlines()
    assert len(actual) == len(expected), (len(actual), len(expected))
    for line, (name, wanted) in zip(actual, expected):
        assert list(map(int, line.split())) == wanted, (name, line, wanted)
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(passed=True, cases=len(cases), native_queries=query_total,
                  original_ppc_cases=differential, original_ppc_instructions=oracle.steps if oracle else 0,
                  original_text_sha256=hashlib.sha256(args.original_text.read_bytes()).hexdigest() if oracle else None,
                  cases_by_name=[name for name, *_ in cases],
                  limits=['Unique hashes per mount only; intra-mount duplicates are rejected.',
                          'Hashes, priorities and mount order are inputs, not recovered path/registry behavior.',
                          'PPC execution covers the lower-bound leaf and signed-priority branch only.',
                          'No guest adapter installed; no guest call paths retired.'])
    (args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k not in ('cases_by_name', 'limits')}))


if __name__ == '__main__':
    main()
