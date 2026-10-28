#!/usr/bin/env python3
"""Build and verify an Amiga distribution archive from an explicit allowlist."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args, cwd):
    result = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'{args[0]} failed ({result.returncode}):\n{result.stdout}')
    return result.stdout


def package(version, lha, strip):
    if not re.fullmatch(r'[0-9]+\.[0-9]+(?:[.-][A-Za-z0-9]+)*', version):
        raise ValueError('version must look like 2.0 or 2.0-rc1')
    archiver = shutil.which(lha)
    if not archiver:
        raise ValueError(f'cannot find {lha}; install a write-capable LHa, or set LHA=/path/to/lha')
    archiver = str(Path(archiver).resolve())
    stripper = shutil.which(strip)
    if not stripper:
        raise ValueError(f'cannot find {strip}; set CROSS to the toolchain prefix')
    release_dir = ROOT / 'build/release'
    release_dir.mkdir(parents=True, exist_ok=True)
    name = f'pciinit-{version}'
    archive = release_dir / f'{name}.lha'
    files = {
        'build/pciinit.library': 'Libs/pciinit.library',
        'build/x86emu.library': 'Libs/x86emu.library',
        'build/nojit/x86emu.library': 'Libs/x86emu.library_nojit',
        'build/PCIoprom': 'C/PCIoprom',
        'LICENSE': 'LICENSE',
        'README.md': 'README.md',
        'docs/release.txt': 'ReadMe',
    }
    for directory in ('clib', 'inline', 'libraries', 'proto'):
        for path in sorted((ROOT / 'include' / directory).glob('*.h')):
            files[str(path.relative_to(ROOT))] = 'SDK/' + str(path.relative_to(ROOT))
    for path in sorted((ROOT / 'fd').glob('*.fd')):
        files[str(path.relative_to(ROOT))] = 'SDK/' + str(path.relative_to(ROOT))
    # User documentation only.
    for basename in ('abi.md', 'ARCHITECTURE.md'):
        files['docs/' + basename] = 'docs/' + basename
    for path in sorted((ROOT / 'docs/diagrams').glob('*.svg')):
        files[str(path.relative_to(ROOT))] = str(path.relative_to(ROOT))
    for basename in ('LICENSE', 'LICENSE_INFO', 'README.amiga'):
        path = 'third_party/libx86emu/' + basename
        files[path] = path
    with tempfile.TemporaryDirectory(prefix='.package-', dir=release_dir) as temp:
        stage = Path(temp)
        tree = stage / name
        for source, target in sorted(files.items()):
            source = ROOT / source
            if not source.is_file() or source.is_symlink():
                raise ValueError(f'missing or nonregular release input: {source}')
            target = tree / target
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            # The debugging hunks serve the emulator fixture, not the Amiga.
            if ROOT / 'build' in source.parents:
                run(stripper, str(target), cwd=ROOT)
        commit = run('git', 'rev-parse', 'HEAD', cwd=ROOT).strip()
        dirty = bool(run('git', 'diff', 'HEAD', '--name-only', cwd=ROOT).strip())
        (tree / 'BUILD.txt').write_text(
            f'Package: {name}\nSource commit: {commit}\n'
            f'Tracked source changes: {"yes" if dirty else "no"}\n'
            'Target: 68020+, AmigaOS 3.0+\n'
            'Binaries are copied from build/ without debugging hunks.\n')
        paths = sorted(path for path in tree.rglob('*') if path.is_file())
        (tree / 'SHA256SUMS').write_text(''.join(
            f'{digest(path)}  {path.relative_to(tree).as_posix()}\n' for path in paths))
        expected = {path.relative_to(stage).as_posix(): digest(path)
                    for path in tree.rglob('*') if path.is_file()}
        candidate = stage / archive.name
        # LH5 with generic level-0 headers is readable by classic Amiga LhA.
        run(archiver, 'ao5qg0', str(candidate), name, cwd=stage)
        run(archiver, 'tq', str(candidate), cwd=stage)
        extracted = stage / 'verify'
        extracted.mkdir()
        run(archiver, 'xq', str(candidate), cwd=extracted)
        actual = {path.relative_to(extracted).as_posix(): digest(path)
                  for path in extracted.rglob('*') if path.is_file()}
        if actual != expected:
            raise ValueError('archive round-trip changed file names or contents')
        # Publish only after CRC and extracted contents have been verified.
        candidate.replace(archive)
    archive.with_suffix('.lha.sha256').write_text(f'{digest(archive)}  {archive.name}\n')
    print(f'{archive.relative_to(ROOT)}: {len(expected)} files, {archive.stat().st_size} bytes')
    print('Verified archive CRCs and every extracted file against the staging tree.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', default='2.0')
    parser.add_argument('--lha', default='lha')
    parser.add_argument('--strip', default='m68k-amigaos-strip')
    args = parser.parse_args()
    try:
        package(args.version, args.lha, args.strip)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f'Cannot package release: {error}\n')


if __name__ == '__main__':
    main()
