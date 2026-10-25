#!/usr/bin/env python3
"""Run an existing production benchmark and retain comparable measurements."""
import argparse
import csv
from datetime import datetime
import hashlib
import json
import math
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import time
import uuid


def metrics(output):
    found = {}
    def flatten(value, prefix=''):
        if isinstance(value, dict):
            for key, item in value.items():
                flatten(item, f'{prefix}.{key}' if prefix else key)
        elif isinstance(value, (float, int)) and not isinstance(value, bool):
            if not math.isfinite(value):
                raise ValueError(f'Non-finite measurement: {prefix}')
            found[prefix] = value
    header = None
    rows = {}
    for line in output.splitlines():
        line = line.strip()
        if line.startswith('{'):
            flatten(json.loads(line))
        elif line.startswith('width,height,range,iteration,'):
            header = next(csv.reader([line]))
        elif header and re.match(r'^\d+,\d+,(HDR|SDR),', line):
            row = dict(zip(header, next(csv.reader([line]))))
            for key in header[4:]:
                if key.endswith(('_ms', '_bytes')):
                    name = f'{row["width"]}x{row["height"]}/{row["range"]}/{key}'
                    rows.setdefault(name, []).append(float(row[key]))
        elif line.startswith('BENCH ') and 'wall_ms=' in line:
            words = line.split()
            for key, value in re.findall(r'(\w+)=(\S+)', line):
                found[f'{words[1]}/{words[2]}/{key}'] = float(value)
    found.update({name: statistics.median(values) for name, values in rows.items()})
    if any(not math.isfinite(v) for v in found.values()):
        raise ValueError('Non-finite benchmark measurement')
    return found


def record(name, command, root):
    root = root.resolve()
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8]
    folder = root / 'runs' / (name + '-' + stamp)
    folder.mkdir(parents=True)
    output_file = folder / 'measurements.json'
    actual = [str(output_file) if arg == '{output}' else arg for arg in command]
    cache = root.parent / 'CMakeCache.txt'
    config = cache.read_bytes() if cache.is_file() else b'no-cmake-cache'
    identity = {'host': platform.node(), 'system': platform.platform(),
                'machine': platform.machine(), 'command': command,
                'configuration': hashlib.sha256(config).hexdigest()}
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:20]
    baseline_path = root / 'baselines' / key / (name + '.json')
    report = {'name': name, 'identity': identity, 'comparison': 'not_completed'}
    try:
        started = time.monotonic()
        result = subprocess.run(actual, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
        raw_output = result.stdout or b''
        (folder / 'output.log').write_bytes(raw_output)
        # CTest/JSON and our C++ sources use UTF-8. Preserve raw diagnostics too,
        # including a native dependency that emits a different Windows code page.
        output = raw_output.decode('utf-8', errors='replace')
        print(output, end='')
        if result.returncode:
            report['returncode'] = result.returncode
            return result.returncode
        if '{output}' in command:
            extra = output_file.read_text(encoding='utf-8')
            output += '\n' + extra
            print(extra)
        measured = metrics(output)
        if not measured:
            raise ValueError('Benchmark completed without recognized measurements')
        # The interaction probe historically printed errors but returned zero.
        for field in ('present_errors', 'presentation.failed'):
            if measured.get(field, 0):
                raise ValueError(f'Benchmark reports {field}={measured[field]}')
        if 'input_events' in measured and measured.get('handled') != measured['input_events']:
            raise ValueError('Interaction benchmark did not handle all input events')
        if re.search(r'"stopped"\s*:\s*false', output):
            raise ValueError('Interaction worker did not stop')
        measured['process_elapsed_seconds'] = time.monotonic() - started
        report['measurements'] = measured
        executable = Path(command[0])
        if executable.is_file():
            report['executable_sha256'] = hashlib.sha256(executable.read_bytes()).hexdigest()
        identity['display_geometry'] = {k: measured[k] for k in
            ('active_dpr','source_dpr','target_width','target_height') if k in measured}
        key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:20]
        baseline_path = root / 'baselines' / key / (name + '.json')
        if baseline_path.exists():
            previous = json.loads(baseline_path.read_text(encoding='utf-8'))['measurements']
            report['comparison'] = 'compared'
            report['delta_percent'] = {n: (v / previous[n] - 1) * 100
                                       for n, v in measured.items() if previous.get(n, 0) > 0}
        else:
            report['comparison'] = 'first_measurement'
            baseline_path.parent.mkdir(parents=True, exist_ok=True)
            baseline_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        report['baseline'] = str(baseline_path)
        report['meaning'] = 'Measurements and existing assertions completed; timing deltas require review, not a claim of no performance regression.'
        return 0
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report['error'] = str(error)
        return 1
    finally:
        (folder / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        print('PERFORMANCE_REPORT ' + json.dumps(report, ensure_ascii=False))
        print('Performance records: ' + str(folder))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command or not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('A safe test name and executable command are required')
    return record(args.name, command, args.root)


if __name__ == '__main__':
    raise SystemExit(main())
