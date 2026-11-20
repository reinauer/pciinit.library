#!/usr/bin/env python3
"""Prepare an isolated PCI bridge + S3 ViRGE guest; never modify the source fixture."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seed', type=Path, default=ROOT / 'fixture/out/seed',
                        help='guest directory with C/Assign, C/Execute, C/Wait and Libs/mathieeedoubbas.library')
    parser.add_argument('--kickstart', type=Path,
                        default=Path.home() / 'Documents/WinUAE/Kickstarts/A4000.47.115.rom')
    parser.add_argument('--rom', type=Path, help='matching S3 ViRGE ROM, if available')
    parser.add_argument('--bridge', choices=('firestorm', 'prometheus'), default='firestorm',
                        help='emulated bus board; only FireStorm reaches legacy VGA memory')
    parser.add_argument('--openpci', type=Path, default=ROOT / 'fixture/out/pinned-openpci.library',
                        help='openpci.library binary to install in the guest')
    parser.add_argument('--suite', choices=('foundation', 'backend', 'post', 'display', 'vbe', 'vbe-cli', 'boing-cli', 'post-timing', 'record', 'profile', 'deadline', 'bustrap', 'abortprobe', 'log'), default='foundation')
    parser.add_argument('--post-source', choices=('card', 'file'), default='card')
    parser.add_argument('--port', type=int, default=2345)
    parser.add_argument('--jit-kb', type=int, choices=(0, 16384), default=0,
                        help='optional WinUAE 68k JIT cache for long POST runs')
    parser.add_argument('--cpu-mhz', type=int, help='cycle-exact 68030 clock; disables host JIT')
    parser.add_argument('--master-abort', action='store_true',
                        help='bridge ends unclaimed PCI cycles with a 68k bus error (WinUAE bridge option '
                             '"masterabort"); the backend suite then expects counted bus errors')
    parser.add_argument('--cpu', type=int, choices=(68020, 68030, 68040, 68060), default=68020,
                        help='CPU model; --cpu-mhz always runs a 68030')
    args = parser.parse_args()
    if args.cpu_mhz is not None and (not 1 <= args.cpu_mhz <= 100 or args.jit_kb):
        parser.error('--cpu-mhz requires 1..100 MHz and --jit-kb 0')
    if args.master_abort and args.cpu == 68020:
        parser.error('--master-abort needs --cpu 68030, 68040 or 68060: WinUAE builds faithful bus error '
                     'frames only with MMU emulation, which the 68020 lacks')
    if args.cpu_mhz is not None and args.cpu != 68020:
        parser.error('--cpu-mhz selects its own 68030; leave --cpu at the default')
    if args.suite == 'post-timing' and args.cpu_mhz is None:
        parser.error('--suite post-timing requires --cpu-mhz')
    dest = ROOT / 'fixture/out/guest-test'
    guest = dest / 'guest'
    for sub in ('C', 'S', 'Libs', 'Devs', 'ENV', 'ENVARC'):
        (guest / sub).mkdir(parents=True, exist_ok=True)
    seed = args.seed
    inputs = [args.kickstart, args.openpci]
    inputs += [seed / 'C' / name for name in ('Assign', 'Execute', 'Wait')]
    inputs.append(seed / 'Libs/mathieeedoubbas.library')
    for path in inputs:
        if not path.is_file():
            parser.error(f'missing input: {path}')
    for name in ('Assign', 'Execute', 'Wait'):
        shutil.copy2(seed / 'C' / name, guest / 'C' / name)
    shutil.copy2(inputs[1], guest / 'Libs/openpci.library')
    shutil.copy2(seed / 'Libs/mathieeedoubbas.library', guest / 'Libs/mathieeedoubbas.library')
    (guest / 'ENVARC/PCI-Configuration').write_text('ReserveMemSpace 0xC0000\n')
    # The guest directory persists between runs; drop a card blacklist left by an
    # earlier PCIoprom-Configuration experiment so every card is executed.
    for env in ('ENV', 'ENVARC'):
        (guest / env / 'PCIoprom-Configuration').unlink(missing_ok=True)
    # Some OpenPCI providers read the configuration from DEVS: instead.
    (guest / 'Devs/PCI-Configuration').write_text('ReserveMemSpace 0xC0000\n')
    commands = ['FailAt 21', 'Stack 131072', 'C:Assign ENV: SYS:ENV', 'C:Assign ENVARC: SYS:ENVARC']
    # The card ROM the emulator exposes is the supplied image; tests that
    # compare the card copy with a file read this copy.
    if args.rom:
        shutil.copy2(args.rom, guest / 'card.rom')
    for name in ('x86emu.library', 'pciinit.library'):
        artifact = ROOT / 'build' / name
        if artifact.exists():
            shutil.copy2(artifact, guest / 'Libs' / name)
            inputs.append(artifact)

    def current(artifact):
        # `make all` does not build the native-only test programs, and a
        # stale one would report on code that no longer exists. Let make
        # decide from its own dependencies whether the program is current.
        options = ROOT / 'build/engine-options'
        backend = options.read_text().split()[0] if options.exists() else 'm68kjit'
        made = subprocess.run(['make', '-s', f'X86_BACKEND={backend}', str(artifact.relative_to(ROOT))],
                              cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if made.returncode:
            parser.error(f'cannot build {artifact.relative_to(ROOT)}:\n{made.stderr}')
        return artifact
    for name in ('engine-test', 'rom-test', 'library-test', 'pciinit-test', 'allocation-test', 'vbe-parse-test', 'vbe-api-test'):
        artifact = ROOT / 'build' / name
        if artifact.exists():
            shutil.copy2(current(artifact), guest / name)
            inputs.append(artifact)
            arguments = ' SYS:card.rom' if name in ('pciinit-test', 'allocation-test') else ''
            commands.append(f'{name}{arguments} >SYS:{name}.txt')
    artifact = ROOT / 'build/PCIoprom'
    if artifact.exists():
        shutil.copy2(current(artifact), guest / 'PCIoprom')
        inputs.append(artifact)
        commands.append('PCIoprom --no-post >SYS:pcioprom.txt')
        commands.append('PCIoprom --no-post SYS:card.rom >SYS:pcioprom-file.txt')
    if args.suite in ('backend', 'display', 'vbe', 'deadline', 'bustrap', 'abortprobe'):
        name = {'backend': 'hardware-test', 'display': 'post-test', 'vbe': 'vbe-native-test', 'deadline': 'deadline-native-test',
                'bustrap': 'bustrap-test', 'abortprobe': 'abortprobe'}[args.suite]
        artifact = ROOT / 'build' / name
        shutil.copy2(current(artifact), guest / name)
        inputs.append(artifact)
        argument = ' masterabort' if args.master_abort and name == 'hardware-test' else ''
        commands = commands[:4] + [f'{name}{argument} >SYS:{name}.txt']
    elif args.suite == 'log':
        commands = commands[:4] + ['PCIoprom --log SYS:access.log --mode 0x111 >SYS:log.txt']
    elif args.suite in ('vbe-cli', 'boing-cli'):
        extra = ' --pattern' if args.suite == 'vbe-cli' else ''
        commands = commands[:4] + [f'PCIoprom --list-modes --mode 0x111 --mode 0x114 --mode 0x112 --mode 0x110 --mode 0x111{extra} >SYS:{args.suite}.txt']
    elif args.suite in ('record', 'profile'):
        artifact = ROOT / 'build/post-record'
        shutil.copy2(current(artifact), guest / artifact.name)
        inputs.append(artifact)
        argument = '--profile' if args.suite == 'profile' else 'SYS:post-record.bin'
        commands = commands[:4] + [f'post-record {argument} >SYS:post-record.txt']
    elif args.suite == 'post-timing':
        artifact = ROOT / 'build/post-timing'
        shutil.copy2(current(artifact), guest / artifact.name)
        inputs += [artifact, ROOT / 'build/engine-options']
        commands = commands[:4] + ['post-timing >SYS:post-timing.txt']
    elif args.suite == 'post':
        if not args.rom:
            parser.error('--suite post requires --rom')
        shutil.copy2(args.rom, guest / 'source.rom')
        argument = ' SYS:source.rom' if args.post_source == 'file' else ''
        commands = commands[:4] + [f'PCIoprom{argument} >SYS:post.txt']
    commands.append('Echo complete >SYS:done.txt')
    (guest / 'S/startup-sequence').write_text('\n'.join(commands) + '\n')
    romdir = dest / 'roms'
    romdir.mkdir(exist_ok=True)
    if args.rom:
        shutil.copy2(args.rom, romdir / 's3virge.rom')
        inputs.append(args.rom)
    config = f'''config_description=pciinit: {args.bridge} + S3 ViRGE
use_gui=no
kickstart_rom_file={args.kickstart.resolve()}
cpu_model=68020
cpu_24bit_addressing=false
cpu_compatible=false
cpu_cycle_exact=false
cpu_memory_cycle_exact=false
blitter_cycle_exact=false
cpu_speed=max
cachesize={args.jit_kb}
chipset=aga
chipset_compatible=A4000
chipmem_size=4
z3mem_size=64
z3mapping=real
floppy0type=-1
floppy1type=-1
nr_floppies=0
filesystem2=rw,DH0:PCIInit:{guest},0
sound_output=none
gfx_width=720
gfx_height=568
gfx_linemode=double
gfx_resolution=hires
gfx_fullscreen_amiga=false
unix.inactive_pause=false
unix.iconified_pause=false
unix.data_path={dest / 'uae-data'}/
unix.rom_path={romdir}/
debugging_features=gdbserver
gdb_port={args.port}
{'prometheusfirestorm' if args.bridge == 'firestorm' else 'prometheus'}_rom_file=:NOROM
{('prometheusfirestorm' if args.bridge == 'firestorm' else 'prometheus') + '_rom_options=masterabort' if args.master_abort else ''}
gfxcard_type=S3VIRGE_PCI
gfxcard_size=4
'''
    if args.cpu != 68020:
        config = config.replace('cpu_model=68020', f'cpu_model={args.cpu}')
        if args.cpu in (68040, 68060):
            # Their exception frames differ from the 68020/030 ones.
            config += f'fpu_model={args.cpu}\n'
    if args.master_abort:
        # The non-MMU CPU cores build incomplete bus error frames (no DF on
        # the 68020/030, a short format 7 on the 68040/060) and cannot
        # continue a faulted instruction; the MMU cores do both. Exec never
        # enables the MMU, so this changes nothing else.
        config = config.replace('cpu_compatible=false', 'cpu_compatible=true')
        config += f'mmu_model={args.cpu}\n'
    if args.cpu_mhz is not None:
        config = config.replace('cpu_model=68020', 'cpu_model=68030')
        config = config.replace('cpu_compatible=false', 'cpu_compatible=true')
        config = config.replace('cpu_cycle_exact=false', 'cpu_cycle_exact=true')
        config = config.replace('cpu_memory_cycle_exact=false', 'cpu_memory_cycle_exact=true')
        config = config.replace('cpu_speed=max', 'cpu_speed=real')
        config += f'cpu_frequency={args.cpu_mhz * 1000000}\ncpu_multiplier=0\n'
        config += 'cpu_data_cache=true\nuaeboard=full+indirect\n'
    (dest / 'test.uae').write_text(config)
    inputs += [dest / 'test.uae', guest / 'S/startup-sequence', guest / 'ENVARC/PCI-Configuration']
    hashes = {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    (dest / 'inputs.json').write_text(json.dumps(hashes, indent=2) + '\n')
    print(dest / 'test.uae')


if __name__ == '__main__':
    main()
