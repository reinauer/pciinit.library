#!/usr/bin/env python3
"""Run upstream fixtures and checked workloads on both native m68k backends.

Requires NASM/ndisasm and Vamos. NASM 3.01 is refused: under -O99 it emits
short jumps for "jcc near", which breaks the near-branch fixtures. Fixtures
are generated outside the vendor checkout. Golden checks compare saved registers and specified memory bytes;
full differential comparisons also include all RAM and stop/budget state.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
GOLDEN_DIFFERENCES={
    '0003_lds_real':(2,'NASM 3 emits equivalent 67/66 prefixes in a different order'),
    '0006_div':(28,'Library policy stops at the first CPU fault'),
    '0007_seg_load_pm16':(6,'Library policy stops at the first CPU fault'),
    '0008_seg_load_pm32':(6,'Library policy stops at the first CPU fault'),
    '0035_idiv':(16,'Library policy stops at the first CPU fault'),
    '0036_div':(12,'Library policy stops at the first CPU fault'),
}



def run(command, **kwargs):
    # Output goes to a file: Vamos raises when a program seeks a pipe.
    with tempfile.TemporaryFile('w+') as out:
        p=subprocess.run(list(map(str,command)),cwd=ROOT,text=True,
                         stdout=out,stderr=subprocess.STDOUT,timeout=300,**kwargs)
        out.seek(0); output=out.read()
    if p.returncode:
        raise RuntimeError(f'{command}\n{output[-5000:]}')
    return output


def fixture(source,dest):
    regs=[]; memory=[]
    for line in source.read_text().splitlines():
        line=line.split(';')[0]
        m=re.match(r'^([0-9a-f]{8}):(.*)',line)
        if m:
            a=int(m[1],16); data=m[2]
            for i in range(0,len(data),4):
                part=data[i:i+4].strip()
                if part: memory.append((a+i//4,int(part[-2:],16)))
        else:
            regs.extend((k,int(v,16)) for k,v in re.findall(r'([a-z0-9.]+)=([0-9a-f]+)',line))
    with dest.open('wb') as out:
        def number(v): out.write(struct.pack('>I',v))
        number(len(regs))
        for k,v in regs:
            name=k.encode(); number(len(name)); out.write(name); number(v)
        number(len(memory))
        for a,v in memory: number(a); number(v)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cross',default='m68k-amigaos-')
    parser.add_argument('--nasm-dir',type=Path)
    parser.add_argument('--output',type=Path,default=ROOT/'build/blockjit-check')
    parser.add_argument('--timed',action='store_true')
    args=parser.parse_args(); dest=args.output.resolve(); dest.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy()
    if args.nasm_dir: env['PATH']=str(args.nasm_dir.resolve())+os.pathsep+env['PATH']
    nasm=run(['nasm','-v'],env=env).split()
    if nasm[2:3]==['3.01']:
        raise SystemExit('NASM 3.01 shortens near jumps under -O99; use another version')
    vendor=ROOT/'third_party/libx86emu/test'
    cases=sorted(vendor.glob('*.done'))
    for golden in cases: shutil.copy2(golden.with_suffix('.tst'),dest)
    run(['perl',vendor/'prepare_test',*[dest/p.with_suffix('.tst').name for p in cases]],env=env)
    for golden in cases:
        fixture(dest/golden.with_suffix('.init').name,dest/(golden.stem+'.input'))
        fixture(golden,dest/(golden.stem+'.golden'))
    manifest=dest/'manifest'; manifest.write_text(''.join(p.stem+'\n' for p in cases))
    python=Path(shutil.which('vamos')).read_text().splitlines()[0][2:]
    flags=['-m68020','-O2','-fno-strict-aliasing','-Iinclude','-Ithird_party/libx86emu/include',
           '-Itests/bench','-DX86EMU_NO_HOST_IO','-DX86EMU_EXTERNAL_CLOCK','-DX86EMU_NO_DISASSEMBLY','-DX86EMU_NO_TRACE_FORMATTING',
           '-DX86EMU_DIRECT_FETCH','-DX86EMU_BULK_STRINGS']
    sources=[ROOT/'third_party/libx86emu'/f'{n}.c' for n in ('decode','ops','ops2','prim_ops')]
    sources += [ROOT/p for p in ('src/x86emu/engine.c','src/x86emu/backends/libx86emu_adapter.c','src/runtime/memory.c')]
    report={'compiler':run([args.cross+'gcc','--version']).splitlines()[0], 'flags':flags,
            'timed':args.timed,'source_sha256':{},'variants':{},'completed':False}
    for backend in ('libx86emu','m68kjit'):
        folder=dest/backend; folder.mkdir(exist_ok=True); objects=[]
        for source in sources+[ROOT/f'src/x86emu/backends/{backend}.c']:
            obj=folder/(source.stem+'.o'); objects.append(obj)
            run([args.cross+'gcc',*flags,'-ffreestanding','-fno-builtin','-c',source,'-o',obj])
            report['source_sha256'][str(source.relative_to(ROOT))]=hashlib.sha256(source.read_bytes()).hexdigest()
        def build(name,files,extra=()):
            exe=folder/name
            run([args.cross+'gcc',*flags,*extra,*files,*objects,'-o',exe]); return exe
        def execute(exe,*argv,cycles=False):
            return run([python,ROOT/'tests/bench/vamos_cycles.py','-S','--vols-base-dir',folder/'vols',
                        '-V','fixtures:'+str(dest),'-q','-C','68020','-s','128','-m','16384','-H','disable','--',exe,*argv])
        exe=build('upstream',[ROOT/'tests/upstream.c'],['-DTEST_JIT'] if backend=='m68kjit' else [])
        output=execute(exe,'fixtures:manifest','fixtures:'); (folder/'upstream.txt').write_text(output)
        entries={}
        for line in output.splitlines():
            if not line.startswith('UPSTREAM ') or line.startswith('UPSTREAM RESULT'): continue
            words=line.split(); entry={k:int(v) for k,v in (w.split('=') for w in words[2:])}
            path=dest/(words[1]+'.output'); saved=folder/path.name; path.rename(saved)
            entry['sha256']=hashlib.sha256(saved.read_bytes()).hexdigest(); entries[words[1]]=entry
            if entry['golden_errors']:
                known=GOLDEN_DIFFERENCES.get(words[1])
                if not known or known[0]!=entry['golden_errors']:
                    raise RuntimeError('unexplained golden mismatch: '+line)
                entry['golden_difference']=known[1]
            if backend=='m68kjit' and entry['golden_errors']!=report['variants']['libx86emu']['upstream'][words[1]]['golden_errors']:
                raise RuntimeError('golden error count changed: '+line)
            if backend=='m68kjit' and saved.read_bytes()!=(dest/'libx86emu'/saved.name).read_bytes():
                raise RuntimeError('upstream differential mismatch: '+words[1])
        if len(entries)!=len(cases): raise RuntimeError(output)
        report['variants'][backend]={'upstream':entries,'workloads':{}}
        if backend=='m68kjit':
            boundary=build('boundaries',[ROOT/'tests/blockjit.c'])
            output=execute(boundary); (folder/'boundaries.txt').write_text(output)
            report['variants'][backend]['boundaries']=re.search(r'BLOCKJIT RESULT .+',output)[0]
        mapped=build('mapped-test',[ROOT/'tests/engine.c'],['-DENGINE_MAPPED'])
        (folder/'mapped.txt').write_text(execute(mapped))
        bench=build('bench',[ROOT/f'tests/bench/{n}.c' for n in ('wrapper','workload','differential')],
                    ['-DWRAPPER_MAPPED']+(['-DWRAPPER_TIMED'] if args.timed else []))
        output=execute(bench,'check'); (folder/'differential.txt').write_text(output)
        fingerprint=re.search(r'DIFFERENTIAL cases=\d+ hash=\w+',output)[0]
        if backend=='libx86emu': reference=fingerprint
        elif fingerprint!=reference: raise RuntimeError('register differential mismatch')
        for i,name in enumerate(('alu16','alu32','memory','rep','io','registers16','registers32')):
            samples=[]
            for repeats in (1,3):
                output=execute(bench,i,repeats); (folder/f'{name}-{repeats}.txt').write_text(output)
                if 'verified=1' not in output: raise RuntimeError(output)
                samples.append(int(re.search(r'BENCH_CYCLES (\d+)',output)[1]))
            report['variants'][backend]['workloads'][name]={'cycles':samples,'per_repeat':(samples[1]-samples[0])/2}
        print(backend,'upstream',len(entries),'native',sum(e['native'] for e in entries.values()),flush=True)
    for path in [Path(__file__),*sorted((ROOT/'include').rglob('*.h')),
                 *sorted((ROOT/'third_party/libx86emu/include').glob('*.h')),
                 *sorted((ROOT/'tests/bench').glob('*')),ROOT/'tests/upstream.c',ROOT/'tests/engine.c',ROOT/'tests/blockjit.c',
                 vendor/'prepare_test',*cases,*[p.with_suffix('.tst') for p in cases]]:
        if path.is_file():
            report['source_sha256'][str(path.relative_to(ROOT))]=hashlib.sha256(path.read_bytes()).hexdigest()
    report['completed']=True
    (dest/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(dest/'results.json')


if __name__=='__main__': main()
