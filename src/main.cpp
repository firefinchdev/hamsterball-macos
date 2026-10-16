#include "emu.h"
#include <SDL.h>
#include <unistd.h>
#include <climits>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

namespace fs = std::filesystem;
std::string g_log_path;

#ifdef __ANDROID__
// The game files ship as APK assets (game/... plus game_manifest.txt listing them). They are
// copied once to the app's private storage, where the game can read and write them.
static std::string android_game_dir() {
    std::string dir = std::string(SDL_AndroidGetInternalStoragePath()) + "/game";
    if (!fs::exists(dir + "/hamsterball.exe")) {
        SDL_RWops* m = SDL_RWFromFile("game_manifest.txt", "rb");
        if (!m) fatal("game files missing from the APK");
        std::string list((size_t)SDL_RWsize(m), '\0');
        SDL_RWread(m, list.data(), 1, list.size());
        SDL_RWclose(m);
        std::error_code ec;
        fs::remove_all(dir, ec);
        size_t pos = 0;
        std::vector<char> buf(1 << 16);
        while (pos < list.size()) {
            size_t nl = list.find('\n', pos);
            std::string rel = list.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? list.size() : nl + 1;
            if (rel.empty()) continue;
            std::string out = dir + "/" + rel;
            fs::create_directories(fs::path(out).parent_path(), ec);
            SDL_RWops* in = SDL_RWFromFile(("game/" + rel).c_str(), "rb");
            FILE* f = fopen(out.c_str(), "wb");
            if (!in || !f) fatal("could not extract %s", rel.c_str());
            size_t n;
            while ((n = SDL_RWread(in, buf.data(), 1, buf.size())) > 0) fwrite(buf.data(), 1, n, f);
            fclose(f);
            SDL_RWclose(in);
        }
    }
    return dir;
}

// Optional test/debug settings: lines of VAR=value in <external files dir>/hb_env.txt
// (push it with adb); relative paths (scripts, screenshots) resolve in that directory.
static void android_env() {
    const char* ext = SDL_AndroidGetExternalStoragePath();
    if (!ext) return;
    if (chdir(ext) != 0) return;
    std::ifstream f("hb_env.txt");
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos && line[0] != '#') setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 1);
    }
}
#endif

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
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // Back pauses (Escape) instead of quitting
    android_env();
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) fatal("SDL_Init: %s", SDL_GetError());
    std::string dir;
#if defined(__ANDROID__)
    dir = android_game_dir();
    seed_registry(dir);
#elif defined(__EMSCRIPTEN__)
    // game files are preloaded into /game; saves come back from IndexedDB
    dir = "/game";
    web_init_storage(dir + "/");
    seed_registry(dir);
#else
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
#endif
    char real[PATH_MAX];
    if (realpath(dir.c_str(), real)) dir = real;
    if (dir.back() != '/') dir += '/';
    g_game_dir = dir;
    g_trace_api = getenv("HB_TRACE") != nullptr;
    if (access((dir + "hamsterball.exe").c_str(), R_OK) != 0) fatal("hamsterball.exe not found in %s", dir.c_str());
    return emu_run(dir + "hamsterball.exe");
}
