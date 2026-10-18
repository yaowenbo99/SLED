#!/usr/bin/env python3
"""Run SLED progressive timing or live snapshots for Figures 15 and 23.

Timing and snapshot jobs deliberately use separate invocations and directories.
All records are supplied externally; this script performs no plotting.
"""
import argparse
from pathlib import Path
import subprocess
import sys
from campaign import atomic_json, digest, file_hash, read_info, read_json

ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data-dir', type=Path)
    p.add_argument('--records', type=Path, help='Use one explicit JSONL fixture instead of --data-dir')
    p.add_argument('--dataset', default='synthetic', help='Label for --records')
    p.add_argument('--datasets', default='nyc_taxi_zones,sla_cadastral_land_parcel')
    p.add_argument('--executable', type=Path, default=ROOT / 'build/sled')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--mode', choices=('timing', 'snapshots'), required=True)
    p.add_argument('--resolution', default='1600x1200')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--resume', action='store_true')
    args = p.parse_args()
    if (args.records is None) == (args.data_dir is None):
        p.error('Provide exactly one of --data-dir or --records')
    binary = args.executable.resolve()
    inputs = ({args.dataset: args.records.resolve()} if args.records else
              {key: args.data_dir.resolve() / (key + '_records.jsonl') for key in args.datasets.split(',')})
    binary_hash = file_hash(binary)
    outputs = []
    for dataset, records in inputs.items():
        if not dataset or Path(dataset).name != dataset or dataset in ('.', '..'):
            p.error('Dataset labels must be single directory names')
        input_hash = file_hash(records)
        for repeat in range(1, 4 if args.mode == 'timing' else 2):
            out = args.output.resolve() / args.mode / dataset / ('r%02d' % repeat)
            command = [str(binary), '--records', str(records), '--dataset', dataset,
                       '--method', 'SLED', '--resolution', args.resolution, '--threads', '1',
                       '--progressive-checkpoints', '25,50,75,100', '--no-progress',
                       '--out-dir', str(out), '--run-name', 'progressive']
            command += ['--discard-output'] if args.mode == 'timing' else ['--progressive-snapshots']
            fingerprint = digest(dict(command=command, binary_sha256=binary_hash, input_sha256=input_hash))
            receipt = out / 'completed.json'
            if args.dry_run:
                import shlex
                print(shlex.join(command))
                continue
            if receipt.exists():
                old = read_json(receipt)
                if not args.resume or old['fingerprint'] != fingerprint:
                    raise ValueError('Existing or changed run: ' + str(out))
                if any(not Path(path).is_file() or file_hash(path) != sha for path, sha in old['files'].items()):
                    raise ValueError('Completed outputs changed: ' + str(out))
                outputs.append(old)
                continue
            if out.exists():
                raise ValueError('Incomplete output exists; move it aside before retrying: ' + str(out))
            out.mkdir(parents=True)
            with (out / 'process.log').open('w') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
            info_paths = list(out.rglob('run_info.txt'))
            if len(info_paths) != 1:
                raise ValueError('Expected exactly one run_info.txt: ' + str(out))
            meta = read_info(info_paths[0])
            if meta.get('timing_contract') != 'v2_dense_memory':
                raise ValueError('Unsupported timing contract')
            result = dict(dataset=dataset, mode=args.mode, repetition=repeat, fingerprint=fingerprint,
                          binary_sha256=binary_hash, input_sha256=input_hash, info=meta,
                          files={str(path): file_hash(path) for path in out.rglob('*') if path.is_file()})
            atomic_json(receipt, result)
            outputs.append(result)
            print('Completed ' + str(out), flush=True)
    if not args.dry_run:
        atomic_json(args.output.resolve() / (args.mode + '-runs.json'), outputs)

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        sys.exit(str(exc))
