#!/usr/bin/env python3
"""Collect Table 3 counts and input materialization memory; no data is bundled."""
import argparse
import csv
from pathlib import Path
import subprocess
from campaign import DATASETS

ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data-dir', type=Path, required=True)
    p.add_argument('--executable', type=Path, default=ROOT / 'build/dataset_read_memory')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--datasets', default=','.join(DATASETS))
    p.add_argument('--dry-run', action='store_true')
    args = p.parse_args()
    if args.output.exists() and not args.dry_run:
        p.error('Output directory already exists; choose a new directory')
    rows = []
    for key in args.datasets.split(','):
        if key not in DATASETS:
            p.error('Unknown dataset: ' + key)
        command = [str(args.executable.resolve()), '--records', str(args.data_dir.resolve() / (key + '_records.jsonl')), '--dataset', key]
        if args.dry_run:
            import shlex
            print(shlex.join(command))
            continue
        result = subprocess.run(command, capture_output=True, text=True, check=True)
        row = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
        rows.append(row)
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / (key + '.txt')).write_text(result.stdout)
    if rows:
        with (args.output / 'dataset-stats.csv').open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

if __name__ == '__main__':
    main()
