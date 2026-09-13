"""Native decoded-range contract, using independent zlib and byte slicing."""
import argparse
import json
from pathlib import Path
import random
import subprocess
import zlib
from archive_contract import bank


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bank', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records = []

    def check(path, index, offset, size, expected=None, code=None):
        result = subprocess.run([str(args.probe), str(path), str(index), '--range',
                                 str(offset), str(size)], capture_output=True, timeout=30)
        assert result.returncode == (0 if code is None else 20 + code), (path, offset, size, result.stderr)
        assert result.stdout == (expected if code is None else b''), (path, offset, size)
        records.append({'bank': str(path), 'index': index, 'offset': offset, 'size': size,
                        'exit_code': result.returncode, 'passed': True})

    # A full input block ending mid-deflate followed by an independent final block.
    # This reproduces the shipped layout without distributing game bytes.
    rng = random.Random(8017)
    random_bytes = rng.randbytes(40000)
    first = zlib.compress(random_bytes)[:32768]
    decoder = zlib.decompressobj()
    first_decoded = decoder.decompress(first)
    assert not decoder.eof and not decoder.unused_data
    tail = b'second independent stream\0' * 83
    raw = first + zlib.compress(tail)
    payload = first_decoded + tail
    compressed = args.output / 'two_blocks.bnk'
    compressed.write_bytes(bank([{'payload': payload, 'raw': raw, 'chunks': [len(first_decoded), len(tail)]}]))
    plain = args.output / 'raw.bnk'
    plain.write_bytes(bank([{'payload': payload, 'raw': payload}], flag=0))
    seam = len(first_decoded)
    ranges = [(0, 0), (0, 1), (0, len(payload)), (seam - 1, 1), (seam - 1, 2),
              (seam, len(tail)), (seam + 1, len(tail) - 1), (len(payload) - 1, 1),
              (len(payload), 0)]
    ranges += [(start, rng.randrange(len(payload) - start + 1))
               for start in (rng.randrange(len(payload) + 1) for _ in range(35))]
    for path in [compressed, plain]:
        for offset, size in ranges:
            check(path, 0, offset, size, payload[offset:offset + size])
        for offset, size in [(len(payload), 1), (len(payload) + 1, 0), (1, 2**64 - 1), (2**64 - 1, 1)]:
            check(path, 0, offset, size, code=9)
        check(path, 99, 0, 0, code=7)

    # Proves the range path does not decode unrelated blocks: a corrupt first
    # block must not prevent reading a valid second block (and vice versa).
    for label, altered, offset, size, expected in [
        ('bad_first', bytes([first[0] ^ 255]) + raw[1:], seam, len(tail), tail),
        ('bad_last', raw[:-1] + bytes([raw[-1] ^ 1]), 0, seam, first_decoded),
    ]:
        path = args.output / (label + '.bnk')
        path.write_bytes(bank([{'payload': payload, 'raw': altered, 'chunks': [seam, len(tail)]}]))
        check(path, 0, offset, size, expected)
        check(path, 0, 0, len(payload), code=5)

    if args.bank:
        from archive_corpus import reference
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lua_mod'))
        from bnk_repack import read_bnk
        _, entries = read_bnk(args.bank)
        expected_entries = reference(args.bank)
        for index, entry in enumerate(entries):
            if len(entry['chunks']) <= 1:
                continue
            payload = expected_entries[index][1]
            seam = 0
            for chunk in entry['chunks'][:-1]:
                seam += chunk
                for offset, size in [(seam - 1, 2), (seam, 1), (seam - 13, 47), (seam, len(payload) - seam)]:
                    check(args.bank, index, offset, size, payload[offset:offset + size])
    report = {'cases': records, 'passed': True}
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS {len(records)} decoded-range cases')


if __name__ == '__main__':
    main()
