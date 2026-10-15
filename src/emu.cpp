#include "emu.h"
#include <cstdarg>
#include <cstdlib>
#include <sys/mman.h>
#include <pthread.h>
#include <SDL.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <unordered_map>
#include <set>
#include <algorithm>

uc_engine* g_uc;
u8* g_mem;
bool g_trace_api = false;
std::string g_game_dir;
u32 g_last_error = 0;

// ---- address space layout --------------------------------------------------
static const u32 MAP_BASE = 0x00010000;
static const u32 MAP_END = 0x80000000;
static const u32 STACK_LO = 0x00100000, STACK_HI = 0x00400000;  // 3MB main stack
static const u32 VALLOC_LO = 0x02000000, VALLOC_HI = 0x20000000;
static const u32 HEAP_LO = 0x20000000, HEAP_HI = 0x70000000;
static const u32 THUNK_BASE = 0x7F000000, THUNK_SIZE = 8, THUNK_MAX = 0x10000;
static const u32 RET_MAGIC = 0x7F7F0000;  // call_guest return target (holds a hlt)
static const u32 EXIT_MAGIC = 0x7F7F0010;
static const u32 TEB_ADDR = 0x7FFDE000, PEB_ADDR = 0x7FFDF000, GDT_ADDR = 0x7FFE0000;
static const u32 TLS_SLOTS_ADDR = 0x7FFDD000;  // 64 TLS slots (TEB+0xE10 normally)

void logf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
#ifdef __ANDROID__
    va_list ap2; va_copy(ap2, ap);
    __android_log_vprint(ANDROID_LOG_INFO, "Hamsterball", fmt, ap2);
    va_end(ap2);
#endif
    vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}
void dump_regs() {
    if (!g_uc) return;
    u32 r[9]; int ids[9] = {UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI,
                            UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP};
    for (int i = 0; i < 9; i++) uc_reg_read(g_uc, ids[i], &r[i]);
    fprintf(stderr, "eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x eip=%08x\n",
            r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
    fprintf(stderr, "stack:");
    for (int i = 0; i < 16; i++) if (r[7] + i * 4 < MAP_END) fprintf(stderr, " %08x", rd32(r[7] + i * 4));
    fprintf(stderr, "\n");
}
void fatal(const char* fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "FATAL: %s\n", msg);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR, "Hamsterball", "FATAL: %s", msg);
#endif
    dump_regs();
    fflush(stderr);
    if (!getenv("HB_SCRIPT")) {
        extern std::string g_log_path;
        std::string text = std::string("The game stopped because of an emulation error:\n\n") + msg;
        if (!g_log_path.empty()) text += "\n\nDetails were written to " + g_log_path;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Hamsterball", text.c_str(), nullptr);
    }
    exit(1);
}

std::string gwstr(u32 a) {
    std::string out;
    if (!a) return out;
    for (;; a += 2) {
        u16 c = rd16(a);
        if (!c) break;
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}
u32 gstrcpy(u32 dst, const std::string& s, u32 max) {
    if (!dst || !max) return 0;
    u32 n = (u32)s.size();
    if (n > max - 1) n = max - 1;
    memcpy(g_mem + dst, s.data(), n);
    g_mem[dst + n] = 0;
    return n;
}

// ---- guest heap -----------------------------------------------------------
// Small blocks: size classes of 16 bytes up to 4KB with free lists.
// Large blocks: best-fit from a coalescing free map.
static u32 heap_bump = HEAP_LO;
static std::unordered_map<u32, u32> heap_used;  // addr -> size (rounded)
static std::vector<u32> small_free[257];
static std::multimap<u32, u32> large_free_by_size;  // size -> addr
static std::map<u32, u32> large_free_by_addr;       // addr -> size

static void large_free_insert(u32 a, u32 sz) {
    // coalesce with neighbours
    auto next = large_free_by_addr.lower_bound(a);
    if (next != large_free_by_addr.end() && next->first == a + sz) {
        u32 nsz = next->second;
        for (auto it = large_free_by_size.lower_bound(nsz); it != large_free_by_size.end() && it->first == nsz; ++it)
            if (it->second == next->first) { large_free_by_size.erase(it); break; }
        large_free_by_addr.erase(next);
        sz += nsz;
    }
    auto prev = large_free_by_addr.lower_bound(a);
    if (prev != large_free_by_addr.begin()) {
        --prev;
        if (prev->first + prev->second == a) {
            u32 psz = prev->second, pa = prev->first;
            for (auto it = large_free_by_size.lower_bound(psz); it != large_free_by_size.end() && it->first == psz; ++it)
                if (it->second == pa) { large_free_by_size.erase(it); break; }
            large_free_by_addr.erase(prev);
            a = pa; sz += psz;
        }
    }
    large_free_by_addr[a] = sz;
    large_free_by_size.insert({sz, a});
}

u32 galloc(u32 size, bool zero) {
    if (size == 0) size = 1;
    u32 rs = (size + 15) & ~15u;
    u32 a = 0;
    if (rs <= 4096) {
        auto& fl = small_free[rs / 16];
        if (!fl.empty()) { a = fl.back(); fl.pop_back(); }
    } else {
        rs = (rs + 4095) & ~4095u;
        auto it = large_free_by_size.lower_bound(rs);
        if (it != large_free_by_size.end()) {
            a = it->second; u32 bs = it->first;
            large_free_by_size.erase(it);
            large_free_by_addr.erase(a);
            if (bs > rs) large_free_insert(a + rs, bs - rs);
        }
    }
    if (!a) {
        if (heap_bump + rs > HEAP_HI) fatal("guest heap exhausted (%u bytes)", size);
        a = heap_bump; heap_bump += rs;
    }
    heap_used[a] = rs;
    if (zero) memset(g_mem + a, 0, rs);
    return a;
}
void gfree(u32 a) {
    if (!a) return;
    auto it = heap_used.find(a);
    if (it == heap_used.end()) { logf("warning: free of unknown block %08x", a); return; }
    u32 rs = it->second;
    heap_used.erase(it);
    if (rs <= 4096) small_free[rs / 16].push_back(a);
    else large_free_insert(a, rs);
}
u32 gsize(u32 a) {
    auto it = heap_used.find(a);
    return it == heap_used.end() ? 0 : it->second;
}
u32 grealloc(u32 a, u32 size) {
    if (!a) return galloc(size);
    u32 old = gsize(a);
    if (size <= old && size + 4096 > old) return a;
    u32 n = galloc(size, true);
    memcpy(g_mem + n, g_mem + a, old < size ? old : size);
    gfree(a);
    return n;
}
u32 gstrdup(const std::string& s) {
    u32 a = galloc((u32)s.size() + 1);
    memcpy(g_mem + a, s.c_str(), s.size() + 1);
    return a;
}

static u32 valloc_bump = VALLOC_LO;
static std::map<u32, u32> valloc_used;
static std::multimap<u32, u32> valloc_free;  // size -> addr
u32 valloc(u32 size, u32 want) {
    size = (size + 0xFFFF) & ~0xFFFFu;
    if (want) {
        // Accept a specific address if it is inside an existing reservation or a free gap.
        for (auto& kv : valloc_used)
            if (want >= kv.first && want < kv.first + kv.second) return want;
        if (want >= MAP_BASE && want + size < VALLOC_LO) { memset(g_mem + want, 0, size); return want; }
    }
    for (auto it = valloc_free.lower_bound(size); it != valloc_free.end(); ++it) {
        if (it->first == size) {
            u32 a = it->second; valloc_free.erase(it);
            valloc_used[a] = size; memset(g_mem + a, 0, size); return a;
        }
    }
    if (valloc_bump + size > VALLOC_HI) fatal("VirtualAlloc space exhausted");
    u32 a = valloc_bump; valloc_bump += size;
    valloc_used[a] = size;
    memset(g_mem + a, 0, size);
    return a;
}
void vfree(u32 a) {
    auto it = valloc_used.find(a);
    if (it == valloc_used.end()) return;
    valloc_free.insert({it->second, a});
    valloc_used.erase(it);
}

// ---- API registry / thunks -------------------------------------------------
static std::vector<ApiDef>& registry() { static std::vector<ApiDef> r; return r; }
static std::unordered_map<std::string, size_t>& registry_index() { static std::unordered_map<std::string, size_t> m; return m; }
struct Thunk { std::string name; int nargs; ApiFn fn; u64 calls = 0; };
static std::vector<Thunk> thunks;
static std::unordered_map<std::string, u32> thunk_by_name;

static std::string lower(std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; }
static std::string norm_dll(std::string dll) {
    dll = lower(dll);
    if (dll.size() > 4 && dll.substr(dll.size() - 4) == ".dll") dll = dll.substr(0, dll.size() - 4);
    return dll;
}

ApiRegistrar::ApiRegistrar(const char* dll, const char* name, int nargs, ApiFn fn, bool com) {
    std::string key = com ? std::string(dll) + "::" + name : norm_dll(dll) + "!" + name;
    registry_index()[key] = registry().size();
    registry().push_back({key, nargs, fn});
}

static u32 new_thunk(const std::string& key, int nargs, ApiFn fn) {
    auto it = thunk_by_name.find(key);
    if (it != thunk_by_name.end()) return it->second;
    u32 id = (u32)thunks.size();
    if (id >= THUNK_MAX / THUNK_SIZE) fatal("too many thunks");
    thunks.push_back({key, nargs, fn});
    u32 addr = THUNK_BASE + id * THUNK_SIZE;
    // The code hook runs the host implementation and sets EAX/EDX; the thunk itself then
    // returns to the caller, popping the stdcall arguments.
    u8 code[8] = {0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};  // ret
    if (nargs > 0) { code[0] = 0xC2; code[1] = (u8)(nargs * 4); code[2] = (u8)((nargs * 4) >> 8); }  // ret n
    memcpy(g_mem + addr, code, 8);
    thunk_by_name[key] = addr;
    return addr;
}
static u32 thunk_key(const std::string& key) {
    auto it = registry_index().find(key);
    if (it != registry_index().end()) { auto& d = registry()[it->second]; return new_thunk(key, d.nargs, d.fn); }
    return new_thunk(key, 0, nullptr);
}
u32 thunk_for(const std::string& dll, const std::string& name) { return thunk_key(norm_dll(dll) + "!" + name); }
u32 thunk_for_ordinal(const std::string& dll, u32 ord) {
    std::string d = norm_dll(dll);
    if (d == "dsound" && ord == 1) return thunk_for(d, "DirectSoundCreate");
    if (d == "ws2_32" || d == "wsock32") {
        static std::map<u32, const char*> ws = {{3, "closesocket"}, {4, "connect"}, {9, "htons"}, {10, "ioctlsocket"},
                                               {11, "inet_addr"}, {12, "inet_ntoa"}, {16, "recv"}, {19, "send"},
                                               {23, "socket"}, {111, "WSAGetLastError"}, {115, "WSAStartup"}, {116, "WSACleanup"}};
        if (ws.count(ord)) return thunk_for(d, ws[ord]);
    }
    return thunk_key(d + "!#" + std::to_string(ord));
}
bool api_exists(const std::string& dll, const std::string& name) {
    return registry_index().count(norm_dll(dll) + "!" + name) > 0;
}
u32 make_vtable(const char* iface, const std::vector<const char*>& methods) {
    u32 vt = galloc((u32)methods.size() * 4);
    for (size_t i = 0; i < methods.size(); i++) wr32(vt + (u32)i * 4, thunk_key(std::string(iface) + "::" + methods[i]));
    return vt;
}
u32 make_com_object(u32 vtbl, u32 host_id, u32 extra) {
    u32 o = galloc(8 + extra);
    wr32(o, vtbl);
    wr32(o + 4, host_id);
    return o;
}

// ---- modules ----------------------------------------------------------------
static std::map<std::string, u32> modules = {
    {"kernel32", 0x7C800000}, {"user32", 0x7E410000}, {"gdi32", 0x77F10000}, {"advapi32", 0x77DD0000},
    {"shell32", 0x7C9C0000}, {"ole32", 0x774E0000}, {"ws2_32", 0x71AB0000}, {"d3d8", 0x6D9A0000},
    {"dsound", 0x73F10000}, {"dinput8", 0x6D7C0000}, {"bass", 0x10000000}, {"winmm", 0x76B40000},
    {"ntdll", 0x7C900000}, {"msvcrt", 0x77C10000}, {"oleaut32", 0x77120000}, {"version", 0x77C00000},
    {"wsock32", 0x71AD0000}, {"msacm32", 0x77BE0000}, {"comctl32", 0x5D090000}, {"shlwapi", 0x77F60000},
};
u32 module_handle(const std::string& dll) {
    std::string d = norm_dll(dll);
    size_t slash = d.find_last_of("\\/");
    if (slash != std::string::npos) d = d.substr(slash + 1);
    if (d == "hamsterball" || d == "hamsterball.exe") return 0x400000;
    auto it = modules.find(d);
    return it == modules.end() ? 0 : it->second;
}
std::string module_name(u32 h) {
    for (auto& kv : modules) if (kv.second == h) return kv.first;
    return "";
}

// ---- CPU / hooks ------------------------------------------------------------
static int g_call_depth = 0;

static void finish_api(u32 eax, Ctx& c) {
    uc_reg_write(g_uc, UC_X86_REG_EAX, &eax);
    if (c.set_edx) uc_reg_write(g_uc, UC_X86_REG_EDX, &c.edx_out);
}

// API thunks are dispatched from a UC_HOOK_CODE hook limited to the thunk page(s).
// (An interrupt-based dispatch breaks nested uc_emu_start on Apple Silicon: Unicorn's
// JIT W^X bookkeeping gets out of sync when a hook run outside translated code nests.)
static void hook_thunk(uc_engine* uc, u64 addr, u32 size, void*) {
    u32 a = (u32)addr;
    if ((a - THUNK_BASE) % THUNK_SIZE) return;
    u32 id = (a - THUNK_BASE) / THUNK_SIZE;
    if (id >= thunks.size()) return;
    Thunk& t = thunks[id];
    u32 esp;
    uc_reg_read(uc, UC_X86_REG_ESP, &esp);
    u32 ret = rd32(esp);
    if (!t.fn) fatal("unimplemented API %s (called from %08x)", t.name.c_str(), ret);
    t.calls++;
    Ctx c; c.esp = esp;
    if (g_trace_api) logf("[api] %s(%08x, %08x, %08x, %08x) from %08x", t.name.c_str(), c.arg(0), c.arg(1), c.arg(2), c.arg(3), ret);
    u32 r = t.fn(c);
    finish_api(r, c);
}

static void hook_intr(uc_engine* uc, u32 intno, void*) {
    u32 eip;
    uc_reg_read(uc, UC_X86_REG_EIP, &eip);
    fatal("unhandled interrupt %u at eip=%08x", intno, eip);
}

static bool hook_unmapped(uc_engine* uc, uc_mem_type type, u64 addr, int size, i64 value, void*) {
    u32 eip; uc_reg_read(uc, UC_X86_REG_EIP, &eip);
    logf("invalid memory %s at %08llx (size %d, value %llx) eip=%08x",
         type == UC_MEM_READ_UNMAPPED ? "read" : type == UC_MEM_WRITE_UNMAPPED ? "write" : type == UC_MEM_FETCH_UNMAPPED ? "fetch" : "access",
         (unsigned long long)addr, size, (long long)value, eip);
    return false;
}

static void run_until(u32 start, u32 until) {
    uc_err e = uc_emu_start(g_uc, start, until, 0, 0);
    if (e != UC_ERR_OK) {
        fatal("emulation error: %s", uc_strerror(e));
    }
}

u32 call_guest(u32 addr, std::initializer_list<u32> args, bool cdecl_) {
    static const int ids[] = {UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI,
                              UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EFLAGS};
    u32 saved[10];
    for (int i = 0; i < 10; i++) uc_reg_read(g_uc, ids[i], &saved[i]);
    u32 esp = saved[7] - 64;  // leave a red zone below the interrupted frame
    std::vector<u32> a(args);
    for (int i = (int)a.size() - 1; i >= 0; i--) { esp -= 4; wr32(esp, a[i]); }
    esp -= 4; wr32(esp, RET_MAGIC);
    uc_reg_write(g_uc, UC_X86_REG_ESP, &esp);
    g_call_depth++;
    run_until(addr, RET_MAGIC);
    g_call_depth--;

    u32 eax; uc_reg_read(g_uc, UC_X86_REG_EAX, &eax);
    for (int i = 0; i < 10; i++) if (ids[i] != UC_X86_REG_EIP) uc_reg_write(g_uc, ids[i], &saved[i]);
    (void)cdecl_;
    return eax;
}

// ---- GDT / TEB ---------------------------------------------------------------
static u64 make_desc(u32 base, u32 limit, bool code, int dpl) {
    // limit in 4K pages, 32-bit, present
    u64 d = 0;
    limit >>= 12;
    d |= (limit & 0xFFFF);
    d |= (u64)(base & 0xFFFFFF) << 16;
    u64 access = 0x80 | (u64)(dpl << 5) | 0x10 | (code ? 0x0A : 0x02);  // present, S=1, code exec/read or data r/w
    d |= access << 40;
    d |= (u64)((limit >> 16) & 0xF) << 48;
    d |= (u64)0xC << 52;  // G=1, D/B=1
    d |= (u64)((base >> 24) & 0xFF) << 56;
    return d;
}

static void setup_segments() {
    uc_x86_mmr gdtr = {};
    gdtr.base = GDT_ADDR;
    gdtr.limit = 32 * 8 - 1;
    u64* gdt = g2h<u64>(GDT_ADDR);
    gdt[1] = make_desc(0, 0xFFFFF000, true, 3);
    gdt[2] = make_desc(0, 0xFFFFF000, false, 3);
    gdt[3] = make_desc(TEB_ADDR, 0x1000, false, 3);
    gdt[4] = make_desc(0, 0xFFFFF000, false, 0);  // ring-0 stack segment (CPL starts at 0)
    uc_reg_write(g_uc, UC_X86_REG_GDTR, &gdtr);
    u32 cs = (1 << 3) | 3, ds = (2 << 3) | 3, fs = (3 << 3) | 3, ss = (4 << 3);
    uc_reg_write(g_uc, UC_X86_REG_SS, &ss);
    uc_reg_write(g_uc, UC_X86_REG_CS, &cs);
    uc_reg_write(g_uc, UC_X86_REG_DS, &ds);
    uc_reg_write(g_uc, UC_X86_REG_ES, &ds);
    uc_reg_write(g_uc, UC_X86_REG_GS, &ds);
    uc_reg_write(g_uc, UC_X86_REG_FS, &fs);

    // TEB / PEB
    wr32(TEB_ADDR + 0x00, 0xFFFFFFFF);  // SEH chain end
    wr32(TEB_ADDR + 0x04, STACK_HI);
    wr32(TEB_ADDR + 0x08, STACK_LO);
    wr32(TEB_ADDR + 0x18, TEB_ADDR);
    wr32(TEB_ADDR + 0x20, 0x1000);  // pid
    wr32(TEB_ADDR + 0x24, 0x1004);  // tid
    wr32(TEB_ADDR + 0x2C, TLS_SLOTS_ADDR);
    wr32(TEB_ADDR + 0x30, PEB_ADDR);
    wr32(PEB_ADDR + 0x08, 0x400000);
    wr32(PEB_ADDR + 0x18, 0x00150000);  // process heap handle (opaque)
    wr32(PEB_ADDR + 0xA4, 5);           // OSMajorVersion
    wr32(PEB_ADDR + 0xA8, 1);           // OSMinorVersion
    wr32(PEB_ADDR + 0xAC, 2600);

    // Windows defaults: x87 all exceptions masked, 53-bit precision; SSE all masked
    u32 fpcw_before = 0, mxcsr_before = 0;
    uc_reg_read(g_uc, UC_X86_REG_FPCW, &fpcw_before);
    uc_reg_read(g_uc, UC_X86_REG_MXCSR, &mxcsr_before);
    u32 fpcw = 0x027F, mxcsr = 0x1F80;
    uc_reg_write(g_uc, UC_X86_REG_FPCW, &fpcw);
    uc_reg_write(g_uc, UC_X86_REG_MXCSR, &mxcsr);
    logf("fpu: fpcw %04x -> %04x, mxcsr %08x -> %08x", fpcw_before, fpcw, mxcsr_before, mxcsr);
    // enable SSE (OSFXSR | OSXMMEXCPT), clear EM, set MP
    u64 cr0, cr4;
    uc_reg_read(g_uc, UC_X86_REG_CR0, &cr0);
    cr0 = (cr0 & ~4ull) | 2ull;
    uc_reg_write(g_uc, UC_X86_REG_CR0, &cr0);
    uc_reg_read(g_uc, UC_X86_REG_CR4, &cr4);
    cr4 |= (1 << 9) | (1 << 10);
    uc_reg_write(g_uc, UC_X86_REG_CR4, &cr4);
}

// ---- PE loading ------------------------------------------------------------------
u32 g_image_base = 0x400000;
u32 g_image_size = 0;
u32 g_rsrc_rva = 0, g_rsrc_size = 0;

static std::vector<u8> read_file(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) fatal("cannot open %s", p.c_str());
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<u8> d(n);
    if (fread(d.data(), 1, n, f) != (size_t)n) fatal("short read %s", p.c_str());
    fclose(f);
    return d;
}

static u32 load_pe(const std::string& path) {
    auto f = read_file(path);
    u32 pe = *(u32*)&f[0x3C];
    if (memcmp(&f[pe], "PE\0\0", 4)) fatal("not a PE");
    u16 nsec = *(u16*)&f[pe + 6];
    u16 optsz = *(u16*)&f[pe + 20];
    u32 opt = pe + 24;
    u32 entry = *(u32*)&f[opt + 16];
    u32 base = *(u32*)&f[opt + 28];
    u32 imgsize = *(u32*)&f[opt + 56];
    u32 hdrsize = *(u32*)&f[opt + 60];
    g_image_base = base; g_image_size = imgsize;
    memcpy(g_mem + base, f.data(), hdrsize);
    u32 sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        u32 vsz = *(u32*)&f[sec + 8], va = *(u32*)&f[sec + 12], rawsz = *(u32*)&f[sec + 16], rawp = *(u32*)&f[sec + 20];
        u32 n = rawsz < vsz || vsz == 0 ? rawsz : vsz;
        if (rawp && n) memcpy(g_mem + base + va, f.data() + rawp, std::min<size_t>(n, f.size() - rawp));
    }
    u32 dd = opt + 96;
    g_rsrc_rva = *(u32*)&f[dd + 2 * 8];
    g_rsrc_size = *(u32*)&f[dd + 2 * 8 + 4];
    // imports
    u32 imp_rva = *(u32*)&f[dd + 1 * 8];
    if (imp_rva) {
        for (u32 d = base + imp_rva; rd32(d + 12); d += 20) {
            std::string dll = gstr(base + rd32(d + 12));
            u32 oft = rd32(d), ft = rd32(d + 16);
            u32 names = base + (oft ? oft : ft), iat = base + ft;
            for (int k = 0; rd32(names + k * 4); k++) {
                u32 ent = rd32(names + k * 4);
                u32 t = (ent & 0x80000000) ? thunk_for_ordinal(dll, ent & 0xFFFF) : thunk_for(dll, gstr(base + ent + 2));
                wr32(iat + k * 4, t);
            }
        }
    }
    logf("loaded %s base=%08x size=%08x entry=%08x", path.c_str(), base, imgsize, base + entry);
    return base + entry;
}

void dump_image(const std::string& out) {
    FILE* f = fopen(out.c_str(), "wb");
    if (!f) return;
    fwrite(g_mem + g_image_base, 1, g_image_size, f);
    fclose(f);
    logf("dumped image to %s", out.c_str());
}

void print_api_stats() {
    std::vector<std::pair<u64, std::string>> v;
    for (auto& t : thunks) if (t.calls) v.push_back({t.calls, t.name});
    std::sort(v.begin(), v.end());
    for (auto& p : v) logf("%10llu %s", (unsigned long long)p.first, p.second.c_str());
}

int emu_run(const std::string& exe_path) {
    uc_err e = uc_open(UC_ARCH_X86, UC_MODE_32, &g_uc);
    if (e) fatal("uc_open: %s", uc_strerror(e));
    // Reserve the 4GB guest space on the host; pages are committed lazily by the OS.
    void* p = mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fatal("mmap failed");
    g_mem = (u8*)p;
    e = uc_mem_map_ptr(g_uc, MAP_BASE, MAP_END - MAP_BASE, UC_PROT_ALL, g_mem + MAP_BASE);
    if (e) fatal("uc_mem_map_ptr: %s", uc_strerror(e));

    uc_hook h1, h2, h3;
    uc_hook_add(g_uc, &h1, UC_HOOK_INTR, (void*)hook_intr, nullptr, 1, 0);
    uc_hook_add(g_uc, &h3, UC_HOOK_CODE, (void*)hook_thunk, nullptr, THUNK_BASE, THUNK_BASE + THUNK_MAX - 1);
    uc_hook_add(g_uc, &h2, UC_HOOK_MEM_UNMAPPED, (void*)hook_unmapped, nullptr, 1, 0);

    g_mem[RET_MAGIC] = 0xF4;  // hlt (never executed: emulation stops at this address)
    g_mem[EXIT_MAGIC] = 0xF4;
    setup_segments();
    u32 entry = load_pe(exe_path);

    u32 esp = STACK_HI - 16;
    wr32(esp, EXIT_MAGIC);
    uc_reg_write(g_uc, UC_X86_REG_ESP, &esp);
    run_until(entry, EXIT_MAGIC);
    logf("guest returned from entry point");
    return 0;
}
