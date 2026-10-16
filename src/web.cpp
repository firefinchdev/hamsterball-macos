// Browser (Emscripten) specifics: yielding to the page, and keeping the game's saved files
// (registry.txt, settings, high scores) in IndexedDB across reloads.
#include "emu.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <filesystem>
#include <SDL.h>

namespace fs = std::filesystem;
static std::string game_dir;
static const char* PERSIST = "/persist";

// The game runs as one long call from main(); it yields at each frame and Sleep() so the
// browser can show the frame, deliver input and run the audio callback. A zero-length yield
// resumes through a MessageChannel message: setTimeout(0) is clamped to ~4ms, and the game
// calls Sleep(0) about 30 times a frame while it waits for its next tick.
EM_ASYNC_JS(void, js_fast_yield, (), {
    if (!Module.hbYield) {
        var ch = new MessageChannel(), waiting = [];
        ch.port1.onmessage = function() { var r = waiting.shift(); if (r) r(); };
        Module.hbYield = function() { return new Promise(function(r) { waiting.push(r); ch.port2.postMessage(0); }); };
    }
    await Module.hbYield();
});
static double last_frame = 0;
void web_frame_done() { last_frame = emscripten_get_now(); }
void web_yield(u32 ms) {
    if (ms) { emscripten_sleep(ms); return; }
    // The game ticks every ~11.4ms. Early in that wait a timer sleep (~4ms in practice) saves
    // CPU; close to the next tick, spin with fast yields so the tick isn't late.
    if (emscripten_get_now() - last_frame < 5.0) emscripten_sleep(1);
    else js_fast_yield();
}

EM_JS(void, js_mount_persist, (), {
    FS.mkdir('/persist');
    FS.mount(IDBFS, {}, '/persist');
    Module.hbSyncDone = 0;
    FS.syncfs(true, function(err) { if (err) console.warn('loading saves failed', err); Module.hbSyncDone = 1; });
});
EM_JS(int, js_sync_done, (), { return Module.hbSyncDone; });
EM_JS(void, js_persist_flush, (), {
    // coalesce bursts of writes into one IndexedDB sync
    if (Module.hbFlushTimer) return;
    Module.hbFlushTimer = setTimeout(function() {
        Module.hbFlushTimer = 0;
        FS.syncfs(false, function(err) { if (err) console.warn('saving failed', err); });
    }, 500);
});

// Mount the persistent store and lay its files over the freshly loaded game folder.
void web_init_storage(const std::string& dir) {
    game_dir = dir;
    js_mount_persist();
    for (int i = 0; i < 1000 && !js_sync_done(); i++) emscripten_sleep(10);
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(PERSIST, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file()) continue;
        fs::path rel = fs::relative(it->path(), PERSIST, ec);
        fs::path dst = fs::path(dir) / rel;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
        logf("restored saved file %s", rel.c_str());
    }
}

void file_written(const std::string& path) {
    if (game_dir.empty() || path.compare(0, game_dir.size(), game_dir) != 0) return;
    std::error_code ec;
    fs::path dst = fs::path(PERSIST) / path.substr(game_dir.size());
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(path, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) logf("could not save %s: %s", path.c_str(), ec.message().c_str());
    else { if (g_trace_api) logf("saved %s", path.c_str()); js_persist_flush(); }
}
#else
void file_written(const std::string&) {}
#endif
