"""Apply hbwrap's performance changes to a Unicorn 2.1.4 source tree (argv[1])."""
import sys, os
root = sys.argv[1]

def patch(rel, old, new):
    p = os.path.join(root, rel)
    s = open(p).read()
    if new in s:
        return
    if old not in s:
        sys.exit("patch target not found in " + rel)
    open(p, "w").write(s.replace(old, new, 1))

# 1. No exit-request helper call after every guest load/store (the TB-start check remains).
patch("qemu/tcg/tcg-op.c",
      "void check_exit_request(TCGContext *tcg_ctx)\n{\n",
      "void check_exit_request(TCGContext *tcg_ctx)\n{\n    return;  // hbwrap: TB-start check is enough\n")
# 2. The guest never modifies its own code: skip the per-write self-modifying-code slow path.
patch("qemu/include/exec/ram_addr.h",
      "static inline bool cpu_physical_memory_is_clean(ram_addr_t addr)\n{\n    return true;\n}",
      "static inline bool cpu_physical_memory_is_clean(ram_addr_t addr)\n{\n    return false;  // hbwrap\n}")
# 3. Unicorn never flushes the TLB, so it never grows from its default size: start large.
patch("qemu/include/exec/cpu-defs.h",
      "#define CPU_TLB_DYN_DEFAULT_BITS 8",
      "#define CPU_TLB_DYN_DEFAULT_BITS 16  /* hbwrap */")
print("unicorn patched")
