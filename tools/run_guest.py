#!/usr/bin/env python3
"""Run only the prepared guest, bound its lifetime and preserve evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def run(args):
    dest = ROOT / 'fixture/out/guest-test'
    job_path = dest / 'job.json'
    job = json.loads(job_path.read_text()) if job_path.exists() else {}
    executable = Path(job.get('winuae', str(args.winuae)))
    seconds = max(1, min(600, int(job.get('seconds', args.seconds))))
    archive = dest / f'attempt-{time.time_ns()}'
    archive.mkdir()
    for path in [dest / 'winuae.log', dest / 'launch.json', dest / 'launch-inputs.json', *sorted((dest / 'guest').glob('*.txt'))]:
        if path.exists():
            path.rename(archive / path.name)
    (dest / 'launch-inputs.json').write_text((dest / 'inputs.json').read_text())
    evidence = {'emulator': str(executable),
                'sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
                'started': time.time()}
    env = os.environ.copy()
    # Report of the --profile emulator build; the patch defaults to the working directory.
    env.setdefault('PCIINIT_POST_PROFILE', str(dest / 'post-pc-profile.tsv'))
    if args.headless:
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                   SDL_RENDER_DRIVER='software')
        evidence['presentation'] = 'SDL dummy video/audio, software renderer'
    with (dest / 'winuae.log').open('w') as log:
        process = subprocess.Popen([str(executable), '-f', str(dest / 'test.uae')],
                                   stdout=log, stderr=subprocess.STDOUT, env=env)
        evidence['pid'] = process.pid
        (dest / 'launch.json').write_text(json.dumps(evidence, indent=2) + '\n')
        print(f'Owned WinUAE PID {process.pid}; GDB port is set in test.uae', flush=True)
        try:
            deadline = time.monotonic() + seconds
            while process.poll() is None and time.monotonic() < deadline:
                if ((dest / 'guest/done.txt').exists() and not (dest / 'HOLD').exists()) or (dest / 'STOP').exists():
                    break
                time.sleep(0.25)
        finally:
            evidence['guest_done'] = (dest / 'guest/done.txt').exists()
            evidence['early_exit'] = process.poll()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            evidence['exit_code'] = process.returncode
            evidence['finished'] = time.time()
            (dest / 'launch.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence, indent=2), flush=True)
    return evidence['guest_done']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--winuae', type=Path,
                        default=ROOT / 'fixture/out/winuae',
                        help='emulator built by tools/build_fixture_emulator.py')
    parser.add_argument('--headless', action='store_true',
                        help='run automated guest tests without a native window')
    parser.add_argument('--seconds', type=int, default=120)
    parser.add_argument('--watch', action='store_true',
                        help='wait for READY to run again, or STOP to exit')
    args = parser.parse_args()
    dest = ROOT / 'fixture/out/guest-test'
    if not args.watch:
        raise SystemExit(0 if run(args) else 1)
    print('Waiting for READY; create STOP or press Ctrl-C to finish.', flush=True)
    while not (dest / 'STOP').exists():
        if (dest / 'READY').exists():
            (dest / 'READY').unlink()
            run(args)
        time.sleep(0.25)


if __name__ == '__main__':
    main()
