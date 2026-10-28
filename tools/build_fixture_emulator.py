#!/usr/bin/env python3
"""Build the WinUAE fixture emulator from the pinned revision.

The result lands in fixture/out/, which git ignores and `make clean` leaves
alone. The source checkout is only read; the pinned revision is exported
with `git archive`, so the checkout's working tree does not matter.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'fixture/winuae'
OUT = ROOT / 'fixture/out'
PROFILE_PATCH = 'post-profile.patch'
# Features the fixture never uses; leaving them out avoids optional
# dependencies and network fetches. Everything else keeps its default.
CMAKE_OFF = ('WINUAE_UNIX_WITH_QT_UI', 'WINUAE_UNIX_WITH_INTEGRATED_QT_UI',
             'WINUAE_UNIX_WITH_LUA', 'WINUAE_UNIX_WITH_ARCHIVES',
             'WINUAE_UNIX_WITH_PRINTER_EMULATION', 'WINUAE_UNIX_WITH_CHD',
             'WINUAE_UNIX_WITH_FLOPPYBRIDGE', 'WINUAE_UNIX_BUILD_FLOPPYBRIDGE_PLUGIN')


def run(*command, **kwargs):
    print('+', ' '.join(str(c) for c in command), flush=True)
    return subprocess.run([str(c) for c in command], check=True, **kwargs)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path.home() / 'git/WinUAE_mac/WinUAE',
                        help='git checkout of the WinUAE fork that contains the pinned revision')
    parser.add_argument('--profile', action='store_true',
                        help='also apply post-profile.patch and name the result winuae-profile')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    parser.add_argument('--cmake-arg', action='append', default=[],
                        help='extra -D option for the configure step')
    args = parser.parse_args()

    url, revision, branch = (FIXTURE / 'revision').read_text().split()
    if not (args.source / '.git').exists():
        parser.error(f'{args.source} is not a git checkout; clone {url} ({branch}) there or pass --source')
    present = subprocess.run(['git', '-C', str(args.source), 'cat-file', '-e', revision + '^{commit}'])
    if present.returncode:
        parser.error(f'{args.source} lacks {revision}; run: git -C {args.source} fetch {url} {branch}')

    name = 'winuae-profile' if args.profile else 'winuae'
    patches = [PROFILE_PATCH] if args.profile else []
    source = OUT / f'{name}-src'
    build = OUT / f'{name}-build'
    OUT.mkdir(parents=True, exist_ok=True)
    if source.exists():
        shutil.rmtree(source)
    source.mkdir()
    archive = subprocess.Popen(['git', '-C', str(args.source), 'archive', '--format=tar', revision],
                               stdout=subprocess.PIPE)
    run('tar', '-x', '-C', source, stdin=archive.stdout)
    if archive.wait():
        sys.exit('git archive failed')
    for patch in patches:
        with (FIXTURE / patch).open() as text:
            run('patch', '-p1', '-s', '-d', source, stdin=text)

    configure = ['cmake', '--fresh', '-S', source, '-B', build, '-DCMAKE_BUILD_TYPE=Release']
    if shutil.which('ninja'):
        configure += ['-G', 'Ninja']
    if sys.platform == 'darwin':
        # Homebrew libraries (SDL3, libpng) are built for the running macOS;
        # the port's default deployment target of 13.0 would reject them.
        version = subprocess.run(['sw_vers', '-productVersion'], capture_output=True, text=True,
                                 check=True).stdout.strip()
        configure += [f'-DCMAKE_OSX_DEPLOYMENT_TARGET={version.split(".")[0]}.0',
                      '-DCMAKE_PREFIX_PATH=/opt/homebrew']
    configure += [f'-D{option}=OFF' for option in CMAKE_OFF]
    configure += [f'-D{option}' for option in args.cmake_arg]
    run(*configure)
    run('cmake', '--build', build, '--target', 'winuae_unix', '-j', args.jobs)

    binary = build / 'winuae'
    result = OUT / name
    shutil.copy2(binary, result)
    record = {'url': url, 'revision': revision, 'patches': {p: sha256(FIXTURE / p) for p in patches},
              'cmake_off': CMAKE_OFF, 'cmake_extra': args.cmake_arg,
              'binary': str(result), 'sha256': sha256(result)}
    (OUT / f'{name}.json').write_text(json.dumps(record, indent=2) + '\n')
    print(result)


if __name__ == '__main__':
    main()
