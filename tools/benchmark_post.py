#!/usr/bin/env python3
"""Measure cold S3 POST in fresh 25 MHz 68030 guests via the host watcher.

The watcher must already be running. Each sample rebuilds only the selected
x86emu.library, prepares a fresh boot, and times RunOptionROM in the guest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import statistics
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]


def run(*args):
    p=subprocess.run(list(map(str,args)),cwd=ROOT,text=True,stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT)
    if p.returncode: raise RuntimeError(p.stdout)
    return p.stdout


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--winuae',type=Path,default=ROOT/'fixture/out/winuae',help='emulator with corrected custom-frequency conversion')
    parser.add_argument('--rom',type=Path,required=True)
    parser.add_argument('--openpci',type=Path,required=True)
    parser.add_argument('--samples',type=int,default=1)
    parser.add_argument('--output',type=Path,default=ROOT/'fixture/out/post-timing-results')
    args=parser.parse_args()
    if not 1<=args.samples<=10: parser.error('--samples must be 1..10')
    dest=args.output.resolve(); dest.mkdir(parents=True,exist_ok=True)
    guest=ROOT/'fixture/out/guest-test'
    if (guest/'STOP').exists() or (guest/'READY').exists():
        parser.error('watcher is stopped or has an outstanding job')
    records=[]
    run('make','build/post-timing')
    for sample in range(args.samples):
        for backend in ('m68kjit','libx86emu'):
            name=f'{backend}-{sample+1}'
            print('Preparing',name,flush=True)
            build=run('make','-j4','build/x86emu.library','X86_BACKEND='+backend)
            (dest/(name+'-build.txt')).write_text(build)
            run('python3','tools/prepare_guest.py','--suite','post-timing','--cpu-mhz','25',
                '--rom',args.rom.resolve(),'--openpci',args.openpci.resolve(),'--port','2347')
            config=(guest/'test.uae').read_bytes()
            (guest/'job.json').write_text(json.dumps({'winuae':str(args.winuae.resolve()),'seconds':600})+'\n')
            queued=time.time(); (guest/'READY').touch()
            deadline=time.monotonic()+620
            while True:
                try: launch=json.loads((guest/'launch.json').read_text())
                except (FileNotFoundError,json.JSONDecodeError): launch={}
                if launch.get('started',0)>=queued and launch.get('finished'):
                    break
                if time.monotonic()>=deadline: raise RuntimeError('watcher did not complete '+name)
                time.sleep(.5)
            run('python3','tools/collect_guest.py','--suite','post-timing','--output',dest/name)
            shutil.copyfile(guest/'winuae.log',dest/(name+'-winuae.log'))
            (dest/(name+'.uae')).write_bytes(config)
            evidence=json.loads((dest/(name+'.json')).read_text())
            record={'backend':backend,'sample':sample+1,'timing':evidence['counts']['post-timing.txt'],
                    'clock':evidence['timing_clock'],'artifacts':evidence['artifacts'],
                    'emulator_sha256':evidence['launch']['sha256'],'rom_sha256':evidence['rom_sha256']}
            records.append(record)
            (dest/'progress.json').write_text(json.dumps(records,indent=2)+'\n')
            print(name,record['timing']['seconds'],'guest seconds',flush=True)
    summary={'samples':records,'median_seconds':{}}
    for backend in ('m68kjit','libx86emu'):
        summary['median_seconds'][backend]=statistics.median(r['timing']['seconds'] for r in records if r['backend']==backend)
    a=summary['median_seconds']['libx86emu']; b=summary['median_seconds']['m68kjit']
    summary.update(speedup=a/b,percent_time_saved=100*(1-b/a))
    for path in ('tests/post_timing.c','tools/prepare_guest.py','tools/collect_guest.py','tools/benchmark_post.py'):
        summary.setdefault('source_sha256',{})[path]=hashlib.sha256((ROOT/path).read_bytes()).hexdigest()
    (dest/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary['median_seconds'],indent=2),flush=True)


if __name__=='__main__': main()
