# crashdump.py DUMP TSFP.exe: the faulting host address and the recompiled guest functions on its stack, from a CrashDumps minidump and the build's PDB
import struct, sys, ctypes
from ctypes import wintypes as W

path, exe = sys.argv[1], sys.argv[2]
d = open(path, 'rb').read()
sig, ver, n, rva = struct.unpack_from('<IIII', d, 0)
streams = {}
for i in range(n):
    t, size, r = struct.unpack_from('<III', d, rva + i * 12)
    streams[t] = (size, r)

def wstr(r):
    ln = struct.unpack_from('<I', d, r)[0]
    return d[r + 4:r + 4 + ln].decode('utf-16le')

mods = []
s, r = streams[4]
cnt = struct.unpack_from('<I', d, r)[0]
for i in range(cnt):
    o = r + 4 + i * 108
    base, size = struct.unpack_from('<QI', d, o)
    name_rva = struct.unpack_from('<I', d, o + 20)[0]
    mods.append((base, size, wstr(name_rva)))

def mod(a):
    for b, s_, nm in mods:
        if b <= a < b + s_:
            return b, nm
    return None, None

# memory ranges
mem = []
if 9 in streams:
    s, r = streams[9]
    cnt, baserva = struct.unpack_from('<QQ', d, r)
    off = baserva
    for i in range(cnt):
        st, sz = struct.unpack_from('<QQ', d, r + 16 + i * 16)
        mem.append((st, sz, off)); off += sz
if 5 in streams:
    s, r = streams[5]
    cnt = struct.unpack_from('<I', d, r)[0]
    for i in range(cnt):
        st, sz, mr = struct.unpack_from('<QII', d, r + 4 + i * 16)
        mem.append((st, sz, mr))

def read(a, n):
    for st, sz, off in mem:
        if st <= a and a + n <= st + sz:
            return d[off + a - st: off + a - st + n]
    return None

s, r = streams[6]
tid = struct.unpack_from('<I', d, r)[0]
code, = struct.unpack_from('<I', d, r + 8)
addr, = struct.unpack_from('<Q', d, r + 8 + 16)
ctx_size, ctx_rva = struct.unpack_from('<II', d, r + 8 + 152)
c = d[ctx_rva:ctx_rva + ctx_size]
rsp, = struct.unpack_from('<Q', c, 0x98)
rip, = struct.unpack_from('<Q', c, 0xF8)
names = ['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi','r8','r9','r10','r11','r12','r13','r14','r15']
regs = struct.unpack_from('<16Q', c, 0x78)
print('thread', tid, 'code %X' % code, 'rip %X' % rip, mod(rip))
print(' '.join('%s=%X' % (a, b) for a, b in zip(names, regs)))

dbg = ctypes.WinDLL('dbghelp')
proc = ctypes.c_void_p(0x1234)
dbg.SymSetOptions(0x2 | 0x10)
dbg.SymInitializeW.argtypes = [ctypes.c_void_p, W.LPCWSTR, W.BOOL]
dbg.SymInitializeW(proc, __import__('os').path.dirname(__import__('os').path.abspath(exe)), False)
dbg.SymLoadModuleExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, W.LPCWSTR, W.LPCWSTR, ctypes.c_uint64, W.DWORD, ctypes.c_void_p, W.DWORD]
dbg.SymLoadModuleExW.restype = ctypes.c_uint64
for b, sz, nm in mods:
    if nm.lower().endswith('tsfp.exe'):
        print('load', hex(dbg.SymLoadModuleExW(proc, None, exe, None, b, sz, None, 0)))
    else:
        dbg.SymLoadModuleExW(proc, None, nm, None, b, sz, None, 0)

class SYM(ctypes.Structure):
    _fields_ = [('SizeOfStruct', W.ULONG), ('TypeIndex', W.ULONG), ('Reserved', ctypes.c_uint64 * 2), ('Index', W.ULONG),
                ('Size', W.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', W.ULONG), ('Value', ctypes.c_uint64),
                ('Address', ctypes.c_uint64), ('Register', W.ULONG), ('Scope', W.ULONG), ('Tag', W.ULONG),
                ('NameLen', W.ULONG), ('MaxNameLen', W.ULONG), ('Name', ctypes.c_char * 256)]
dbg.SymFromAddr.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYM)]

def sym(a):
    s_ = SYM(); s_.SizeOfStruct = 88; s_.MaxNameLen = 255
    dsp = ctypes.c_uint64()
    if dbg.SymFromAddr(proc, a, ctypes.byref(dsp), ctypes.byref(s_)):
        return '%s+%X' % (s_.Name.decode(), dsp.value)
    b, nm = mod(a)
    return '%s+%X' % (nm, a - b) if b else None

print('rip', sym(rip))
stack=b''
s_, r_ = streams[3]
for i in range(struct.unpack_from('<I', d, r_)[0]):
    o = r_ + 4 + i * 48
    if struct.unpack_from('<I', d, o)[0] == tid:
        st, sz, rv = struct.unpack_from('<QII', d, o + 24)
        stack = read(rsp, st + sz - rsp) or b''
print('stack bytes', len(stack))
for i in range(0, len(stack) - 7, 8):
    v, = struct.unpack_from('<Q', stack, i)
    b, nm = mod(v)
    if b and nm.lower().endswith('tsfp.exe'):
        print('%04X %X %s' % (i, v, sym(v)))
