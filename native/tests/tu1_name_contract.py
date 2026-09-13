import argparse
from pathlib import Path
import struct
import subprocess
from original_ppc_index import OriginalIndex


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--probe', type=Path, required=True)
    ap.add_argument('--original-text', type=Path)
    args = ap.parse_args()
    oracle = OriginalIndex(args.original_text.read_bytes()) if args.original_text else None
    units = list(range(65536))
    expected = []
    for unit in units:
        folded = unit + 32 if 65 <= unit <= 90 else unit
        if oracle:
            regs = [0] * 32
            regs[3] = unit
            oracle.execute(0x8217AED0, 0x8217AEF8, regs, bytearray())
            assert regs[3] == folded, (unit, regs[3], folded)
        expected.append(92 if folded == 47 else folded)
    blob = struct.pack('>65536H', *units)
    run = subprocess.run([str(args.probe), '65536'], input=blob, capture_output=True, timeout=30)
    assert run.returncode == 0 and run.stdout == struct.pack('>65536H', *expected)
    samples = ['', 'DATA/Script.LUA', 'A//B/../C ', '\\Already\\Mixed', '\0A/\ud800\udfff\uffff']
    for sample in samples:
        blob = sample.encode('utf-16-be', errors='surrogatepass')
        reference = ''.join(chr(ord(c)+32) if 'A' <= c <= 'Z' else '\\' if c == '/' else c for c in sample)
        run = subprocess.run([str(args.probe), str(len(blob)//2)], input=blob, capture_output=True, timeout=30)
        assert run.returncode == 0 and run.stdout == reference.encode('utf-16-be', errors='surrogatepass')
        if blob:
            denied = subprocess.run([str(args.probe), str(len(blob)//2-1)], input=blob, capture_output=True, timeout=30)
            assert denied.returncode == 24 and not denied.stdout
    print(f'65536 normalized units and 9 string/budget cases passed; original PPC checks={65536 if oracle else 0}')


if __name__ == '__main__': main()
