// kernel32 replacement: memory, files, resources, TLS, misc process APIs.
#include "emu.h"
#include <SDL.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <ctime>
#include <algorithm>
#include <set>

extern u32 g_image_base, g_rsrc_rva, g_rsrc_size;
void print_api_stats();

static const u32 PROCESS_HEAP = 0x00150000;
static const u32 INVALID_HANDLE = 0xFFFFFFFF;
static const char* GUEST_DIR = "C:\\Program Files\\Hamsterball\\";

// ---- path mapping -----------------------------------------------------------------
// Guest paths are mapped onto the host game directory. Absolute paths under the
// fake install dir and relative paths both resolve there; lookup is case-insensitive.
static std::string find_ci(const std::string& dir, const std::string& name) {
    DIR* d = opendir(dir.c_str());
    if (!d) return name;
    std::string res = name;
    while (dirent* e = readdir(d)) {
        if (strcasecmp(e->d_name, name.c_str()) == 0) { res = e->d_name; break; }
    }
    closedir(d);
    return res;
}
std::string host_path(const char* gp) {
    std::string p = gp ? gp : "";
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string base = GUEST_DIR;
    if (strncasecmp(p.c_str(), base.c_str(), base.size()) == 0) p = p.substr(base.size());
    else if (p.size() > 2 && p[1] == ':') {
        // other absolute path: keep only the file name inside the game dir's "drive_c" sandbox
        p = "drive_c\\" + p.substr(3);
    }
    while (p.rfind(".\\", 0) == 0) p = p.substr(2);
    std::string out = g_game_dir;
    size_t start = 0;
    while (start <= p.size()) {
        size_t sep = p.find('\\', start);
        std::string comp = p.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        if (!comp.empty() && comp != ".") {
            if (comp == "..") { /* ignore: stay inside sandbox */ }
            else out += find_ci(out, comp) + (sep == std::string::npos ? "" : "/");
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return out;
}

// ---- handles --------------------------------------------------------------------
struct KHandle {
    enum Kind { File, Mapping, Find, Event, Other } kind;
    FILE* f = nullptr;
    std::string path;
    u32 file_handle = 0;  // for mappings
};
static std::map<u32, KHandle> handles;
static u32 next_handle = 0x100;
static u32 add_handle(KHandle h) { u32 id = next_handle; next_handle += 4; handles[id] = h; return id; }
static KHandle* get_handle(u32 h) { auto it = handles.find(h); return it == handles.end() ? nullptr : &it->second; }

// ---- misc -------------------------------------------------------------------------
API(kernel32, GetTickCount, 0) { return SDL_GetTicks(); }
API(kernel32, Sleep, 1) {
    platform_pump();
    if (c.arg(0)) SDL_Delay(c.arg(0));
    else usleep(300);  // the game spins on Sleep(0); yield a little instead of burning a core
    return 0;
}
API(kernel32, GetLastError, 0) { return g_last_error; }
API(kernel32, SetLastError, 1) { g_last_error = c.arg(0); return 0; }
API(kernel32, GetCurrentProcess, 0) { return 0xFFFFFFFF; }
API(kernel32, GetCurrentProcessId, 0) { return 0x1000; }
API(kernel32, GetCurrentThreadId, 0) { return 0x1004; }
API(kernel32, SetErrorMode, 1) { return 0; }
API(kernel32, SetUnhandledExceptionFilter, 1) { return 0; }
API(kernel32, UnhandledExceptionFilter, 1) { return 0; }
API(kernel32, IsDebuggerPresent, 0) { return 0; }
API(kernel32, SetHandleCount, 1) { return c.arg(0); }
API(kernel32, GetACP, 0) { return 1252; }
API(kernel32, GetOEMCP, 0) { return 437; }
API(kernel32, ExitProcess, 1) {
    logf("ExitProcess(%u)", c.arg(0));
    if (getenv("HB_STATS")) print_api_stats();
    SDL_Quit();
    exit((int)c.arg(0));
}
API(kernel32, TerminateProcess, 2) {
    logf("TerminateProcess(%u)", c.arg(1));
    SDL_Quit();
    exit((int)c.arg(1));
}
API(kernel32, IsProcessorFeaturePresent, 1) {
    u32 f = c.arg(0);
#ifdef HB_RECOMP
    (void)f;
    return 0;  // the recompiled CPU is x87-only
#else
    return f == 6 /*XMMI*/ || f == 10 /*XMMI64 (SSE2)*/ || f == 3 /*MMX*/ ? 1 : 0;
#endif
}
API(kernel32, GetVersionExA, 1) {
    u32 p = c.arg(0);
    u32 sz = rd32(p);
    wr32(p + 4, 5); wr32(p + 8, 1); wr32(p + 12, 2600); wr32(p + 16, 2);  // XP SP2, VER_PLATFORM_WIN32_NT
    gstrcpy(p + 20, "Service Pack 2", 128);
    if (sz >= 156) { wr16(p + 148, 2); wr16(p + 150, 0); wr16(p + 152, 0); wr8(p + 154, 1); }
    return 1;
}
API(kernel32, GetVersion, 0) { return (2600u << 16) | 0x0105; }
API(kernel32, GetSystemInfo, 1) {
    u32 p = c.arg(0);
    memset(g2h(p), 0, 36);
    wr32(p + 4, 0x1000);        // page size
    wr32(p + 8, 0x10000);       // min app addr
    wr32(p + 12, MAP_END - 0x10001);  // max app addr
    wr32(p + 16, 1);            // active processor mask
    wr32(p + 20, 1);            // number of processors
    wr32(p + 24, 586);          // processor type
    wr32(p + 28, 0x10000);      // allocation granularity
    wr16(p + 32, 6); wr16(p + 34, 0x0F00);
    return 0;
}
API(kernel32, GetSystemTimeAsFileTime, 1) {
    u64 t = ((u64)time(nullptr) + 11644473600ull) * 10000000ull;
    wr32(c.arg(0), (u32)t); wr32(c.arg(0) + 4, (u32)(t >> 32));
    return 0;
}
API(kernel32, QueryPerformanceCounter, 1) {
    u64 t = SDL_GetPerformanceCounter();
    wr32(c.arg(0), (u32)t); wr32(c.arg(0) + 4, (u32)(t >> 32));
    return 1;
}
API(kernel32, QueryPerformanceFrequency, 1) {
    u64 t = SDL_GetPerformanceFrequency();
    wr32(c.arg(0), (u32)t); wr32(c.arg(0) + 4, (u32)(t >> 32));
    return 1;
}
API(kernel32, GetLocalTime, 1) {
    time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
    u32 p = c.arg(0);
    wr16(p, tm.tm_year + 1900); wr16(p + 2, tm.tm_mon + 1); wr16(p + 4, tm.tm_wday); wr16(p + 6, tm.tm_mday);
    wr16(p + 8, tm.tm_hour); wr16(p + 10, tm.tm_min); wr16(p + 12, tm.tm_sec); wr16(p + 14, 0);
    return 0;
}
API(kernel32, GetSystemTime, 1) {
    time_t now = time(nullptr); struct tm tm; gmtime_r(&now, &tm);
    u32 p = c.arg(0);
    wr16(p, tm.tm_year + 1900); wr16(p + 2, tm.tm_mon + 1); wr16(p + 4, tm.tm_wday); wr16(p + 6, tm.tm_mday);
    wr16(p + 8, tm.tm_hour); wr16(p + 10, tm.tm_min); wr16(p + 12, tm.tm_sec); wr16(p + 14, 0);
    return 0;
}
API(kernel32, GetTimeZoneInformation, 1) { memset(g2h(c.arg(0)), 0, 172); return 0; }
API(kernel32, GetStartupInfoA, 1) {
    u32 p = c.arg(0);
    memset(g2h(p), 0, 68);
    wr32(p, 68);
    return 0;
}
static u32 cmdline_a = 0;
API(kernel32, GetCommandLineA, 0) {
    if (!cmdline_a) cmdline_a = gstrdup(std::string("\"") + GUEST_DIR + "hamsterball.exe\"");
    return cmdline_a;
}
API(kernel32, GetStdHandle, 1) { return 0; }  // no console
API(kernel32, SetStdHandle, 2) { return 1; }
API(kernel32, GetFileType, 1) { return 2; }  // FILE_TYPE_CHAR
static u32 env_a = 0, env_w = 0;
API(kernel32, GetEnvironmentStrings, 0) {
    if (!env_a) { env_a = galloc(16); }
    return env_a;
}
API(kernel32, GetEnvironmentStringsA, 0) { return api_kernel32_GetEnvironmentStrings(c); }
API(kernel32, GetEnvironmentStringsW, 0) {
    if (!env_w) { env_w = galloc(16); }
    return env_w;
}
API(kernel32, FreeEnvironmentStringsA, 1) { return 1; }
API(kernel32, FreeEnvironmentStringsW, 1) { return 1; }
API(kernel32, GetEnvironmentVariableA, 3) { return 0; }
API(kernel32, GetModuleFileNameA, 3) {
    std::string s = std::string(GUEST_DIR) + "hamsterball.exe";
    u32 h = c.arg(0);
    if (h && h != g_image_base) {
        std::string m = module_name(h);
        s = "C:\\WINDOWS\\system32\\" + m + ".dll";
    }
    return gstrcpy(c.arg(1), s, c.arg(2));
}
API(kernel32, GetWindowsDirectoryA, 2) { return gstrcpy(c.arg(0), "C:\\WINDOWS", c.arg(1)); }
API(kernel32, GetSystemDirectoryA, 2) { return gstrcpy(c.arg(0), "C:\\WINDOWS\\system32", c.arg(1)); }
API(kernel32, GetCurrentDirectoryA, 2) { return gstrcpy(c.arg(1), "C:\\Program Files\\Hamsterball", c.arg(0)); }
API(kernel32, SetCurrentDirectoryA, 1) { return 1; }
API(kernel32, GetLocaleInfoA, 4) {
    u32 type = c.arg(1) & 0xFFFF;
    std::string v;
    switch (type) {
        case 0x1004: v = "1252"; break;   // LOCALE_IDEFAULTANSICODEPAGE
        case 0x000B: v = "437"; break;    // LOCALE_IDEFAULTCODEPAGE
        case 0x0003: v = "ENU"; break;    // LOCALE_SABBREVLANGNAME
        case 0x0007: v = "USA"; break;    // LOCALE_SABBREVCTRYNAME
        case 0x0059: v = "en"; break;     // LOCALE_SISO639LANGNAME
        case 0x005A: v = "US"; break;     // LOCALE_SISO3166CTRYNAME
        case 0x000E: v = "."; break;      // LOCALE_SDECIMAL
        case 0x000F: v = ","; break;      // LOCALE_STHOUSAND
        case 0x0001: v = "0409"; break;
        default: v = ""; break;
    }
    if (!c.arg(3)) return (u32)v.size() + 1;
    gstrcpy(c.arg(2), v, c.arg(3));
    return (u32)v.size() + 1;
}
API(kernel32, GetUserDefaultLCID, 0) { return 0x409; }
API(kernel32, GetThreadLocale, 0) { return 0x409; }
API(kernel32, GetUserDefaultLangID, 0) { return 0x409; }
API(kernel32, GetSystemDefaultLangID, 0) { return 0x409; }
API(kernel32, GetCPInfo, 2) {
    u32 p = c.arg(1);
    memset(g2h(p), 0, 20);
    wr32(p, 1);
    wr8(p + 4, '?');
    return 1;
}
API(kernel32, MultiByteToWideChar, 6) {
    u32 src = c.arg(2); i32 n = c.iarg(3); u32 dst = c.arg(4); i32 dn = c.iarg(5);
    if (n < 0) n = (i32)strlen(gstr(src)) + 1;
    if (dn == 0) return n;
    i32 k = std::min(n, dn);
    for (i32 i = 0; i < k; i++) wr16(dst + i * 2, rd8(src + i));
    return k;
}
API(kernel32, WideCharToMultiByte, 8) {
    u32 src = c.arg(2); i32 n = c.iarg(3); u32 dst = c.arg(4); i32 dn = c.iarg(5);
    if (n < 0) { n = 0; while (rd16(src + n * 2)) n++; n++; }
    if (dn == 0) return n;
    i32 k = std::min(n, dn);
    for (i32 i = 0; i < k; i++) { u16 ch = rd16(src + i * 2); wr8(dst + i, ch < 256 ? (u8)ch : '?'); }
    return k;
}
API(kernel32, GetStringTypeA, 5) {
    u32 src = c.arg(2); i32 n = c.iarg(3); u32 out = c.arg(4);
    if (n < 0) n = (i32)strlen(gstr(src)) + 1;
    for (i32 i = 0; i < n; i++) {
        u8 ch = rd8(src + i); u16 t = 0;
        if (isupper(ch)) t |= 1; if (islower(ch)) t |= 2; if (isdigit(ch)) t |= 4; if (isspace(ch)) t |= 8;
        if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ') t |= 0x40; if (isxdigit(ch)) t |= 0x80;
        if (isalpha(ch)) t |= 0x100;
        wr16(out + i * 2, t);
    }
    return 1;
}
API(kernel32, GetStringTypeW, 4) {
    u32 src = c.arg(1); i32 n = c.iarg(2); u32 out = c.arg(3);
    if (n < 0) { n = 0; while (rd16(src + n * 2)) n++; n++; }
    for (i32 i = 0; i < n; i++) {
        u16 w = rd16(src + i * 2); u8 ch = w < 128 ? (u8)w : 0; u16 t = 0;
        if (isupper(ch)) t |= 1; if (islower(ch)) t |= 2; if (isdigit(ch)) t |= 4; if (isspace(ch)) t |= 8;
        if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ') t |= 0x40; if (isxdigit(ch)) t |= 0x80;
        if (isalpha(ch)) t |= 0x100;
        wr16(out + i * 2, t);
    }
    return 1;
}
API(kernel32, LCMapStringA, 6) {
    u32 flags = c.arg(1), src = c.arg(2); i32 n = c.iarg(3); u32 dst = c.arg(4); i32 dn = c.iarg(5);
    if (n < 0) n = (i32)strlen(gstr(src)) + 1;
    if (dn == 0) return n;
    for (i32 i = 0; i < n && i < dn; i++) {
        u8 ch = rd8(src + i);
        if (flags & 0x100) ch = (u8)tolower(ch); else if (flags & 0x200) ch = (u8)toupper(ch);
        wr8(dst + i, ch);
    }
    return std::min(n, dn);
}
API(kernel32, LCMapStringW, 6) {
    u32 flags = c.arg(1), src = c.arg(2); i32 n = c.iarg(3); u32 dst = c.arg(4); i32 dn = c.iarg(5);
    if (n < 0) { n = 0; while (rd16(src + n * 2)) n++; n++; }
    if (dn == 0) return n;
    for (i32 i = 0; i < n && i < dn; i++) {
        u16 ch = rd16(src + i * 2);
        if (ch < 128) { if (flags & 0x100) ch = (u16)tolower(ch); else if (flags & 0x200) ch = (u16)toupper(ch); }
        wr16(dst + i * 2, ch);
    }
    return std::min(n, dn);
}

// ---- modules ----------------------------------------------------------------------
API(kernel32, GetModuleHandleA, 1) {
    if (!c.arg(0)) return g_image_base;
    u32 h = module_handle(gstr(c.arg(0)));
    if (!h) g_last_error = 126;
    return h;
}
API(kernel32, GetModuleHandleW, 1) {
    if (!c.arg(0)) return g_image_base;
    return module_handle(gwstr(c.arg(0)));
}
API(kernel32, LoadLibraryA, 1) {
    const char* n = gstr(c.arg(0));
    u32 h = module_handle(n);
    static std::set<std::string> seen;
    if (seen.insert(n).second) logf("LoadLibraryA(%s) -> %08x", n, h);
    if (!h) g_last_error = 126;
    return h;
}
API(kernel32, LoadLibraryExA, 3) { return module_handle(gstr(c.arg(0))); }
API(kernel32, FreeLibrary, 1) { return 1; }
API(kernel32, GetProcAddress, 2) {
    u32 h = c.arg(0), n = c.arg(1);
    std::string dll = module_name(h);
    if (dll.empty()) { logf("GetProcAddress on unknown module %08x", h); return 0; }
    if (n < 0x10000) return thunk_for_ordinal(dll, n);
    std::string name = gstr(n);
    if (!api_exists(dll, name)) {
        static std::set<std::string> seen;
        if (seen.insert(dll + name).second) logf("GetProcAddress(%s, %s) -> not implemented, returning NULL", dll.c_str(), name.c_str());
        g_last_error = 127;
        return 0;
    }
    return thunk_for(dll, name);
}

// ---- memory -----------------------------------------------------------------------
API(kernel32, GetProcessHeap, 0) { return PROCESS_HEAP; }
API(kernel32, HeapCreate, 3) { return PROCESS_HEAP + 0x1000; }
API(kernel32, HeapDestroy, 1) { return 1; }
API(kernel32, HeapAlloc, 3) { return galloc(c.arg(2), true); }
API(kernel32, HeapFree, 3) { gfree(c.arg(2)); return 1; }
API(kernel32, HeapReAlloc, 4) {
    u32 a = c.arg(2), sz = c.arg(3);
    if (c.arg(1) & 0x10) {  // HEAP_REALLOC_IN_PLACE_ONLY
        return sz <= gsize(a) ? a : 0;
    }
    return grealloc(a, sz);
}
API(kernel32, HeapSize, 3) { return gsize(c.arg(2)); }
API(kernel32, HeapValidate, 3) { return 1; }
API(kernel32, HeapCompact, 2) { return 0x100000; }
API(kernel32, GlobalAlloc, 2) { return galloc(c.arg(1), true); }
API(kernel32, GlobalFree, 1) { gfree(c.arg(0)); return 0; }
API(kernel32, GlobalLock, 1) { return c.arg(0); }
API(kernel32, GlobalUnlock, 1) { return 1; }
API(kernel32, GlobalSize, 1) { return gsize(c.arg(0)); }
API(kernel32, LocalAlloc, 2) { return galloc(c.arg(1), true); }
API(kernel32, LocalFree, 1) { gfree(c.arg(0)); return 0; }
API(kernel32, VirtualAlloc, 4) {
    u32 a = valloc(c.arg(1), c.arg(0));
    return a;
}
API(kernel32, VirtualFree, 3) {
    if (c.arg(2) & 0x8000) vfree(c.arg(0));  // MEM_RELEASE
    return 1;
}
API(kernel32, VirtualProtect, 4) {
    if (c.arg(3)) wr32(c.arg(3), 0x40);  // old protect = PAGE_EXECUTE_READWRITE
    return 1;
}
API(kernel32, VirtualQuery, 3) {
    u32 a = c.arg(0) & ~0xFFFu, p = c.arg(1);
    memset(g2h(p), 0, 28);
    wr32(p, a); wr32(p + 4, a & 0xFFFF0000); wr32(p + 8, 0x40);
    wr32(p + 12, 0x1000); wr32(p + 16, 0x1000 /*MEM_COMMIT*/); wr32(p + 20, 0x40); wr32(p + 24, 0x20000);
    return 28;
}
API(kernel32, IsBadReadPtr, 2) { return c.arg(0) < 0x10000 ? 1 : 0; }
API(kernel32, IsBadWritePtr, 2) { return c.arg(0) < 0x10000 ? 1 : 0; }
API(kernel32, IsBadCodePtr, 1) { return c.arg(0) < 0x10000 ? 1 : 0; }

// ---- TLS / sync ----------------------------------------------------------------------
static u32 tls_used = 0;
static const u32 TLS_SLOTS = TLS_SLOTS_ADDR;
API(kernel32, TlsAlloc, 0) {
    for (u32 i = 0; i < 64; i++) if (!(tls_used & 0)) {}
    static u32 next = 0;
    if (next >= 1024) return 0xFFFFFFFF;
    tls_used++;
    return next++;
}
API(kernel32, TlsFree, 1) { return 1; }
API(kernel32, TlsSetValue, 2) { wr32(TLS_SLOTS + 0x100 + c.arg(0) * 4, c.arg(1)); return 1; }
API(kernel32, TlsGetValue, 1) { g_last_error = 0; return rd32(TLS_SLOTS + 0x100 + c.arg(0) * 4); }
API(kernel32, InitializeCriticalSection, 1) { memset(g2h(c.arg(0)), 0, 24); return 0; }
API(kernel32, InitializeCriticalSectionAndSpinCount, 2) { memset(g2h(c.arg(0)), 0, 24); return 1; }
API(kernel32, DeleteCriticalSection, 1) { return 0; }
API(kernel32, EnterCriticalSection, 1) { return 0; }
API(kernel32, LeaveCriticalSection, 1) { return 0; }
API(kernel32, InterlockedIncrement, 1) { u32 v = rd32(c.arg(0)) + 1; wr32(c.arg(0), v); return v; }
API(kernel32, InterlockedDecrement, 1) { u32 v = rd32(c.arg(0)) - 1; wr32(c.arg(0), v); return v; }
API(kernel32, InterlockedExchange, 2) { u32 v = rd32(c.arg(0)); wr32(c.arg(0), c.arg(1)); return v; }

// ---- exceptions ------------------------------------------------------------------------
API(kernel32, RaiseException, 4) {
    fatal("RaiseException code=%08x flags=%x nargs=%u", c.arg(0), c.arg(1), c.arg(2));
}
API(kernel32, RtlUnwind, 4) {
    // Unlink SEH frames up to the target frame.
    u32 target = c.arg(0);
    u32 teb = TEB_ADDR;
    if (target) wr32(teb, target);
    else wr32(teb, 0xFFFFFFFF);
    return 0;
}

// ---- files --------------------------------------------------------------------------------
static u32 create_file(const std::string& gpath, u32 access, u32 disposition) {
    std::string hp = host_path(gpath.c_str());
    if (g_trace_api) logf("[file] open %s", gpath.c_str());
    const char* mode = nullptr;
    struct stat st;
    bool exists = stat(hp.c_str(), &st) == 0;
    bool write = access & 0x40000000;
    switch (disposition) {
        case 1: /*CREATE_NEW*/ if (exists) { g_last_error = 80; return INVALID_HANDLE; } mode = "w+b"; break;
        case 2: /*CREATE_ALWAYS*/ mode = "w+b"; break;
        case 3: /*OPEN_EXISTING*/ if (!exists) { g_last_error = 2; return INVALID_HANDLE; } mode = write ? "r+b" : "rb"; break;
        case 4: /*OPEN_ALWAYS*/ mode = exists ? (write ? "r+b" : "rb") : "w+b"; break;
        case 5: /*TRUNCATE_EXISTING*/ if (!exists) { g_last_error = 2; return INVALID_HANDLE; } mode = "w+b"; break;
        default: mode = "rb";
    }
    if (exists && S_ISDIR(st.st_mode)) { g_last_error = 5; return INVALID_HANDLE; }
    if (!exists && mode[0] == 'w') {
        // create missing parent directories (guest expects e.g. C:\WINDOWS to exist)
        for (size_t i = g_game_dir.size(); i < hp.size(); i++)
            if (hp[i] == '/') mkdir(hp.substr(0, i).c_str(), 0755);
    }
    FILE* f = fopen(hp.c_str(), mode);
    if (!f) { g_last_error = 2; logf("CreateFile failed: %s (%s)", gpath.c_str(), hp.c_str()); return INVALID_HANDLE; }
    KHandle h{KHandle::File}; h.f = f; h.path = hp;
    g_last_error = exists && (disposition == 2 || disposition == 4) ? 183 : 0;
    return add_handle(h);
}
API(kernel32, CreateFileA, 7) { return create_file(gstr(c.arg(0)), c.arg(1), c.arg(4)); }
API(kernel32, CreateFileW, 7) { return create_file(gwstr(c.arg(0)), c.arg(1), c.arg(4)); }
API(kernel32, CloseHandle, 1) {
    auto it = handles.find(c.arg(0));
    if (it == handles.end()) return 1;
    if (it->second.kind == KHandle::File && it->second.f) fclose(it->second.f);
    handles.erase(it);
    return 1;
}
API(kernel32, ReadFile, 5) {
    KHandle* h = get_handle(c.arg(0));
    if (!h || !h->f) { wr32(c.arg(3), 0); return 0; }
    size_t n = fread(g2h(c.arg(1)), 1, c.arg(2), h->f);
    wr32(c.arg(3), (u32)n);
    return 1;
}
API(kernel32, WriteFile, 5) {
    KHandle* h = get_handle(c.arg(0));
    if (!h || !h->f) { wr32(c.arg(3), c.arg(2)); return 1; }  // console etc.
    size_t n = fwrite(g2h(c.arg(1)), 1, c.arg(2), h->f);
    wr32(c.arg(3), (u32)n);
    return 1;
}
API(kernel32, FlushFileBuffers, 1) { KHandle* h = get_handle(c.arg(0)); if (h && h->f) fflush(h->f); return 1; }
API(kernel32, SetEndOfFile, 1) {
    KHandle* h = get_handle(c.arg(0));
    if (h && h->f) { fflush(h->f); if (ftruncate(fileno(h->f), ftell(h->f))) {} }
    return 1;
}
API(kernel32, GetFileSize, 2) {
    KHandle* h = get_handle(c.arg(0));
    if (!h || !h->f) return 0xFFFFFFFF;
    long cur = ftell(h->f); fseek(h->f, 0, SEEK_END); long n = ftell(h->f); fseek(h->f, cur, SEEK_SET);
    if (c.arg(1)) wr32(c.arg(1), 0);
    return (u32)n;
}
API(kernel32, SetFilePointer, 4) {
    KHandle* h = get_handle(c.arg(0));
    if (!h || !h->f) return 0xFFFFFFFF;
    int whence = c.arg(3) == 0 ? SEEK_SET : c.arg(3) == 1 ? SEEK_CUR : SEEK_END;
    fseek(h->f, (long)c.iarg(1), whence);
    if (c.arg(2)) wr32(c.arg(2), 0);
    return (u32)ftell(h->f);
}
API(kernel32, DeleteFileA, 1) { return unlink(host_path(gstr(c.arg(0))).c_str()) == 0 ? 1 : 0; }
API(kernel32, CreateDirectoryA, 2) {
    std::string p = host_path(gstr(c.arg(0)));
    if (mkdir(p.c_str(), 0755) == 0) return 1;
    g_last_error = 183;
    return 0;
}
API(kernel32, GetFileAttributesA, 1) {
    struct stat st;
    std::string p = host_path(gstr(c.arg(0)));
    if (stat(p.c_str(), &st) != 0) { g_last_error = 2; return 0xFFFFFFFF; }
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
static void fill_find_data(u32 p, const std::string& name, const struct stat& st) {
    memset(g2h(p), 0, 320);
    wr32(p, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    wr32(p + 32, (u32)st.st_size);
    gstrcpy(p + 44, name, 260);
}
static bool wild_match(const char* pat, const char* s) {
    if (!*pat) return !*s;
    if (*pat == '*') return wild_match(pat + 1, s) || (*s && wild_match(pat, s + 1));
    if (*pat == '?') return *s && wild_match(pat + 1, s + 1);
    return tolower((u8)*pat) == tolower((u8)*s) && wild_match(pat + 1, s + 1);
}
struct FindState { std::vector<std::pair<std::string, struct stat>> entries; size_t pos = 0; };
static std::map<u32, FindState> finds;
API(kernel32, FindFirstFileA, 2) {
    std::string gp = gstr(c.arg(0));
    std::replace(gp.begin(), gp.end(), '/', '\\');
    size_t sl = gp.find_last_of('\\');
    std::string dir = sl == std::string::npos ? "" : gp.substr(0, sl);
    std::string pat = sl == std::string::npos ? gp : gp.substr(sl + 1);
    std::string hdir = host_path(dir.c_str());
    FindState fs;
    if (DIR* d = opendir(hdir.c_str())) {
        while (dirent* e = readdir(d)) {
            if (!wild_match(pat.c_str(), e->d_name)) continue;
            struct stat st;
            std::string full = hdir + (hdir.empty() || hdir.back() == '/' ? "" : "/") + e->d_name;
            if (stat(full.c_str(), &st) == 0) fs.entries.push_back({e->d_name, st});
        }
        closedir(d);
    }
    if (fs.entries.empty()) { g_last_error = 2; return INVALID_HANDLE; }
    fill_find_data(c.arg(1), fs.entries[0].first, fs.entries[0].second);
    fs.pos = 1;
    u32 h = add_handle({KHandle::Find});
    finds[h] = fs;
    return h;
}
API(kernel32, FindNextFileA, 2) {
    auto it = finds.find(c.arg(0));
    if (it == finds.end() || it->second.pos >= it->second.entries.size()) { g_last_error = 18; return 0; }
    auto& e = it->second.entries[it->second.pos++];
    fill_find_data(c.arg(1), e.first, e.second);
    return 1;
}
API(kernel32, FindClose, 1) { finds.erase(c.arg(0)); handles.erase(c.arg(0)); return 1; }
API(kernel32, CreateFileMappingA, 6) {
    KHandle h{KHandle::Mapping}; h.file_handle = c.arg(0);
    return add_handle(h);
}
static std::map<u32, u32> views;  // guest addr -> mapping handle
API(kernel32, MapViewOfFile, 5) {
    KHandle* m = get_handle(c.arg(0));
    if (!m) return 0;
    KHandle* fh = get_handle(m->file_handle);
    if (!fh || !fh->f) return 0;
    fseek(fh->f, 0, SEEK_END); long n = ftell(fh->f); fseek(fh->f, 0, SEEK_SET);
    u32 a = galloc((u32)n + 1);
    if (fread(g2h(a), 1, n, fh->f) != (size_t)n) logf("MapViewOfFile: short read");
    views[a] = c.arg(0);
    return a;
}
API(kernel32, UnmapViewOfFile, 1) { views.erase(c.arg(0)); gfree(c.arg(0)); return 1; }

// ---- resources -----------------------------------------------------------------------------
static bool res_name_match(u32 rsrc, u32 entry, u32 name) {
    u32 id = rd32(entry);
    if (name < 0x10000) return !(id & 0x80000000) && (id & 0xFFFF) == name;
    if (!(id & 0x80000000)) {
        const char* s = gstr(name);
        if (s[0] == '#') return (u32)atoi(s + 1) == (id & 0xFFFF);
        return false;
    }
    u32 str = rsrc + (id & 0x7FFFFFFF);
    u16 len = rd16(str);
    std::string a = gstr(name);
    if (a.size() != len) return false;
    for (u16 i = 0; i < len; i++) if (toupper(rd16(str + 2 + i * 2)) != toupper((u8)a[i])) return false;
    return true;
}
static u32 res_find(u32 dir, u32 rsrc, u32 name) {  // returns entry offset value or 0
    u16 nnamed = rd16(dir + 12), nid = rd16(dir + 14);
    for (u32 i = 0; i < (u32)(nnamed + nid); i++) {
        u32 e = dir + 16 + i * 8;
        if (name == 0xFFFFFFFF || res_name_match(rsrc, e, name)) return rd32(e + 4);
    }
    return 0;
}
API(kernel32, FindResourceA, 3) {
    u32 rsrc = g_image_base + g_rsrc_rva;
    if (!g_rsrc_rva) return 0;
    u32 t = res_find(rsrc, rsrc, c.arg(2));
    if (!t || !(t & 0x80000000)) return 0;
    u32 n = res_find(rsrc + (t & 0x7FFFFFFF), rsrc, c.arg(1));
    if (!n || !(n & 0x80000000)) return 0;
    u32 l = res_find(rsrc + (n & 0x7FFFFFFF), rsrc, 0xFFFFFFFF);  // first language
    if (!l || (l & 0x80000000)) return 0;
    return rsrc + l;  // data entry
}
API(kernel32, LoadResource, 2) { return c.arg(1) ? g_image_base + rd32(c.arg(1)) : 0; }
API(kernel32, SizeofResource, 2) { return c.arg(1) ? rd32(c.arg(1) + 4) : 0; }
API(kernel32, LockResource, 1) { return c.arg(0); }
API(kernel32, FreeResource, 1) { return 0; }

API(kernel32, OutputDebugStringA, 1) { logf("[debug] %s", gstr(c.arg(0))); return 0; }
