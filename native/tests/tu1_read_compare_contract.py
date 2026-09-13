import argparse
import json
from pathlib import Path
import subprocess
from archive_contract import bank

ap = argparse.ArgumentParser()
ap.add_argument('--probe', required=True, type=Path)
ap.add_argument('--output', required=True, type=Path)
args = ap.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
fixture = args.output/'read.bnk'
fixture.write_bytes(bank([{'payload': b'archive A'}]))
run = subprocess.run([str(args.probe.resolve()), str(fixture.resolve())], capture_output=True, text=True, timeout=30)
report = dict(passed=run.returncode == 0, stdout=run.stdout, stderr=run.stderr,
              scope='Post-completion native comparison core; guest observations are fixtures, not live captures.')
(args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
assert run.returncode == 0, report
print(run.stdout.strip())
