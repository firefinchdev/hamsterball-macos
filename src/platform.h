// Host platform state shared between user32, d3d8 and dinput.
#pragma once
#include "emu.h"
#include <SDL.h>
#include <deque>

struct GuestMsg { u32 hwnd, msg, wparam, lparam; };

namespace plat {
extern SDL_Window* window;
extern SDL_GLContext gl;
extern u32 main_hwnd;
extern int client_w, client_h;   // guest-visible client size (back buffer size)
extern int mouse_x, mouse_y;     // in client coordinates
extern int mouse_dx, mouse_dy;   // accumulated relative motion since last DirectInput read
extern int wheel_accum;
extern u32 mouse_buttons;        // bit0 L, bit1 R, bit2 M
extern bool focused;
extern bool quit_requested;
extern std::deque<GuestMsg> queue;

void ensure_window(int w, int h);
void set_client_size(int w, int h);
// Map window pixel coordinates to client (back buffer) coordinates.
void window_to_client(float wx, float wy, int& cx, int& cy);
void client_to_window(int cx, int cy, int& wx, int& wy);
// Letterboxed viewport of the back buffer inside the drawable (pixels).
void present_rect(int& x, int& y, int& w, int& h);
}

// Windows virtual-key / DirectInput scan codes from SDL scancodes
u32 sdl_to_vk(SDL_Scancode sc);
u32 sdl_to_dik(SDL_Scancode sc);
void dinput_key_event(u32 dik, bool down);
void dinput_mouse_event(u32 ofs, u32 data);
