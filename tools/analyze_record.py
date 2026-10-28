#!/usr/bin/env python3
"""Summarize a private POST recording without emitting ROM or RAM contents."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct


def analyze(path):
    data = path.read_bytes()
    if data[:4] != b'PIR1':
        raise ValueError('not a PIR1 recording')
    kinds, ports, memory, interrupts = Counter(), Counter(), Counter(), Counter()
    offset = 4
    elements = 0
    bulk_bytes = Counter()
    while offset < len(data):
        if len(data) - offset < 8:
            raise ValueError('truncated event header')
        kind, size = struct.unpack_from('>II', data, offset)
        offset += 8
        if size > len(data) - offset:
            raise ValueError('truncated event payload')
        payload = data[offset:offset+size]
        offset += size
        kinds[kind] += 1
        if kind == 3:
            if size != 20:
                raise ValueError('invalid access event')
            address, width, operation, _, status = struct.unpack('>IIIII', payload)
            if width not in (1, 2, 4) or operation > 4:
                raise ValueError('invalid access width or operation')
            name = ('read', 'write', 'fetch', 'in', 'out')[operation]
            target = ports if operation >= 3 else memory
            target[(address, width, name, status)] += 1
        elif kind == 4:
            if size != 32:
                raise ValueError('invalid bulk event')
            _, operation, _, _, width, count, _, _ = struct.unpack('>8I', payload)
            if operation > 1 or width not in (1, 2, 4):
                raise ValueError('invalid bulk operation or width')
            elements += count
            bulk_bytes[('fill', 'copy')[operation]] += width * count
        elif kind == 5:
            if size != 124:
                raise ValueError('invalid interrupt event')
            interrupts[struct.unpack_from('>I', payload)[0]] += 1
    def entries(counter):
        return [{'address':hex(a), 'width':w, 'operation':op, 'status':s, 'count':n}
                for (a,w,op,s),n in sorted(counter.items())]
    return {'sha256':hashlib.sha256(data).hexdigest(), 'bytes':len(data),
            'events':sum(kinds.values()), 'event_kinds':dict(sorted(kinds.items())),
            'ports':entries(ports), 'interrupts':{hex(k):v for k,v in sorted(interrupts.items())},
            'configuration_port_accesses':sum(n for (a,w,_,_),n in ports.items()
                                              if a < 0xd00 and a+w > 0xcf8),
            'vga_memory_accesses':sum(n for (a,w,_,_),n in memory.items()
                                      if 0xa0000 <= a and a+w <= 0xc0000),
            'other_memory_accesses':entries(Counter({k:v for k,v in memory.items()
                                       if not (0xa0000 <= k[0] and k[0]+k[1] <= 0xc0000)})),
            'vga_memory_bytes':{name:sum(w*n for (a,w,op,status),n in memory.items()
                                        if op==name and not status and 0xa0000<=a and a+w<=0xc0000)
                                for name in ('read','write','fetch')},
            'card_port_operations_excluding_pit':sum(n for (a,_,_,status),n in ports.items()
                                                     if not status and a not in (0x42,0x43,0x61)),
            'bulk_bytes':dict(bulk_bytes), 'bulk_elements':elements}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.recording), indent=2))
