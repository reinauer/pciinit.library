#!/usr/bin/env python3
"""Attribute private WinUAE POST cycle records using matching Hunk/object symbols."""
import argparse
import bisect
from collections import Counter
import json
from pathlib import Path
import re
import subprocess


def symbols(nm, path):
    output = subprocess.check_output([nm, '-n', str(path)], text=True)
    return [(int(address, 16), kind, name)
            for address, kind, name in
            re.findall(r'^([0-9a-f]+) ([Tt]) (.+)$', output, re.M)]


def ranges(nm, build, library, base):
    linked = symbols(nm, build / (library + '.library'))
    globals_ = {name: offset for offset, kind, name in linked}
    resolved = {offset: name for offset, kind, name in linked}
    dirs = ('engine', 'runtime', 'x86emu') if library == 'x86emu' else ('pciinit', 'runtime')
    for directory in dirs:
        objects = sorted((build / directory).glob('*.o'))
        if not objects:
            raise ValueError(f'missing matching objects: {build / directory}')
        for obj in objects:
            entries = symbols(nm, obj)
            offsets = {globals_[name] - offset for offset, kind, name in entries
                       if kind == 'T' and name in globals_}
            if len(offsets) != 1:
                continue
            start = offsets.pop()
            for offset, kind, name in entries:
                resolved[start + offset] = obj.stem + ':' + name
    entries = sorted(resolved.items())
    return [(base + start, base + end, library + '/' + name)
            for (start, name), (end, _) in zip(entries, entries[1:]) if end > start]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--pci-build', type=Path, default=Path('build'))
    parser.add_argument('--x86-build', type=Path, default=Path('build'))
    parser.add_argument('--nm', default='m68k-amigaos-nm')
    parser.add_argument('--eclock-hz', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    match = re.search(r'PROFILE SEG pci=([0-9a-f]+) x86=([0-9a-f]+)', args.report.read_text())
    if not match or args.eclock_hz <= 0:
        parser.error('need marker segment bases and a positive guest EClock frequency')
    pci, x86 = (int(value, 16) for value in match.groups())
    mapping = sorted(ranges(args.nm, args.pci_build, 'pciinit', pci) +
                     ranges(args.nm, args.x86_build, 'x86emu', x86))
    starts = [item[0] for item in mapping]
    functions, ips = Counter(), Counter()
    total, cycle_unit = 0, None
    with args.profile.open() as stream:
        for line in stream:
            if line.startswith('# CYCLE_UNIT='):
                cycle_unit = int(line.split('=')[1])
                continue
            if not line.strip() or line.startswith('#'):
                continue
            ip, pc, ticks, visits = line.split()
            ip, pc, ticks = int(ip, 16), int(pc, 16), int(ticks)
            index = bisect.bisect_right(starts, pc) - 1
            if index >= 0 and pc < mapping[index][1]:
                name = mapping[index][2]
            else:
                name = 'ROM' if 0xf80000 <= pc < 0x1000000 else 'other/generated'
            functions[name] += ticks
            ips[ip] += ticks
            total += ticks
    if not cycle_unit or cycle_unit <= 0:
        parser.error('missing positive CYCLE_UNIT in profile header')
    units_per_second = cycle_unit * 5 * args.eclock_hz
    result = {
        'seconds': total / units_per_second,
        'functions': {name: ticks / units_per_second for name, ticks in functions.most_common()},
        'ip': {hex(ip): ticks / units_per_second for ip, ticks in ips.most_common()},
    }
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'Total: {result["seconds"]:.6f} s')
    for name, seconds in list(result['functions'].items())[:20]:
        print(f'{seconds:9.6f} s  {name}')


if __name__ == '__main__':
    main()
