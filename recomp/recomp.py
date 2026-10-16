#!/usr/bin/env python3
"""Static recompiler: translates the x86 code of hamsterball.exe into C.

Usage: recomp.py <hamsterball.exe> <outdir> [coverage files...]

Each x86 function becomes a C function `void F_<addr>(void)`. Inside a function the eight
registers and the arithmetic flags are C locals, so the C compiler drops flag computations
nobody reads. Calls spill the registers to the shared `cpu` struct; `ret` stores its return
address in cpu.ret_to, and every call site checks it came back where x86 would have.
Calls through pointers go via hb_call(), which looks the target up in the function table or
runs a Windows API thunk.

Code is found from the entry point, direct calls, pointers into the code in the data
sections and instruction immediates, switch jump tables, and coverage files recorded by the
Unicorn build (HB_COVERAGE: "<previous block> <block>" lines; previous 0 = entered from the
host, 1 = returned from an API).
"""
import collections
import os
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import (X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_CS, X86_REG_DS,
                          X86_REG_ES, X86_REG_FS, X86_REG_GS, X86_REG_INVALID, X86_REG_SS)

BASE = 0x400000
SETJMP = set()  # MSVC _setjmp3 entry points (found by find_setjmp)


class Image:
    def __init__(self, path):
        f = open(path, "rb").read()
        pe = struct.unpack_from("<I", f, 0x3C)[0]
        nsec = struct.unpack_from("<H", f, pe + 6)[0]
        opt = pe + 24
        self.entry = BASE + struct.unpack_from("<I", f, opt + 16)[0]
        size = struct.unpack_from("<I", f, opt + 56)[0]
        self.mem = bytearray(size)
        s = opt + struct.unpack_from("<H", f, pe + 20)[0]
        self.sections = []
        for i in range(nsec):
            name = f[s:s + 8].rstrip(b"\0").decode()
            vs, va, rs, rp = struct.unpack_from("<IIII", f, s + 8)
            n = min(rs, vs) if vs else rs
            self.mem[va:va + n] = f[rp:rp + n]
            self.sections.append((name, BASE + va, BASE + va + max(vs, rs)))
            s += 40
        text = [x for x in self.sections if x[0] == ".text"][0]
        self.tlo, self.thi = text[1], text[2]

    def in_text(self, a):
        return self.tlo <= a < self.thi

    def u32(self, a):
        return struct.unpack_from("<I", self.mem, a - BASE)[0]

    def u8(self, a):
        return self.mem[a - BASE]


img = None
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
_dec = {}


def decode(a):
    if a in _dec:
        return _dec[a]
    i = None
    if img.in_text(a):
        for x in md.disasm(bytes(img.mem[a - BASE:a - BASE + 16]), a, 1):
            i = x
    _dec[a] = i
    return i


def is_call(i):
    return i.mnemonic == "call"


def is_jmp(i):
    return i.mnemonic == "jmp"


def is_jcc(i):
    return i.mnemonic.startswith("j") and i.mnemonic not in ("jmp",)


def is_ret(i):
    return i.mnemonic in ("ret", "retf", "iretd")


def ends_flow(i):
    return is_jmp(i) or is_ret(i) or i.mnemonic in ("hlt", "int3", "ud2")


def imm_target(i):
    op = i.operands[0]
    return op.imm & 0xFFFFFFFF if op.type == X86_OP_IMM else None


# ---- discovery ------------------------------------------------------------------------------

class Func:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}        # addr -> insn
        self.labels = {entry}
        self.switch = {}       # jmp addr -> set of targets
        self.calls = set()


def jump_table_targets(i, f):
    """Targets of `jmp [reg*4 + table]`: read entries while they point into the code."""
    op = i.operands[0]
    if op.type != X86_OP_MEM or op.mem.index == X86_REG_INVALID or op.mem.scale != 4 or op.mem.base != X86_REG_INVALID:
        return set()
    tab = op.mem.disp & 0xFFFFFFFF
    out = set()
    for k in range(1024):
        a = tab + 4 * k
        if not (BASE <= a < BASE + len(img.mem) - 4):
            break
        t = img.u32(a)
        if not img.in_text(t) or abs(t - i.address) > 0x20000 or decode(t) is None:
            break
        if a in f.insns:
            break
        out.add(t)
    return out


def build_func(entry, funcs, dyn_jumps):
    f = Func(entry)
    work = [entry]
    while work:
        a = work.pop()
        while a not in f.insns:
            i = decode(a)
            if i is None:
                break
            f.insns[a] = i
            nxt = a + i.size
            if is_call(i):
                t = imm_target(i)
                if t is not None:
                    f.calls.add(t)
                a = nxt
                continue
            if is_jcc(i):
                t = imm_target(i)
                f.labels.add(t)
                work.append(t)
                f.labels.add(nxt)
                a = nxt
                continue
            if is_jmp(i):
                t = imm_target(i)
                if t is not None:
                    if t in funcs and t != entry:
                        pass  # tail call
                    else:
                        f.labels.add(t)
                        work.append(t)
                else:
                    op = i.operands[0]
                    table = op.type == X86_OP_MEM and op.mem.index != X86_REG_INVALID
                    # recorded targets only for switch tables; others (longjmp...) go through hb_jmp
                    ts = jump_table_targets(i, f) | (dyn_jumps.get(a, set()) if table else set())
                    ts = {t for t in ts if t not in funcs or t == entry}
                    if ts:
                        f.switch[a] = ts
                        for t in ts:
                            f.labels.add(t)
                            work.append(t)
                break
            if ends_flow(i):
                break
            a = nxt
    return f


def block_last_insn(a):
    """Decode a Unicorn block starting at a; return its last (control-flow) instruction."""
    for _ in range(4096):
        i = decode(a)
        if i is None:
            return None
        if is_call(i) or is_jmp(i) or is_jcc(i) or ends_flow(i) or i.mnemonic in ("loop", "jecxz", "jcxz"):
            return i
        a += i.size
    return None


def discover(cov_files):
    funcs = {img.entry}
    dyn_jumps = collections.defaultdict(set)
    for path in cov_files:
        for line in open(path):
            p = line.split()
            if len(p) != 2:
                continue
            frm, to = int(p[0], 16), int(p[1], 16)
            if frm == 0:
                funcs.add(to)
                continue
            if frm == 1:
                continue
            last = block_last_insn(frm)
            if last is None:
                continue
            nxt = last.address + last.size
            if is_call(last) and to != nxt:
                funcs.add(to)
            elif is_jmp(last) and imm_target(last) is None:
                dyn_jumps[last.address].add(to)
    ndyn = len(funcs)
    # pointers into the code from the data sections (vtables, callbacks, function tables)
    for name, lo, hi in img.sections:
        if name in (".text", ".rsrc", ".aspack"):
            continue
        for a in range(lo, min(hi, BASE + len(img.mem)) - 3, 4):
            t = img.u32(a)
            if img.in_text(t) and plausible_entry(t):
                funcs.add(t)
    # iterate: direct call targets and code addresses used as immediates
    done = {}
    work = list(funcs)
    while work:
        e = work.pop()
        if e in done or not img.in_text(e):
            continue
        f = build_func(e, funcs, dyn_jumps)
        done[e] = f
        new = set(f.calls)
        for i in f.insns.values():
            for op in i.operands:
                if op.type == X86_OP_IMM and not (is_call(i) or is_jmp(i) or is_jcc(i)):
                    t = op.imm & 0xFFFFFFFF
                    if img.in_text(t) and plausible_entry(t):
                        new.add(t)
        for t in new:
            if t not in funcs:
                funcs.add(t)
                work.append(t)
    # a function entry found later may turn earlier jumps into tail calls; rebuild once
    final = {e: build_func(e, funcs, dyn_jumps) for e in sorted(done)}
    print("functions: %d (%d from coverage), instructions: %d" % (
        len(final), ndyn, sum(len(f.insns) for f in final.values())))
    return final


def plausible_entry(t):
    """A pointer target that decodes as several sane instructions."""
    a = t
    for _ in range(4):
        i = decode(a)
        if i is None or i.mnemonic in ("int3", "hlt", "in", "out", "insb", "insd", "outsb", "outsd", "arpl",
                                       "bound", "into", "les", "lds", "retf", "iretd", "sti", "cli", "aam",
                                       "aad", "daa", "das", "aaa", "aas", "salc", "fwait"):
            return False
        if is_ret(i) or is_jmp(i):
            return True
        a += i.size
    return True


# ---- code generation ----------------------------------------------------------------------------

R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
R16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
R8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
R8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}
STYPE = {1: "int8_t", 2: "int16_t", 4: "int32_t"}
UTYPE = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}


class Unsupported(Exception):
    pass


class Gen:
    def __init__(self, f, funcs):
        self.f = f
        self.funcs = funcs
        self.out = []
        self.flagsrc = None  # (kind, size) of the last flag-setting op in this block
        self.unsupported = collections.Counter()
        self.uses_setjmp = False

    def emit(self, s):
        self.out.append("    " + s)

    # operands
    def regname(self, i, r):
        return i.reg_name(r)

    def addr(self, i, op):
        m = op.mem
        parts = []
        if m.base != X86_REG_INVALID:
            parts.append(self.read_reg(i.reg_name(m.base)))
        if m.index != X86_REG_INVALID:
            idx = self.read_reg(i.reg_name(m.index))
            parts.append(idx if m.scale == 1 else "%s*%d" % (idx, m.scale))
        if m.disp:
            parts.append("0x%Xu" % (m.disp & 0xFFFFFFFF))
        if m.segment == X86_REG_FS:
            parts.append("FS_BASE")
        elif m.segment == X86_REG_GS:
            raise Unsupported("gs")
        if not parts:
            return "0u"
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def read_reg(self, n):
        if n in R32:
            return n
        if n in R16:
            return "(uint16_t)%s" % R16[n]
        if n in R8L:
            return "(uint8_t)%s" % R8L[n]
        if n in R8H:
            return "(uint8_t)(%s >> 8)" % R8H[n]
        raise Unsupported("reg " + n)

    def write_reg(self, n, v):
        if n in R32:
            return "%s = (uint32_t)(%s);" % (n, v)
        if n in R16:
            r = R16[n]
            return "%s = (%s & 0xFFFF0000u) | (uint16_t)(%s);" % (r, r, v)
        if n in R8L:
            r = R8L[n]
            return "%s = (%s & 0xFFFFFF00u) | (uint8_t)(%s);" % (r, r, v)
        if n in R8H:
            r = R8H[n]
            return "%s = (%s & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(%s) << 8);" % (r, r, v)
        raise Unsupported("reg " + n)

    def rd(self, i, op, size=None):
        size = size or op.size
        if op.type == X86_OP_REG:
            return self.read_reg(i.reg_name(op.reg))
        if op.type == X86_OP_IMM:
            return "0x%Xu" % (op.imm & int(MASK[size][:-1], 16))
        if op.type == X86_OP_MEM:
            return "R%d(%s)" % (size * 8, self.addr(i, op))
        raise Unsupported("operand")

    def wr(self, i, op, v):
        if op.type == X86_OP_REG:
            return self.write_reg(i.reg_name(op.reg), v)
        if op.type == X86_OP_MEM:
            return "W%d(%s, %s);" % (op.size * 8, self.addr(i, op), v)
        raise Unsupported("write operand")

    # flags
    def set_logic(self, r, size):
        n = size * 8
        self.emit("CF = 0; OF = 0; ZF = (%s) == 0; SF = ((%s) >> %d) & 1; PF = PARITY(%s);" % (r, r, n - 1, r))
        self.emit("fr = %s;" % r)
        self.flagsrc = ("logic", size)

    def cond(self, cc):
        """C condition for an x86 condition code, using the last flag source when possible."""
        src = self.flagsrc
        if src:
            kind, size = src
            st = STYPE[size]
            if kind == "sub":
                m = {"e": "fr == 0", "z": "fr == 0", "ne": "fr != 0", "nz": "fr != 0",
                     "b": "fd < fs", "c": "fd < fs", "nae": "fd < fs", "ae": "fd >= fs", "nb": "fd >= fs",
                     "nc": "fd >= fs", "be": "fd <= fs", "na": "fd <= fs", "a": "fd > fs", "nbe": "fd > fs",
                     "l": "(%s)fd < (%s)fs" % (st, st), "nge": "(%s)fd < (%s)fs" % (st, st),
                     "ge": "(%s)fd >= (%s)fs" % (st, st), "nl": "(%s)fd >= (%s)fs" % (st, st),
                     "le": "(%s)fd <= (%s)fs" % (st, st), "ng": "(%s)fd <= (%s)fs" % (st, st),
                     "g": "(%s)fd > (%s)fs" % (st, st), "nle": "(%s)fd > (%s)fs" % (st, st),
                     "s": "(%s)fr < 0" % st, "ns": "(%s)fr >= 0" % st}
                if cc in m:
                    return m[cc]
            if kind == "logic":
                m = {"e": "fr == 0", "z": "fr == 0", "ne": "fr != 0", "nz": "fr != 0",
                     "s": "(%s)fr < 0" % st, "ns": "(%s)fr >= 0" % st, "l": "(%s)fr < 0" % st,
                     "ge": "(%s)fr >= 0" % st, "le": "(%s)fr <= 0" % st, "g": "(%s)fr > 0" % st,
                     "be": "fr == 0", "a": "fr != 0", "b": "0", "ae": "1"}
                if cc in m:
                    return m[cc]
            if kind == "zs":  # inc/dec/neg...: ZF and SF from fr
                m = {"e": "fr == 0", "z": "fr == 0", "ne": "fr != 0", "nz": "fr != 0",
                     "s": "(%s)fr < 0" % st, "ns": "(%s)fr >= 0" % st}
                if cc in m:
                    return m[cc]
        m = {"o": "OF", "no": "!OF", "b": "CF", "c": "CF", "nae": "CF", "ae": "!CF", "nb": "!CF", "nc": "!CF",
             "e": "ZF", "z": "ZF", "ne": "!ZF", "nz": "!ZF", "be": "(CF | ZF)", "na": "(CF | ZF)",
             "a": "!(CF | ZF)", "nbe": "!(CF | ZF)", "s": "SF", "ns": "!SF", "p": "PF", "pe": "PF",
             "np": "!PF", "po": "!PF", "l": "(SF != OF)", "nge": "(SF != OF)", "ge": "(SF == OF)",
             "nl": "(SF == OF)", "le": "(ZF | (SF != OF))", "ng": "(ZF | (SF != OF))",
             "g": "(!ZF & (SF == OF))", "nle": "(!ZF & (SF == OF))"}
        return m[cc]

    # control flow helpers
    def goto(self, t):
        if t in self.f.insns and t in self.f.labels:
            return "goto L_%X;" % t
        if t in self.funcs:
            return "SPILL(); F_%X(); return;" % t
        return "SPILL(); hb_jmp(0x%Xu); return;" % t

    def call_direct(self, t, nxt):
        if t in SETJMP:
            # host setjmp in this C frame; guest longjmp ends in an indirect jmp to nxt (hb_call -> host longjmp)
            self.uses_setjmp = True
            self.emit("esp -= 4; W32(esp, 0x%Xu); SPILL(); hb_sj_link(&sjr, 0x%Xu);" % (nxt, nxt))
            self.emit("if (setjmp(sjr.jb) == 0) { F_%X(); RELOAD(); CHECKRET(0x%Xu); } else { RELOAD(); }" % (t, nxt))
            return
        if t in self.funcs:
            c = "F_%X();" % t
        else:
            c = "hb_call(0x%Xu);" % t
        self.emit("esp -= 4; W32(esp, 0x%Xu); SPILL(); %s RELOAD(); CHECKRET(0x%Xu);" % (nxt, c, nxt))

    # main
    def insn(self, i):
        mn = i.mnemonic
        ops = i.operands
        nxt = i.address + i.size
        b0 = self.opcode_byte(i)
        if 0xD8 <= b0 <= 0xDF:
            self.fpu(i)
            return
        e = self.emit
        if mn in ("nop", "wait", "fwait", "emms", "pause", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "prefetch"):
            return
        if mn == "mov":
            if ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) in ("cs", "ds", "es", "ss", "fs", "gs"):
                return
            if ops[1].type == X86_OP_REG and i.reg_name(ops[1].reg) in ("cs", "ds", "es", "ss", "fs", "gs"):
                e(self.wr(i, ops[0], "0x23u"))
                return
            e(self.wr(i, ops[0], self.rd(i, ops[1], ops[0].size)))
            return
        if mn in ("movzx", "movsx"):
            v = self.rd(i, ops[1])
            if mn == "movsx":
                v = "(uint32_t)(int32_t)(%s)(%s)" % (STYPE[ops[1].size], v)
            e(self.wr(i, ops[0], v))
            return
        if mn == "lea":
            e(self.wr(i, ops[0], self.addr(i, ops[1]).replace("FS_BASE", "0")))
            return
        if mn == "push":
            if ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) in ("cs", "ds", "es", "ss", "fs", "gs"):
                e("esp -= 4; W32(esp, 0x23u);")
                return
            sz = ops[0].size if ops[0].type != X86_OP_IMM else (2 if i.prefix[2] == 0x66 else 4)
            e("{ uint32_t v = %s; esp -= %d; W%d(esp, v); }" % (self.rd(i, ops[0], sz), sz, sz * 8))
            return
        if mn == "pop":
            if ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) in ("cs", "ds", "es", "ss", "fs", "gs"):
                e("esp += 4;")
                return
            sz = ops[0].size
            e("{ uint32_t v = R%d(esp); esp += %d; %s }" % (sz * 8, sz, self.wr(i, ops[0], "v")))
            return
        if mn in ("pushal", "pushad"):
            e("{ uint32_t o = esp; esp -= 32; W32(esp+28, eax); W32(esp+24, ecx); W32(esp+20, edx); W32(esp+16, ebx);"
              " W32(esp+12, o); W32(esp+8, ebp); W32(esp+4, esi); W32(esp, edi); }")
            return
        if mn in ("popal", "popad"):
            e("edi = R32(esp); esi = R32(esp+4); ebp = R32(esp+8); ebx = R32(esp+16); edx = R32(esp+20);"
              " ecx = R32(esp+24); eax = R32(esp+28); esp += 32;")
            return
        if mn in ("pushfd", "pushf"):
            e("esp -= 4; W32(esp, MAKE_EFLAGS());")
            self.flagsrc = None
            return
        if mn in ("popfd", "popf"):
            e("{ uint32_t v = R32(esp); esp += 4; SET_EFLAGS(v); }")
            self.flagsrc = None
            return
        if mn == "sahf":
            e("{ uint32_t v = (eax >> 8) & 0xFF; CF = v & 1; PF = (v >> 2) & 1; ZF = (v >> 6) & 1; SF = (v >> 7) & 1; }")
            self.flagsrc = None
            return
        if mn == "lahf":
            e("eax = (eax & 0xFFFF00FFu) | ((MAKE_EFLAGS() & 0xFF) << 8);")
            return
        if mn == "xchg":
            a, b = ops
            e("{ uint32_t t1 = %s, t2 = %s; %s %s }" % (self.rd(i, a), self.rd(i, b), self.wr(i, a, "t2"), self.wr(i, b, "t1")))
            return
        if mn in ("add", "sub", "cmp", "adc", "sbb"):
            self.arith(i, mn)
            return
        if mn in ("and", "or", "xor", "test"):
            sz = ops[0].size
            op = {"and": "&", "test": "&", "or": "|", "xor": "^"}[mn]
            e("{ uint32_t r = (%s %s %s) & %s;" % (self.rd(i, ops[0]), op, self.rd(i, ops[1], sz), MASK[sz]))
            self.set_logic("r", sz)
            if mn != "test":
                e(self.wr(i, ops[0], "r"))
            e("}")
            return
        if mn in ("inc", "dec"):
            sz = ops[0].size
            n = sz * 8
            d = "+" if mn == "inc" else "-"
            e("{ uint32_t d = %s, r = (d %s 1) & %s;" % (self.rd(i, ops[0]), d, MASK[sz]))
            if mn == "inc":
                e("OF = r == 0x%Xu;" % (1 << (n - 1)))
            else:
                e("OF = r == 0x%Xu;" % ((1 << (n - 1)) - 1))
            e("ZF = r == 0; SF = (r >> %d) & 1; PF = PARITY(r); fr = r;" % (n - 1))
            e(self.wr(i, ops[0], "r"))
            e("}")
            self.flagsrc = ("zs", sz)
            return
        if mn == "neg":
            sz = ops[0].size
            n = sz * 8
            e("{ uint32_t d = %s, r = (0u - d) & %s; CF = d != 0; OF = r == 0x%Xu; ZF = r == 0; SF = (r >> %d) & 1; PF = PARITY(r);"
              " fd = 0; fs = d; fr = r;" % (self.rd(i, ops[0]), MASK[sz], 1 << (n - 1), n - 1))
            e(self.wr(i, ops[0], "r"))
            e("}")
            self.flagsrc = ("sub", sz)
            return
        if mn == "not":
            e(self.wr(i, ops[0], "~%s" % self.rd(i, ops[0])))
            return
        if mn in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            self.shift(i, mn)
            return
        if mn in ("shld", "shrd"):
            sz = ops[0].size
            if sz != 4:
                raise Unsupported(mn + "16")
            cnt = self.rd(i, ops[2], 1) if len(ops) > 2 else "(uint8_t)ecx"
            e("{ uint32_t c = (%s) & 31; if (c) { uint32_t d = %s, s = %s, r;" % (cnt, self.rd(i, ops[0]), self.rd(i, ops[1])))
            if mn == "shld":
                e("r = (d << c) | (s >> (32 - c)); CF = (d >> (32 - c)) & 1;")
            else:
                e("r = (d >> c) | (s << (32 - c)); CF = (d >> (c - 1)) & 1;")
            e("OF = ((r ^ d) >> 31) & 1; ZF = r == 0; SF = r >> 31; PF = PARITY(r); fr = r; %s } }" % self.wr(i, ops[0], "r"))
            self.flagsrc = None
            return
        if mn == "imul":
            self.imul(i)
            return
        if mn == "mul":
            sz = ops[0].size
            s = self.rd(i, ops[0])
            if sz == 4:
                e("{ uint64_t p = (uint64_t)eax * %s; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); CF = OF = edx != 0; }" % s)
            elif sz == 1:
                e("{ uint32_t p = (uint8_t)eax * (uint32_t)%s; eax = (eax & 0xFFFF0000u) | (p & 0xFFFF); CF = OF = (p >> 8) != 0; }" % s)
            else:
                e("{ uint32_t p = (uint16_t)eax * (uint32_t)%s; eax = (eax & 0xFFFF0000u) | (p & 0xFFFF);"
                  " edx = (edx & 0xFFFF0000u) | (p >> 16); CF = OF = (p >> 16) != 0; }" % s)
            self.flagsrc = None
            return
        if mn in ("div", "idiv"):
            sz = ops[0].size
            s = self.rd(i, ops[0])
            a = i.address
            if sz == 4:
                if mn == "div":
                    e("{ uint32_t s = %s; if (!s) hb_divide_error(0x%Xu); uint64_t n = ((uint64_t)edx << 32) | eax;"
                      " uint64_t q = n / s; if (q >> 32) hb_divide_error(0x%Xu); eax = (uint32_t)q; edx = (uint32_t)(n %% s); }" % (s, a, a))
                else:
                    e("{ int32_t s = (int32_t)%s; if (!s) hb_divide_error(0x%Xu); int64_t n = (int64_t)(((uint64_t)edx << 32) | eax);"
                      " if (s == -1) { eax = (uint32_t)(0u - eax); edx = 0u - edx - (eax != 0); if ((int64_t)(((uint64_t)edx << 32) | eax) != -n) hb_divide_error(0x%Xu); edx = 0; }"
                      " else { int64_t q = n / s; if (q != (int32_t)q) hb_divide_error(0x%Xu); eax = (uint32_t)q; edx = (uint32_t)(int32_t)(n %% s); } }" % (s, a, a, a))
            elif sz == 1:
                if mn == "div":
                    e("{ uint32_t s = %s; if (!s) hb_divide_error(0x%Xu); uint32_t n = (uint16_t)eax, q = n / s;"
                      " if (q > 0xFF) hb_divide_error(0x%Xu); eax = (eax & 0xFFFF0000u) | ((n %% s) << 8) | q; }" % (s, a, a))
                else:
                    e("{ int32_t s = (int8_t)%s; if (!s) hb_divide_error(0x%Xu); int32_t n = (int16_t)eax, q = n / s;"
                      " if (q != (int8_t)q) hb_divide_error(0x%Xu); eax = (eax & 0xFFFF0000u) | (((uint32_t)(n %% s) & 0xFF) << 8) | ((uint32_t)q & 0xFF); }" % (s, a, a))
            else:
                if mn == "div":
                    e("{ uint32_t s = %s; if (!s) hb_divide_error(0x%Xu); uint32_t n = ((edx & 0xFFFF) << 16) | (eax & 0xFFFF), q = n / s;"
                      " if (q > 0xFFFF) hb_divide_error(0x%Xu); eax = (eax & 0xFFFF0000u) | q; edx = (edx & 0xFFFF0000u) | (n %% s); }" % (s, a, a))
                else:
                    e("{ int32_t s = (int16_t)%s; if (!s) hb_divide_error(0x%Xu); int32_t n = (int32_t)(((edx & 0xFFFF) << 16) | (eax & 0xFFFF)), q = n / s;"
                      " if (q != (int16_t)q) hb_divide_error(0x%Xu); eax = (eax & 0xFFFF0000u) | ((uint32_t)q & 0xFFFF); edx = (edx & 0xFFFF0000u) | ((uint32_t)(n %% s) & 0xFFFF); }" % (s, a, a))
            self.flagsrc = None
            return
        if mn == "cdq":
            e("edx = (uint32_t)((int32_t)eax >> 31);")
            return
        if mn == "cwde":
            e("eax = (uint32_t)(int32_t)(int16_t)eax;")
            return
        if mn == "cbw":
            e("eax = (eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)eax;")
            return
        if mn == "cwd":
            e("edx = (edx & 0xFFFF0000u) | (((int16_t)eax < 0) ? 0xFFFFu : 0u);")
            return
        if mn.startswith("set"):
            e(self.wr(i, ops[0], "(%s) ? 1u : 0u" % self.cond(mn[3:])))
            return
        if mn.startswith("cmov"):
            e("if (%s) { %s }" % (self.cond(mn[4:]), self.wr(i, ops[0], self.rd(i, ops[1]))))
            return
        if mn in ("bt", "bts", "btr", "btc"):
            self.bt(i, mn)
            return
        if mn in ("bsf", "bsr"):
            s = self.rd(i, ops[1])
            if mn == "bsf":
                r = "(uint32_t)__builtin_ctz(s)"
            else:
                r = "(uint32_t)(31 - __builtin_clz(s))"
            e("{ uint32_t s = %s; ZF = s == 0; if (s) { %s } }" % (s, self.wr(i, ops[0], r)))
            self.flagsrc = None
            return
        if mn == "bswap":
            r = i.reg_name(ops[0].reg)
            e("%s = __builtin_bswap32(%s);" % (r, r))
            return
        if mn == "xadd":
            sz = ops[0].size
            e("{ uint32_t d = %s, s = %s, r = (d + s) & %s; CF = r < d; ZF = r == 0; SF = (r >> %d) & 1; PF = PARITY(r);"
              " OF = (((d ^ r) & (s ^ r)) >> %d) & 1; %s %s }" % (self.rd(i, ops[0]), self.rd(i, ops[1]), MASK[sz], sz * 8 - 1,
                                                                 sz * 8 - 1, self.wr(i, ops[1], "d"), self.wr(i, ops[0], "r")))
            self.flagsrc = None
            return
        if mn == "cmpxchg":
            sz = ops[0].size
            acc = {4: "eax", 2: "(uint16_t)eax", 1: "(uint8_t)eax"}[sz]
            accw = {4: "eax", 2: "ax", 1: "al"}[sz]
            e("{ uint32_t d = %s; ZF = d == %s; if (ZF) { %s } else { %s } }" % (
                self.rd(i, ops[0]), acc, self.wr(i, ops[0], self.rd(i, ops[1])), self.write_reg(accw, "d")))
            self.flagsrc = None
            return
        if mn in ("clc", "stc", "cmc"):
            e({"clc": "CF = 0;", "stc": "CF = 1;", "cmc": "CF = !CF;"}[mn])
            self.flagsrc = None
            return
        if mn == "cld":
            e("cpu.df = 0;")
            return
        if mn == "std":
            e("cpu.df = 1;")
            return
        if mn == "leave":
            e("esp = ebp; ebp = R32(esp); esp += 4;")
            return
        if mn == "enter":
            n = ops[0].imm
            if ops[1].imm != 0:
                raise Unsupported("enter nested")
            e("esp -= 4; W32(esp, ebp); ebp = esp; esp -= %du;" % n)
            return
        if mn == "cpuid":
            e("hb_cpuid(&eax, &ebx, &ecx, &edx);")
            return
        if mn == "rdtsc":
            e("{ uint64_t t = hb_rdtsc(); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); }")
            return
        if self.string_op(i):
            return
        # control flow
        if mn == "jmp":
            t = imm_target(i)
            if t is not None:
                e(self.goto(t))
                return
            v = self.rd(i, ops[0])
            if i.address in self.f.switch:
                e("{ uint32_t t = %s; switch (t) {" % v)
                for t in sorted(self.f.switch[i.address]):
                    e("  case 0x%Xu: goto L_%X;" % (t, t))
                e("  default: SPILL(); hb_jmp(t); return; } }")
            else:
                e("{ uint32_t t = %s; SPILL(); hb_jmp(t); return; }" % v)
            return
        if is_jcc(i) and mn not in ("jecxz", "jcxz"):
            t = imm_target(i)
            e("if (%s) %s" % (self.cond(mn[1:]), self.goto(t)))
            return
        if mn in ("jecxz", "jcxz"):
            t = imm_target(i)
            e("if (%s == 0) %s" % ("ecx" if mn == "jecxz" else "(uint16_t)ecx", self.goto(t)))
            return
        if mn in ("loop", "loope", "loopne"):
            t = imm_target(i)
            c = {"loop": "", "loope": " && ZF", "loopne": " && !ZF"}[mn]
            e("ecx--; if (ecx != 0%s) %s" % (c, self.goto(t)))
            return
        if mn == "call":
            t = imm_target(i)
            if t is not None:
                self.call_direct(t, nxt)
            else:
                e("{ uint32_t t = %s; esp -= 4; W32(esp, 0x%Xu); SPILL(); hb_call(t); RELOAD(); CHECKRET(0x%Xu); }" % (
                    self.rd(i, ops[0]), nxt, nxt))
            self.flagsrc = None
            return
        if mn == "ret":
            n = ops[0].imm if ops else 0
            e("cpu.ret_to = R32(esp); esp += %d; SPILL(); return;" % (4 + n))
            return
        if mn in ("hlt", "int3", "ud2", "int"):
            e("SPILL(); hb_trap(0x%Xu); return;" % i.address)
            return
        raise Unsupported(mn)

    def opcode_byte(self, i):
        for b in i.bytes:
            if b in (0x66, 0x67, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x9B, 0xF0):
                continue
            return b
        return 0

    def arith(self, i, mn):
        ops = i.operands
        sz = ops[0].size
        n = sz * 8
        e = self.emit
        d, s = self.rd(i, ops[0]), self.rd(i, ops[1], sz)
        if mn == "add":
            e("{ uint32_t d = %s, s = %s, r = (d + s) & %s; CF = r < d; OF = (((d ^ r) & (s ^ r)) >> %d) & 1;" % (d, s, MASK[sz], n - 1))
        elif mn in ("sub", "cmp"):
            e("{ uint32_t d = %s, s = %s, r = (d - s) & %s; CF = d < s; OF = (((d ^ s) & (d ^ r)) >> %d) & 1;" % (d, s, MASK[sz], n - 1))
        elif mn == "adc":
            e("{ uint32_t d = %s, s = %s; uint64_t t = (uint64_t)d + s + CF; uint32_t r = (uint32_t)t & %s; CF = t > %s;"
              " OF = (((d ^ r) & (s ^ r)) >> %d) & 1;" % (d, s, MASK[sz], MASK[sz], n - 1))
        else:  # sbb
            e("{ uint32_t d = %s, s = %s; uint32_t r = (d - s - CF) & %s; CF = (uint64_t)d < (uint64_t)s + CF;"
              " OF = (((d ^ s) & (d ^ r)) >> %d) & 1;" % (d, s, MASK[sz], n - 1))
        e("ZF = r == 0; SF = (r >> %d) & 1; PF = PARITY(r); fd = d; fs = s; fr = r;" % (n - 1))
        if mn != "cmp":
            e(self.wr(i, ops[0], "r"))
        e("}")
        self.flagsrc = ("sub", sz) if mn in ("sub", "cmp") else None

    def shift(self, i, mn):
        ops = i.operands
        sz = ops[0].size
        n = sz * 8
        e = self.emit
        cnt = self.rd(i, ops[1], 1) if len(ops) > 1 else "1u"
        d = self.rd(i, ops[0])
        e("{ uint32_t c = (%s) & 31; if (c) { uint32_t d = %s, r;" % (cnt, d))
        if mn in ("shl", "sal"):
            e("r = (uint32_t)(((uint64_t)d << c) & %s); CF = (uint32_t)(((uint64_t)d << c) >> %d) & 1; OF = ((r >> %d) & 1) ^ CF;" % (MASK[sz], n, n - 1))
        elif mn == "shr":
            e("r = (uint32_t)(((uint64_t)d) >> c); CF = (uint32_t)((((uint64_t)d) >> (c - 1)) & 1); OF = (d >> %d) & 1;" % (n - 1))
        elif mn == "sar":
            e("int64_t sd = (%s)d; r = (uint32_t)(sd >> (c > %d ? %d : c)) & %s; CF = (uint32_t)((sd >> (c - 1 > %d ? %d : c - 1)) & 1); OF = 0;" % (
                STYPE[sz], n - 1, n - 1, MASK[sz], n - 1, n - 1))
        elif mn in ("rol", "ror"):
            e("uint32_t k = c %% %d;" % n)
            if mn == "rol":
                e("r = k ? ((d << k) | (d >> (%d - k))) & %s : d; CF = r & 1; OF = ((r >> %d) & 1) ^ CF;" % (n, MASK[sz], n - 1))
            else:
                e("r = k ? ((d >> k) | (d << (%d - k))) & %s : d; CF = (r >> %d) & 1; OF = ((r >> %d) ^ (r >> %d)) & 1;" % (n, MASK[sz], n - 1, n - 1, n - 2))
            e("%s } }" % self.wr(i, ops[0], "r"))
            self.flagsrc = None
            return
        else:  # rcl / rcr: bit by bit
            if mn == "rcl":
                e("r = d; for (uint32_t k = 0; k < c %% %d; k++) { uint32_t o = (r >> %d) & 1; r = ((r << 1) | CF) & %s; CF = o; } OF = ((r >> %d) & 1) ^ CF;" % (n + 1, n - 1, MASK[sz], n - 1))
            else:
                e("r = d; for (uint32_t k = 0; k < c %% %d; k++) { uint32_t o = r & 1; r = (r >> 1) | (CF << %d); CF = o; } OF = ((r >> %d) ^ (r >> %d)) & 1;" % (n + 1, n - 1, n - 1, n - 2))
            e("%s } }" % self.wr(i, ops[0], "r"))
            self.flagsrc = None
            return
        e("ZF = r == 0; SF = (r >> %d) & 1; PF = PARITY(r); fr = r; %s } }" % (n - 1, self.wr(i, ops[0], "r")))
        self.flagsrc = None

    def imul(self, i):
        ops = i.operands
        e = self.emit
        if len(ops) == 1:
            sz = ops[0].size
            s = self.rd(i, ops[0])
            if sz == 4:
                e("{ int64_t p = (int64_t)(int32_t)eax * (int32_t)%s; eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32); CF = OF = p != (int32_t)p; }" % s)
            elif sz == 1:
                e("{ int32_t p = (int8_t)eax * (int8_t)%s; eax = (eax & 0xFFFF0000u) | ((uint32_t)p & 0xFFFF); CF = OF = p != (int8_t)p; }" % s)
            else:
                e("{ int32_t p = (int16_t)eax * (int16_t)%s; eax = (eax & 0xFFFF0000u) | ((uint32_t)p & 0xFFFF);"
                  " edx = (edx & 0xFFFF0000u) | (((uint32_t)p >> 16) & 0xFFFF); CF = OF = p != (int16_t)p; }" % s)
        else:
            sz = ops[0].size
            if len(ops) == 2:
                a, b = self.rd(i, ops[0]), self.rd(i, ops[1], sz)
            else:
                a, b = self.rd(i, ops[1]), self.rd(i, ops[2], sz)
            st = STYPE[sz]
            e("{ int64_t p = (int64_t)(%s)%s * (int64_t)(%s)%s; CF = OF = p != (%s)p; %s }" % (
                st, a, st, b, st, self.wr(i, ops[0], "(uint32_t)p")))
        self.flagsrc = None

    def bt(self, i, mn):
        ops = i.operands
        e = self.emit
        sz = ops[0].size
        if ops[0].type == X86_OP_MEM and ops[1].type == X86_OP_REG:
            off = self.rd(i, ops[1])
            e("{ int32_t o = (int32_t)%s; uint32_t a = %s + (uint32_t)((o >> 5) * 4); uint32_t b = (uint32_t)o & 31, v = R32(a); CF = (v >> b) & 1;" % (off, self.addr(i, ops[0])))
            w = "W32(a, %s);"
        else:
            off = self.rd(i, ops[1], 1)
            e("{ uint32_t b = (%s) & %d, v = %s; CF = (v >> b) & 1;" % (off, sz * 8 - 1, self.rd(i, ops[0])))
            w = None
        if mn != "bt":
            nv = {"bts": "v | (1u << b)", "btr": "v & ~(1u << b)", "btc": "v ^ (1u << b)"}[mn]
            if w:
                e(w % nv)
            else:
                e(self.wr(i, ops[0], nv))
        e("}")
        self.flagsrc = None

    def string_op(self, i):
        mn = i.mnemonic
        parts = mn.split()
        rep = parts[0] if len(parts) == 2 else None
        op = parts[-1]
        names = {"movsb": ("movs", 1), "movsw": ("movs", 2), "movsd": ("movs", 4), "stosb": ("stos", 1),
                 "stosw": ("stos", 2), "stosd": ("stos", 4), "lodsb": ("lods", 1), "lodsw": ("lods", 2),
                 "lodsd": ("lods", 4), "cmpsb": ("cmps", 1), "cmpsw": ("cmps", 2), "cmpsd": ("cmps", 4),
                 "scasb": ("scas", 1), "scasw": ("scas", 2), "scasd": ("scas", 4)}
        if op not in names:
            return False
        b = self.opcode_byte(i)
        if b == 0x0F:  # SSE movsd / cmpsd
            raise Unsupported("sse " + mn)
        kind, sz = names[op]
        if rep in ("rep", "repe", "repz") and kind in ("movs", "stos", "lods"):
            rep = "rep"
        self.emit("STR_%s%s(%d);" % (kind.upper(), "_" + rep.upper().replace("REPZ", "REPE").replace("REPNZ", "REPNE") if rep else "", sz))
        if kind in ("cmps", "scas"):
            self.flagsrc = ("sub", sz)
        return True

    # x87, decoded from the opcode bytes
    def fpu(self, i):
        e = self.emit
        bs = list(i.bytes)
        k = 0
        while bs[k] in (0x66, 0x67, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x9B, 0xF0, 0xF2, 0xF3):
            k += 1
        op, modrm = bs[k], bs[k + 1]
        reg, rm = (modrm >> 3) & 7, modrm & 7
        if modrm < 0xC0:
            mops = [o for o in i.operands if o.type == X86_OP_MEM]
            a = self.addr(i, mops[0])
            arith = ["FADD", "FMUL", "FCOM", "FCOMP", "FSUB", "FSUBR", "FDIV", "FDIVR"]
            if op in (0xD8, 0xDC, 0xDA, 0xDE):
                src = {0xD8: "(double)RF32(a)", 0xDC: "RF64(a)", 0xDA: "(double)(int32_t)R32(a)", 0xDE: "(double)(int16_t)R16(a)"}[op]
                e("{ uint32_t a = %s; FPU_%s_ST0(%s); }" % (a, arith[reg], src))
                return
            tbl = {
                (0xD9, 0): "FPU_PUSH((double)RF32(a));",
                (0xD9, 2): "WF32(a, (float)ST(0));",
                (0xD9, 3): "WF32(a, (float)ST(0)); FPU_POP();",
                (0xD9, 4): "fpu_ldenv(M, a);",
                (0xD9, 5): "fpu.cw = R16(a);",
                (0xD9, 6): "fpu_stenv(M, a);",
                (0xD9, 7): "W16(a, fpu.cw);",
                (0xDB, 0): "FPU_PUSH((double)(int32_t)R32(a));",
                (0xDB, 1): "W32(a, (uint32_t)fpu_trunc32(ST(0))); FPU_POP();",
                (0xDB, 2): "W32(a, (uint32_t)fpu_int32(ST(0)));",
                (0xDB, 3): "W32(a, (uint32_t)fpu_int32(ST(0))); FPU_POP();",
                (0xDB, 5): "FPU_PUSH(fpu_ld80(M, a));",
                (0xDB, 7): "fpu_st80(M, a, ST(0)); FPU_POP();",
                (0xDD, 0): "FPU_PUSH(RF64(a));",
                (0xDD, 1): "W64(a, (uint64_t)fpu_trunc64(ST(0))); FPU_POP();",
                (0xDD, 2): "WF64(a, ST(0));",
                (0xDD, 3): "WF64(a, ST(0)); FPU_POP();",
                (0xDD, 4): "fpu_rstor(M, a);",
                (0xDD, 6): "fpu_save(M, a);",
                (0xDD, 7): "W16(a, FPU_SW());",
                (0xDF, 0): "FPU_PUSH((double)(int16_t)R16(a));",
                (0xDF, 1): "W16(a, (uint16_t)fpu_trunc16(ST(0))); FPU_POP();",
                (0xDF, 2): "W16(a, (uint16_t)fpu_int16(ST(0)));",
                (0xDF, 3): "W16(a, (uint16_t)fpu_int16(ST(0))); FPU_POP();",
                (0xDF, 5): "FPU_PUSH((double)(int64_t)R64(a));",
                (0xDF, 7): "W64(a, (uint64_t)fpu_int64(ST(0))); FPU_POP();",
            }
            if (op, reg) not in tbl:
                raise Unsupported("x87 %02X /%d" % (op, reg))
            e("{ uint32_t a = %s; %s }" % (a, tbl[(op, reg)]))
            return
        j = rm
        if op == 0xD8:
            e(["ST(0) = ST(0) + ST(%d);", "ST(0) = ST(0) * ST(%d);", "FPU_COM(%d);", "FPU_COM(%d); FPU_POP();",
               "ST(0) = ST(0) - ST(%d);", "ST(0) = ST(%d) - ST(0);", "ST(0) = ST(0) / ST(%d);", "ST(0) = ST(%d) / ST(0);"][reg] % j)
            return
        if op == 0xDC:
            e(["ST(%d) = ST(%d) + ST(0);", "ST(%d) = ST(%d) * ST(0);", "FPU_COM(%d);%.0s", "FPU_COM(%d); FPU_POP();%.0s",
               "ST(%d) = ST(0) - ST(%d);", "ST(%d) = ST(%d) - ST(0);", "ST(%d) = ST(0) / ST(%d);", "ST(%d) = ST(%d) / ST(0);"][reg] % (j, j))
            return
        if op == 0xDE:
            if modrm == 0xD9:
                e("FPU_COM(1); FPU_POP(); FPU_POP();")
                return
            f = ["ST(%d) = ST(%d) + ST(0);", "ST(%d) = ST(%d) * ST(0);", None, None,
                 "ST(%d) = ST(0) - ST(%d);", "ST(%d) = ST(%d) - ST(0);", "ST(%d) = ST(0) / ST(%d);", "ST(%d) = ST(%d) / ST(0);"][reg]
            if f is None:
                raise Unsupported("x87 DE %02X" % modrm)
            e((f % (j, j)) + " FPU_POP();")
            return
        if op == 0xD9:
            if reg == 0:
                e("{ double v = ST(%d); FPU_PUSH(v); }" % j)
                return
            if reg == 1:
                e("{ double t = ST(0); ST(0) = ST(%d); ST(%d) = t; }" % (j, j))
                return
            m = {0xD0: "", 0xE0: "ST(0) = -ST(0);", 0xE1: "ST(0) = fabs(ST(0));", 0xE4: "FPU_TST();", 0xE5: "fpu_xam();",
                 0xE8: "FPU_PUSH(1.0);", 0xE9: "FPU_PUSH(3.321928094887362347870319429489390175864831393);",
                 0xEA: "FPU_PUSH(1.442695040888963407359924681001892137426645954);",
                 0xEB: "FPU_PUSH(3.141592653589793238462643383279502884197169399);",
                 0xEC: "FPU_PUSH(0.301029995663981195213738894724493026768189881);",
                 0xED: "FPU_PUSH(0.693147180559945309417232121458176568075500134);", 0xEE: "FPU_PUSH(0.0);",
                 0xF0: "ST(0) = exp2(ST(0)) - 1.0;", 0xF1: "ST(1) = ST(1) * log2(ST(0)); FPU_POP();",
                 0xF2: "ST(0) = tan(ST(0)); FPU_PUSH(1.0); FPU_C2(0);", 0xF3: "ST(1) = atan2(ST(1), ST(0)); FPU_POP();",
                 0xF4: "{ int ex; double m = frexp(ST(0), &ex); ST(0) = (double)(ex - 1); FPU_PUSH(m * 2.0); }",
                 0xF5: "ST(0) = remainder(ST(0), ST(1)); FPU_C2(0);", 0xF6: "fpu.top = (fpu.top - 1) & 7;",
                 0xF7: "fpu.top = (fpu.top + 1) & 7;", 0xF8: "ST(0) = fmod(ST(0), ST(1)); FPU_C2(0);",
                 0xF9: "ST(1) = ST(1) * log2(ST(0) + 1.0); FPU_POP();", 0xFA: "ST(0) = sqrt(ST(0));",
                 0xFB: "{ double v = ST(0); ST(0) = sin(v); FPU_PUSH(cos(v)); FPU_C2(0); }", 0xFC: "ST(0) = fpu_round(ST(0));",
                 0xFD: "ST(0) = ldexp(ST(0), (int)trunc(ST(1)));", 0xFE: "ST(0) = sin(ST(0)); FPU_C2(0);",
                 0xFF: "ST(0) = cos(ST(0)); FPU_C2(0);"}
            if modrm in m:
                e(m[modrm])
                return
        if op == 0xDA and modrm == 0xE9:
            e("FPU_COM(1); FPU_POP(); FPU_POP();")
            return
        if op == 0xDA and reg in (0, 1, 2, 3):
            c = ["CF", "ZF", "(CF | ZF)", "PF"][reg]
            e("if (%s) ST(0) = ST(%d);" % (c, j))
            return
        if op == 0xDB:
            if reg in (0, 1, 2, 3):
                c = ["!CF", "!ZF", "!(CF | ZF)", "!PF"][reg]
                e("if (%s) ST(0) = ST(%d);" % (c, j))
                return
            if modrm == 0xE2:
                e("fpu.sw &= 0x7F00;")
                return
            if modrm == 0xE3:
                e("fpu_init();")
                return
            if reg in (5, 6):
                e("FPU_COMI(%d);" % j)
                self.flagsrc = None
                return
        if op == 0xDD:
            if reg == 0:
                return  # ffree
            if reg == 2:
                e("ST(%d) = ST(0);" % j)
                return
            if reg == 3:
                e("ST(%d) = ST(0); FPU_POP();" % j)
                return
            if reg == 4:
                e("FPU_COM(%d);" % j)
                return
            if reg == 5:
                e("FPU_COM(%d); FPU_POP();" % j)
                return
        if op == 0xDF:
            if modrm == 0xE0:
                e("eax = (eax & 0xFFFF0000u) | FPU_SW();")
                return
            if reg in (5, 6):
                e("FPU_COMI(%d); FPU_POP();" % j)
                self.flagsrc = None
                return
        raise Unsupported("x87 %02X %02X" % (op, modrm))

    def function(self):
        f = self.f
        name = "F_%X" % f.entry
        out = self.out
        out.append("void %s(void) {" % name)
        out.append("    uint8_t* const M = g_mem;")
        out.append("    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;")
        out.append("    uint32_t CF = 0, ZF = 0, SF = 0, OF = 0, PF = 0, fd = 0, fs = 0, fr = 0;")
        out.append("    (void)CF; (void)ZF; (void)SF; (void)OF; (void)PF; (void)fd; (void)fs; (void)fr;")
        out.append("    RELOAD();")
        head = len(out)
        addrs = sorted(f.insns)
        if addrs[0] != f.entry:
            out.append("    goto L_%X;" % f.entry)
        prev_next = None
        for a in addrs:
            i = f.insns[a]
            if prev_next is not None and prev_next != a:
                out.append("    " + self.goto(prev_next))
                if prev_next not in f.labels and prev_next in f.insns:
                    raise RuntimeError("missing label")
            if a in f.labels:
                out.append("L_%X:" % a)
                self.flagsrc = None
            out.append("    /* %X: %s %s */" % (a, i.mnemonic, i.op_str.replace("*/", "* /")))
            try:
                self.insn(i)
            except Unsupported as ex:
                self.unsupported[str(ex)] += 1
                out.append("    SPILL(); hb_unimpl(0x%Xu, \"%s %s\"); return;" % (a, i.mnemonic, i.op_str))
                self.flagsrc = None
            prev_next = None if ends_flow(i) else a + i.size
            if prev_next is not None and prev_next not in f.insns:
                pass
        if prev_next is not None:
            out.append("    " + self.goto(prev_next))
        out.append("}")
        if self.uses_setjmp:
            out.insert(head, "    hb_sjrec sjr __attribute__((cleanup(hb_sj_unlink))); sjr.linked = 0;")
        return out


def fix_fallthrough_labels(funcs):
    """Instructions reached by falling through a gap need labels too."""
    for f in funcs.values():
        addrs = sorted(f.insns)
        prev = None
        for a in addrs:
            if prev is not None:
                i = f.insns[prev]
                nxt = prev + i.size
                if not ends_flow(i) and nxt != a:
                    f.labels.add(nxt)
            prev = a


def find_setjmp():
    # mov edx,[esp+4]; mov [edx],ebp; mov [edx+4],ebx; mov [edx+8],edi; mov [edx+0Ch],esi; mov [edx+10h],esp
    pat = bytes.fromhex("8b542404892a895a04897a0889720c896210")
    m = bytes(img.mem)
    k = m.find(pat)
    while k >= 0:
        SETJMP.add(BASE + k)
        k = m.find(pat, k + 1)
    print("setjmp at", " ".join("%X" % a for a in sorted(SETJMP)))


def main():
    global img
    exe, outdir = sys.argv[1], sys.argv[2]
    cov = sys.argv[3:]
    img = Image(exe)
    find_setjmp()
    funcs = discover(cov)
    fix_fallthrough_labels(funcs)
    os.makedirs(outdir, exist_ok=True)
    entries = sorted(funcs)
    unsupported = collections.Counter()
    hdr = ["/* generated by recomp/recomp.py */", "#pragma once"]
    hdr += ["void F_%X(void);" % e for e in entries]
    open(os.path.join(outdir, "gen_funcs.h"), "w").write("\n".join(hdr) + "\n")
    per_file = 400
    nfiles = 0
    for k in range(0, len(entries), per_file):
        lines = ['#include "recomp_rt.h"', '#include "gen_funcs.h"', ""]
        for e in entries[k:k + per_file]:
            g = Gen(funcs[e], funcs)
            lines += g.function()
            lines.append("")
            unsupported.update(g.unsupported)
        open(os.path.join(outdir, "gen_%03d.c" % nfiles), "w").write("\n".join(lines))
        nfiles += 1
    tab = ['#include "recomp_rt.h"', '#include "gen_funcs.h"', "const hb_func_entry hb_func_table[] = {"]
    tab += ["    {0x%Xu, F_%X}," % (e, e) for e in entries]
    tab += ["};", "const unsigned hb_func_count = %d;" % len(entries)]
    open(os.path.join(outdir, "gen_table.c"), "w").write("\n".join(tab) + "\n")
    print("wrote %d files to %s" % (nfiles + 2, outdir))
    if unsupported:
        print("unsupported instructions (emitted as hb_unimpl):")
        for k, v in unsupported.most_common():
            print("  %6d  %s" % (v, k))


if __name__ == "__main__":
    main()
