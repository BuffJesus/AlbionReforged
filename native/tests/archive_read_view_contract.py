"""Resolved-table snapshots select and read native archives without guest execution."""
import argparse
import json
from pathlib import Path
import random
import struct
import subprocess
import zlib
from archive_contract import bank
from original_ppc_index import OriginalIndex


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--probe', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--bank', type=Path, action='append', default=[])
    ap.add_argument('--original-text', type=Path)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    rng = random.Random(0x82B453F8)
    first = zlib.compress(rng.randbytes(40000))[:32768]
    decoded_first = zlib.decompressobj().decompress(first)
    tail = b'owned range tail' * 91
    large = decoded_first + tail
    payloads = [[b'first duplicate', b'second duplicate', large], [b'other mount', b'']]
    a, b = args.output/'a.bnk', args.output/'b.bnk'
    a.write_bytes(bank([{'name': b'duplicate', 'payload': payloads[0][0]},
                        {'name': b'duplicate', 'payload': payloads[0][1]},
                        {'name': b'large', 'payload': large, 'raw': first+zlib.compress(tail),
                         'chunks': [len(decoded_first), len(tail)]}]))
    b.write_bytes(bank([{'name': b'raw', 'payload': p, 'raw': p} for p in payloads[1]], flag=0))
    records = []
    oracle = OriginalIndex(args.original_text.read_bytes()) if args.original_text else None
    original_checks = 0

    def check(label, rows, query, offset, count, expected=None, code=None, budget=1000000, mode=None, paths=None):
        table = args.output / f'{label}.table'
        table.write_bytes(rows if isinstance(rows, bytes) else b''.join(struct.pack('>III', *row) for row in rows))
        banks = paths or (a, b)
        cmd = [str(args.probe.resolve()), str(table.resolve()), *(str(p.resolve()) for p in banks),
               str(query), str(offset), str(count), str(budget)]
        if mode: cmd.append(mode)
        run = subprocess.run(cmd, capture_output=True, timeout=30)
        assert run.returncode == (0 if code is None else 20+code), (label, run.returncode, run.stderr)
        assert run.stdout == (expected if code is None else b''), (label, len(run.stdout), expected)
        records.append(dict(case=label, passed=True, exit_code=run.returncode))

    rows = [(0, 0, 1), (1, 1, 0), (0x7fffffff, 0, 2), (0x80000000, 0, 0), (0xffffffff, 1, 1)]
    for h, m, e in rows:
        p = payloads[m][e]
        check(f'full_{h}', rows, h, 0, len(p), p)
    check('missing', rows, 2, 0, 0, code=7)
    check('empty_table', [], 0, 0, 0, code=7, budget=0)
    check('duplicate_hash_first_record', [(7, 0, 1), (7, 0, 0)], 7, 0, 16, b'second duplicate')
    check('duplicate_hash_reversed_record', [(7, 0, 0), (7, 0, 1)], 7, 0, 15, b'first duplicate')
    check('duplicate_hash_other_mount', [(7, 1, 0), (7, 0, 0)], 7, 0, 11, b'other mount')
    check('unordered', [(9, 0, 0), (8, 0, 1)], 8, 0, 0, code=3)
    check('signed_order_wrong', [(0x80000000, 0, 0), (0x7fffffff, 0, 1)], 0, 0, 0, code=3)
    check('unknown_mount', [(9, 2, 0)], 9, 0, 0, code=9)
    check('unknown_entry', [(9, 0, 3)], 9, 0, 0, code=9)
    check('invalid_unselected_row', [(9, 0, 0), (10, 0xffffffff, 0)], 9, 0, 0, code=9)
    check('null_mount', [(9, 1, 0)], 9, 0, 0, mode='--null-mount', code=3)
    check('null_span', rows, 0, 0, 0, mode='--null-span', code=3)
    check('null_empty_span', b'', 0, 0, 0, mode='--null-span', code=7)
    check('input_budget', rows, 0, 0, 0, budget=4, code=4)
    check('exact_input_budget', rows, 0, 0, 16, b'second duplicate', budget=5)
    check('wrong_endian_locations', struct.pack('<III', 0x12345678, 0, 1), 0x78563412, 0, 0, code=9)
    for size in range(1, 12):
        check(f'truncated_{size}', b'\0'*size, 0, 0, 0, code=3)
    seam = len(decoded_first)
    slices = [(0, 0), (seam-1, 3), (seam, len(tail)), (len(large), 0)]
    slices += [(s, rng.randrange(len(large)-s+1)) for s in [rng.randrange(len(large)+1) for _ in range(35)]]
    for i, (offset, count) in enumerate(slices):
        check(f'range_{i}', rows, 0x7fffffff, offset, count, large[offset:offset+count])
    for i, (offset, count) in enumerate([(len(large), 1), (len(large)+1, 0), (1, 2**64-1), (2**64-1, 2)]):
        check(f'range_invalid_{i}', rows, 0x7fffffff, offset, count, code=9)
    for iteration in range(100):
        candidates = [(rng.choice([0, 1, 0x7fffffff, 0x80000000, 0xffffffff]),
                       rng.randrange(2), rng.randrange(2)) for _ in range(rng.randrange(1, 30))]
        candidates.sort(key=lambda row: row[0])
        query = rng.choice([0, 1, 2, 0x7fffffff, 0x80000000, 0xffffffff])
        first_match = next((row for row in candidates if row[0] == query), None)
        if oracle:
            position = oracle.lower_bound([row[0] for row in candidates], query)
            actual = candidates[position] if position < len(candidates) and candidates[position][0] == query else None
            assert actual == first_match
            original_checks += 1
        if first_match:
            _, m, e = first_match
            payload = payloads[m][e]
            check(f'ordered_random_{iteration}', candidates, query, 0, len(payload), payload)
        else:
            check(f'ordered_random_{iteration}', candidates, query, 0, 0, code=7)
    broken = args.output/'broken.bnk'
    packed = first + zlib.compress(tail)
    broken.write_bytes(bank([{'payload': large, 'raw': packed[:-1] + bytes([packed[-1] ^ 1]),
                             'chunks': [len(decoded_first), len(tail)]}]))
    check('unread_corrupt_block_isolated', [(1, 0, 0)], 1, 0, 32, decoded_first[:32], paths=(broken, b))
    check('selected_corrupt_block_no_partial_output', [(1, 0, 0)], 1, 0, len(large), code=5, paths=(broken, b))
    # Real banks supply bytes; records below are fixture-selected, not a live
    # manager dump. Include duplicate names and multiblock seams when available.
    if args.bank:
        from archive_corpus import reference
        for n, path in enumerate(args.bank):
            entries = reference(path)
            selected = sorted(set([0, len(entries)-1] + list(range(min(8, len(entries))))))
            selected += [i for i, (_, p) in enumerate(entries) if len(p) > 32768][:5]
            duplicate_names = {name for name, _ in entries if sum(other == name for other, _ in entries) > 1}
            selected += [i for i, (name, _) in enumerate(entries) if name in duplicate_names]
            for i in sorted(set(selected)):
                p = entries[i][1]
                offset = max(0, min(len(p)//2, 32767))
                count = min(511, len(p)-offset)
                check(f'bank_{n}_entry_{i}', [(i, 0, i)], i, offset, count, p[offset:offset+count], paths=(path, path))
    report = dict(passed=True, cases=len(records), records=records, original_ppc_checks=original_checks,
                  scope='Native resolved snapshots and actual archive reads; live guest capture and ABI adapter remain untested.')
    (args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(dict(passed=True, cases=len(records))))


if __name__ == '__main__': main()
