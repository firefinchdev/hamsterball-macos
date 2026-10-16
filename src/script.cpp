// Test automation: HB_SCRIPT=file with lines "<frame> <cmd> args...":
//   click X Y        left click at client coords
//   rclick X Y
//   mdown / mup      left button press / release at current position
//   move X Y         absolute mouse move (client coords)
//   rel DX DY        relative mouse motion
//   keydown NAME / keyup NAME / key NAME   (SDL scancode names, e.g. Up, Space, Return)
//   hold NAME N      hold key for N frames
//   shot PATH        screenshot (PPM)
//   pad axis N V / pad button N 0|1   drive the HB_VIRTUAL_PAD gamepad (SDL controller axis/button ids)
//   quit
#include "platform.h"
#include <fstream>
#include <sstream>
#include <unistd.h>

void d3d_screenshot(const char* path);

struct ScriptCmd { u32 frame; std::string cmd; std::vector<std::string> args; };
static std::vector<ScriptCmd> script;
static size_t script_pos = 0;
static bool script_loaded = false;
static std::map<SDL_Scancode, u32> held;
SDL_Joystick* g_virtual_pad = nullptr;  // key -> release frame
u8 g_script_keys[SDL_NUM_SCANCODES];

static void load() {
    script_loaded = true;
    const char* p = getenv("HB_SCRIPT");
    if (!p) return;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        ScriptCmd c; ss >> c.frame >> c.cmd;
        std::string a; while (ss >> a) c.args.push_back(a);
        script.push_back(c);
    }
}

static void push_mouse_msg(u32 msg) {
    plat::queue.push_back({plat::main_hwnd, msg, (u32)(msg == 0x201 ? 1 : msg == 0x204 ? 2 : 0), ((u32)(u16)plat::mouse_y << 16) | (u16)plat::mouse_x});
}
static void key_event(SDL_Scancode sc, bool down) {
    g_script_keys[sc] = down;
    if (u32 dik = sdl_to_dik(sc)) dinput_key_event(dik, down);
    if (u32 vk = sdl_to_vk(sc)) plat::queue.push_back({plat::main_hwnd, down ? 0x100u : 0x101u, vk, down ? 1u : 0xC0000001u});
}

void script_tick(u32 frame) {
    if (!script_loaded) load();
    for (auto it = held.begin(); it != held.end();) {
        if (frame >= it->second) { key_event(it->first, false); it = held.erase(it); } else ++it;
    }
    while (script_pos < script.size() && script[script_pos].frame <= frame) {
        ScriptCmd& c = script[script_pos++];
        logf("[script] frame %u: %s", frame, c.cmd.c_str());
        auto I = [&](int i) { return i < (int)c.args.size() ? atoi(c.args[i].c_str()) : 0; };
        if (c.cmd == "click" || c.cmd == "rclick") {
            plat::mouse_x = I(0); plat::mouse_y = I(1);
            push_mouse_msg(0x200);
            push_mouse_msg(c.cmd == "click" ? 0x201 : 0x204);
            push_mouse_msg(c.cmd == "click" ? 0x202 : 0x205);
        } else if (c.cmd == "mdown" || c.cmd == "mup") {
            bool down = c.cmd == "mdown";
            if (down) plat::mouse_buttons |= 1; else plat::mouse_buttons &= ~1u;
            push_mouse_msg(down ? 0x201 : 0x202);
        } else if (c.cmd == "move") {
            plat::mouse_dx += I(0) - plat::mouse_x; plat::mouse_dy += I(1) - plat::mouse_y;
            plat::mouse_x = I(0); plat::mouse_y = I(1);
            push_mouse_msg(0x200);
        } else if (c.cmd == "rel") {
            plat::mouse_dx += I(0); plat::mouse_dy += I(1);
            plat::mouse_x += I(0); plat::mouse_y += I(1);
            push_mouse_msg(0x200);
        } else if (c.cmd == "keydown" || c.cmd == "keyup" || c.cmd == "key" || c.cmd == "hold") {
            SDL_Scancode sc = SDL_GetScancodeFromName(c.args.empty() ? "" : c.args[0].c_str());
            if (sc == SDL_SCANCODE_UNKNOWN) { logf("[script] unknown key"); continue; }
            if (c.cmd == "keydown") key_event(sc, true);
            else if (c.cmd == "keyup") key_event(sc, false);
            else if (c.cmd == "key") { key_event(sc, true); held[sc] = frame + 2; }
            else { key_event(sc, true); held[sc] = frame + (u32)I(1); }
        } else if (c.cmd == "shot") {
            d3d_screenshot(c.args.empty() ? "shot.ppm" : c.args[0].c_str());
        } else if (c.cmd == "pad" && g_virtual_pad && c.args.size() >= 3) {
            if (c.args[0] == "axis") SDL_JoystickSetVirtualAxis(g_virtual_pad, I(1), (Sint16)I(2));
            else SDL_JoystickSetVirtualButton(g_virtual_pad, I(1), (Uint8)I(2));
        } else if (c.cmd == "quit") {
            extern void print_api_stats();
            if (getenv("HB_STATS")) print_api_stats();
            _exit(0);
        }
    }
}
