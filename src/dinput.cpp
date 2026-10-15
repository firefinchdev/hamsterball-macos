// DirectInput8: system keyboard and mouse backed by SDL state, plus game controllers
// (DualSense, DualShock, Xbox, MFi... over Bluetooth or USB) via SDL's GameController API.
#include "platform.h"
#include <cmath>
extern u8 g_script_keys[SDL_NUM_SCANCODES];

struct DIEvent { u32 ofs, data, time, seq; };
struct DIDev {
    bool mouse = false;
    bool joy = false;
    SDL_JoystickID joy_id = -1;        // SDL instance id of the controller
    SDL_GameController* gc = nullptr;
    i32 range_min = 0, range_max = 65535;  // DirectInput default axis range
    u32 deadzone = 1000;                   // 0..10000 (DIPROP_DEADZONE); 10% by default
    bool acquired = false;
    bool exclusive = false;
    u32 buffer_size = 0;
    std::deque<DIEvent> events;
};
static std::vector<DIDev> devs;
static u32 seq = 0;
static u32 vt_di = 0, vt_dev = 0;

void dinput_key_event(u32 dik, bool down) {
    for (auto& d : devs)
        if (!d.mouse && d.acquired && d.buffer_size) {
            d.events.push_back({dik, down ? 0x80u : 0u, SDL_GetTicks(), seq++});
            while (d.events.size() > d.buffer_size) d.events.pop_front();
        }
}
void dinput_mouse_event(u32 ofs, u32 data) {
    for (auto& d : devs)
        if (d.mouse && d.acquired && d.buffer_size) {
            d.events.push_back({ofs, data, SDL_GetTicks(), seq++});
            while (d.events.size() > d.buffer_size) d.events.pop_front();
        }
}

static const u8 GUID_SysMouse[16] = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static const u8 GUID_SysKeyboard[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};

// Game controllers get instance GUIDs {48424A00+id-0000-0000-hbwrap...} built from the SDL instance id.
static void joy_guid(SDL_JoystickID id, u8* g) {
    static const u8 tail[8] = {'h', 'b', 'w', 'r', 'a', 'p', 'J', 'S'};
    u32 d1 = 0x48424A00u + (u32)id;
    memcpy(g, &d1, 4); memset(g + 4, 0, 4); memcpy(g + 8, tail, 8);
}
static bool joy_from_guid(const u8* g, SDL_JoystickID& id) {
    u8 probe[16];
    u32 d1; memcpy(&d1, g, 4);
    if ((d1 & 0xFFFFFF00u) != 0x48424A00u) return false;
    id = (SDL_JoystickID)(d1 & 0xFF);
    joy_guid(id, probe);
    return !memcmp(probe, g, 16);
}
static std::string joy_name(SDL_JoystickID id) {
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) {
            const char* n = SDL_GameControllerNameForIndex(i);
            return n ? n : "Gamepad";
        }
    return "Gamepad";
}
// DIDEVICEINSTANCEA (580 bytes)
static void fill_device_instance(u32 p, SDL_JoystickID id) {
    u32 size = rd32(p) ? rd32(p) : 580;
    memset(g2h(p), 0, size);
    wr32(p, size);
    joy_guid(id, g2h(p + 4));
    joy_guid(id, g2h(p + 20));
    wr32(p + 36, 0x15 | (2 << 8));  // DI8DEVTYPE_GAMEPAD, standard gamepad
    std::string n = joy_name(id);
    gstrcpy(p + 40, n, 260);
    gstrcpy(p + 300, n, 260);
    if (size >= 580) { wr16(p + 576, 1); wr16(p + 578, 5); }  // HID usage page generic desktop / gamepad
}
static void init_controllers() {
    static bool done = false;
    if (done) return;
    done = true;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) { logf("gamepads unavailable: %s", SDL_GetError()); return; }
    if (getenv("HB_VIRTUAL_PAD")) {
        // test hook: a virtual gamepad driven by HB_SCRIPT "pad" commands
        extern SDL_Joystick* g_virtual_pad;
        SDL_VirtualJoystickDesc desc = {};
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        desc.name = "Virtual Test Pad";
        int idx = SDL_JoystickAttachVirtualEx(&desc);
        if (idx >= 0) g_virtual_pad = SDL_JoystickOpen(idx);
        logf("virtual pad: %s", g_virtual_pad ? "attached" : SDL_GetError());
    }
    // give Bluetooth controllers a moment to be reported
    for (int i = 0; i < 10 && SDL_NumJoysticks() == 0; i++) { SDL_PumpEvents(); SDL_Delay(10); }
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        logf("gamepad %d: %s%s", i, SDL_JoystickNameForIndex(i), SDL_IsGameController(i) ? "" : " (not a recognised game controller)");
}

API(dinput8, DirectInput8Create, 5) {
    init_controllers();
    if (!vt_di) {
        vt_di = make_vtable("IDirectInput8", {"QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices",
                                              "GetDeviceStatus", "RunControlPanel", "Initialize", "FindDevice",
                                              "EnumDevicesBySemantics", "ConfigureDevices"});
        vt_dev = make_vtable("IDirectInputDevice8", {"QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects",
                                                     "GetProperty", "SetProperty", "Acquire", "Unacquire", "GetDeviceState",
                                                     "GetDeviceData", "SetDataFormat", "SetEventNotification", "SetCooperativeLevel",
                                                     "GetObjectInfo", "GetDeviceInfo", "RunControlPanel", "Initialize",
                                                     "CreateEffect", "EnumEffects", "GetEffectInfo", "GetForceFeedbackState",
                                                     "SendForceFeedbackCommand", "EnumCreatedEffectObjects", "Escape", "Poll",
                                                     "SendDeviceData", "EnumEffectsInFile", "WriteEffectToFile", "BuildActionMap",
                                                     "SetActionMap", "GetImageInfo"});
    }
    wr32(c.arg(3), make_com_object(vt_di, 0));
    return 0;
}
COM(IDirectInput8, QueryInterface, 3) { wr32(c.arg(2), c.arg(0)); return 0; }
COM(IDirectInput8, AddRef, 1) { return 1; }
COM(IDirectInput8, Release, 1) { return 0; }
COM(IDirectInput8, CreateDevice, 4) {
    const u8* g = g2h(c.arg(1));
    DIDev d;
    SDL_JoystickID jid;
    if (!memcmp(g, GUID_SysMouse, 16)) d.mouse = true;
    else if (!memcmp(g, GUID_SysKeyboard, 16)) d.mouse = false;
    else if (joy_from_guid(g, jid)) {
        d.joy = true;
        d.joy_id = jid;
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            if (SDL_JoystickGetDeviceInstanceID(i) == jid) d.gc = SDL_GameControllerOpen(i);
        if (!d.gc) { wr32(c.arg(2), 0); return 0x80040154; }
        logf("DInput: opened gamepad '%s'", SDL_GameControllerName(d.gc));
    }
    else { wr32(c.arg(2), 0); return 0x80040154; }  // REGDB_E_CLASSNOTREG / not found
    devs.push_back(d);
    wr32(c.arg(2), make_com_object(vt_dev, (u32)devs.size() - 1));
    return 0;
}
COM(IDirectInput8, EnumDevices, 5) {
    u32 type = c.arg(1) & 0xFF, cb = c.arg(2), ref = c.arg(3);
    // DI8DEVCLASS_ALL (0), DI8DEVCLASS_GAMECTRL (4) or a specific joystick/gamepad type
    if (!(type == 0 || type == 4 || (type >= 0x14 && type <= 0x18))) return 0;
    platform_pump();
    std::vector<SDL_JoystickID> ids;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) ids.push_back(SDL_JoystickGetDeviceInstanceID(i));
    u32 inst = galloc(580);
    for (SDL_JoystickID id : ids) {
        wr32(inst, 580);
        fill_device_instance(inst, id);
        if (call_guest(cb, {inst, ref}) == 0) break;  // DIENUM_STOP
    }
    gfree(inst);
    return 0;
}
COM(IDirectInput8, GetDeviceStatus, 2) {
    SDL_JoystickID id;
    if (joy_from_guid(g2h(c.arg(1)), id)) return 0;
    return 1;  // DI_NOTATTACHED
}
COM(IDirectInput8, Initialize, 3) { return 0; }

static DIDev& dev(Ctx& c) { return devs[com_id(c.arg(0))]; }
COM(IDirectInputDevice8, QueryInterface, 3) { wr32(c.arg(2), c.arg(0)); return 0; }
COM(IDirectInputDevice8, AddRef, 1) { return 1; }
COM(IDirectInputDevice8, Release, 1) {
    DIDev& d = dev(c);
    if (d.mouse && d.acquired && d.exclusive) SDL_SetRelativeMouseMode(SDL_FALSE);
    d.acquired = false;
    if (d.gc) { SDL_GameControllerClose(d.gc); d.gc = nullptr; }
    return 0;
}
COM(IDirectInputDevice8, GetCapabilities, 2) {
    u32 p = c.arg(1);
    u32 sz = rd32(p);
    memset(g2h(p + 4), 0, sz - 4);
    wr32(p + 4, 1);  // DIDC_ATTACHED
    DIDev& d = dev(c);
    if (d.joy) {
        wr32(p + 8, 0x15 | (2 << 8));
        wr32(p + 12, 6); wr32(p + 16, 14); wr32(p + 20, 1);  // axes, buttons, POVs
        return 0;
    }
    wr32(p + 8, d.mouse ? 0x12 : 0x13);
    wr32(p + 12, d.mouse ? 3 : 0);    // axes
    wr32(p + 16, d.mouse ? 3 : 128);  // buttons
    return 0;
}

// Gamepad objects as DirectInput reports them for a standard gamepad (DIJOYSTATE layout).
struct JoyObj { u8 guid_lo; u32 ofs; u32 type; const char* name; };
static const JoyObj joy_axes[6] = {
    {0xE0, 0, 0x002, "X Axis"}, {0xE1, 4, 0x102, "Y Axis"}, {0xE2, 8, 0x202, "Z Axis"},
    {0xF4, 12, 0x302, "X Rotation"}, {0xF5, 16, 0x402, "Y Rotation"}, {0xE3, 20, 0x502, "Z Rotation"},
};
COM(IDirectInputDevice8, EnumObjects, 4) {
    DIDev& d = dev(c);
    u32 cb = c.arg(1), ref = c.arg(2), flags = c.arg(3);
    if (!d.joy) return 0;
    // DIDEVICEOBJECTINSTANCEA (316 bytes); guidType = {A36D02xx-C9F3-11CF-BFC7-444553540000}
    u32 o = galloc(316);
    auto emit = [&](u8 guid_lo, u32 ofs, u32 type, const char* name) -> bool {
        memset(g2h(o), 0, 316);
        wr32(o, 316);
        static const u8 g[16] = {0x00, 0x02, 0x6D, 0xA3, 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
        memcpy(g2h(o + 4), g, 16);
        wr8(o + 4, guid_lo);
        wr32(o + 20, ofs); wr32(o + 24, type); wr32(o + 28, 0);
        gstrcpy(o + 32, name, 260);
        return call_guest(cb, {o, ref}) != 0;
    };
    bool want_axes = flags == 0 || (flags & 3), want_buttons = flags == 0 || (flags & 0xC), want_pov = flags == 0 || (flags & 0x10);
    bool go = true;
    if (want_axes) for (auto& a : joy_axes) if (go) go = emit(a.guid_lo, a.ofs, a.type, a.name);
    if (want_pov && go) go = emit(0xF2, 32, 0x010, "Hat Switch");
    for (u32 b = 0; want_buttons && go && b < 14; b++) {
        char n[16]; snprintf(n, sizeof(n), "Button %u", b);
        go = emit(0xF0, 48 + b, 0x00C | (b << 8), n);
    }
    gfree(o);
    return 0;
}
COM(IDirectInputDevice8, GetProperty, 3) {
    DIDev& d = dev(c);
    u32 prop = c.arg(1), hdr = c.arg(2);
    if (prop == 4 /*DIPROP_RANGE*/) { wr32(hdr + 16, (u32)d.range_min); wr32(hdr + 20, (u32)d.range_max); }
    else if (prop == 5 /*DIPROP_DEADZONE*/) wr32(hdr + 16, d.deadzone);
    else if (prop == 1) wr32(hdr + 16, d.buffer_size);
    return 0;
}
COM(IDirectInputDevice8, SetProperty, 3) {
    DIDev& d = dev(c);
    u32 prop = c.arg(1), hdr = c.arg(2);
    if (prop == 1 /*DIPROP_BUFFERSIZE*/) d.buffer_size = rd32(hdr + 16);
    else if (prop == 4 /*DIPROP_RANGE*/) { d.range_min = (i32)rd32(hdr + 16); d.range_max = (i32)rd32(hdr + 20); }
    else if (prop == 5 /*DIPROP_DEADZONE*/) d.deadzone = std::min(10000u, rd32(hdr + 16));
    return 0;
}

// Fill a DIJOYSTATE / DIJOYSTATE2 from the SDL controller.
static void joy_state(DIDev& d, u32 p, u32 n) {
    SDL_GameController* gc = d.gc;
    auto raw = [&](SDL_GameControllerAxis a) { return gc ? SDL_GameControllerGetAxis(gc, a) / 32767.0f : 0.0f; };
    auto btn = [&](SDL_GameControllerButton b) { return gc && SDL_GameControllerGetButton(gc, b); };
    float dz = d.deadzone / 10000.0f;
    auto shape = [&](float v) {  // dead zone, rescaled so the live range still reaches full deflection
        float a = fabsf(v);
        if (a <= dz) return 0.0f;
        return (v < 0 ? -1.0f : 1.0f) * std::min(1.0f, (a - dz) / (1.0f - dz));
    };
    auto axis = [&](float v) {  // [-1,1] -> [range_min, range_max]
        float mid = (d.range_min + d.range_max) * 0.5f, half = (d.range_max - d.range_min) * 0.5f;
        return (u32)(i32)lroundf(mid + v * half);
    };
    float lx = shape(raw(SDL_CONTROLLER_AXIS_LEFTX)), ly = shape(raw(SDL_CONTROLLER_AXIS_LEFTY));
    bool up = btn(SDL_CONTROLLER_BUTTON_DPAD_UP), down = btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    bool left = btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT), right = btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    // the d-pad also steers when the left stick is centred
    if (lx == 0 && ly == 0) { lx = right ? 1.0f : left ? -1.0f : 0.0f; ly = down ? 1.0f : up ? -1.0f : 0.0f; }
    float lt = raw(SDL_CONTROLLER_AXIS_TRIGGERLEFT), rt = raw(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    wr32(p + 0, axis(lx));
    wr32(p + 4, axis(ly));
    wr32(p + 8, axis(rt - lt));
    wr32(p + 12, axis(shape(raw(SDL_CONTROLLER_AXIS_RIGHTX))));
    wr32(p + 16, axis(shape(raw(SDL_CONTROLLER_AXIS_RIGHTY))));
    wr32(p + 20, axis(0));
    // POV hat in hundredths of degrees, 0xFFFFFFFF = centred
    u32 pov = 0xFFFFFFFF;
    if (up) pov = right ? 4500 : left ? 31500 : 0;
    else if (down) pov = right ? 13500 : left ? 22500 : 18000;
    else if (right) pov = 9000;
    else if (left) pov = 27000;
    wr32(p + 32, pov);
    for (int i = 1; i < 4; i++) wr32(p + 32 + i * 4, 0xFFFFFFFF);
    // buttons in the order a DirectInput gamepad driver usually reports them
    const bool b[14] = {btn(SDL_CONTROLLER_BUTTON_A), btn(SDL_CONTROLLER_BUTTON_B), btn(SDL_CONTROLLER_BUTTON_X),
                        btn(SDL_CONTROLLER_BUTTON_Y), btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER),
                        btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), lt > 0.5f, rt > 0.5f, btn(SDL_CONTROLLER_BUTTON_BACK),
                        btn(SDL_CONTROLLER_BUTTON_START), btn(SDL_CONTROLLER_BUTTON_LEFTSTICK),
                        btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK), btn(SDL_CONTROLLER_BUTTON_GUIDE),
                        btn(SDL_CONTROLLER_BUTTON_TOUCHPAD)};
    for (u32 i = 0; i < 14 && 48 + i < n; i++) wr8(p + 48 + i, b[i] ? 0x80 : 0);
}
static void update_mouse_mode() {
    bool want = false;
    for (auto& d : devs) if (d.mouse && d.acquired && d.exclusive) want = true;
    if (getenv("HB_NO_RELATIVE")) want = false;
    if ((bool)SDL_GetRelativeMouseMode() != want) SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
}
COM(IDirectInputDevice8, Acquire, 1) {
    DIDev& d = dev(c);
    if (d.joy) { bool was = d.acquired; d.acquired = true; return was ? 1 : 0; }
    if (!plat::focused) return 0x8007000C;  // DIERR_OTHERAPPHASPRIO
    bool was = d.acquired;
    d.acquired = true;
    if (d.mouse) { if (!was) plat::mouse_dx = plat::mouse_dy = 0; update_mouse_mode(); }
    return was ? 1 /*S_FALSE*/ : 0;
}
COM(IDirectInputDevice8, Unacquire, 1) {
    DIDev& d = dev(c);
    d.acquired = false;
    d.events.clear();
    if (d.mouse) update_mouse_mode();
    return 0;
}
COM(IDirectInputDevice8, GetDeviceState, 3) {
    DIDev& d = dev(c);
    u32 n = c.arg(1), p = c.arg(2);
    if (!d.acquired) return 0x8007001E;  // DIERR_NOTACQUIRED
    if (d.joy) {
        if (!d.gc || !SDL_GameControllerGetAttached(d.gc)) return 0x8007001E;  // unplugged
        platform_pump();
        memset(g2h(p), 0, n);
        joy_state(d, p, n);
        return 0;
    }
    if (!plat::focused) { d.acquired = false; return 0x8007001E; }
    platform_pump();
    memset(g2h(p), 0, n);
    if (d.mouse) {
        wr32(p, (u32)plat::mouse_dx);
        wr32(p + 4, (u32)plat::mouse_dy);
        wr32(p + 8, (u32)plat::wheel_accum);
        plat::mouse_dx = plat::mouse_dy = plat::wheel_accum = 0;
        for (int b = 0; b < 3 && 12 + b < (int)n; b++) wr8(p + 12 + b, (plat::mouse_buttons >> b) & 1 ? 0x80 : 0);
    } else {
        const u8* ks = SDL_GetKeyboardState(nullptr);
        for (int s = 0; s < SDL_NUM_SCANCODES; s++)
            if (ks[s] || g_script_keys[s]) { u32 k = sdl_to_dik((SDL_Scancode)s); if (k && k < n) wr8(p + k, 0x80); }
    }
    return 0;
}
COM(IDirectInputDevice8, GetDeviceData, 5) {
    DIDev& d = dev(c);
    u32 objsz = c.arg(1), buf = c.arg(2), inout = c.arg(3), flags = c.arg(4);
    if (!d.acquired) return 0x8007001E;
    platform_pump();
    u32 want = rd32(inout), got = 0;
    if (!buf) {  // flush / count
        got = (u32)d.events.size();
        if (!(flags & 1 /*DIGDD_PEEK*/)) { u32 k = std::min(got, want); d.events.erase(d.events.begin(), d.events.begin() + k); got = k; }
        wr32(inout, got);
        return 0;
    }
    while (got < want && got < d.events.size()) {
        DIEvent& e = d.events[got];
        u32 o = buf + got * objsz;
        memset(g2h(o), 0, objsz);
        wr32(o, e.ofs); wr32(o + 4, e.data); wr32(o + 8, e.time); wr32(o + 12, e.seq);
        got++;
    }
    if (!(flags & 1)) d.events.erase(d.events.begin(), d.events.begin() + got);
    wr32(inout, got);
    return 0;
}
COM(IDirectInputDevice8, SetDataFormat, 2) { return 0; }
COM(IDirectInputDevice8, SetEventNotification, 2) { return 0; }
COM(IDirectInputDevice8, SetCooperativeLevel, 3) {
    dev(c).exclusive = (c.arg(2) & 1) != 0;  // DISCL_EXCLUSIVE
    logf("DInput %s SetCooperativeLevel(%x)", dev(c).joy ? "gamepad" : dev(c).mouse ? "mouse" : "keyboard", c.arg(2));
    return 0;
}
COM(IDirectInputDevice8, Poll, 1) {
    DIDev& d = dev(c);
    if (d.joy) return d.acquired ? 0 : 0x8007001E;
    return 1;  // DI_NOEFFECT
}
COM(IDirectInputDevice8, GetDeviceInfo, 2) {
    DIDev& d = dev(c);
    if (!d.joy) return 0x80004001;
    fill_device_instance(c.arg(1), d.joy_id);
    return 0;
}
