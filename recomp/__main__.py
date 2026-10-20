"""Recompile Towns programs to C++, one module per program.

    python -m fmtownskit.recomp --out build/recomp \\
        --exp duel=DUEL.EXP@0c+103e2,c0b8,c124 \\
        --raw tbios=TBIOS.BIN@110:20-100 \\
        [--optest] [--selftest 3]

--exp NAME=FILE@CS[+SEED,...]      a Phar Lap P3 program, run under code selector CS
--raw NAME=FILE@CS[:FROM-TO][+SEED,...]
                                   a flat image at offset 0 of its segment, its
                                   entries the far pointers stored in FROM-TO
                                   and the seeds (TBIOS: its table at 0x20)

Each program is discovered (`recomp.discover`) and emitted (`recomp.emit`)
into its own namespace, p_<name>; functions are keyed by their offset in the
program's code segment. Writes, into --out:

    p_<name>_funcs.h        prototypes
    p_<name>_NNN.cpp        the functions, split by instruction count
    p_<name>_table.cpp      the module: its selector, image, crc32 and entries
    modules.cpp             every module of the build (g_x86_modules)
    CMakeLists.txt          the library `recomp`, the runtime, the self-test
    report.txt              discovery's report per module
    selftest/<name>.txt     with --selftest N: N vectors per function from i386emu
    selftest/optest.txt     with --optest: the emitter's own test (module OPTEST)
"""
import argparse
import os
import zlib

from . import discover
from . import emit as E
from . import selftest as ST

RUNTIME = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', 'runtime'))

CMAKE = """cmake_minimum_required(VERSION 3.20)
project(fmt_recomp CXX)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(FMT_RUNTIME {rt})
add_library(recomp STATIC
    {srcs})
target_include_directories(recomp PUBLIC ${{FMT_RUNTIME}})
target_compile_options(recomp PRIVATE -Wno-unused-label -Wno-unused-variable)
add_library(fmt_x86 STATIC ${{FMT_RUNTIME}}/x86_core.cpp ${{FMT_RUNTIME}}/x86_flags.cpp)
target_include_directories(fmt_x86 PUBLIC ${{FMT_RUNTIME}})
add_executable(selftest ${{FMT_RUNTIME}}/x86_selftest.cpp modules.cpp)
target_link_libraries(selftest recomp fmt_x86)
"""


class Out:
    """A generated file, written only if its content changed."""

    def __init__(self, path):
        self.path, self.parts = path, []

    def write(self, s):
        self.parts.append(s)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        text = ''.join(self.parts)
        try:
            with open(self.path, encoding='utf-8', newline='') as f:
                if f.read() == text:
                    return
        except OSError:
            pass
        with open(self.path, 'w', encoding='utf-8', newline='') as f:
            f.write(text)


class Module:
    def __init__(self, name, prog, cs, image, base):
        self.name, self.prog, self.cs, self.image, self.base = name, prog, cs, image, base
        self.ns = 'p_' + name.lower()


def parse(spec, raw):
    name, _, rest = spec.partition('=')
    rest, _, seeds = rest.partition('+')
    path, _, sel = rest.rpartition('@')
    far = None
    if raw and ':' in sel:
        sel, _, rng = sel.partition(':')
        lo, hi = rng.split('-')
        far = (int(lo, 16), int(hi, 16))
    seeds = [int(s, 16) for s in seeds.split(',') if s]
    return name, path, int(sel, 16), far, seeds


def write_module(out, m, per_file=6000):
    p, ns = m.prog, m.ns
    em = E.Emitter(p, ns)
    funcs = sorted(p.functions)
    with Out(os.path.join(out, '%s_funcs.h' % ns)) as f:
        f.write('#pragma once\n#include "x86.h"\nnamespace %s {\n' % ns)
        for a in funcs:
            f.write('void f_%08x(X86Cpu& c);\n' % a)
        f.write('}\n')
    files, chunk, count, k = [], [], 0, 0

    def flush():
        nonlocal chunk, count, k
        if not chunk:
            return
        path = os.path.join(out, '%s_%03d.cpp' % (ns, k))
        with Out(path) as f:
            f.write('#include "%s_funcs.h"\nnamespace %s {\n' % (ns, ns))
            f.write(''.join(chunk))
            f.write('}\n')
        files.append(path)
        chunk, count, k = [], 0, k + 1
    for a in funcs:
        if a not in p.insn:
            chunk.append('void f_%08x(X86Cpu& c) { x86_fault(c, 0x%xu, "no code at this entry: %s"); }\n'
                         % (a, a, p.refused.get(a, 'not decoded').replace('"', "'")))
            continue
        text = em.function(a)
        chunk.append(text)
        count += text.count('\n')
        if count > per_file:
            flush()
    flush()
    path = os.path.join(out, '%s_table.cpp' % ns)
    with Out(path) as f:
        f.write('#include "%s_funcs.h"\n' % ns)
        f.write('static const uint8_t k_image[%d] = {\n' % max(1, len(m.image)))
        for o in range(0, len(m.image), 24):
            f.write('    ' + ','.join(str(b) for b in m.image[o:o + 24]) + ',\n')
        f.write('};\n')
        f.write('static const X86FuncEntry k_funcs[] = {\n')
        for a in funcs:
            f.write('    {0x%xu, %s::f_%08x},\n' % (a, ns, a))
        f.write('};\n')
        f.write('extern const X86Module g_mod_%s = {"%s", 0x%x, %du, 0x%08xu, k_funcs, %du, 0x%xu, k_image};\n'
                % (ns, m.name.upper(), m.cs, len(m.image), zlib.crc32(m.image), len(funcs), m.base))
    files.append(path)
    return files


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', required=True)
    ap.add_argument('--exp', action='append', default=[])
    ap.add_argument('--raw', action='append', default=[])
    ap.add_argument('--optest', action='store_true')
    ap.add_argument('--selftest', type=int, default=0, metavar='N')
    ap.add_argument('--limit', type=int, default=20000, help="a vector's step limit in the interpreter")
    a = ap.parse_args(argv)
    os.makedirs(os.path.join(a.out, 'selftest'), exist_ok=True)
    mods = []
    from .. import exp as expmod
    for spec in a.exp:
        name, path, cs, _, seeds = parse(spec, False)
        e = expmod.Exp(open(path, 'rb').read())
        prog = discover.Program(e.image, e.base_offset, [e.eip] + seeds, name)
        mods.append(Module(name, prog, cs, e.image, e.base_offset))
    for spec in a.raw:
        name, path, cs, far, seeds = parse(spec, True)
        data = open(path, 'rb').read()
        prog = discover.Program(data, 0, seeds, name, far)
        mods.append(Module(name, prog, cs, data, 0))
    if a.optest:
        image, entries = ST.optest_image([m.prog for m in mods])
        prog = discover.Program(image, ST.OPTEST_BASE, entries, 'optest')
        mods.append(Module('optest', prog, 0, image, ST.OPTEST_BASE))
    srcs = []
    with Out(os.path.join(a.out, 'report.txt')) as rep:
        for m in mods:
            srcs += write_module(a.out, m)
            rep.write(m.prog.report() + '\n\n')
            print(m.prog.report().split('\n')[0])
    with Out(os.path.join(a.out, 'modules.cpp')) as f:
        f.write('#include "x86.h"\n')
        for m in mods:
            f.write('extern const X86Module g_mod_%s;\n' % m.ns)
        f.write('const X86Module* const g_x86_modules[] = {%s};\n' % ', '.join('&g_mod_%s' % m.ns for m in mods))
        f.write('const int g_x86_nmodules = %d;\n' % len(mods))
    with Out(os.path.join(a.out, 'CMakeLists.txt')) as f:
        rel = [os.path.relpath(s, a.out).replace('\\', '/') for s in srcs]
        f.write(CMAKE.format(rt=RUNTIME.replace('\\', '/'), srcs='\n    '.join(rel)))
    # vectors
    for m in sorted(mods, key=lambda m: m.name != 'optest'):       # the quick one first
        per = a.selftest if m.name != 'optest' else max(a.selftest, 3)
        if m.name != 'optest' and not a.selftest:
            continue
        lines, done, skipped = ST.vectors(m.name.upper(), [(m.base, m.image)], sorted(m.prog.functions), per,
                                          limit=a.limit, cs=m.cs)
        with Out(os.path.join(a.out, 'selftest', '%s.txt' % m.name.lower())) as f:
            f.write('\n'.join(lines) + '\n')
        print('%s: %d vectors, %d runs reached the machine or the step limit' % (m.name, done, skipped))


if __name__ == '__main__':
    main()
