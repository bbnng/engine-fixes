# Engine Fixes

A [BLSE](https://gitlab.com/bigbannng/bloodlines-script-extender) plugin for *Vampire: The Masquerade – Bloodlines* that fixes bugs in the game's engine and game code, plus a few optional smoothing tweaks. It changes no game data (models, maps, scripts or text), so it works alongside content mods.

## What it covers

**Bug fixes**

| Setting | Fix |
|---|---|
| `orphan_particles` | Discipline effects that outlive their owner are removed, including Presence's halo staying on a vampire after it dies |
| `eye_target_interpolation` | NPC eyes no longer jump the wrong way for a frame whenever they look somewhere new |
| `idle_sequence_consistency` | Idle fidgets play to the end instead of being swapped mid-animation |
| `same_tick_walk_phase` | An NPC that stops and restarts in the same tick keeps its walk cycle |
| `npc_idle_pool` | NPCs no longer pick the player character's idle animations |
| `npc_move_yaw_blend` | Turns off a debug setting the game ships with, which snaps NPC legs to a new walking direction |
| `npc_pose_interpolation` | NPC leg movement is blended every frame instead of updating 10 times a second |

**Tweaks** (on by default, each can be switched off)

| Setting | Tweak |
|---|---|
| `npc_steering_damping` | Smoother avoidance: no zig-zag, sidestep flicks or stalls when NPCs walk around each other or obstacles |
| `npc_move_yaw_smoothing` | Limits how fast the legs' walking-direction blend can change |
| `[EyeGlances]` | Calmer dialogue glances. The game's slow sweeps read as eye rolls; glances are now gentler, rarer and mostly sideways. All of it is adjustable, or glances can be switched off |

Every fix checks the exact game files before changing anything. On an unknown game build, or if another mod already patched the same code, that fix stays off and `blse.log` says why.

## Requirements

- *Bloodlines* with the Unofficial Patch (Basic or Plus)
- BLSE 0.6.1 or newer

## Installing

Copy the `Bin` folder from the release archive into the game folder (next to `Vampire.exe`). Settings are in `Bin/loader/blse/mods/engine_fixes/engine_fixes.ini`.

## Building

Requires Visual Studio with the C++ x86 tools.

```
build.cmd                      builds build/engine_fixes.dll and runs the tests
python tools/package_release.py   writes dist/EngineFixes-<version>.zip
```

`include/blse_api.h` is the BLSE plugin header (MIT, see `include/BLSE-LICENSE.txt`).

## License

MIT, see `LICENSE`.
