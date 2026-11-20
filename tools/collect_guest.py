#!/usr/bin/env python3
"""Validate a completed guest run and collect reports without ROM data."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REPORTS = {
    'engine-test.txt': 'ENGINE RESULT',
    'rom-test.txt': 'ROM RESULT',
    'library-test.txt': 'LIBRARY RESULT',
    'pciinit-test.txt': 'PCIINIT RESULT',
    'allocation-test.txt': 'ALLOCATION RESULT',
    'vbe-parse-test.txt': 'VBE PARSE RESULT',
    'vbe-api-test.txt': 'VBE API RESULT',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def collect(dest, suite):
    reports_for_suite = REPORTS if suite == 'foundation' else {
        'backend': {'hardware-test.txt': 'HARDWARE RESULT'},
        'post': {},
        'display': {'post-test.txt': 'DISPLAY RESULT'},
        'vbe': {'vbe-native-test.txt': 'VBE RESULT'},
        'vbe-cli': {},
        'boing-cli': {},
        'post-timing': {},
        'record': {},
        'profile': {},
        'deadline': {'deadline-native-test.txt':'DEADLINE RESULT'},
        'bustrap': {'bustrap-test.txt': 'BUSTRAP RESULT'},
        'abortprobe': {'abortprobe.txt': 'ABORTPROBE RESULT'},
    }[suite]
    scanners = ('post-record.txt',) if suite in ('record', 'profile') else ('post-timing.txt',) if suite == 'post-timing' else (suite+'.txt',) if suite in ('vbe-cli','boing-cli') else ('post.txt',) if suite == 'post' else ('pcioprom.txt', 'pcioprom-file.txt') if suite == 'foundation' else ()
    launch = json.loads((dest / 'launch.json').read_text())
    if not launch.get('guest_done') or not launch.get('finished'):
        raise ValueError('guest run did not finish')
    if sha(Path(launch['emulator'])) != launch['sha256']:
        raise ValueError('emulator changed since the run')
    if (dest / 'inputs.json').stat().st_mtime > launch['started']:
        raise ValueError('fixture was prepared again after this run started')
    inputs = json.loads((dest / 'inputs.json').read_text())
    executed = {}
    source_changes = []
    for name, digest in inputs.items():
        source = Path(name)
        # Executed copies are authoritative when another checkout is rebuilt.
        if source.name == 'pinned-openpci.library':
            actual = dest / 'guest/Libs/openpci.library'
        elif source.name in ('x86emu.library', 'pciinit.library', 'openpci.library',
                            'mathieeedoubbas.library'):
            actual = dest / 'guest/Libs' / source.name
        elif source.name in ('Assign', 'Execute', 'Wait'):
            actual = dest / 'guest/C' / source.name
        elif source.name in ('PCIoprom', 'engine-test', 'rom-test',
                              'library-test', 'pciinit-test', 'allocation-test',
                              'hardware-test', 'post-test',
                              'vbe-native-test', 'vbe-api-test', 'vbe-parse-test', 'post-timing', 'post-record',
                              'bustrap-test', 'abortprobe'):
            actual = dest / 'guest' / source.name
        elif (dest / 'roms/s3virge.rom').exists() and sha(dest / 'roms/s3virge.rom') == digest:
            actual = dest / 'roms/s3virge.rom'
        else:
            actual = source  # Kickstart and configuration are read in place.
        if sha(actual) != digest:
            raise ValueError(f'executed input changed since preparation: {actual}')
        executed[str(actual.resolve())] = digest
        if not source.exists() or sha(source) != digest:
            source_changes.append(name)
    timing_clock = None
    if suite == 'post-timing':
        config = dict(line.split('=', 1) for line in (dest / 'test.uae').read_text().splitlines() if '=' in line)
        required = {'cpu_model':'68030', 'cpu_speed':'real', 'cpu_compatible':'true',
                    'cpu_cycle_exact':'true', 'cpu_memory_cycle_exact':'true',
                    'cpu_data_cache':'true', 'cpu_multiplier':'0', 'cachesize':'0'}
        if any(config.get(k) != v for k, v in required.items()):
            raise ValueError('POST timing requires the fixed-clock 68030 configuration')
        frequency = int(config['cpu_frequency'])
        clock = 3546895  # PAL chipset clock, also the basis of CPU multipliers.
        unit = 512 * clock // frequency
        log = (dest / 'winuae.log').read_text(errors='replace')
        units = re.findall(r'CPU cycleunit: (\d+)', log)
        if not units or int(units[-1]) != unit or unit < 1:
            raise ValueError('invalid custom CPU frequency conversion; reject this timing run')
        if 'CPU=68030, FPU=0, MMU=0, JIT=0.' not in log or 'Data cache emulation requires' in log:
            raise ValueError('emulator did not accept the required CPU/cache configuration')
        timing_clock = {'requested_hz':frequency, 'cycle_unit':unit,
                        'effective_hz':512 * clock / unit, 'model':'WinUAE approximate cycle-exact 68030'}
    reports, counts, combined = {}, {}, []
    for name in (*reports_for_suite, *scanners):
        path = dest / 'guest' / name
        if path.stat().st_mtime < launch['started']:
            raise ValueError(f'stale guest report: {name}')
        text = path.read_text()
        if re.search(r'^FAIL\b', text, re.M):
            raise ValueError(f'failed assertion in {name}')
        if name in reports_for_suite:
            match = re.search(r'^' + reports_for_suite[name] + r' checks=(\d+) failures=(\d+)\b', text, re.M)
            if not match or int(match[1]) == 0 or int(match[2]):
                raise ValueError(f'missing or failed result in {name}')
            counts[name] = int(match[1])
        elif suite == 'profile':
            match = re.search(r'^PROFILE RESULT status=0 records=(\d+) bytes=0 overflow=0 failures=0$', text, re.M)
            if not match:
                raise ValueError('profiling failed')
            counts[name] = {'events':int(match[1])}
        elif suite == 'record':
            match = re.search(r'^RECORD RESULT status=0 records=(\d+) bytes=(\d+) overflow=0 failures=0$', text, re.M)
            if not match or int(match[1]) == 0:
                raise ValueError('recording failed or overflowed')
            recording = dest / 'guest/post-record.bin'
            if recording.stat().st_mtime < launch['started'] or recording.stat().st_size != int(match[2])+4:
                raise ValueError('stale or incomplete recording')
            counts[name] = {'records':int(match[1]),'bytes':int(match[2]),'sha256':sha(recording)}
        elif suite == 'post-timing':
            match = re.search(r'^POST TIMING status=(\d+) stop=(\d+) instructions=(\d+) accesses=(\d+) hz=(\d+) ticks_hi=(\d+) ticks_lo=(\d+) seconds=(\d+\.\d+)$', text, re.M)
            if not match or match[1] != '0' or match[2] != '0' or 'POST TIMING RESULT failures=0' not in text:
                raise ValueError('POST timing failed or did not complete')
            values = list(map(int, match.groups()[:7]))
            ticks = (values[5] << 32) | values[6]
            if not values[4] or not ticks or 'POST TIMING VBE version=0102 modes=25 memory=4194304' not in text:
                raise ValueError('POST timing lacks clock or VBE verification')
            counts[name] = dict(zip(('status','stop','instructions','accesses','hz','ticks_hi','ticks_lo'),values))
            counts[name]['seconds'] = ticks / values[4]
            counts[name]['backend_options'] = (ROOT / 'build/engine-options').read_text().strip()
        else:
            match = re.search(r'^PCIOPROM RESULT cards=(\d+) loaded=(\d+) initialized=(\d+) blocked=(\d+) failures=(\d+)(?: ignored=\d+)?$', text, re.M)
            if not match or int(match[2]) == 0 or int(match[5]):
                raise ValueError(f'missing or failed scanner result in {name}')
            if suite in ('post', 'vbe-cli', 'boing-cli') and (int(match[3]) != int(match[2]) or int(match[4])):
                raise ValueError('POST did not initialize every loaded card')
            counts[name] = dict(zip(('cards', 'loaded', 'initialized', 'blocked', 'failures'),
                                    map(int, match.groups())))
        if suite in ('vbe-cli', 'boing-cli'):
            selected = re.findall(r'^  SET (0x[0-9a-f]+) current=([0-9a-f]+)', text, re.M)
            if [int(mode, 16) for mode, _ in selected] != [0x111, 0x114, 0x112, 0x110, 0x111]:
                raise ValueError('missing CLI mode sequence')
            if any(int(mode, 16) != (int(current, 16) & 0x3fff) for mode, current in selected):
                raise ValueError('CLI current-mode mismatch')
            # A ROM without linear modes sets every mode but maps no framebuffer.
            marker = 'pattern=verified' if suite == 'vbe-cli' else 'boing=drawn'
            drawn = text.count(marker) + text.count('DISPLAY: no linear framebuffer')
            if drawn != 5 or len(re.findall(r'^  MODE ', text, re.M)) == 0:
                raise ValueError('missing CLI enumeration or framebuffer drawing result')
        reports[name] = sha(path)
        combined.append(f'=== {name} ===\n{text.rstrip()}\n')
    rom = dest / 'roms/s3virge.rom'
    rom_hash = sha(rom) if rom.exists() else None  # suites that take no --rom
    if suite == 'post' and 'SYS:source.rom' in (dest / 'guest/S/startup-sequence').read_text():
        if sha(dest / 'guest/source.rom') != rom_hash:
            raise ValueError('executed file ROM differs from supplied ROM')
    if suite == 'foundation' and sha(dest / 'guest/card.rom') != rom_hash:
        raise ValueError('card ROM differs from the supplied emulator ROM')
    artifacts = {}
    for name in ('x86emu.library', 'pciinit.library', 'PCIoprom'):
        path = ROOT / 'build' / name
        guest = dest / 'guest' / ('Libs/' + name if name.endswith('.library') else name)
        if sha(guest) != sha(path):
            raise ValueError(f'guest artifact differs from build: {name}')
        artifacts[name] = {'bytes': path.stat().st_size, 'sha256': sha(path)}
    result = {
        'scope': f'WinUAE {suite} tests; no physical hardware claim',
        **({'timing_clock':timing_clock} if timing_clock else {}),
        'collected_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'launch': launch, 'counts': counts, 'rom_sha256': rom_hash,
        'artifacts': artifacts, 'report_sha256': reports, 'inputs': inputs,
        'executed_inputs': executed, 'changed_sources_after_copy': source_changes,
    }
    return result, '\n'.join(combined)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', choices=('foundation', 'backend', 'post', 'display', 'vbe', 'vbe-cli', 'boing-cli', 'post-timing', 'record', 'profile', 'deadline', 'bustrap', 'abortprobe'), default='foundation')
    parser.add_argument('--output', type=Path, default=ROOT / 'fixture/out/guest-test/foundation',
                        help='output path prefix for JSON and text reports')
    args = parser.parse_args()
    try:
        result, text = collect(ROOT / 'fixture/out/guest-test', args.suite)
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, f'Cannot collect: {error}\n')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
    args.output.with_suffix('.txt').write_text(text)
    print(json.dumps(result['counts'], indent=2))


if __name__ == '__main__':
    main()
