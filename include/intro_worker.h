// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
#pragma once
#include <windows.h>
#include <atomic>

// Process-lifetime owner: never destroy this object while its thread runs.
// Implementation is independent of engine/BLSE/Python and owns its window/Bink.
namespace engine_fixes_intro {
using Log = void (*)(const char*);
struct Config {
    HWND game_window=nullptr;
    RECT screen_rect={};
    HICON icon=nullptr; // borrowed; worker copies before owning/destroying an icon
    wchar_t media_root[MAX_PATH]={};
    wchar_t bink_path[MAX_PATH]={};
    Log log=nullptr; // caller supplies a thread-safe logger
    // Added for the cover handoff (optional; zero keeps the default). Upper bound for the
    // topmost cover phase after `handoff`; the worker then destroys its window on its own.
    // 0 = 3000 ms; clamped to 250..10000 ms.
    DWORD cover_timeout_ms=0;
    // No Bink loading/decoding: show a black initialization cover, then use the same handoff.
    bool skip_videos=false;
};
struct Worker {
    HANDLE first_frame=nullptr; // signaled only after first video frame is visible
    HANDLE failed=nullptr;      // pre-first-frame failure; parent chooses fallback
    HANDLE videos_done=nullptr; // all Bink buffers/videos closed, black window may remain
    HANDLE release=nullptr;     // main window is shown; worker can destroy its window
    HANDLE abort=nullptr;       // stop promptly, release Bink and destroy window
    HANDLE thread=nullptr;
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> close_requested{false}; // defer closing game to main after Init
    Config config;
    // Added for the cover handoff (optional; never setting `handoff` keeps the old behaviour).
    // Main sets `handoff` after videos_done and BEFORE showing/activating the game window.
    // While holding, the worker turns its window topmost (no activation, mouse clicks eaten),
    // waits for one DWM composition and sets `covering`. Main then shows/activates the game
    // under the cover and sets `release` once the game has presented real frames. If release
    // or abort does not arrive within cover_timeout_ms, the worker destroys its window itself.
    // Main must wait for covering OR thread exit with its own bound: a worker with no shown
    // window (failure/abort) never covers.
    HANDLE handoff=nullptr;  // main -> worker
    HANDLE covering=nullptr; // worker -> main: topmost cover is composed
};
// Must refuse reuse while an old thread remains active. No engine calls.
bool start(Worker& worker,const Config& config) noexcept;
// Only after the thread has exited; otherwise leave the worker intact.
void destroy(Worker& worker) noexcept;
}
