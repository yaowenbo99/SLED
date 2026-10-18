#!/usr/bin/env python3
"""Validate an exact built executable using only bundled synthetic records."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, default=ROOT / 'build/sled')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/validation.json')
    args = parser.parse_args()
    binary = args.executable.resolve()
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    report = {'passed': False, 'binary_sha256': digest, 'checks': []}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    commands = [
        ('campaign_unit_tests', [sys.executable, str(ROOT / 'tests/test_campaign.py')]),
        ('geometry_interaction_progressive_contract', [sys.executable, str(ROOT / 'tests/round4_contract.py'), str(binary)]),
        ('cli_config_ratio_baseline_smoke', ['bash', str(ROOT / 'tests/smoke.sh'), str(binary)]),
    ]
    with tempfile.TemporaryDirectory(prefix='sled-validation-') as work:
        for name, command in commands:
            print('Checking ' + name, flush=True)
            result = subprocess.run(command, cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            log_path = args.output.parent / (name + '.log')
            log_path.write_text(result.stdout)
            report['checks'].append({'name': name, 'passed': result.returncode == 0})
            args.output.write_text(json.dumps(report, indent=2) + '\n')
            if result.returncode:
                print(result.stdout[-12000:], file=sys.stderr)
                raise SystemExit('Validation failed; full log: ' + str(log_path))
    if hashlib.sha256(binary.read_bytes()).hexdigest() != digest:
        raise SystemExit('Executable changed during validation')
    report['passed'] = True
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('Validation passed: ' + str(args.output))

if __name__ == '__main__':
    main()
