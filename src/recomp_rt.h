/* Runtime interface for the recompiled game code (build/recomp/gen_*.c, from recomp/recomp.py).
   Shared by the generated C and the host (src/recomp_cpu.cpp). */
#pragma once
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t ret_to;   /* return address popped by the last `ret` */
    uint32_t df;       /* direction flag */
    uint32_t fs_base;  /* TEB */
} hb_cpu;
typedef struct {
    double st[8];
    int top;
    uint16_t cw, sw;
} hb_fpu;
typedef void (*hb_fn)(void);
typedef struct { uint32_t addr; hb_fn fn; } hb_func_entry;

extern hb_cpu cpu;
extern hb_fpu fpu;
#ifndef __cplusplus
extern uint8_t* g_mem;  /* C++ declares it in emu.h */
#endif
extern const hb_func_entry hb_func_table[];
extern const unsigned hb_func_count;

void hb_call(uint32_t target);  /* return address already pushed */
void hb_badret(uint32_t expected, uint32_t got);
void hb_unimpl(uint32_t addr, const char* what);
void hb_trap(uint32_t addr);
void hb_divide_error(uint32_t addr);
void hb_cpuid(uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d);
uint64_t hb_rdtsc(void);
int32_t fpu_int32(double v);
int16_t fpu_int16(double v);
int64_t fpu_int64(double v);
int32_t fpu_trunc32(double v);
int16_t fpu_trunc16(double v);
int64_t fpu_trunc64(double v);
double fpu_round(double v);
double fpu_ld80(uint8_t* M, uint32_t a);
void fpu_st80(uint8_t* M, uint32_t a, double v);
void fpu_save(uint8_t* M, uint32_t a);
void fpu_rstor(uint8_t* M, uint32_t a);
void fpu_stenv(uint8_t* M, uint32_t a);
void fpu_ldenv(uint8_t* M, uint32_t a);
void fpu_xam(void);
void fpu_init(void);

/* guest setjmp/longjmp: each C function calling _setjmp3 keeps a host jmp_buf, linked while it runs */
typedef struct hb_sjrec { jmp_buf jb; uint32_t ret; struct hb_sjrec* prev; int linked; } hb_sjrec;
void hb_sj_link(hb_sjrec* r, uint32_t ret);
void hb_sj_unlink(hb_sjrec* r);

#define hb_jmp hb_call  /* tail jump: the caller's return address is already on the stack */

/* guest memory (M is the function's local copy of g_mem) */
static inline uint8_t hb_r8(const uint8_t* M, uint32_t a) { return M[a]; }
static inline uint16_t hb_r16(const uint8_t* M, uint32_t a) { uint16_t v; memcpy(&v, M + a, 2); return v; }
static inline uint32_t hb_r32(const uint8_t* M, uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline uint64_t hb_r64(const uint8_t* M, uint32_t a) { uint64_t v; memcpy(&v, M + a, 8); return v; }
static inline float hb_rf32(const uint8_t* M, uint32_t a) { float v; memcpy(&v, M + a, 4); return v; }
static inline double hb_rf64(const uint8_t* M, uint32_t a) { double v; memcpy(&v, M + a, 8); return v; }
static inline void hb_w8(uint8_t* M, uint32_t a, uint32_t v) { M[a] = (uint8_t)v; }
static inline void hb_w16(uint8_t* M, uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(M + a, &x, 2); }
static inline void hb_w32(uint8_t* M, uint32_t a, uint32_t v) { memcpy(M + a, &v, 4); }
static inline void hb_w64(uint8_t* M, uint32_t a, uint64_t v) { memcpy(M + a, &v, 8); }
static inline void hb_wf32(uint8_t* M, uint32_t a, float v) { memcpy(M + a, &v, 4); }
static inline void hb_wf64(uint8_t* M, uint32_t a, double v) { memcpy(M + a, &v, 8); }
#define R8(a) hb_r8(M, (a))
#define R16(a) hb_r16(M, (a))
#define R32(a) hb_r32(M, (a))
#define R64(a) hb_r64(M, (a))
#define RF32(a) hb_rf32(M, (a))
#define RF64(a) hb_rf64(M, (a))
#define W8(a, v) hb_w8(M, (a), (v))
#define W16(a, v) hb_w16(M, (a), (v))
#define W32(a, v) hb_w32(M, (a), (v))
#define W64(a, v) hb_w64(M, (a), (v))
#define WF32(a, v) hb_wf32(M, (a), (v))
#define WF64(a, v) hb_wf64(M, (a), (v))
#define FS_BASE cpu.fs_base

#define PARITY(r) ((uint32_t)!__builtin_parity((r) & 0xFFu))
#define SPILL() (cpu.eax = eax, cpu.ecx = ecx, cpu.edx = edx, cpu.ebx = ebx, cpu.esp = esp, cpu.ebp = ebp, cpu.esi = esi, cpu.edi = edi)
#define RELOAD() (eax = cpu.eax, ecx = cpu.ecx, edx = cpu.edx, ebx = cpu.ebx, esp = cpu.esp, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi)
#define CHECKRET(n) do { if (__builtin_expect(cpu.ret_to != (n), 0)) hb_badret((n), cpu.ret_to); } while (0)
#define MAKE_EFLAGS() (CF | (PF << 2) | (ZF << 6) | (SF << 7) | (cpu.df << 10) | (OF << 11) | 0x202u)
#define SET_EFLAGS(v) (CF = (v) & 1, PF = ((v) >> 2) & 1, ZF = ((v) >> 6) & 1, SF = ((v) >> 7) & 1, cpu.df = ((v) >> 10) & 1, OF = ((v) >> 11) & 1)

/* string instructions */
#define STR_STEP(sz) (cpu.df ? (uint32_t)-(sz) : (uint32_t)(sz))
#define STR_LOAD(sz, a) ((sz) == 1 ? (uint32_t)R8(a) : (sz) == 2 ? (uint32_t)R16(a) : R32(a))
#define STR_STORE(sz, a, v) do { if ((sz) == 1) W8(a, v); else if ((sz) == 2) W16(a, v); else W32(a, v); } while (0)
#define STR_MOVS(sz) do { STR_STORE(sz, edi, STR_LOAD(sz, esi)); esi += STR_STEP(sz); edi += STR_STEP(sz); } while (0)
#define STR_MOVS_REP(sz) do { \
    if (!cpu.df && (edi <= esi || edi >= esi + ecx * (sz))) { memmove(M + edi, M + esi, (size_t)ecx * (sz)); esi += ecx * (sz); edi += ecx * (sz); ecx = 0; } \
    else while (ecx) { STR_MOVS(sz); ecx--; } } while (0)
#define STR_STOS(sz) do { STR_STORE(sz, edi, eax); edi += STR_STEP(sz); } while (0)
#define STR_STOS_REP(sz) do { \
    if (!cpu.df && ((sz) == 1 || ((sz) == 4 && (eax & 0xFFu) * 0x01010101u == eax))) { \
        memset(M + edi, (int)(eax & 0xFF), (size_t)ecx * (sz)); edi += ecx * (sz); ecx = 0; } \
    else while (ecx) { STR_STOS(sz); ecx--; } } while (0)
#define STR_LODS(sz) do { uint32_t v_ = STR_LOAD(sz, esi); if ((sz) == 4) eax = v_; else if ((sz) == 2) eax = (eax & 0xFFFF0000u) | v_; else eax = (eax & 0xFFFFFF00u) | v_; esi += STR_STEP(sz); } while (0)
#define STR_LODS_REP(sz) do { while (ecx) { STR_LODS(sz); ecx--; } } while (0)
#define STR_FLAGS_SUB(sz, d_, s_) do { uint32_t m_ = (sz) == 1 ? 0xFFu : (sz) == 2 ? 0xFFFFu : 0xFFFFFFFFu; \
    fd = (d_) & m_; fs = (s_) & m_; fr = (fd - fs) & m_; CF = fd < fs; ZF = fr == 0; SF = (fr >> ((sz) * 8 - 1)) & 1; \
    OF = (((fd ^ fs) & (fd ^ fr)) >> ((sz) * 8 - 1)) & 1; PF = PARITY(fr); } while (0)
#define STR_CMPS(sz) do { STR_FLAGS_SUB(sz, STR_LOAD(sz, esi), STR_LOAD(sz, edi)); esi += STR_STEP(sz); edi += STR_STEP(sz); } while (0)
#define STR_CMPS_REPE(sz) do { while (ecx) { STR_CMPS(sz); ecx--; if (!ZF) break; } } while (0)
#define STR_CMPS_REPNE(sz) do { while (ecx) { STR_CMPS(sz); ecx--; if (ZF) break; } } while (0)
#define STR_SCAS(sz) do { STR_FLAGS_SUB(sz, eax, STR_LOAD(sz, edi)); edi += STR_STEP(sz); } while (0)
#define STR_SCAS_REPE(sz) do { while (ecx) { STR_SCAS(sz); ecx--; if (!ZF) break; } } while (0)
#define STR_SCAS_REPNE(sz) do { while (ecx) { STR_SCAS(sz); ecx--; if (ZF) break; } } while (0)

/* x87: values are kept as doubles */
#define ST(i) fpu.st[(fpu.top + (i)) & 7]
#define FPU_PUSH(v) do { double v_ = (v); fpu.top = (fpu.top - 1) & 7; fpu.st[fpu.top] = v_; } while (0)
#define FPU_POP() (fpu.top = (fpu.top + 1) & 7)
#define FPU_SW() ((uint32_t)((fpu.sw & ~0x3800u) | ((uint32_t)fpu.top << 11)) & 0xFFFFu)
#define FPU_C2(v) (fpu.sw = (uint16_t)((fpu.sw & ~0x400u) | ((v) ? 0x400u : 0u)))
static inline void fpu_cmp(double a, double b) {
    uint16_t c = (a != a || b != b) ? 0x4500 : a < b ? 0x0100 : a == b ? 0x4000 : 0;
    fpu.sw = (uint16_t)((fpu.sw & ~0x4500) | c);
}
#define FPU_COM(i) fpu_cmp(ST(0), ST(i))
#define FPU_TST() fpu_cmp(ST(0), 0.0)
#define FPU_COMI(i) do { double a_ = ST(0), b_ = ST(i); OF = SF = 0; \
    if (a_ != a_ || b_ != b_) { ZF = PF = CF = 1; } else { PF = 0; ZF = a_ == b_; CF = a_ < b_; } } while (0)
#define FPU_FADD_ST0(v) (ST(0) = ST(0) + (v))
#define FPU_FMUL_ST0(v) (ST(0) = ST(0) * (v))
#define FPU_FCOM_ST0(v) fpu_cmp(ST(0), (v))
#define FPU_FCOMP_ST0(v) (fpu_cmp(ST(0), (v)), FPU_POP())
#define FPU_FSUB_ST0(v) (ST(0) = ST(0) - (v))
#define FPU_FSUBR_ST0(v) (ST(0) = (v) - ST(0))
#define FPU_FDIV_ST0(v) (ST(0) = ST(0) / (v))
#define FPU_FDIVR_ST0(v) (ST(0) = (v) / ST(0))

#ifdef __cplusplus
}
#endif
