#!/usr/bin/env python3
"""Emit an executable-only policy override from one immutable source snapshot.

Use the same script on both sides. Only the brush admission union changes;
shape/text tables and each side's defaults remain exactly as frozen.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
original = (args.source / 'src/domain/annotation/annotation_style_policy.cpp').read_bytes()
text = original.decode()
text, count = re.subn(r'std::array<std::uint16_t,\s*\d+>\s+brush\{[^}]*\}',
    'std::array<std::uint16_t, 9> brush{2, 4, 24, 32, 48, 72, 96, 144, 192}', text)
if count != 1:
    raise SystemExit('Expected exactly one brush admission table; refusing guessed adaptation')
args.output.write_text('// Benchmark executable admission override; generated from its own source snapshot.\n' + text)
print(json.dumps({'source': str(args.source.resolve()),
    'original_sha256': hashlib.sha256(original).hexdigest(),
    'adapter': str(args.output.resolve()),
    'adapter_sha256': hashlib.sha256(args.output.read_bytes()).hexdigest(),
    'exact_difference': 'Only brush admission table replaced with 2/4/24/32/48/72/96/144/192'}, indent=2))
