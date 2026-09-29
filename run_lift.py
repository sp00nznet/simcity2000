#!/usr/bin/env python3
"""SimCity 2000 (Win95, SIMCITY.EXE 1996-03-07) lift driver.

Lifts every function of the original binary to C with the shared pcrecomp
lifter (`tools/lift/generate.py` + `lift32.py`); nothing here forks them.

Inside IDA's functions, instructions are decoded only at IDA's verified heads
(analysis/ida_codemap.json, from tools/ida/ida_export.py --key va), so jump
tables kept in .text are never decoded as code. Other entries use the shared
linear sweep.

Function entries come from three sources, unioned:
  1. IDA's catalog (analysis/ida_funcs.json, from tools/ida/ida_funcs.py), which
     carries exact [start, end) bounds.
  2. `recover.recover_functions`: jmp-thunk / tail-call targets and stored
     function pointers IDA never listed.
  3. The .reloc table. Every absolute pointer from a data section into .text is
     ground truth for a code address somebody can reach indirectly: MFC message
     maps and vtables (new entries), and C++ EH unwind funclets inside bodies
     (forced entries, so exception unwinding can be made to work later).

Output goes to src/recomp/gen/, which is gitignored: it is derived from the
game binary and never distributed (see README).

    py -3 run_lift.py [--exe original/SIMCITY.EXE] [--out src/recomp/gen]
"""
import argparse
import bisect
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# pcrecomp checkout: $PCRECOMP, else a sibling ../tools
_PC = os.path.join(os.environ.get('PCRECOMP') or os.path.join(_HERE, '..', 'tools'), 'tools')
sys.path.insert(0, os.path.join(_PC, 'lift'))
sys.path.insert(0, os.path.join(_PC, 'pe'))

import pefile                                               # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_32            # noqa: E402
from generate import (LinearInstruction,                     # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                   # noqa: E402
from pe_analyze import analyze_pe, build_iat_map            # noqa: E402
from recover import recover_functions                       # noqa: E402

# Functions whose body is hand-written in src/runtime/shims.c instead of lifted.
HOST_SHIM = {
}


def reloc_code_targets(pe, cs, ce):
    """Values of every HIGHLOW reloc outside .text that point into [cs, ce)."""
    text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    lo, hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
    base = pe.OPTIONAL_HEADER.ImageBase
    out = set()
    for blk in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
        for e in blk.entries:
            if e.type == 3 and not lo <= e.rva < hi:
                v = pe.get_dword_at_rva(e.rva)
                if cs <= v < ce:
                    out.add(v)
    assert base == 0x400000
    return out


def head_disassemble_function(md, code, cs, start, end, heads):
    """Decode only at IDA's verified instruction heads in [start, end).

    A linear sweep decodes whatever follows an instruction, and MSVC's CRT keeps
    jump tables inside .text: `_memcpy` does `jmp [edx*4 + 0x48F810]` and the
    table's bytes decode as `dec eax; clc; ...`, running straight over the arm
    at 0x48F820, which then never gets a label and the switch tail-calls a VA
    nobody lifted. IDA already separated code from data, so ask it.
    """
    lo, hi = bisect.bisect_left(heads, start), bisect.bisect_left(heads, end)
    insns, leaders = [], {start}
    for h in heads[lo:hi]:
        i = next(md.disasm(code[h - cs:h - cs + 16], h, count=1), None)
        if i is None:
            continue
        li = LinearInstruction(i)
        insns.append(li)
        if li.is_cond_jump or li.is_uncond_jump:
            t = li.get_branch_target()
            if t and start <= t < end:
                leaders.add(t)
            leaders.add(li.end_address)
    return insns, leaders


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(_HERE, 'original', 'SIMCITY.EXE'))
    ap.add_argument('--catalog', default=os.path.join(_HERE, 'analysis', 'ida_funcs.json'))
    ap.add_argument('--codemap', default=os.path.join(_HERE, 'analysis', 'ida_codemap.json'))
    ap.add_argument('--out', default=os.path.join(_HERE, 'src', 'recomp', 'gen'))
    ap.add_argument('--split', type=int, default=400)
    args = ap.parse_args()

    info = analyze_pe(args.exe)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    print('[*] base=0x%08X code=0x%08X-0x%08X IAT=%d' % (info.image_base, cs, ce, len(iat)))

    pe_data = open(args.exe, 'rb').read()
    text = [s for s in info.sections if s.name == '.text'][0]
    code = pe_data[text.raw_offset:text.raw_offset + min(text.virtual_size, text.raw_size)]

    cat = json.load(open(args.catalog, encoding='utf-8'))
    fns = sorted((f['ea'], min(f['end'], ce)) for f in cat['functions'] if cs <= f['ea'] < ce)
    starts = [a for a, _ in fns]
    ida_entries = set(starts)
    cmap = json.load(open(args.codemap))
    heads = sorted(h for seg in cmap.values() for h in seg['heads'])
    print('[*] IDA code map: %d instruction heads' % len(heads))
    print('[*] IDA catalog: %d functions' % len(fns))

    targets = reloc_code_targets(pefile.PE(args.exe, fast_load=False), cs, ce)
    starts_set = set(starts)
    forced = sorted(t for t in targets if t not in starts_set)
    extra = recover_functions(code, cs, ce, fns, forced=forced)
    print('[*] recovered %d more (%d forced from .reloc, e.g. EH funclets)'
          % (len(extra), len(forced)))
    fns = sorted(set(fns) | set(extra))
    known = {a for a, _ in fns}
    lost = sorted(t for t in targets if t not in known)
    if lost:
        print('[!] %d reloc code targets still not function entries: %s'
              % (len(lost), ', '.join('0x%08X' % t for t in lost[:8])))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    # `lifted` makes a call to an address outside the set an ICALL (reported at
    # runtime) instead of a call to an undeclared function (a link error).
    lifter = Lifter(iat_map=iat, lifted=known)
    os.makedirs(args.out, exist_ok=True)
    entries, chunk, idx, errors, t0 = [], [], 0, 0, time.time()
    seen = set()
    for addr, end in fns:
        if addr in seen:
            continue          # same start listed twice with different bounds
        seen.add(addr)
        name = 'sub_%08X' % addr
        if addr in HOST_SHIM:
            chunk.append(('/* %s: host shim */\nextern void %s(void);\n' % (name, name), addr, name))
        else:
            try:
                if addr in ida_entries:
                    insns, leaders = head_disassemble_function(md, code, cs, addr, end, heads)
                else:
                    insns, leaders = linear_disassemble_function(md, code, cs, addr, end)
                body = (lift_function_linear(lifter, name, insns, leaders, addr) if insns
                        else 'void %s(void) { }\n' % name)
                chunk.append((body, addr, name))
            except Exception as e:                              # noqa: BLE001
                chunk.append(('/* ERROR %s: %s */\nvoid %s(void) {}\n' % (name, e, name), addr, name))
                errors += 1
        entries.append((addr, name))
        if len(chunk) >= args.split:
            write_chunk(args.out, idx, chunk)
            idx += 1
            chunk = []
            print('[*]   %d/%d (%d err)' % (len(entries), len(fns), errors), flush=True)
    if chunk:
        write_chunk(args.out, idx, chunk)
        idx += 1

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* SimCity 2000 recomp - AUTO-GENERATED */\n#pragma once\n#include <stdint.h>\n\n')
        for a, n in entries:
            f.write('void %s(void);  /* 0x%08X */\n' % (n, a))
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* SimCity 2000 recomp - AUTO-GENERATED */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n' % len(entries))
        f.write('const uint32_t recomp_entry_va = 0x%08Xu;\n' % (info.image_base + info.entry_point_rva
                if hasattr(info, 'entry_point_rva') else info.entry_point))

    lines = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8', errors='replace'))
                for fn in os.listdir(args.out))
    stats = {'functions': len(entries), 'errors': errors, 'files': idx, 'lines': lines}
    json.dump(stats, open(os.path.join(_HERE, 'analysis', 'lift_stats.json'), 'w'), indent=1)
    print('=' * 60)
    print('  functions %d   errors %d   files %d   lines %s   %.1fs'
          % (len(entries), errors, idx, format(lines, ','), time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
