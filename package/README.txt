Engine Fixes and Tweaks 1.2
for Vampire: The Masquerade - Bloodlines
==========================================

Fixes bugs in Bloodlines' engine and game code, makes startup and loading faster, and adds
optional tweaks for NPC movement and eyes. It changes no game data files (models, maps,
scripts or text), so it works alongside content mods.


REQUIREMENTS
------------
- Vampire: The Masquerade - Bloodlines with the Unofficial Patch (Basic or Plus).
- BLSE (Bloodlines Script Extender) 0.6.1 or newer.
- With Unofficial Patch 12.0 or newer: BLSE 0.6.3 or newer. With an older BLSE, the
  fixes for NPCs and eyes that live in vampire.dll stay off on these patch versions.
- The faster startup and loading, the restart crash fix and the startup companion are
  made for the Unofficial Patch 12 game files. The startup companion also needs the game
  to run in windowed mode. On other versions or in full screen they stay off and the game
  starts and loads as usual.

Every fix checks the game files and the exact code it changes before it touches anything.
Newer Unofficial Patch releases that edit the game's DLLs in place should keep the bug
fixes and tweaks working without an update. On a game build it does not recognize, where a
patch changed the code a fix needs, or if another mod already changed the same code, that
part stays off and Bin\loader\blse\blse.log says why. The rest of the game is not affected.


INSTALLING
----------
1. Install BLSE first.
2. Copy the Bin folder from this archive into your game folder (the folder that contains
   Vampire.exe), merging it with the existing Bin folder.
   This adds Bin\loader\blse\mods\engine_fixes, and Bin\loader\engine_fixes_intro.vtm with
   its settings file engine_fixes_intro.ini.
3. Start the game. Bin\loader\blse\blse.log lists each part as it switches on.

Uninstalling: delete the folder Bin\loader\blse\mods\engine_fixes and the files
Bin\loader\engine_fixes_intro.vtm, engine_fixes_intro.ini and engine_fixes_intro.log.
Updating: copy the new Bin folder over the old one. This replaces the settings files, so
back them up first if you changed settings. Settings files from 1.0 still work, but they
have no [Startup] and [Loading] sections, so startup and loading stay at the game's speed.


SPEED
-----
- Faster startup: the game starts loading at once instead of after the logo videos, with a
  dark window instead of a grey or white one while it loads (the startup companion,
  engine_fixes_intro.vtm). The startup work itself is shorter too: the game's packs are
  indexed once, so its many file look-ups at startup no longer search every pack from
  start to end, and each sound folder is listed once instead of twice.
- Faster first load: the first save you load in a session builds the game's sound tables
  with the same pack index, about 2 seconds faster.
- Faster loads: every save load and area change looks up sounds, missing files and models
  faster, and the last part of a load (before you can play) is no longer held back by the
  game's frame limit.
Tip: the Unofficial Patch can also skip its "press Enter to continue" prompt after loads:
in Bin\loader\loader.ini, set "Loading Wait For Enter = 0" in the [Tweaks] section.


FIXES
-----
- Crash after an engine restart: the game could crash when the engine restarts (the
  console's _restart command, or changing some video settings) after you had played.
  Fixed.
- Discipline effects left floating: Presence's golden halo stays on a vampire after it
  dies and disintegrates, and attached effects can outlive the character they belong to.
  They are now removed with their owner.
- Eyes twitching the wrong way: whenever an NPC looks somewhere new, the game first moves
  its eyes in the opposite direction for one frame. Fixed.
- Idle fidgets cut off: NPC idle animations could be swapped out mid-animation, making the
  NPC flick into another pose and back. Fixed.
- Walk cycle restarts: an NPC that stops and starts walking in the same instant restarted
  its walk cycle, a visible hitch. Fixed.
- NPCs using the player's idle animations: some NPCs could play the player character's
  idles. Fixed.
- Leg direction snapping: the game ships with a debug setting on that snaps NPC legs to a
  new walking direction instead of blending. Turned off.
- Leg movement in steps: NPC leg movement updated only 10 times per second on screen
  while the rest of the body moved smoothly. It is now blended every frame.


TWEAKS
------
On by default; each can be switched off.
- Smoother NPC steering: NPCs walking around each other or obstacles no longer zig-zag,
  flick into a sidestep or backpedal, or stall for a moment. Their steering is smoothed,
  and the game's own direction is used whenever the smoothed one would be blocked.
- Leg direction smoothing: limits how fast the legs' walking-direction blend can change.
- Relaxed eyes for Knox: Knox Gorman's eyes are opened very wide, so he seems to stare.
  His upper eyelids are relaxed; how much is adjustable.
- Calmer dialogue glances: in dialogue, NPCs glance away every few seconds. In the
  unmodified game the eyes sweep slowly up, down or sideways and back, which looks like
  an eye roll. The glances are reshaped: gentler, less frequent and mostly sideways. Speed,
  angle, timing and direction odds can all be adjusted, or glances switched off. Moods
  with hand-made glances (for example Apathy's bored eye roll) are left as made by default.


SETTINGS
--------
Bin\loader\blse\mods\engine_fixes\engine_fixes.ini, grouped into fixes, speed and tweaks.
Bin\loader\engine_fixes_intro.ini for the startup companion (for example, to keep the logo
videos). Every option is explained in the files. 1 = on, 0 = off. Restart the game after
changing anything.


REPORTING A PROBLEM
-------------------
Include Bin\loader\blse\blse.log from the session where it happened (and the crash log
next to it, if the game crashed). For startup problems, also include
Bin\loader\engine_fixes_intro.log.


LICENSE
-------
Engine Fixes and Tweaks is free software under the GNU GPL version 3, with an attribution
term and a permission to combine it with the game. See NOTICE and LICENSE in this folder.
Source code: https://gitlab.com/bigbannng/engine-fixes
