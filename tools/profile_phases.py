#!/usr/bin/env python3
"""Split a private WinUAE POST cycle profile into guest phases and native functions."""
import argparse
import bisect
from collections import Counter, defaultdict
import json
from pathlib import Path
import re

from analyze_post_profile import ranges


def parse_phase(text):
    name, span = text.split('=', 1)
    low, _, high = span.partition('-')
    low = int(low, 16)
    return name, low, int(high, 16) if high else low


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--pci-build', type=Path, default=Path('build'))
    parser.add_argument('--x86-build', type=Path, default=Path('build'))
    parser.add_argument('--nm', default='m68k-amigaos-nm')
    parser.add_argument('--eclock-hz', type=int, default=709379)
    parser.add_argument('--cpu-hz', type=float, default=25222364.44)
    parser.add_argument('--phase', action='append', default=[],
                        help='name=ip or name=low-high (hex); unmatched IPs are "other"')
    parser.add_argument('--functions', type=int, default=12)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    phases = [parse_phase(p) for p in args.phase] or [
        ('pit-loop', 0x4187, 0x419b), ('rep-stosw-clear', 0xeda, 0xedc),
        ('rep-scasd-readback', 0xf53, 0xf56)]
    match = re.search(r'PROFILE SEG pci=([0-9a-f]+) x86=([0-9a-f]+)', args.report.read_text())
    if not match:
        parser.error('report lacks PROFILE SEG marker bases')
    pci, x86 = (int(v, 16) for v in match.groups())
    mapping = sorted(ranges(args.nm, args.pci_build, 'pciinit', pci) +
                     ranges(args.nm, args.x86_build, 'x86emu', x86))
    starts = [m[0] for m in mapping]

    def function(pc):
        index = bisect.bisect_right(starts, pc) - 1
        if index >= 0 and pc < mapping[index][1]:
            return mapping[index][2]
        return 'Kickstart' if 0xf80000 <= pc < 0x1000000 else 'other/generated'

    def phase(ip):
        for name, low, high in phases:
            if low <= ip <= high:
                return name
        return 'other'

    unit = None
    ticks, calls, table = Counter(), Counter(), defaultdict(Counter)
    with args.profile.open() as stream:
        for line in stream:
            if line.startswith('# CYCLE_UNIT='):
                unit = int(line.split('=')[1])
                continue
            if not line.strip() or line.startswith('#'):
                continue
            ip, pc, t, c = line.split()
            name = phase(int(ip, 16))
            ticks[name] += int(t)
            calls[name] += int(c)
            table[name][function(int(pc, 16))] += int(t)
    if not unit:
        parser.error('missing CYCLE_UNIT header')
    per_second = unit * 5 * args.eclock_hz
    result = {}
    for name, t in ticks.most_common():
        result[name] = {
            'seconds': t / per_second,
            'm68k_instructions': calls[name],
            'cycles_per_instruction': t / per_second * args.cpu_hz / calls[name],
            'functions': {f: v / per_second for f, v in table[name].most_common(args.functions)},
        }
        print(f'{name}: {t / per_second:.3f} s, {calls[name]} 68k instructions, '
              f'{result[name]["cycles_per_instruction"]:.1f} cycles each')
        for f, v in result[name]['functions'].items():
            print(f'  {v:8.3f} s  {f}')
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
