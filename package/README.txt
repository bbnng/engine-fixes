Engine Fixes 1.0.0
for Vampire: The Masquerade - Bloodlines
==========================================

Fixes bugs in Bloodlines' engine and game code, plus optional smoothing tweaks for
NPC movement and eyes. It changes no game data files (models, maps, scripts or text),
so it works alongside content mods.


REQUIREMENTS
------------
- Vampire: The Masquerade - Bloodlines with the Unofficial Patch (Basic or Plus).
- BLSE (Bloodlines Script Extender) 0.6.1 or newer.

Every fix checks the exact game files before it touches anything. On a game build it
does not recognize, or if another mod already changed the same code, that fix stays off
and Bin\loader\blse\blse.log says why. The rest of the game is not affected.


INSTALLING
----------
1. Install BLSE first.
2. Copy the Bin folder from this archive into your game folder (the folder that contains
   Vampire.exe), merging it with the existing Bin folder.
   This adds Bin\loader\blse\mods\engine_fixes.
3. Start the game. Bin\loader\blse\blse.log lists each fix as it switches on after you
   load a save.

Uninstalling: delete the folder Bin\loader\blse\mods\engine_fixes.
Updating: copy the new Bin folder over the old one. This replaces engine_fixes.ini, so
back it up first if you changed settings.


WHAT IT FIXES
-------------
Bug fixes:
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

Tweaks (on by default, each can be switched off):
- Smoother NPC steering: NPCs walking around each other or obstacles no longer zig-zag,
  flick into a sidestep or backpedal, or stall for a moment. Their steering is smoothed,
  and the game's own direction is used whenever the smoothed one would be blocked.
- Leg direction smoothing: limits how fast the legs' walking-direction blend can change.
- Calmer dialogue glances: in dialogue, NPCs glance away every few seconds. In the
  unmodified game the eyes sweep slowly up, down or sideways and back, which looks like
  an eye roll. The glances are reshaped: gentler, less frequent and mostly sideways. Speed,
  angle, timing and direction odds can all be adjusted, or glances switched off. Moods
  with hand-made glances (for example Apathy's bored eye roll) are kept as made.


SETTINGS
--------
Bin\loader\blse\mods\engine_fixes\engine_fixes.ini. Every option is explained in the file.
1 = on, 0 = off. Restart the game after changing anything.


REPORTING A PROBLEM
-------------------
Include Bin\loader\blse\blse.log from the session where it happened (and the crash log
next to it, if the game crashed).
