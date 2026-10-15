// advapi32 (registry persisted to a file), shell32, ole32, ws2_32 (offline), winmm bits.
#include "platform.h"
#include <fstream>
#include <sstream>

// ---- registry --------------------------------------------------------------------------
// Keys are case-insensitive paths "HKCU\SOFTWARE\...". Values stored as (type, bytes).
struct RegValue { u32 type; std::vector<u8> data; };
static std::map<std::string, std::map<std::string, RegValue>> reg;
static std::map<u32, std::string> reg_handles;
static u32 next_hkey = 0x3000;
static bool reg_loaded = false;

static std::string up(std::string s) { for (auto& c : s) c = (char)toupper((u8)c); return s; }
static std::string reg_file() { return g_game_dir + "registry.txt"; }
static void reg_save() {
    std::ofstream f(reg_file());
    for (auto& k : reg) {
        f << "[" << k.first << "]\n";
        for (auto& v : k.second) {
            f << v.first << "=" << v.second.type << ":";
            char hex[3];
            for (u8 b : v.second.data) { snprintf(hex, 3, "%02x", b); f << hex; }
            f << "\n";
        }
    }
}
static void reg_load() {
    if (reg_loaded) return;
    reg_loaded = true;
    std::ifstream f(reg_file());
    std::string line, key;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line[0] == '[') { key = line.substr(1, line.size() - 2); reg[key]; continue; }
        size_t eq = line.rfind('='), colon = line.find(':', eq);
        if (eq == std::string::npos || colon == std::string::npos) continue;
        RegValue v; v.type = (u32)atoi(line.substr(eq + 1, colon - eq - 1).c_str());
        std::string hex = line.substr(colon + 1);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) v.data.push_back((u8)strtol(hex.substr(i, 2).c_str(), nullptr, 16));
        reg[key][line.substr(0, eq)] = v;
    }
}
static std::string root_name(u32 h) {
    switch (h) {
        case 0x80000000: return "HKCR"; case 0x80000001: return "HKCU"; case 0x80000002: return "HKLM";
        case 0x80000003: return "HKU";
    }
    auto it = reg_handles.find(h);
    return it == reg_handles.end() ? "" : it->second;
}
static std::string subkey(u32 parent, u32 sub) {
    std::string p = root_name(parent);
    if (p.empty()) return "";
    if (sub && *gstr(sub)) p += "\\" + up(gstr(sub));
    return p;
}
static u32 open_key(u32 parent, u32 sub, u32 out, bool create) {
    reg_load();
    std::string k = subkey(parent, sub);
    if (k.empty()) return 6;  // ERROR_INVALID_HANDLE
    if (!reg.count(k)) {
        if (!create) return 2;
        reg[k];
    }
    u32 h = next_hkey; next_hkey += 4;
    reg_handles[h] = k;
    wr32(out, h);
    return 0;
}
API(advapi32, RegOpenKeyA, 3) { return open_key(c.arg(0), c.arg(1), c.arg(2), false); }
API(advapi32, RegOpenKeyExA, 5) { return open_key(c.arg(0), c.arg(1), c.arg(4), false); }
API(advapi32, RegCreateKeyA, 3) { return open_key(c.arg(0), c.arg(1), c.arg(2), true); }
API(advapi32, RegCreateKeyExA, 9) {
    u32 r = open_key(c.arg(0), c.arg(1), c.arg(7), true);
    if (c.arg(8)) wr32(c.arg(8), 2);
    return r;
}
API(advapi32, RegCloseKey, 1) { reg_handles.erase(c.arg(0)); return 0; }
API(advapi32, RegQueryValueExA, 6) {
    reg_load();
    std::string k = root_name(c.arg(0));
    std::string name = c.arg(1) ? up(gstr(c.arg(1))) : "";
    auto ki = reg.find(k);
    if (ki == reg.end()) return 2;
    auto vi = ki->second.find(name);
    if (vi == ki->second.end()) return 2;
    if (c.arg(3)) wr32(c.arg(3), vi->second.type);
    u32 need = (u32)vi->second.data.size();
    if (c.arg(5)) {
        u32 have = rd32(c.arg(5));
        wr32(c.arg(5), need);
        if (c.arg(4)) {
            if (have < need) return 234;  // ERROR_MORE_DATA
            memcpy(g2h(c.arg(4)), vi->second.data.data(), need);
        }
    }
    return 0;
}
API(advapi32, RegSetValueExA, 6) {
    reg_load();
    std::string k = root_name(c.arg(0));
    if (k.empty()) return 6;
    RegValue v; v.type = c.arg(3);
    u32 n = c.arg(5);
    v.data.assign(g2h(c.arg(4)), g2h(c.arg(4)) + n);
    reg[k][c.arg(1) ? up(gstr(c.arg(1))) : ""] = v;
    reg_save();
    return 0;
}
API(advapi32, RegDeleteValueA, 2) {
    reg[root_name(c.arg(0))].erase(c.arg(1) ? up(gstr(c.arg(1))) : "");
    reg_save();
    return 0;
}

// ---- shell32 / ole32 ------------------------------------------------------------------------
API(shell32, ShellExecuteA, 6) {
    const char* file = gstr(c.arg(2));
    logf("ShellExecuteA(%s)", file ? file : "");
    if (file && (!strncasecmp(file, "http://", 7) || !strncasecmp(file, "https://", 8))) SDL_OpenURL(file);
    return 42;
}
API(ole32, CoInitialize, 1) { return 0; }
API(ole32, CoUninitialize, 0) { return 0; }
API(ole32, CLSIDFromString, 2) { memset(g2h(c.arg(1)), 0, 16); return 0; }

// ---- ws2_32: no network. Online features fail gracefully. --------------------------------------
API(ws2_32, WSAStartup, 2) {
    u32 p = c.arg(1);
    if (p) { memset(g2h(p), 0, 400); wr16(p, 0x0202); wr16(p + 2, 0x0202); }
    return 0;
}
API(ws2_32, WSACleanup, 0) { return 0; }
API(ws2_32, WSAGetLastError, 0) { return 10050; }  // WSAENETDOWN
API(ws2_32, socket, 3) { return 0xFFFFFFFF; }
API(ws2_32, closesocket, 1) { return 0; }
API(ws2_32, connect, 3) { return 0xFFFFFFFF; }
API(ws2_32, send, 4) { return 0xFFFFFFFF; }
API(ws2_32, recv, 4) { return 0xFFFFFFFF; }
API(ws2_32, ioctlsocket, 3) { return 0; }
API(ws2_32, htons, 1) { u32 v = c.arg(0) & 0xFFFF; return ((v >> 8) | (v << 8)) & 0xFFFF; }
API(ws2_32, inet_addr, 1) { return 0xFFFFFFFF; }
static u32 ntoa_buf = 0;
API(ws2_32, inet_ntoa, 1) {
    if (!ntoa_buf) ntoa_buf = galloc(32);
    u32 a = c.arg(0);
    char s[32]; snprintf(s, 32, "%u.%u.%u.%u", a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24);
    gstrcpy(ntoa_buf, s, 32);
    return ntoa_buf;
}
API(ws2_32, WSAAsyncGetHostByName, 5) {
    // Report failure asynchronously: post the completion message with an error in the high word.
    plat::queue.push_back({c.arg(0), c.arg(1), 0x4242, (u32)11001 << 16});  // WSAHOST_NOT_FOUND
    return 0x4242;
}
API(ws2_32, WSACancelAsyncRequest, 1) { return 0; }

// ---- version.dll (used by crash reporter / D3DX probing) ----------------------------------------
API(version, GetFileVersionInfoSizeA, 2) { return 0; }
API(version, GetFileVersionInfoA, 4) { return 0; }
API(version, VerQueryValueA, 4) { return 0; }
