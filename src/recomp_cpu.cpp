// Host side of the statically recompiled CPU (HB_RECOMP builds): register state, dispatch of
// calls through pointers (recompiled functions and API thunks), and x87 helpers.
#ifdef HB_RECOMP
#include "emu.h"
#include "recomp_rt.h"
#include <cmath>
#include <SDL.h>

extern "C" {
hb_cpu cpu;
hb_fpu fpu;
}

static const u32 CODE_LO = 0x401000, CODE_HI = 0x4DE000;
static hb_fn* code_table;  // (addr - CODE_LO) -> recompiled function

void recomp_init() {
    code_table = (hb_fn*)calloc(CODE_HI - CODE_LO, sizeof(hb_fn));
    for (unsigned i = 0; i < hb_func_count; i++) code_table[hb_func_table[i].addr - CODE_LO] = hb_func_table[i].fn;
    fpu_init();
    fpu.cw = 0x027F;
    logf("recompiled code: %u functions", hb_func_count);
}

bool recomp_api_thunk(u32 target);  // emu.cpp: runs an API thunk and pops its arguments

static hb_sjrec* sj_top = nullptr;
extern "C" void hb_sj_link(hb_sjrec* r, uint32_t ret) {
    r->ret = ret;
    if (r->linked) return;
    r->prev = sj_top; sj_top = r; r->linked = 1;
}
extern "C" void hb_sj_unlink(hb_sjrec* r) {
    if (!r->linked) return;
    for (hb_sjrec** p = &sj_top; *p; p = &(*p)->prev)
        if (*p == r) { *p = r->prev; break; }
    r->linked = 0;
}
// Guest longjmp finishes with `jmp [buf+14h]` to the instruction after its setjmp call.
static void sj_longjmp(u32 t) {
    for (hb_sjrec* r = sj_top; r; r = r->prev) {
        if (r->ret != t) continue;
        for (hb_sjrec* q = sj_top; q != r; q = q->prev) q->linked = 0;  // frames being skipped
        sj_top = r;
        longjmp(r->jb, 1);
    }
}

extern "C" void hb_call(uint32_t t) {
    if (t >= CODE_LO && t < CODE_HI) {
        if (hb_fn f = code_table[t - CODE_LO]) { f(); return; }
    } else if (recomp_api_thunk(t)) {
        return;
    }
    sj_longjmp(t);
    fatal("call to %08x: not recompiled code (called from %08x)", t, rd32(cpu.esp));
}

#ifdef HB_FPU_CHECK
#include <set>
extern "C" void hb_fpu_check(uint32_t func, int top0) {
    static std::set<u32> seen;
    int d = (top0 - fpu.top) & 7;  // values left on the stack
    if (d != 0 && d != 1 && seen.insert(func).second) logf("fpu: %08x returned with %d value(s) left (top %d -> %d)", func, d, top0, fpu.top);
}
#endif
extern "C" void hb_badret(uint32_t expected, uint32_t got) {
    fatal("returned to %08x, expected %08x", got, expected);
}
extern "C" void hb_unimpl(uint32_t addr, const char* what) { fatal("unsupported instruction at %08x: %s", addr, what); }
extern "C" void hb_trap(uint32_t addr) { fatal("trap instruction at %08x", addr); }
extern "C" void hb_divide_error(uint32_t addr) { fatal("integer divide error at %08x", addr); }

// A plain Pentium Pro: x87 only, so the game and D3DX take their non-SIMD code paths.
extern "C" void hb_cpuid(uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    u32 leaf = *a;
    if (leaf == 0) { *a = 1; *b = 0x756E6547; *d = 0x49656E69; *c = 0x6C65746E; }  // GenuineIntel
    else if (leaf == 1) { *a = 0x00000611; *b = 0; *c = 0; *d = 0x00008111; }      // FPU, TSC, CX8, CMOV
    else { *a = 0; *b = 0; *c = 0; *d = 0; }
}
extern "C" uint64_t hb_rdtsc(void) { return SDL_GetPerformanceCounter(); }

// ---- x87 --------------------------------------------------------------------------------
static double round_cw(double v) {
    switch ((fpu.cw >> 10) & 3) {
        case 0: return nearbyint(v);  // the host rounds to nearest-even by default
        case 1: return floor(v);
        case 2: return ceil(v);
        default: return trunc(v);
    }
}
extern "C" double fpu_round(double v) { return round_cw(v); }
extern "C" int32_t fpu_int32(double v) {
    double r = round_cw(v);
    return (r >= -2147483648.0 && r < 2147483648.0) ? (int32_t)r : INT32_MIN;
}
extern "C" int16_t fpu_int16(double v) {
    double r = round_cw(v);
    return (r >= -32768.0 && r < 32768.0) ? (int16_t)r : INT16_MIN;
}
extern "C" int64_t fpu_int64(double v) {
    double r = round_cw(v);
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (int64_t)r : INT64_MIN;
}
extern "C" int32_t fpu_trunc32(double v) { double r = trunc(v); return (r >= -2147483648.0 && r < 2147483648.0) ? (int32_t)r : INT32_MIN; }
extern "C" int16_t fpu_trunc16(double v) { double r = trunc(v); return (r >= -32768.0 && r < 32768.0) ? (int16_t)r : INT16_MIN; }
extern "C" int64_t fpu_trunc64(double v) {
    double r = trunc(v);
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (int64_t)r : INT64_MIN;
}

// 80-bit extended <-> double
extern "C" double fpu_ld80(uint8_t* M, uint32_t a) {
    uint64_t mant; uint16_t se;
    memcpy(&mant, M + a, 8); memcpy(&se, M + a + 8, 2);
    int sign = se >> 15, ex = se & 0x7FFF;
    double v;
    if (ex == 0 && mant == 0) v = 0.0;
    else if (ex == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, ex - 16383 - 63);
    return sign ? -v : v;
}
extern "C" void fpu_st80(uint8_t* M, uint32_t a, double v) {
    uint64_t mant = 0; uint16_t se = 0;
    if (std::signbit(v)) { se = 0x8000; v = -v; }
    if (v == 0.0) {
    } else if (std::isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (std::isnan(v)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else {
        int ex; double m = frexp(v, &ex);  // v = m * 2^ex, 0.5 <= m < 1
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(ex - 1 + 16383);
    }
    memcpy(M + a, &mant, 8); memcpy(M + a + 8, &se, 2);
}
static u16 tag_word() { return 0; }  // all registers reported valid
extern "C" void fpu_stenv(uint8_t* M, uint32_t a) {
    memset(M + a, 0, 28);
    uint32_t sw = FPU_SW();
    memcpy(M + a, &fpu.cw, 2); memcpy(M + a + 4, &sw, 2);
    u16 tw = tag_word(); memcpy(M + a + 8, &tw, 2);
}
extern "C" void fpu_ldenv(uint8_t* M, uint32_t a) {
    uint16_t sw;
    memcpy(&fpu.cw, M + a, 2); memcpy(&sw, M + a + 4, 2);
    fpu.sw = sw; fpu.top = (sw >> 11) & 7;
}
extern "C" void fpu_save(uint8_t* M, uint32_t a) {
    fpu_stenv(M, a);
    for (int i = 0; i < 8; i++) fpu_st80(M, a + 28 + i * 10, ST(i));
    fpu_init();
}
extern "C" void fpu_rstor(uint8_t* M, uint32_t a) {
    fpu_ldenv(M, a);
    for (int i = 0; i < 8; i++) ST(i) = fpu_ld80(M, a + 28 + i * 10);
}
extern "C" void fpu_xam(void) {
    double v = ST(0);
    u16 c;  // C3 C2 C0
    if (std::isnan(v)) c = 0x0100;
    else if (std::isinf(v)) c = 0x0500;
    else if (v == 0.0) c = 0x4000;
    else if (std::fpclassify(v) == FP_SUBNORMAL) c = 0x4400;
    else c = 0x0400;
    if (std::signbit(v)) c |= 0x0200;  // C1
    fpu.sw = (uint16_t)((fpu.sw & ~0x4700) | c);
}
extern "C" void fpu_init(void) {
    fpu.cw = 0x037F; fpu.sw = 0; fpu.top = 0;
    for (double& d : fpu.st) d = 0;
}
#endif
