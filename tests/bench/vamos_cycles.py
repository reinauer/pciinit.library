#!/usr/bin/env python3
"""Count Vamos's modeled CPU cycles, excluding Python trap execution time."""
import sys
from amitools.vamos.machine import Machine
from amitools.vamos.main import main
cycles=0
execute=Machine.execute


def measured(self,max_cycles=1000):
    global cycles
    result=execute(self,max_cycles)
    cycles+=result.cycles
    return result


Machine.execute=measured
status=main(args=sys.argv[1:])
print(f'BENCH_CYCLES {cycles}')
raise SystemExit(status)
