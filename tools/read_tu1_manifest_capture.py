"""Read bounded TU1MAN01 sidecar evidence; names are counted UTF-16 units."""
import argparse
import json
import struct
from pathlib import Path


def decode(blob):
    if len(blob) < 24 or len(blob) > 24 + 32 * (40 + 4096) or blob[:8] != b'TU1MAN01':
        raise ValueError('Invalid manifest capture header or size')
    manager, query, total, count = struct.unpack_from('>4I', blob, 8)
    if not manager or not 1 <= total <= 65536 or count != min(total, 32):
        raise ValueError('Invalid capture counts or manager')
    offset, rows = 24, []
    for _ in range(count):
        if len(blob) - offset < 40:
            raise ValueError('Truncated manifest record')
        mount, key, token, status, parent, vtable, names, record, ordinal, units = struct.unpack_from('>10I', blob, offset)
        offset += 40
        if mount >= 256 or status > 4 or units > 2048 or units * 2 > len(blob) - offset:
            raise ValueError('Invalid manifest record')
        if status == 3:
            if not token or not parent or not names or not record:
                raise ValueError('Copied manifest record has missing identity')
        elif any((parent, vtable, names, record, ordinal, units)):
            raise ValueError('Failed capture contains published payload')
        raw = blob[offset:offset + units * 2]
        offset += units * 2
        rows.append(dict(mount=mount, hash=key, token=token, status=status,
                         parent=parent, parent_vtable=vtable, list=names, record=record,
                         stored_ordinal=ordinal, units=units, utf16be=raw.hex(),
                         name=raw.decode('utf-16-be', errors='surrogatepass') if status == 3 else None))
    if offset != len(blob):
        raise ValueError('Trailing bytes in manifest capture')
    return dict(manager=manager, query=query, eligible=total, captured=count,
                truncated=total > count, records=rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    args = ap.parse_args()
    with args.capture.open('rb') as stream:
        blob = stream.read(24 + 32 * (40 + 4096) + 1)
    print(json.dumps(decode(blob), indent=2, ensure_ascii=True))


if __name__ == '__main__':
    main()
