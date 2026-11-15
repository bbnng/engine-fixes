# Engine Fixes and Tweaks

A [BLSE](https://gitlab.com/bigbannng/bloodlines-script-extender) plugin for *Vampire: The Masquerade – Bloodlines* that fixes bugs in the game's engine and game code, makes startup and loading faster, and adds a few optional tweaks for NPC movement and eyes. It changes no game data (models, maps, scripts or text), so it works alongside content mods.

## What it covers

**Fixes**

| Setting | Fix |
|---|---|
| `restart_crash` | The game no longer crashes when the engine restarts (console `_restart`, some video settings) after you have played |
| `orphan_particles` | Discipline effects that outlive their owner are removed, including Presence's halo staying on a vampire after it dies |
| `eye_target_interpolation` | NPC eyes no longer jump the wrong way for a frame whenever they look somewhere new |
| `idle_sequence_consistency` | Idle fidgets play to the end instead of being swapped mid-animation |
| `same_tick_walk_phase` | An NPC that stops and restarts in the same tick keeps its walk cycle |
| `npc_idle_pool` | NPCs no longer pick the player character's idle animations |
| `npc_move_yaw_blend` | Turns off a debug setting the game ships with, which snaps NPC legs to a new walking direction |
| `npc_pose_interpolation` | NPC leg movement is blended every frame instead of updating 10 times a second |

**Speed** (`[Startup]` and `[Loading]`; made for the Unofficial Patch 12 game files)

| Setting | What it speeds up |
|---|---|
| `engine_fixes_intro.vtm` | Startup companion for the UP loader: the game starts loading at once instead of after the logo videos, behind a dark window (windowed mode; settings in `Bin/loader/engine_fixes_intro.ini`) |
| `startup_pack_prefix` | Indexes the game's packs once, so file look-ups at startup don't search every pack from start to end |
| `startup_sound_schemes`, `startup_sound_scheme_cache`, `startup_table_append` | Sound-scheme folders are listed once instead of twice; data tables are built without walking each list to its end |
| `server_sound_tables` | The first save load of a session builds the sound tables with the pack index (about 2 s faster) |
| `sound_precache_index`, `loose_stat_fast`, `model_name_cache` | Every load: faster sound, missing-file and model look-ups |
| `signon_fps_max` | The last part of a load, before you can play, is no longer held back by the game's frame limit |

`startup_trace` and `loading_trace` must stay on for these; they also write a few timing lines to `blse.log`.

**Tweaks** (on by default, each can be switched off)

| Setting | Tweak |
|---|---|
| `npc_steering_damping` | Smoother avoidance: no zig-zag, sidestep flicks or stalls when NPCs walk around each other or obstacles |
| `npc_move_yaw_smoothing` | Limits how fast the legs' walking-direction blend can change |
| `knox_relaxed_eyes` | Relaxes Knox Gorman's wide-open eyelids; how much is adjustable |
| `[EyeGlances]` | Calmer dialogue glances. The game's slow sweeps read as eye rolls; glances are now gentler, rarer and mostly sideways. All of it is adjustable, or glances can be switched off |

Every fix checks the game files and the exact code it changes before changing anything. Newer Unofficial Patch releases that edit the game's DLLs in place should keep the bug fixes and tweaks working without an update; the speed settings, the startup companion and `restart_crash` need the Unofficial Patch 12 files. On an unknown game build, or if another mod already patched the same code, that fix stays off and `blse.log` says why.

## Requirements

- *Bloodlines* with the Unofficial Patch (Basic or Plus)
- BLSE 0.6.1 or newer (0.6.3 or newer with Unofficial Patch 12.0 and later)

## Installing

Copy the `Bin` folder from the release archive into the game folder (next to `Vampire.exe`). Settings are in `Bin/loader/blse/mods/engine_fixes/engine_fixes.ini` (fixes, speed and tweaks) and `Bin/loader/engine_fixes_intro.ini` (startup companion). Settings files from 1.0 still work, but without `[Startup]` and `[Loading]` the speed settings stay off.

## Building

Requires Visual Studio with the C++ x86 tools and Python 3.

```
build.cmd                          builds build/engine_fixes.dll and build/engine_fixes_intro.vtm
python tools/package_release.py    writes the release and source archives to dist/
```


## License

GPL-3.0-only, with an attribution term and a permission to combine it with the game; see [LICENSE](LICENSE) and [NOTICE](NOTICE). `include/blse_api.h` is the BLSE plugin header (MIT, see `include/BLSE-LICENSE.txt`).
