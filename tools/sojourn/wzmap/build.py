#!/usr/bin/env python3
"""Cross-build WzMap (OpenWarzone/WzMap) for Windows x64 with MinGW. usage: build.py [-j N]"""
import os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

B = '/home/claude/wzbuild'
SRC = B + '/src'
PROJ = '/home/claude/wzmap/wzmap/msvc2010'
OBJ = B + '/obj'
CXX = 'x86_64-w64-mingw32-g++-posix'
CC = 'x86_64-w64-mingw32-gcc-posix'
ASSIMP_INC = ['/home/claude/assimp-src/include', '/home/claude/assimp-build/include']
MDLIB = '/home/claude/sj/lib'


def proj_files(p):
    t = open(p, encoding='utf-8', errors='replace').read()
    out = []
    for f in re.findall(r'ClCompile Include="([^"]+)"', t):
        f = f.replace('\\', '/')
        f = os.path.normpath(os.path.join(os.path.dirname(p), f))
        f = f.replace('/home/claude/wzmap/wzmap/src', SRC)
        out.append(f)
    return out


files = proj_files(PROJ + '/BloodMap64.vcxproj')
for sub in ('picomodel/picomodel64', 'ddslib/ddslib64', 'l_net/l_net64', 'mathlib/mathlib64', 'jpeg6/jpeg664'):
    p = PROJ + '/' + sub + '.vcxproj'
    if not os.path.exists(p): p = p.replace('64.vcxproj', '.vcxproj')
    files += proj_files(p)
files = [f for f in dict.fromkeys(files) if not f.endswith(('gldraw.c', 'glad_gl.c'))]

cfiles = [MDLIB + '/libpng/' + f for f in os.listdir(MDLIB + '/libpng') if f.endswith('.c')]
cfiles += [MDLIB + '/zlib/' + f for f in os.listdir(MDLIB + '/zlib') if f.endswith('.c')]

inc = ['-I' + B + '/shim', '-I' + SRC, '-I' + SRC + '/games', '-I' + SRC + '/common', '-I' + SRC + '/libs', '-I' + SRC + '/libs/jpeg6',
       '-I' + SRC + '/libs/picomodel', '-I' + SRC + '/include', '-I' + MDLIB + '/libpng/include', '-I' + MDLIB + '/libpng',
       '-I' + MDLIB + '/zlib/include', '-I' + MDLIB + '/zlib'] + ['-I' + i for i in ASSIMP_INC]
defs = ['-DWIN32', '-DWIN64', '-D_WIN32', '-D_CONSOLE', '-DNDEBUG', '-D_CRT_SECURE_NO_DEPRECATE', '-DNOMINMAX']
common = ['-include', B + '/shim/wzfix.h', '-O2', '-msse2', '-fopenmp', '-w', '-fpermissive', '-fno-strict-aliasing']

os.makedirs(OBJ, exist_ok=True)


def objname(f):
    return OBJ + '/' + f.replace('/', '_').lstrip('_') + '.o'


def compile_one(f, lang):
    o = objname(f)
    if os.path.exists(o) and os.path.getmtime(o) > os.path.getmtime(f): return o, None
    if lang == 'c++':
        cmd = [CXX, '-x', 'c++', '-std=gnu++14'] + common + defs + inc + ['-c', f, '-o', o]
    else:
        cmd = [CC] + common + defs + inc + ['-c', f, '-o', o]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return o, (r.stderr if r.returncode else None)


jobs = int(sys.argv[sys.argv.index('-j') + 1]) if '-j' in sys.argv else 16
errs = {}
objs = []
with ThreadPoolExecutor(jobs) as ex:
    futs = [(f, ex.submit(compile_one, f, 'c++')) for f in files] + [(f, ex.submit(compile_one, f, 'c')) for f in cfiles]
    for f, fu in futs:
        o, e = fu.result()
        if e: errs[f] = e
        else: objs.append(o)
for f, e in errs.items():
    print('==== ' + f)
    print('\n'.join([l for l in e.split('\n') if 'error' in l][:8]))
if errs:
    print('%d files failed' % len(errs)); sys.exit(1)
res = OBJ + '/bloodmap_res.o'
subprocess.run(['x86_64-w64-mingw32-windres', '-I' + SRC, SRC + '/bloodmap.rc', '-O', 'coff', '-o', res])
if os.path.exists(res): objs.append(res)
out = B + '/WzMap.exe'
cmd = [CXX, '-o', out] + objs + ['/home/claude/assimp-build/lib/libassimp.a', '/home/claude/assimp-build/contrib/zlib/libzlibstatic.a',
       '-fopenmp', '-Wl,--allow-multiple-definition', '-static', '-static-libgcc', '-static-libstdc++', '-Wl,--stack,8388608',
       '-lws2_32', '-lwsock32', '-lshlwapi', '-lole32', '-luuid', '-lgdi32', '-luser32']
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    und = sorted(set(re.findall(r"undefined reference to `([^']+)'", r.stderr)))
    print('\n'.join(und[:80]) if und else r.stderr[-4000:])
    print('link failed: %d undefined' % len(und)); sys.exit(1)
print('built', out, os.path.getsize(out))
