#include "emu.h"
#include <SDL.h>
#include <unistd.h>
#include <climits>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

namespace fs = std::filesystem;
std::string g_log_path;

static bool is_file(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }

// First launch default: every race, Rodent Rumble arena and the Mirror Tournament unlocked.
static void seed_registry(const std::string& game) {
    std::string p = game + "/registry.txt";
    if (is_file(p)) return;
    std::ofstream f(p);
    f << "[HKCU\\SOFTWARE\\RAPTISOFT\\HAMSTERBALL]\n";
    for (const char* n : {"WARMUP", "BEGINNER", "INTERMEDIATE", "DIZZY", "TOWER", "UP", "NEON", "EXPERT", "ODD", "TOOB",
                          "WOBBLY", "GLASS", "SKY", "MASTER", "IMPOSSIBLE"})
        f << n << "RACE=3:01\n" << n << "ARENA=3:01\n";
    f << "MIRRORTOURNAMENT=3:01\n";
}

// When running from Hamsterball.app, the pristine game files live in Contents/Resources/game.
// They are copied once to ~/Library/Application Support/Hamsterball so settings, saves and
// high scores are writable and survive app updates.
static std::string app_game_dir(const std::string& bundled) {
    const char* home = getenv("HOME");
    std::string support = std::string(home ? home : "/tmp") + "/Library/Application Support/Hamsterball";
    std::string dir = support + "/game";
    if (!is_file(dir + "/hamsterball.exe")) {
        std::error_code ec;
        fs::create_directories(support, ec);
        fs::copy(bundled, dir, fs::copy_options::recursive, ec);
        if (ec) fatal("could not copy game files to %s: %s", dir.c_str(), ec.message().c_str());
    }
    seed_registry(dir);
    return dir;
}

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) fatal("SDL_Init: %s", SDL_GetError());
    std::string dir;
    if (argc > 1 && argv[1][0] != '-') dir = argv[1];
    else if (const char* e = getenv("HB_GAME_DIR")) dir = e;
    else {
        char* base = SDL_GetBasePath();
        std::string b = base ? base : "./";
        SDL_free(base);
        if (b.find(".app/Contents/Resources") != std::string::npos) {
            // app bundle: log to ~/Library/Logs and use the writable copy
            if (const char* home = getenv("HOME")) {
                std::error_code ec;
                fs::create_directories(std::string(home) + "/Library/Logs", ec);
                g_log_path = std::string(home) + "/Library/Logs/Hamsterball.log";
                if (!freopen(g_log_path.c_str(), "w", stderr)) g_log_path.clear();
                else setvbuf(stderr, nullptr, _IOLBF, 0);
            }
            dir = app_game_dir(b + "game");
        } else {
            dir = b + "game";
        }
    }
    char real[PATH_MAX];
    if (realpath(dir.c_str(), real)) dir = real;
    if (dir.back() != '/') dir += '/';
    g_game_dir = dir;
    g_trace_api = getenv("HB_TRACE") != nullptr;
    if (access((dir + "hamsterball.exe").c_str(), R_OK) != 0) fatal("hamsterball.exe not found in %s", dir.c_str());
    return emu_run(dir + "hamsterball.exe");
}
