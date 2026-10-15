// Core emulator: guest memory, CPU, API thunks, guest callbacks.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <initializer_list>
#include <unicorn/unicorn.h>

typedef uint32_t u32;
typedef int32_t i32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef uint64_t u64;
typedef int64_t i64;

extern uc_engine* g_uc;
extern u8* g_mem;          // host base of guest address space
extern bool g_trace_api;   // log every API call
extern std::string g_game_dir;  // host directory containing hamsterball.exe (with trailing '/')

// ---- guest memory helpers -------------------------------------------------
template <typename T = u8>
inline T* g2h(u32 addr) { return reinterpret_cast<T*>(g_mem + addr); }
inline u32 h2g(const void* p) { return (u32)((const u8*)p - g_mem); }
inline u32 rd32(u32 a) { u32 v; memcpy(&v, g_mem + a, 4); return v; }
inline u16 rd16(u32 a) { u16 v; memcpy(&v, g_mem + a, 2); return v; }
inline u8 rd8(u32 a) { return g_mem[a]; }
inline float rdf(u32 a) { float v; memcpy(&v, g_mem + a, 4); return v; }
inline void wr32(u32 a, u32 v) { if (a) memcpy(g_mem + a, &v, 4); }
inline void wr16(u32 a, u16 v) { if (a) memcpy(g_mem + a, &v, 2); }
inline void wr8(u32 a, u8 v) { if (a) g_mem[a] = v; }
inline void wrf(u32 a, float v) { if (a) memcpy(g_mem + a, &v, 4); }
inline const char* gstr(u32 a) { return a ? (const char*)(g_mem + a) : nullptr; }
std::string gwstr(u32 a);  // UTF-16 guest string -> UTF-8
u32 gstrcpy(u32 dst, const std::string& s, u32 max);  // returns length copied (excl NUL)

// ---- guest heap (host-managed allocator over a guest region) --------------
u32 galloc(u32 size, bool zero = true);
void gfree(u32 addr);
u32 gsize(u32 addr);
u32 grealloc(u32 addr, u32 size);
u32 gstrdup(const std::string& s);
// page-granular allocations (VirtualAlloc)
u32 valloc(u32 size, u32 want_addr = 0);
void vfree(u32 addr);

// ---- API dispatch ---------------------------------------------------------
struct Ctx {
    u32 esp;            // esp at time of call ([esp] = return address)
    u32 edx_out = 0;    // optional EDX result
    bool set_edx = false;
    u32 arg(int i) const { return rd32(esp + 4 + 4 * i); }
    i32 iarg(int i) const { return (i32)arg(i); }
    float farg(int i) const { return rdf(esp + 4 + 4 * i); }
    u32 argaddr(int i) const { return esp + 4 + 4 * i; }
};
typedef u32 (*ApiFn)(Ctx&);
struct ApiDef {
    std::string name;  // "kernel32!GetTickCount" or "IDirect3DDevice8::Clear"
    int nargs;         // stdcall arg count (dwords popped); -1 => cdecl
    ApiFn fn;
};
struct ApiRegistrar { ApiRegistrar(const char* dll, const char* name, int nargs, ApiFn fn, bool com = false); };

#define API(dll, name, nargs)                                                          \
    static u32 api_##dll##_##name(Ctx& c);                                              \
    static ApiRegistrar reg_##dll##_##name(#dll, #name, nargs, api_##dll##_##name);     \
    static u32 api_##dll##_##name(Ctx& c)

// COM method: nargs includes `this`.
#define COM(iface, name, nargs)                                                         \
    static u32 com_##iface##_##name(Ctx& c);                                              \
    static ApiRegistrar regc_##iface##_##name(#iface, #name, nargs, com_##iface##_##name, true); \
    static u32 com_##iface##_##name(Ctx& c)

// Look up (or create a logging stub for) an API thunk address by dll/name.
u32 thunk_for(const std::string& dll, const std::string& name);
u32 thunk_for_ordinal(const std::string& dll, u32 ord);
bool api_exists(const std::string& dll, const std::string& name);

// COM: build a vtable of thunks for "Iface::Method" names (in order), return guest address.
u32 make_vtable(const char* iface, const std::vector<const char*>& methods);
// Allocate a guest COM object: [vtbl][host id]. Returns guest pointer.
u32 make_com_object(u32 vtbl, u32 host_id, u32 extra_bytes = 0);
inline u32 com_id(u32 obj) { return rd32(obj + 4); }

// Call a guest function (stdcall or cdecl; caller cleans if cdecl) and return EAX.
u32 call_guest(u32 addr, std::initializer_list<u32> args, bool cdecl_ = false);

// Module handles for LoadLibrary/GetModuleHandle
u32 module_handle(const std::string& dll);  // 0 if unknown
std::string module_name(u32 h);

[[noreturn]] void fatal(const char* fmt, ...);
void logf(const char* fmt, ...);
void dump_regs();

// Implemented in platform/main: pump host events, called from message APIs.
void platform_pump();
extern u32 g_last_error;

// Run until the guest exits.
int emu_run(const std::string& exe_path);
