"""Make independent BNK fixtures and exercise production archive handle lifetime."""
import argparse
import json
from pathlib import Path
import subprocess
from archive_contract import bank

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--probe', type=Path, required=True)
ap.add_argument('--output', type=Path, required=True)
args = ap.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
a, b = args.output/'a.bnk', args.output/'b.bnk'
a.write_bytes(bank([{'payload': b'archive A'}]))
b.write_bytes(bank([{'payload': b'archive B'}]))
run = subprocess.run([str(args.probe.resolve()), str(a.resolve()), str(b.resolve())],
                     capture_output=True, text=True, timeout=60)
report = dict(passed=run.returncode == 0, stdout=run.stdout, stderr=run.stderr,
              close_acquire_races=128, generation_cycles=65535,
              scope='Native table and actual archive reads; no guest scheduling or ABI mapping.')
(args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
assert run.returncode == 0, report
print(run.stdout.strip())
