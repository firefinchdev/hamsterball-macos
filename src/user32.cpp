// user32 / gdi32 replacement backed by a single SDL window.
#include "platform.h"
extern u8 g_script_keys[SDL_NUM_SCANCODES];
#include <algorithm>

namespace plat {
SDL_Window* window = nullptr;
SDL_GLContext gl = nullptr;
u32 main_hwnd = 0;
int client_w = 800, client_h = 600;
int mouse_x = 0, mouse_y = 0, mouse_dx = 0, mouse_dy = 0, wheel_accum = 0;
u32 mouse_buttons = 0;
bool focused = true;
bool quit_requested = false;
std::deque<GuestMsg> queue;

void ensure_window(int w, int h) {
    if (window) return;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int scale = 1;
    if (const char* s = getenv("HB_SCALE")) scale = std::max(1, atoi(s));
    window = SDL_CreateWindow("Hamsterball", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w * scale, h * scale,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);
    if (!window) fatal("SDL_CreateWindow: %s", SDL_GetError());
    gl = SDL_GL_CreateContext(window);
    if (!gl) fatal("SDL_GL_CreateContext: %s", SDL_GetError());
    // No vsync by default: the game paces itself (88 simulation ticks/s, one frame each), and
    // a 60 Hz swap wait would throw away a third of its frames. macOS composites windows, so
    // this doesn't tear; on ProMotion displays every frame is shown. HB_VSYNC=1 caps to the display.
    SDL_GL_SetSwapInterval(getenv("HB_VSYNC") ? atoi(getenv("HB_VSYNC")) : 0);
    client_w = w; client_h = h;
    SDL_StartTextInput();
}
void set_client_size(int w, int h) {
    if (w == client_w && h == client_h) return;
    client_w = w; client_h = h;
    if (window && !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
        int ww, wh; SDL_GetWindowSize(window, &ww, &wh);
        if (ww < w || wh < h) SDL_SetWindowSize(window, w, h);
    }
}
void present_rect(int& x, int& y, int& w, int& h) {
    int dw, dh; SDL_GL_GetDrawableSize(window, &dw, &dh);
    float s = std::min((float)dw / client_w, (float)dh / client_h);
    w = (int)(client_w * s); h = (int)(client_h * s);
    x = (dw - w) / 2; y = (dh - h) / 2;
}
void window_to_client(float wx, float wy, int& cx, int& cy) {
    int ww, wh, dw, dh;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    float px = wx * dw / ww, py = wy * dh / wh;
    int x, y, w, h; present_rect(x, y, w, h);
    cx = (int)((px - x) * client_w / w);
    cy = (int)((py - y) * client_h / h);
    cx = std::clamp(cx, 0, client_w - 1);
    cy = std::clamp(cy, 0, client_h - 1);
}
void client_to_window(int cx, int cy, int& wx, int& wy) {
    int ww, wh, dw, dh;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    int x, y, w, h; present_rect(x, y, w, h);
    float px = x + (cx + 0.5f) * w / client_w, py = y + (cy + 0.5f) * h / client_h;
    wx = (int)(px * ww / dw); wy = (int)(py * wh / dh);
}
}  // namespace plat
using namespace plat;

// ---- window classes / windows ------------------------------------------------------
struct WndClass { u32 wndproc; std::string name; };
struct Wnd { u32 wndproc; std::string cls; u32 style, exstyle; int x, y, w, h; u32 userdata = 0; u32 parent = 0; u32 id = 0; std::string text; };
static std::map<std::string, WndClass> classes;
static std::map<u32, Wnd> windows;
static u32 next_hwnd = 0x10010;
static u32 capture_hwnd = 0;

static std::string lc(std::string s) { for (auto& c : s) c = (char)tolower((u8)c); return s; }
static std::string class_name_arg(u32 a) { return a < 0x10000 ? "#" + std::to_string(a) : lc(gstr(a)); }

static u32 send_msg(u32 hwnd, u32 msg, u32 wp, u32 lp) {
    auto it = windows.find(hwnd);
    if (it == windows.end() || !it->second.wndproc) return 0;
    return call_guest(it->second.wndproc, {hwnd, msg, wp, lp});
}

API(user32, RegisterClassA, 1) {
    u32 p = c.arg(0);
    WndClass wc{rd32(p + 4), class_name_arg(rd32(p + 36))};
    classes[wc.name] = wc;
    return 0xC000 + (u32)classes.size();
}
API(user32, RegisterClassExA, 1) {
    u32 p = c.arg(0);
    WndClass wc{rd32(p + 8), class_name_arg(rd32(p + 40))};
    classes[wc.name] = wc;
    return 0xC000 + (u32)classes.size();
}
API(user32, CreateWindowExA, 12) {
    std::string cls = class_name_arg(c.arg(1));
    std::string title = c.arg(2) ? gstr(c.arg(2)) : "";
    Wnd w;
    w.cls = cls; w.text = title; w.exstyle = c.arg(0); w.style = c.arg(3);
    w.x = c.iarg(4); w.y = c.iarg(5); w.w = c.iarg(6); w.h = c.iarg(7);
    w.parent = c.arg(8); w.id = c.arg(9);
    auto ci = classes.find(cls);
    w.wndproc = ci != classes.end() ? ci->second.wndproc : 0;
    u32 hwnd = next_hwnd; next_hwnd += 0x10;
    logf("CreateWindowExA(class=%s, title=%s, style=%08x, %d,%d %dx%d, parent=%08x) -> %08x wndproc=%08x",
         cls.c_str(), title.c_str(), w.style, w.x, w.y, w.w, w.h, w.parent, hwnd, w.wndproc);
    if (!w.parent && ci != classes.end()) {
        if (!main_hwnd) {
            main_hwnd = hwnd;
            int cw = w.w > 0 && w.w < 4096 ? w.w : 800, ch = w.h > 0 && w.h < 4096 ? w.h : 600;
            ensure_window(cw, ch);
            if (!title.empty()) SDL_SetWindowTitle(window, title.c_str());
        }
    }
    windows[hwnd] = w;
    if (w.wndproc) {
        // WM_NCCREATE / WM_CREATE with a CREATESTRUCT
        u32 cs = galloc(48);
        wr32(cs + 0, c.arg(11)); wr32(cs + 4, c.arg(10)); wr32(cs + 8, c.arg(9)); wr32(cs + 12, c.arg(8));
        wr32(cs + 16, w.h); wr32(cs + 20, w.w); wr32(cs + 24, w.y); wr32(cs + 28, w.x);
        wr32(cs + 32, w.style); wr32(cs + 36, c.arg(2)); wr32(cs + 40, c.arg(1)); wr32(cs + 44, w.exstyle);
        send_msg(hwnd, 0x0081, 0, cs);  // WM_NCCREATE
        if ((i32)send_msg(hwnd, 0x0001, 0, cs) == -1) { windows.erase(hwnd); gfree(cs); return 0; }
        gfree(cs);
        send_msg(hwnd, 0x0005, 0, ((u32)client_h << 16) | (u32)client_w);  // WM_SIZE
        if (hwnd == main_hwnd) {
            // Windows activates a newly shown top-level window
            queue.push_back({hwnd, 0x001C /*WM_ACTIVATEAPP*/, 1, 0});
            queue.push_back({hwnd, 0x0006 /*WM_ACTIVATE*/, 1, 0});
            queue.push_back({hwnd, 0x0007 /*WM_SETFOCUS*/, 0, 0});
        }
    }
    return hwnd;
}
API(user32, DestroyWindow, 1) {
    u32 h = c.arg(0);
    if (!windows.count(h)) return 0;
    send_msg(h, 0x0002, 0, 0);  // WM_DESTROY
    send_msg(h, 0x0082, 0, 0);  // WM_NCDESTROY
    windows.erase(h);
    return 1;
}
API(user32, ShowWindow, 2) {
    if (c.arg(0) == main_hwnd && window) {
        if (c.arg(1) == 0) {} else SDL_ShowWindow(window);
    }
    return 1;
}
API(user32, UpdateWindow, 1) { return 1; }
API(user32, SetFocus, 1) { return main_hwnd; }
API(user32, GetFocus, 0) { return main_hwnd; }
API(user32, GetActiveWindow, 0) { return main_hwnd; }
API(user32, GetForegroundWindow, 0) { return main_hwnd; }
API(user32, SetForegroundWindow, 1) { return 1; }
API(user32, IsIconic, 1) { return 0; }
API(user32, IsWindowVisible, 1) { return 1; }
API(user32, FindWindowA, 2) { return 0; }
API(user32, EnableWindow, 2) { return 0; }
API(user32, GetDlgCtrlID, 1) { auto it = windows.find(c.arg(0)); return it == windows.end() ? 0 : it->second.id; }
API(user32, SetWindowTextA, 2) {
    auto it = windows.find(c.arg(0));
    if (it != windows.end()) it->second.text = gstr(c.arg(1));
    if (c.arg(0) == main_hwnd && window) SDL_SetWindowTitle(window, gstr(c.arg(1)));
    return 1;
}
API(user32, GetWindowLongA, 2) {
    auto it = windows.find(c.arg(0));
    if (it == windows.end()) return 0;
    switch (c.iarg(1)) {
        case -4: return it->second.wndproc;   // GWL_WNDPROC
        case -16: return it->second.style;
        case -20: return it->second.exstyle;
        case -21: return it->second.userdata;  // GWL_USERDATA
        case -12: return it->second.id;
    }
    return 0;
}
API(user32, SetWindowLongA, 3) {
    auto it = windows.find(c.arg(0));
    if (it == windows.end()) return 0;
    u32 old = 0, v = c.arg(2);
    switch (c.iarg(1)) {
        case -4: old = it->second.wndproc; it->second.wndproc = v; break;
        case -16: old = it->second.style; it->second.style = v; break;
        case -20: old = it->second.exstyle; it->second.exstyle = v; break;
        case -21: old = it->second.userdata; it->second.userdata = v; break;
    }
    return old;
}
API(user32, SetWindowPos, 7) {
    u32 flags = c.arg(6);
    if (c.arg(0) == main_hwnd && !(flags & 1 /*SWP_NOSIZE*/) && c.iarg(4) > 0 && c.iarg(5) > 0) {
        // don't resize the host window; the client size follows the D3D back buffer
    }
    return 1;
}
API(user32, MoveWindow, 6) { return 1; }
API(user32, AdjustWindowRect, 3) { return 1; }
API(user32, AdjustWindowRectEx, 4) { return 1; }
API(user32, GetClientRect, 2) {
    u32 r = c.arg(1);
    auto it = windows.find(c.arg(0));
    int w = client_w, h = client_h;
    if (c.arg(0) != main_hwnd && it != windows.end()) { w = it->second.w; h = it->second.h; }
    wr32(r, 0); wr32(r + 4, 0); wr32(r + 8, w); wr32(r + 12, h);
    return 1;
}
API(user32, GetWindowRect, 2) {
    u32 r = c.arg(1);
    wr32(r, 0); wr32(r + 4, 0); wr32(r + 8, client_w); wr32(r + 12, client_h);
    return 1;
}
API(user32, ClientToScreen, 2) { return 1; }  // client origin == screen origin
API(user32, ScreenToClient, 2) { return 1; }
API(user32, GetSystemMetrics, 1) {
    switch (c.arg(0)) {
        case 0: return client_w;   // SM_CXSCREEN
        case 1: return client_h;
        case 4: return 0;          // caption
        case 5: case 6: return 0;  // borders
        case 7: case 8: return 0;
        case 32: case 33: return 0;
    }
    return 0;
}
API(user32, GetDesktopWindow, 0) { return 0x10000; }
API(user32, GetDC, 1) { return 0xDC000001; }
API(user32, ReleaseDC, 2) { return 1; }

// ---- cursor ---------------------------------------------------------------------------
static bool cursor_shown = true;
API(user32, LoadCursorA, 2) { return 0xC0000000 | (c.arg(1) & 0xFFFF); }
API(user32, LoadIconA, 2) { return 0x1C000001; }
API(user32, SetCursor, 1) {
    bool show = c.arg(0) != 0;
    if (show != cursor_shown) { SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); cursor_shown = show; }
    return 0xC0007F00;
}
API(user32, ShowCursor, 1) {
    static int count = 0;
    count += c.arg(0) ? 1 : -1;
    SDL_ShowCursor(count >= 0 ? SDL_ENABLE : SDL_DISABLE);
    cursor_shown = count >= 0;
    return (u32)count;
}
API(user32, GetCursorPos, 1) { wr32(c.arg(0), mouse_x); wr32(c.arg(0) + 4, mouse_y); return 1; }
// The game steers the ball by re-centring the cursor every frame (SetCursorPos) and
// reading how far it moved. On macOS that is done with SDL relative mouse mode and a
// virtual cursor; we fall back to the normal cursor once the game stops re-centring.
static u32 last_setcursorpos = 0;
static bool virtual_cursor = false;
static float mouse_sens = -1;
static void set_virtual_cursor(bool on) {
    if (on == virtual_cursor) return;
    virtual_cursor = on;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    if (!on && window && focused) {
        int wx, wy; client_to_window(mouse_x, mouse_y, wx, wy);
        SDL_WarpMouseInWindow(window, wx, wy);
    }
}
API(user32, SetCursorPos, 2) {
    mouse_x = c.iarg(0); mouse_y = c.iarg(1);
    last_setcursorpos = SDL_GetTicks();
    if (window && focused && !getenv("HB_NO_RELATIVE")) set_virtual_cursor(true);
    return 1;
}
void cursor_tick() {
    if (virtual_cursor && (!focused || SDL_GetTicks() - last_setcursorpos > 250)) set_virtual_cursor(false);
}
void virtual_mouse_motion(int xrel, int yrel) {
    if (mouse_sens < 0) { const char* e = getenv("HB_MOUSE_SENS"); mouse_sens = e ? (float)atof(e) : 1.0f; }
    static float fx = 0, fy = 0;
    fx += xrel * mouse_sens; fy += yrel * mouse_sens;
    int dx = (int)fx, dy = (int)fy;
    fx -= dx; fy -= dy;
    mouse_x += dx; mouse_y += dy;
}
API(user32, WindowFromPoint, 2) { return main_hwnd; }
API(user32, SetCapture, 1) { u32 o = capture_hwnd; capture_hwnd = c.arg(0); SDL_CaptureMouse(SDL_TRUE); return o; }
API(user32, ReleaseCapture, 0) { capture_hwnd = 0; SDL_CaptureMouse(SDL_FALSE); return 1; }
API(user32, GetCapture, 0) { return capture_hwnd; }
API(user32, ClipCursor, 1) { return 1; }
API(user32, GetAsyncKeyState, 1) {
    const u8* ks = SDL_GetKeyboardState(nullptr);
    u32 vk = c.arg(0);
    if (vk == 1) return (mouse_buttons & 1) ? 0x8000 : 0;
    if (vk == 2) return (mouse_buttons & 2) ? 0x8000 : 0;
    if (vk == 4) return (mouse_buttons & 4) ? 0x8000 : 0;
    for (int s = 0; s < SDL_NUM_SCANCODES; s++)
        if ((ks[s] || g_script_keys[s]) && sdl_to_vk((SDL_Scancode)s) == vk) return 0x8000;
    return 0;
}
API(user32, GetKeyState, 1) { return api_user32_GetAsyncKeyState(c); }

// ---- messages ------------------------------------------------------------------------
struct Timer { u32 hwnd, id, interval, proc; u32 next; };
static std::vector<Timer> timers;

static u32 key_lparam(SDL_Scancode sc, bool up, bool repeat) {
    u32 scan = sdl_to_dik(sc) & 0x7F;
    u32 lp = 1 | (scan << 16);
    if (sdl_to_dik(sc) & 0x80) lp |= 1 << 24;
    if (repeat || up) lp |= 1u << 30;
    if (up) lp |= 1u << 31;
    return lp;
}
static u32 mk_flags() {
    u32 f = 0;
    if (mouse_buttons & 1) f |= 1; if (mouse_buttons & 2) f |= 2; if (mouse_buttons & 4) f |= 0x10;
    const u8* ks = SDL_GetKeyboardState(nullptr);
    if (ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT]) f |= 4;
    if (ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL]) f |= 8;
    return f;
}
static void post(u32 msg, u32 wp, u32 lp) { queue.push_back({main_hwnd, msg, wp, lp}); }

void cursor_tick();
void virtual_mouse_motion(int xrel, int yrel);
void platform_pump() {
    if (!window) { SDL_PumpEvents(); return; }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: post(0x0010 /*WM_CLOSE*/, 0, 0); break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    focused = true; post(0x001C /*WM_ACTIVATEAPP*/, 1, 0); post(0x0006 /*WM_ACTIVATE*/, 1, 0);
                } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    focused = false; post(0x0006, 0, 0); post(0x001C, 0, 0);
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                bool up = e.type == SDL_KEYUP;
                SDL_Scancode sc = e.key.keysym.scancode;
                // Cmd+Q / Cmd+W quit; Cmd+F / Alt+Enter toggles fullscreen
                if (!up && (e.key.keysym.mod & KMOD_GUI) && (sc == SDL_SCANCODE_Q || sc == SDL_SCANCODE_W)) { post(0x0010, 0, 0); break; }
                if (!up && (((e.key.keysym.mod & KMOD_GUI) && sc == SDL_SCANCODE_F) || ((e.key.keysym.mod & KMOD_ALT) && sc == SDL_SCANCODE_RETURN))) {
                    bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                }
                if (!e.key.repeat) if (u32 dik = sdl_to_dik(sc)) dinput_key_event(dik, !up);
                u32 vk = sdl_to_vk(sc);
                if (!vk) break;
                post(up ? 0x0101 : 0x0100, vk, key_lparam(sc, up, e.key.repeat));
                break;
            }
            case SDL_TEXTINPUT:
                for (const char* p = e.text.text; *p; p++) if ((u8)*p < 0x80) post(0x0102 /*WM_CHAR*/, (u8)*p, 1);
                break;
            case SDL_MOUSEMOTION:
                if (e.motion.xrel) dinput_mouse_event(0, (u32)e.motion.xrel);
                if (e.motion.yrel) dinput_mouse_event(4, (u32)e.motion.yrel);
                if (SDL_GetRelativeMouseMode()) {
                    mouse_dx += e.motion.xrel; mouse_dy += e.motion.yrel;
                    virtual_mouse_motion(e.motion.xrel, e.motion.yrel);
                    post(0x0200, mk_flags(), ((u32)(u16)mouse_y << 16) | (u16)mouse_x);
                } else {
                    window_to_client((float)e.motion.x, (float)e.motion.y, mouse_x, mouse_y);
                    mouse_dx += e.motion.xrel; mouse_dy += e.motion.yrel;
                    post(0x0200, mk_flags(), ((u32)(u16)mouse_y << 16) | (u16)mouse_x);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                bool down = e.type == SDL_MOUSEBUTTONDOWN;
                int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
                if (b < 0) break;
                if (down) mouse_buttons |= 1u << b; else mouse_buttons &= ~(1u << b);
                dinput_mouse_event(12 + b, down ? 0x80 : 0);
                if (!SDL_GetRelativeMouseMode()) window_to_client((float)e.button.x, (float)e.button.y, mouse_x, mouse_y);
                static const u32 msgs[3][2] = {{0x0202, 0x0201}, {0x0205, 0x0204}, {0x0208, 0x0207}};
                u32 lp = ((u32)(u16)mouse_y << 16) | (u16)mouse_x;
                post(msgs[b][down], mk_flags(), lp);
                if (down && e.button.clicks == 2) post(msgs[b][1] + 2 /*DBLCLK*/, mk_flags(), lp);
                break;
            }
            case SDL_MOUSEWHEEL:
                wheel_accum += e.wheel.y * 120;
                dinput_mouse_event(8, (u32)(e.wheel.y * 120));
                post(0x020A, ((u32)(u16)(e.wheel.y * 120) << 16) | mk_flags(), ((u32)(u16)mouse_y << 16) | (u16)mouse_x);
                break;
        }
    }
    cursor_tick();
    u32 now = SDL_GetTicks();
    for (auto& t : timers) {
        if ((i32)(now - t.next) >= 0) {
            t.next = now + std::max(t.interval, 1u);
            bool pending = false;
            for (auto& m : queue) if (m.msg == 0x0113 && m.wparam == t.id) pending = true;
            if (!pending) queue.push_back({t.hwnd, 0x0113, t.id, t.proc});
        }
    }
}

static bool get_message(u32 msgp, u32 hwnd, u32 lo, u32 hi, bool remove) {
    platform_pump();
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (lo || hi) { if (it->msg < lo || it->msg > hi) continue; }
        GuestMsg m = *it;
        wr32(msgp, m.hwnd); wr32(msgp + 4, m.msg); wr32(msgp + 8, m.wparam); wr32(msgp + 12, m.lparam);
        wr32(msgp + 16, SDL_GetTicks()); wr32(msgp + 20, mouse_x); wr32(msgp + 24, mouse_y);
        if (remove) queue.erase(it);
        return true;
    }
    return false;
}
API(user32, PeekMessageA, 5) { return get_message(c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4) & 1) ? 1 : 0; }
API(user32, GetMessageA, 4) {
    while (!get_message(c.arg(0), c.arg(1), c.arg(2), c.arg(3), true)) SDL_WaitEventTimeout(nullptr, 10);
    return rd32(c.arg(0) + 4) == 0x0012 ? 0 : 1;
}
API(user32, TranslateMessage, 1) {
    u32 m = c.arg(0);
    if (rd32(m + 4) == 0x0100) {
        u32 vk = rd32(m + 8);
        if (vk == 8 || vk == 13 || vk == 27 || vk == 9) queue.push_front({rd32(m), 0x0102, vk, rd32(m + 12)});
    }
    return 1;
}
API(user32, DispatchMessageA, 1) {
    u32 m = c.arg(0);
    u32 hwnd = rd32(m), msg = rd32(m + 4);
    if (msg == 0x0113 && rd32(m + 12)) return call_guest(rd32(m + 12), {hwnd, msg, rd32(m + 8), SDL_GetTicks()});
    return send_msg(hwnd ? hwnd : main_hwnd, msg, rd32(m + 8), rd32(m + 12));
}
API(user32, SendMessageA, 4) { return send_msg(c.arg(0), c.arg(1), c.arg(2), c.arg(3)); }
API(user32, PostMessageA, 4) { queue.push_back({c.arg(0), c.arg(1), c.arg(2), c.arg(3)}); return 1; }
API(user32, PostQuitMessage, 1) { queue.push_back({0, 0x0012, c.arg(0), 0}); return 0; }
API(user32, DefWindowProcA, 4) {
    u32 hwnd = c.arg(0), msg = c.arg(1);
    switch (msg) {
        case 0x0010: send_msg(hwnd, 0x0002, 0, 0); windows.erase(hwnd); return 0;  // WM_CLOSE -> destroy
        case 0x0081: return 1;  // WM_NCCREATE
        case 0x0020: return 0;  // WM_SETCURSOR
        case 0x0084: return 1;  // WM_NCHITTEST -> HTCLIENT
    }
    return 0;
}
API(user32, SetTimer, 4) {
    u32 id = c.arg(1) ? c.arg(1) : 0x7000 + (u32)timers.size();
    for (auto& t : timers) if (t.hwnd == c.arg(0) && t.id == id) { t.interval = c.arg(2); t.proc = c.arg(3); return id; }
    timers.push_back({c.arg(0), id, c.arg(2), c.arg(3), SDL_GetTicks() + c.arg(2)});
    return id;
}
API(user32, KillTimer, 2) {
    timers.erase(std::remove_if(timers.begin(), timers.end(), [&](const Timer& t) { return t.hwnd == c.arg(0) && t.id == c.arg(1); }), timers.end());
    return 1;
}

API(user32, MessageBoxA, 4) {
    const char* text = gstr(c.arg(1));
    const char* cap = gstr(c.arg(2));
    u32 type = c.arg(3) & 0xF;
    logf("MessageBox [%s]: %s (type %u)", cap ? cap : "", text ? text : "", type);
    SDL_MessageBoxButtonData btns[3];
    int n = 0;
    switch (type) {
        case 1: btns[n++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "OK"}; btns[n++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"}; break;
        case 3: btns[n++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 6, "Yes"}; btns[n++] = {0, 7, "No"}; btns[n++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"}; break;
        case 4: btns[n++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 6, "Yes"}; btns[n++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 7, "No"}; break;
        case 5: btns[n++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 4, "Retry"}; btns[n++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"}; break;
        default: btns[n++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "OK"}; break;
    }
    SDL_MessageBoxData d = {SDL_MESSAGEBOX_INFORMATION, window, cap ? cap : "", text ? text : "", n, btns, nullptr};
    int hit = 1;
    if (SDL_ShowMessageBox(&d, &hit) != 0) hit = btns[0].buttonid;
    return (u32)hit;
}
API(user32, MessageBeep, 1) { return 1; }

// clipboard
static u32 clip_buf = 0;
API(user32, OpenClipboard, 1) { return 1; }
API(user32, CloseClipboard, 0) { return 1; }
API(user32, GetClipboardData, 1) {
    if (c.arg(0) != 1 || !SDL_HasClipboardText()) return 0;
    char* t = SDL_GetClipboardText();
    if (clip_buf) gfree(clip_buf);
    clip_buf = gstrdup(t ? t : "");
    SDL_free(t);
    return clip_buf;
}

// painting (only used by fallback/error windows)
API(user32, BeginPaint, 2) { memset(g2h(c.arg(1)), 0, 64); wr32(c.arg(1), 0xDC000001); return 0xDC000001; }
API(user32, EndPaint, 2) { return 1; }
API(user32, InvalidateRect, 3) { return 1; }
API(user32, FillRect, 3) { return 1; }
API(user32, DrawTextA, 5) { return 16; }
API(user32, GetSysColorBrush, 1) { return 0xB0000000 | c.arg(0); }
API(user32, wsprintfA, -1) { return 0; }

API(gdi32, DeleteObject, 1) { return 1; }
API(gdi32, SetTextColor, 2) { return 0; }
API(gdi32, SetBkColor, 2) { return 0; }
API(gdi32, CreateSolidBrush, 1) { return 0xB1000000; }
API(gdi32, GetDeviceCaps, 2) {
    switch (c.arg(1)) {
        case 8: return client_w; case 10: return client_h; case 12: return 32; case 116: return 60;
    }
    return 0;
}

// ---- key maps --------------------------------------------------------------------------
u32 sdl_to_vk(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 0x70 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return 0x61 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
        case SDL_SCANCODE_ESCAPE: return 0x1B;
        case SDL_SCANCODE_BACKSPACE: return 0x08;
        case SDL_SCANCODE_TAB: return 0x09;
        case SDL_SCANCODE_SPACE: return 0x20;
        case SDL_SCANCODE_LEFT: return 0x25; case SDL_SCANCODE_UP: return 0x26;
        case SDL_SCANCODE_RIGHT: return 0x27; case SDL_SCANCODE_DOWN: return 0x28;
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
        case SDL_SCANCODE_PAUSE: return 0x13;
        case SDL_SCANCODE_CAPSLOCK: return 0x14;
        case SDL_SCANCODE_PAGEUP: return 0x21; case SDL_SCANCODE_PAGEDOWN: return 0x22;
        case SDL_SCANCODE_END: return 0x23; case SDL_SCANCODE_HOME: return 0x24;
        case SDL_SCANCODE_INSERT: return 0x2D; case SDL_SCANCODE_DELETE: return 0x2E;
        case SDL_SCANCODE_KP_0: return 0x60;
        case SDL_SCANCODE_KP_MULTIPLY: return 0x6A; case SDL_SCANCODE_KP_PLUS: return 0x6B;
        case SDL_SCANCODE_KP_MINUS: return 0x6D; case SDL_SCANCODE_KP_PERIOD: return 0x6E; case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
        case SDL_SCANCODE_SEMICOLON: return 0xBA; case SDL_SCANCODE_EQUALS: return 0xBB; case SDL_SCANCODE_COMMA: return 0xBC;
        case SDL_SCANCODE_MINUS: return 0xBD; case SDL_SCANCODE_PERIOD: return 0xBE; case SDL_SCANCODE_SLASH: return 0xBF;
        case SDL_SCANCODE_GRAVE: return 0xC0; case SDL_SCANCODE_LEFTBRACKET: return 0xDB; case SDL_SCANCODE_BACKSLASH: return 0xDC;
        case SDL_SCANCODE_RIGHTBRACKET: return 0xDD; case SDL_SCANCODE_APOSTROPHE: return 0xDE;
        default: return 0;
    }
}
u32 sdl_to_dik(SDL_Scancode sc) {
    static const u8 letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                   0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return letters[sc - SDL_SCANCODE_A];
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return 0x02 + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) return 0x3B + (sc - SDL_SCANCODE_F1);
    switch (sc) {
        case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
        case SDL_SCANCODE_ESCAPE: return 0x01; case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
        case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
        case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_LCTRL: return 0x1D; case SDL_SCANCODE_SEMICOLON: return 0x27;
        case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29; case SDL_SCANCODE_LSHIFT: return 0x2A;
        case SDL_SCANCODE_BACKSLASH: return 0x2B; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
        case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
        case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return 0x3A;
        case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
        case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48; case SDL_SCANCODE_KP_9: return 0x49;
        case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_4: return 0x4B; case SDL_SCANCODE_KP_5: return 0x4C;
        case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_PLUS: return 0x4E; case SDL_SCANCODE_KP_1: return 0x4F;
        case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
        case SDL_SCANCODE_KP_PERIOD: return 0x53;
        case SDL_SCANCODE_KP_ENTER: return 0x9C; case SDL_SCANCODE_RCTRL: return 0x9D; case SDL_SCANCODE_KP_DIVIDE: return 0xB5;
        case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_HOME: return 0xC7; case SDL_SCANCODE_UP: return 0xC8;
        case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_LEFT: return 0xCB; case SDL_SCANCODE_RIGHT: return 0xCD;
        case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_DOWN: return 0xD0; case SDL_SCANCODE_PAGEDOWN: return 0xD1;
        case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_DELETE: return 0xD3; case SDL_SCANCODE_PAUSE: return 0xC5;
        default: return 0;
    }
}
