import argparse
from pathlib import Path
import random
import subprocess
import tempfile
from archive_contract import bank

parser = argparse.ArgumentParser()
parser.add_argument('--probe', required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='albion-sources-') as root:
    root = Path(root)
    payload = random.Random(0x82B531E8).randbytes(12000)
    for compressed in [0, 1]:
        entry = {'payload': payload}
        if not compressed:
            entry['raw'] = payload
        inner = bank([entry], flag=compressed)
        # The selected duplicate is the second entry, and the outer layer is compressed.
        outer = bank([{'name': b'same.bnk', 'payload': b'not an archive'},
                      {'name': b'same.bnk', 'payload': inner}])
        (root/'wrapped.bin').write_bytes(b'x'*17 + inner + b'outside slice')
        (root/'outer.bnk').write_bytes(outer)
        (root/'expected.bin').write_bytes(payload)
        subprocess.run([args.probe, str(root/'wrapped.bin'), str(len(inner)),
                        str(root/'outer.bnk'), str(root/'expected.bin')], check=True)
