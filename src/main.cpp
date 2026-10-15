#include "emu.h"
#include <SDL.h>
#include <unistd.h>
#include <climits>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace fs = std::filesystem;
extern char** environ;
std::string g_log_path;

static bool is_dir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static bool is_file(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// Run a program; returns its exit status and (optionally) its stdout.
static int run(const std::vector<std::string>& args, std::string* out = nullptr) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    int fds[2];
    if (pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    std::string data;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) data.append(buf, (size_t)n);
    close(fds[0]);
    if (rc != 0) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    if (out) *out = data;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int ask(const char* title, const std::string& text, std::vector<const char*> buttons) {
    std::vector<SDL_MessageBoxButtonData> b;
    for (size_t i = 0; i < buttons.size(); i++)
        b.push_back({(Uint32)(i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : i + 1 == buttons.size() ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0),
                     (int)i, buttons[i]});
    SDL_MessageBoxData d = {SDL_MESSAGEBOX_INFORMATION, nullptr, title, text.c_str(), (int)b.size(), b.data(), nullptr};
    int hit = -1;
    if (SDL_ShowMessageBox(&d, &hit) != 0) return -1;
    return hit;
}

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

static std::string find_game_exe(const std::string& root) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file() && lower(it->path().filename().string()) == "hamsterball.exe") return it->path().string();
    return "";
}

// Import the game from either the original installer (extracted with the bundled innoextract)
// or hamsterball.exe of an installed copy. Returns an error message, or "" on success.
static std::string import_game(const std::string& picked, const std::string& dest, const std::string& macos_dir) {
    std::error_code ec;
    std::string src_dir, tmp;
    if (lower(fs::path(picked).filename().string()) == "hamsterball.exe") {
        src_dir = fs::path(picked).parent_path().string();
    } else {
        std::string tool = macos_dir + "innoextract";
        if (!is_file(tool)) return "The bundled installer extractor is missing.";
        char tmpl[] = "/tmp/hamsterball-import-XXXXXX";
        if (!mkdtemp(tmpl)) return "Could not create a temporary folder.";
        tmp = tmpl;
        std::string out;
        if (run({tool, "-s", "-d", tmp, picked}, &out) != 0) {
            fs::remove_all(tmp, ec);
            return "That file isn't a Hamster Ball installer this app can read.\n\nChoose \"Hamster Ball Gold.exe\" (the setup program) or hamsterball.exe from an installed copy.";
        }
        std::string exe = find_game_exe(tmp);
        if (exe.empty()) { fs::remove_all(tmp, ec); return "The installer doesn't contain hamsterball.exe."; }
        src_dir = fs::path(exe).parent_path().string();
    }
    if (!is_dir(src_dir + "/Textures") && !is_dir(src_dir + "/textures")) {
        if (!tmp.empty()) fs::remove_all(tmp, ec);
        return "The game's data folders (Textures, Levels, ...) weren't found next to hamsterball.exe.";
    }
    fs::remove_all(dest, ec);
    fs::create_directories(fs::path(dest).parent_path(), ec);
    fs::copy(src_dir, dest, fs::copy_options::recursive, ec);
    if (!tmp.empty()) fs::remove_all(tmp, ec);
    if (ec) return "Copying the game files failed: " + ec.message();
    // drop installer leftovers
    for (auto& e : fs::directory_iterator(dest, ec)) {
        std::string n = lower(e.path().filename().string());
        if (n.rfind("unins", 0) == 0 || n == "desktop.ini" || e.path().extension() == ".url") fs::remove(e.path(), ec);
    }
    if (!is_file(dest + "/hamsterball.exe")) {
        std::string exe = find_game_exe(dest);
        if (!exe.empty()) fs::rename(exe, dest + "/hamsterball.exe", ec);
    }
    return "";
}

// App bundle: game files live in ~/Library/Application Support/Hamsterball/game. On the first
// launch they come from the player's own copy of the game (or, for personal builds, from
// Contents/Resources/game).
static std::string app_game_dir(const std::string& resources, const std::string& macos_dir) {
    const char* home = getenv("HOME");
    std::string support = std::string(home ? home : "/tmp") + "/Library/Application Support/Hamsterball";
    std::string dir = support + "/game";
    std::error_code ec;
    if (!is_file(dir + "/hamsterball.exe")) {
        if (is_dir(resources + "game")) {
            fs::create_directories(support, ec);
            fs::copy(resources + "game", dir, fs::copy_options::recursive, ec);
            if (ec) fatal("could not copy game files to %s: %s", dir.c_str(), ec.message().c_str());
        } else {
            std::string msg =
                "Hamster Ball needs the original Windows game, which isn't included.\n\n"
                "Choose the installer (\"Hamster Ball Gold.exe\") or hamsterball.exe from an installed copy. "
                "The game files are copied to ~/Library/Application Support/Hamsterball.";
            if (const char* imp = getenv("HB_IMPORT")) {  // non-interactive import (scripts/tests)
                std::string err = import_game(imp, dir, macos_dir);
                if (!err.empty()) fatal("import failed: %s", err.c_str());
            }
            while (!is_file(dir + "/hamsterball.exe")) {
                if (ask("Hamster Ball", msg, {"Choose File...", "Quit"}) != 0) exit(0);
                std::string picked;
                int rc = run({"/usr/bin/osascript", "-e",
                              "POSIX path of (choose file with prompt \"Choose Hamster Ball Gold.exe or hamsterball.exe\")"},
                             &picked);
                while (!picked.empty() && (picked.back() == '\n' || picked.back() == '\r')) picked.pop_back();
                if (rc != 0 || picked.empty()) continue;  // cancelled
                std::string err = import_game(picked, dir, macos_dir);
                if (!err.empty()) msg = err;
            }
        }
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
            std::string macos_dir = b.substr(0, b.find("Contents/Resources")) + "Contents/MacOS/";
            dir = app_game_dir(b, macos_dir);
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
