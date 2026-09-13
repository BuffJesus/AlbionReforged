import argparse
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from read_tu1_table_capture import decode
from read_tu1_manifest_capture import decode as decode_manifest

parser = argparse.ArgumentParser()
parser.add_argument('--probe', required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='tu1-capture-') as folder:
    for mode in ['manifest', 'success', 'disabled', 'wrong_caller', 'unreadable', 'existing',
                 'unknown_manager', 'other_manager', 'bad_owner', 'null_source', 'base_direct', 'cache_then_miss']:
        path = Path(folder) / (mode + '.bin')
        subprocess.run([args.probe, mode, str(path)], check=True)
        if mode == 'manifest':
            blob = Path(str(path) + '.manifest').read_bytes()
            report = decode_manifest(blob)
            assert report['eligible'] == report['captured'] == 1 and not report['truncated']
            row = report['records'][0]
            assert row == dict(mount=0, hash=7, token=1, status=3, parent=0xD00,
                               parent_vtable=0x8200DEA0, list=0xB00, record=0xC00,
                               stored_ordinal=1, units=3, utf16be='0058002f004e', name='X/N')
            assert decode(path.read_bytes())['first_equal_selection'] == {'mount': 0, 'entry': 1}
            malformed = [blob[:i] for i in range(len(blob))] + [blob + b'x']
            for at, value in [(16,0),(16,2),(20,33),(24,256),(36,5),(40,0),(60,2049)]:
                changed = bytearray(blob)
                changed[at:at+4] = value.to_bytes(4,'big')
                malformed.append(changed)
            for bad in malformed:
                try: decode_manifest(bad)
                except ValueError: pass
                else: raise AssertionError('Malformed sidecar accepted')
        if mode == 'success':
            report = decode(path.read_bytes())
            assert report['first_equal_selection'] == {'mount': 0, 'entry': 9}
            assert report['mounts'][0]['observed_open_name'] == 'a.bnk'
            records = Path(folder) / 'records.bin'
            reader = Path(__file__).resolve().parents[2] / 'tools/read_tu1_table_capture.py'
            command = [sys.executable, str(reader), str(path), '--records-output', str(records)]
            subprocess.run(command, check=True, capture_output=True)
            assert records.read_bytes() == bytes.fromhex('000000070000000000000009')
            assert subprocess.run(command, capture_output=True).returncode != 0
            assert records.read_bytes() == bytes.fromhex('000000070000000000000009')
        if mode == 'existing':
            assert path.read_bytes() == b'keep'
