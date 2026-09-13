"""Bounded real-data parity for BNK v3, including raw audio/texture containers.

No device, guest runtime, asset mutation or extraction to named paths is involved.
The native probe returns framed bytes over stdout; Python independently reads
uncompressed ranges, and reuses the existing extractor for compressed entries.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lua_mod'))
from bnk_repack import read_bnk
from script_index import entry_bytes


def reference(path):
    data = path.read_bytes()
    base, version = struct.unpack_from('>II', data)
    assert version == 3
    if data[8] == 1:
        _, entries = read_bnk(path)
        return [(e['name'].encode(), entry_bytes(e)) for e in entries]
    assert data[8] == 0
    pos, table, inflater = 9, bytearray(), zlib.decompressobj()
    while True:
        stored, size = struct.unpack_from('>II', data, pos); pos += 8
        if stored == 0:
            break
        table.extend(inflater.decompress(data[pos:pos + stored])); pos += stored
    stream = io.BytesIO(table)

    def word():
        return struct.unpack('>I', stream.read(4))[0]

    result = []
    for _ in range(word()):
        name = stream.read(word()).removesuffix(b'\0')
        offset, size = word(), word()
        raw = data[base + offset:base + offset + size]
        assert len(raw) == size
        result.append((name, raw))
    assert stream.tell() == len(table)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--max-bank-bytes', type=int, default=4 << 20)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    results, skipped = [], []
    for path in sorted(args.root.rglob('*.bnk')):
        with path.open('rb') as file:
            header = file.read(9)
        if len(header) < 9 or struct.unpack_from('>I', header, 4)[0] != 3:
            skipped.append({'path': str(path), 'reason': 'not BNK v3'})
            continue
        if path.stat().st_size > args.max_bank_bytes:
            skipped.append({'path': str(path), 'reason': 'corpus size budget'})
            continue
        expected = reference(path)
        process = subprocess.run([str(args.probe), str(path)], capture_output=True, timeout=120)
        record = {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                  'flag': header[8], 'entries': len(expected), 'bytes_compared': 0,
                  'exit_code': process.returncode, 'error': process.stderr.decode(errors='replace')}
        if process.returncode == 0:
            stream = io.BytesIO(process.stdout)
            for name, raw in expected:
                name_size, = struct.unpack('<I', stream.read(4))
                actual_name = stream.read(name_size)
                size, = struct.unpack('<Q', stream.read(8))
                actual = stream.read(size)
                assert actual_name == name and actual == raw, (path, name)
                record['bytes_compared'] += size
            assert stream.tell() == len(process.stdout)
            record['passed'] = True
        else:
            record['passed'] = False
        results.append(record)
    report = {'banks': results, 'skipped': skipped, 'max_bank_bytes': args.max_bank_bytes,
              'passed': all(r['passed'] for r in results)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'passed': report['passed'], 'banks': len(results),
                      'entries': sum(r['entries'] for r in results),
                      'bytes_compared': sum(r['bytes_compared'] for r in results),
                      'skipped': len(skipped),
                      'failures': [r for r in results if not r['passed']]}, indent=2))
    return 0 if report['passed'] and results else 1


if __name__ == '__main__':
    raise SystemExit(main())
