"""Independent Python/zlib fixtures plus optional byte parity on a user-owned BNK."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib


def word(value):
    return struct.pack('>I', value)


def bank(entries, *, flag=1, version=3, table_transform=lambda x: x,
         terminator=b'\0' * 8, split_table=False):
    table, data = word(len(entries)), b''
    for e in entries:
        name = e.get('name', b'Folder\\Item.bin') + b'\0'
        raw = e.get('raw', zlib.compress(e.get('payload', b'payload')))
        size = e.get('size', len(e.get('payload', b'payload')))
        table += word(len(name)) + name + word(e.get('offset', len(data))) + word(size)
        if flag:
            chunks = e.get('chunks', [size])
            table += word(len(raw)) + word(len(chunks)) + b''.join(map(word, chunks))
        data += raw
    table = table_transform(table)
    compressed = zlib.compress(table)
    if split_table:
        mid = len(compressed) // 2
        frames = word(mid) + word(0) + compressed[:mid]
        frames += word(len(compressed) - mid) + word(len(table)) + compressed[mid:]
    else:
        frames = word(len(compressed)) + word(len(table)) + compressed
    header = word(9 + len(frames) + len(terminator)) + word(version) + bytes([flag])
    return header + frames + terminator + data


def run(probe, path, name=None, mode=None):
    args = [str(probe), str(path)]
    if name is not None:
        args.append(name)
    if mode:
        args.append(mode)
    return subprocess.run(args, capture_output=True, timeout=60)


def contract(probe, output):
    payload = (b'Native owned archive data\0' * 128) + bytes(range(256))
    complete = zlib.compress(payload)
    compressor = zlib.compressobj()
    framed = compressor.compress(payload) + compressor.flush(zlib.Z_SYNC_FLUSH)
    records = []

    def check(label, content, expected=None, code=None, name='Folder\\Item.bin', mode=None):
        path = output / (label + '.bnk')
        path.write_bytes(content)
        result = run(probe, path, name, mode)
        if code is None:
            assert result.returncode == 0, (label, result.stderr)
            assert result.stdout == expected, (label, len(result.stdout), len(expected))
        else:
            assert result.returncode == 20 + code, (label, result.returncode, result.stderr)
            assert result.stdout == b'', label
        records.append({'case': label, 'exit_code': result.returncode,
                        'stderr': result.stderr.decode().strip(), 'passed': True})

    e = {'payload': payload}
    check('complete_stream', bank([e]), payload)
    check('sync_flush', bank([{**e, 'raw': framed}]), payload)
    check('split_continuous_table', bank([e], split_table=True), payload)
    check('concurrent_owned_reads', bank([e]), payload, mode='--concurrent')
    check('uncompressed', bank([{**e, 'raw': payload}], flag=0), payload)
    check('empty_stream', bank([{'payload': b''}]), b'')
    check('empty_uncompressed', bank([{'payload': b'', 'raw': b''}], flag=0), b'')
    check('missing_entry', bank([e]), code=7, name='missing')
    check('exact_case', bank([e]), code=7, name='folder\\item.bin')
    check('no_script_rewriting', bank([e]), code=7, name='Item')
    check('entry_budget', bank([e]), code=4, mode='--small-budget')
    check('bad_checksum', bank([{**e, 'raw': complete[:-1] + bytes([complete[-1] ^ 1])}]), code=5)
    check('missing_checksum', bank([{**e, 'raw': complete[:-4]}]), code=5)
    check('truncated_flush', bank([{**e, 'raw': framed[:-2]}]), code=5)
    check('short_output', bank([{**e, 'size': len(payload) + 1}]), code=6)
    check('long_output', bank([{**e, 'size': len(payload) - 1}]), code=6)
    check('chunk_sum', bank([{**e, 'chunks': [len(payload) - 1]}]), code=6)
    check('trailing_stream_bytes', bank([{**e, 'raw': complete + b'junk'}]), code=3)
    check('duplicate_names', bank([e, e]), code=8)
    duplicate_entries = [e, {'payload': b'different bytes'}]
    framed_result = b''
    for entry in duplicate_entries:
        name = b'Folder\\Item.bin'
        framed_result += struct.pack('<I', len(name)) + name
        framed_result += struct.pack('<Q', len(entry['payload'])) + entry['payload']
    check('duplicate_index_reads', bank(duplicate_entries), framed_result, name=None)
    check('embedded_nul', bank([{**e, 'name': b'Folder\0Item'}]), code=3)
    check('range_overflow', bank([{**e, 'offset': 0xffffffff}]), code=3)
    check('truncated_header', b'\0' * 8, code=3)
    check('unsupported_v2', bank([e], version=2), code=2)
    check('unsupported_flag', bank([e], flag=2), code=2)
    check('missing_terminator', bank([e], terminator=b''), code=3)
    check('nonzero_terminator', bank([e], terminator=word(0) + word(1)), code=3)
    check('trailing_table', bank([e], table_transform=lambda t: t + b'junk'), code=3)
    check('truncated_table', bank([e], table_transform=lambda t: t[:-1]), code=3)
    check('empty_archive', bank([]), code=7)
    check('bad_table_count', bank([e], table_transform=lambda t: word(0xffffffff) + t[4:]), code=4)
    return records


def parity(probe, source):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lua_mod'))
    from bnk_repack import read_bnk
    from script_index import entry_bytes
    _, entries = read_bnk(source)
    result = run(probe, source)
    assert result.returncode == 0, result.stderr.decode()
    data, pos, decoded_bytes, streams, sync_final, full_final, nonfinal = result.stdout, 0, 0, 0, 0, 0, 0
    entry_hashes = []
    for e in entries:
        name_size, = struct.unpack_from('<I', data, pos); pos += 4
        name = data[pos:pos + name_size].decode(); pos += name_size
        size, = struct.unpack_from('<Q', data, pos); pos += 8
        actual = data[pos:pos + size]; pos += size
        expected = entry_bytes(e)
        assert name == e['name'] and actual == expected and size == e['dsz'], name
        decoded_bytes += size
        for index, offset in enumerate(range(0, len(e['raw']), 32768)):
            raw = e['raw'][offset:offset + 32768]
            decoder = zlib.decompressobj()
            decoded = decoder.decompress(raw)
            assert len(decoded) == e['chunks'][index] and not decoder.unused_data
            streams += 1
            if offset + len(raw) < len(e['raw']):
                nonfinal += 1
            elif decoder.eof:
                full_final += 1
            else:
                assert raw.endswith(b'\0\0\xff\xff')
                sync_final += 1
        entry_hashes.append({'name': name, 'size': size, 'sha256': hashlib.sha256(actual).hexdigest()})
    assert pos == len(data)
    return {'source': str(source.resolve()), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
            'entries_matched': len(entries), 'decoded_bytes': decoded_bytes,
            'streams': streams, 'sync_flush_final_blocks': sync_final,
            'complete_final_streams': full_final, 'nonfinal_blocks': nonfinal,
            'entry_hashes': entry_hashes}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bank', type=Path, action='append', default=[])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'contract': contract(args.probe, args.output),
              'parity': [parity(args.probe, bank) for bank in args.bank]}
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"PASS {len(report['contract'])} contract cases; "
          f"{sum(p['entries_matched'] for p in report['parity'])} real entries matched")


if __name__ == '__main__':
    main()
