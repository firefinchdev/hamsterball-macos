"""Disassemble hamsterball.exe: disasm.py <exe> <hexaddr> [count]"""
import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import recomp
img = recomp.Image(sys.argv[1])
a = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 20
md = Cs(CS_ARCH_X86, CS_MODE_32)
for i in md.disasm(bytes(img.mem[a - 0x400000:a - 0x400000 + n * 15]), a, n):
    print("%08x  %-24s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))
