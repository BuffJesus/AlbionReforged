"""Inspect bounded TU1TAB01/02 captures; names are observations, not file identity."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def decode(blob):
    if len(blob) < 24 or blob[:8] not in (b'TU1TAB01', b'TU1TAB02'):
        raise ValueError('invalid TU1TAB header')
    version = blob[:8].decode('ascii')
    manager, query, mount_bytes, record_bytes = struct.unpack_from('>4I', blob, 8)
    if (not manager or manager > 0xFFFFFFFF - 0x40 or mount_bytes % 36 or
            record_bytes % 12 or mount_bytes > 256 * 36 or record_bytes > 65536 * 12 or
            len(blob) < 24 + mount_bytes + record_bytes):
        raise ValueError('invalid bounds, budget, or file length')
    mounts = []
    for pos in range(24, 24 + mount_bytes, 36):
        mounts.append({'object_address': f'{struct.unpack_from(">I", blob, pos)[0]:08x}',
                       'priority': struct.unpack_from('>i', blob, pos + 32)[0]})
    end = 24 + mount_bytes + record_bytes
    records = list(struct.iter_unpack('>III', blob[24 + mount_bytes:end]))
    if any(m >= len(mounts) for _, m, _ in records):
        raise ValueError('record references missing mount')
    if any(a[0] > b[0] for a, b in zip(records, records[1:])):
        raise ValueError('records not unsigned-hash ordered')
    selected = next(({'mount': m, 'entry': e} for h, m, e in records if h == query), None)
    if version == 'TU1TAB02':
        for mount in mounts:
            if len(blob) - end < 12:
                raise ValueError('truncated name header')
            status, vtable, units = struct.unpack_from('>III', blob, end)
            end += 12
            if (status > 3 or units > 512 or (status != 3 and units) or
                    (status == 3 and vtable != 0x8200DEA0) or len(blob) - end < units * 2):
                raise ValueError('invalid directory name metadata')
            mount.update(name_status=['unreadable', 'unsupported_object', 'invalid', 'copied'][status],
                         vtable=f'{vtable:08x}')
            if status == 3:
                name = blob[end:end + units * 2].decode('utf-16-be', errors='strict')
                if '\0' in name:
                    raise ValueError('embedded NUL in name')
                mount['observed_open_name'] = name
            end += units * 2
    if end != len(blob):
        raise ValueError('unexpected trailing data')
    return {'format': version, 'sha256': hashlib.sha256(blob).hexdigest(),
            'manager': f'{manager:08x}', 'query': f'{query:08x}', 'mounts': mounts,
            'record_count': len(records),
            'adjacent_equal_hash_pairs': sum(a[0] == b[0] for a, b in zip(records, records[1:])),
            'first_equal_selection': selected,
            'bank_identity': 'unresolved; observed names may resolve through another mounted archive'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--records-output', type=Path,
                        help='exclusive-create raw table for archive_read_view_probe')
    args = parser.parse_args()
    # Bound input before loading it; reject partial and appended files in decode.
    if args.capture.stat().st_size > 24 + 256 * (36 + 12 + 1024) + 65536 * 12:
        parser.error('capture exceeds budget')
    blob = args.capture.read_bytes()
    report = decode(blob)
    if args.records_output:
        mount_bytes = struct.unpack_from('>I', blob, 16)[0]
        record_bytes = struct.unpack_from('>I', blob, 20)[0]
        with args.records_output.open('xb') as output:
            output.write(blob[24 + mount_bytes:24 + mount_bytes + record_bytes])
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
