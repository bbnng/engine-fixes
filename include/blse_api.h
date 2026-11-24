/* Bloodlines Script Extender (BLSE) native plugin API.
 *
 * A plugin is a 32-bit x86 DLL placed in Bin/loader/blse/mods/<mod>/blse/plugins/ that exports
 *
 *     BLSE_EXPORT int BLSE_CALL BLSE_PluginLoad(const BLSE_HostAPI* host);
 *
 * Return nonzero to stay loaded, zero to be unloaded.
 *
 * BLSE is in beta, but this API is stable: later versions only add to it (new functions at the end of
 * BLSE_HostAPI, new messages) and never change or remove what is here, so a plugin built for one
 * version keeps working with later ones.
 *
 * Compatibility rules
 *  - api_version changes only on incompatible changes. New fields are appended to
 *    BLSE_HostAPI; check BLSE_HOST_HAS(host, field) before using any field after `log`.
 *  - The host structure stays valid for the process lifetime (since b0.5.6; earlier versions
 *    required copying it during BLSE_PluginLoad), so the pointer may be kept.
 *  - All host functions must be called on the thread that called BLSE_PluginLoad
 *    (the game's main thread). Calls from other threads fail and return 0.
 *  - A plugin that returns 0 must leave no threads, hooks or callbacks behind;
 *    its subscriptions are removed by the host before it is unloaded.
 */
#ifndef BLSE_API_H
#define BLSE_API_H
#include <stddef.h>
#include <stdint.h>

#define BLSE_API_VERSION 1u

#if defined(_WIN32)
#define BLSE_EXPORT __declspec(dllexport)
#define BLSE_CALL __cdecl
#else
#define BLSE_EXPORT
#define BLSE_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (BLSE_CALL *BLSE_LogFn)(const char *message);
/* topic and payload are borrowed for the duration of the call only. */
typedef void (BLSE_CALL *BLSE_MessageFn)(const char *topic, const void *payload, size_t size, void *user);
/* Returns a nonzero subscription token, or 0 on failure. */
typedef uint64_t (BLSE_CALL *BLSE_SubscribeFn)(void *context, const char *topic, BLSE_MessageFn callback, void *user);
/* Removes one of the calling plugin's own subscriptions. Returns 1 on success. */
typedef int (BLSE_CALL *BLSE_UnsubscribeFn)(void *context, uint64_t subscription);
/* Synchronously delivers to current subscribers (native and Python). Returns 1 if accepted. */
typedef int (BLSE_CALL *BLSE_PublishFn)(void *context, const char *topic, const void *payload, size_t size);
/* Save-game data. Stores a copy of `size` bytes under `key`; data NULL with size 0 removes the key.
 * Returns 1 on success, 0 if the key is invalid or longer than 117 bytes (keys are stored as one
 * line, and the game's own save reader must read them when BLSE is removed; quotes, backslashes and
 * bytes outside printable ASCII count as 2 to 4), the value is larger than 1 MiB, or no game
 * session is available yet (the game's script globals do not exist before the main menu). */
typedef int (BLSE_CALL *BLSE_SaveSetFn)(void *context, const char *key, const void *data, size_t size);
/* Returns 1 if `key` exists and stores its full length in *size (if size is not NULL). Copies
 * min(capacity, length) bytes to buffer (which may be NULL to query the length). Returns 0 if the
 * key does not exist or the data is unavailable. */
typedef int (BLSE_CALL *BLSE_SaveGetFn)(void *context, const char *key, void *buffer, size_t capacity, size_t *size);
/* Console command handler. argv[0] is the command name; `args` is everything after it as typed.
 * All strings are borrowed for the duration of the call. Runs on the main thread. */
typedef void (BLSE_CALL *BLSE_CommandFn)(int argc, const char *const *argv, const char *args, void *user);
/* Adds a console command. Names are 1..63 characters from [A-Za-z0-9_] (prefix them with your mod
 * name; "blse_" is reserved). Calling it again for a name the same plugin added replaces the
 * handler. Returns 1 on success, 0 if the name is invalid or taken by the game or another mod.
 * The command becomes usable once the engine console is ready (shortly after plugins load). */
typedef int (BLSE_CALL *BLSE_AddCommandFn)(void *context, const char *name, const char *help, BLSE_CommandFn callback, void *user);
/* Prints text to the game console as is (end lines with a newline yourself). */
typedef void (BLSE_CALL *BLSE_ConsolePrintFn)(void *context, const char *text);
/* Game Python (BLSE b0.5.2). The game's scripting API is Python: through these, a plugin can use
 * everything game scripts can, e.g. __main__.FindPlayer(), entities and G.
 * python_exec runs Python 2.1 statements in __main__. Returns 1 if they ran, 2 if they were
 * queued because the call came from inside game code (an entity_output handler); queued code runs
 * at the next safe point (end of the server frame). Returns 0 on error (the traceback is printed
 * to the game console) or when Python is unavailable. */
typedef int (BLSE_CALL *BLSE_PythonExecFn)(void *context, const char *code);
/* python_eval evaluates one Python expression in __main__ and stores str(result) in buffer
 * (NUL-terminated, truncated to capacity - 1); *size receives the full length. Returns 1 on
 * success, 0 on error, when Python is unavailable, or when called from inside game code. */
typedef int (BLSE_CALL *BLSE_PythonEvalFn)(void *context, const char *expression, char *buffer, size_t capacity, size_t *size);

/* Damage (BLSE b0.5.3). A damage filter sees every hit before the game applies it and may change
 * `damage` (0 = no damage). The strings are the entities' classnames and targetnames ("" if none)
 * and are valid during the call only. Filters run inside game code: keep them short, and do not
 * call python_eval there (python_exec is queued). Only on recognized game builds. */
typedef struct BLSE_Damage {
    uint32_t struct_size;        /* sizeof(BLSE_Damage) when called */
    float damage;                /* writable: the damage the game will apply (before its own scaling) */
    float original_damage;       /* what the game asked for */
    uint32_t damage_type;        /* the game's damage type bits */
    const char *victim_class, *victim_name;
    const char *attacker_class, *attacker_name;
    const char *inflictor_class, *inflictor_name; /* e.g. the weapon or projectile */
} BLSE_Damage;
typedef void (BLSE_CALL *BLSE_DamageFilterFn)(BLSE_Damage *damage, void *user);
/* Returns 1 if the filter was added, 0 if damage hooks are unavailable (unknown build). */
typedef int (BLSE_CALL *BLSE_AddDamageFilterFn)(void *context, BLSE_DamageFilterFn filter, void *user);

/* Disciplines (BLSE b0.5.20). A Discipline filter is called when a character is about
 * to activate a Discipline, before the game applies it, and may block it (allow = 0: no effect and no
 * blood cost) or scale its blood cost (cost_factor; 0 = free, 2 = double; rounded to whole blood
 * points). Python rules (blse.set_discipline_rule) are applied first. Filters run inside game code;
 * strings are valid during the call only. Recognized builds only. */
typedef struct BLSE_Discipline {
    uint32_t struct_size;               /* sizeof(BLSE_Discipline) when called */
    const char *owner_class, *owner_name;
    int32_t owner_index;                /* entity index; the player is 1 */
    const char *discipline;             /* stats.txt name in lower case, e.g. "potence" */
    int32_t level;
    int32_t allow;                      /* writable: 0 blocks the activation */
    float cost_factor;                  /* writable: multiplies the blood cost */
} BLSE_Discipline;
typedef void (BLSE_CALL *BLSE_DisciplineFilterFn)(BLSE_Discipline *discipline, void *user);
/* Returns 1 if the filter was added, 0 if Discipline control is unavailable (unknown build). */
typedef int (BLSE_CALL *BLSE_AddDisciplineFilterFn)(void *context, BLSE_DisciplineFilterFn filter, void *user);

/* Dialogue control (BLSE b0.5.22). A dialogue filter is called for every line the
 * player is about to see: the NPC's line (is_reply = 0) and each reply whose .dlg condition passed
 * (is_reply = 1). It may hide a reply (visible = 0; the remaining replies move up; the last
 * visible reply is never hidden) and replace the text shown (replace_text: copied when the filter
 * returns, up to 1000 bytes; the voice-over and the line's conditions, actions and next line stay).
 * Python rules (blse.hide_dialog_reply, blse.set_dialog_text) are applied first. Filters run
 * inside game code; strings are valid during the call only. Recognized builds only. */
typedef struct BLSE_DialogLine {
    uint32_t struct_size;               /* sizeof(BLSE_DialogLine) when called */
    const char *file;                   /* e.g. "dlg/main characters/jack_tutorial.dlg" */
    const char *npc_class, *npc_name;
    int32_t line;                       /* line number in the .dlg file */
    int32_t is_reply;
    const char *text_male, *text_female;/* the .dlg texts (the game shows one of them) */
    int32_t visible;                    /* writable, replies only: 0 hides the reply */
    const char *replace_text;           /* writable: the text to show instead (NULL: unchanged) */
} BLSE_DialogLine;
typedef void (BLSE_CALL *BLSE_DialogFilterFn)(BLSE_DialogLine *line, void *user);
/* Returns 1 if the filter was added, 0 if dialogue control is unavailable (unknown build). */
typedef int (BLSE_CALL *BLSE_AddDialogFilterFn)(void *context, BLSE_DialogFilterFn filter, void *user);

/* Python functions from plugins (BLSE b0.5.7): native code adds functions to the game's Python.
 * add_python_function makes blse_native.<name> callable from Python: from game scripts, Python
 * mods and dialogue conditions (__main__.blse_native is set for them), e.g.
 *     blse_native.mymod_bonus(FindPlayer().GetName(), 2)
 * Arguments and the result are None, int, float or str values. The function runs on the main
 * thread, inside the calling Python code; it may use python_exec and python_eval.
 * Return 1 with *result filled (it starts as None), or 0 to raise RuntimeError in Python (a STRING
 * *result is then the message). A STRING result is copied when the function returns.
 * Names are Python identifiers of 1-63 characters; prefix them with your mod name. Adding a name
 * again replaces your earlier function. Returns 1 on success, 0 if the name is invalid, taken by
 * another mod, or Python is unavailable. */
enum { BLSE_VALUE_NONE = 0, BLSE_VALUE_INT = 1, BLSE_VALUE_FLOAT = 2, BLSE_VALUE_STRING = 3 };
typedef struct BLSE_Value {
    uint32_t type;   /* BLSE_VALUE_* */
    int32_t i;       /* INT */
    double f;        /* FLOAT */
    const char *s;   /* STRING: size bytes, NUL-terminated, may contain NUL bytes */
    size_t size;
} BLSE_Value;
typedef int (BLSE_CALL *BLSE_PythonFunctionFn)(const BLSE_Value *args, int argc, BLSE_Value *result, void *user);
typedef int (BLSE_CALL *BLSE_AddPythonFunctionFn)(void *context, const char *name, const char *help, BLSE_PythonFunctionFn fn, void *user);
/* Inventory listing (BLSE b0.5.12): the item classnames carried by the character with targetname
 * `name` (NULL or "" = the player), in slot order, each followed by a NUL, in buffer (truncated to
 * capacity; *size receives the full length). Returns 1 on success, 0 if no such character exists,
 * no level is running, or the build is not recognized. */
typedef int (BLSE_CALL *BLSE_ListInventoryFn)(void *context, const char *name, char *buffer, size_t capacity, size_t *size);
/* On-screen text (BLSE b0.5.16): shows `text` (UTF-8, up to about 190 bytes; longer text is cut) to
 * the player through the game's own TextMsg message: BLSE_TEXT_CENTER in the middle of the screen,
 * BLSE_TEXT_TOP as the top-left HUD line, BLSE_TEXT_CONSOLE in the console. Any characters are safe
 * ("%" is escaped); "#word" is replaced if it is a localization key. Returns 1 if sent, 0 if
 * unavailable (unknown build), no level is running, or `kind` is invalid. Main thread only. */
enum { BLSE_TEXT_CONSOLE = 2, BLSE_TEXT_TOP = 3, BLSE_TEXT_CENTER = 4 };
typedef int (BLSE_CALL *BLSE_ShowTextFn)(void *context, const char *text, int kind);
/* HUD overlay (BLSE b0.5.18): text elements drawn every frame on top of the game's
 * interface while a level runs. hud_set shows or updates element `id` (1-63 characters, per
 * plugin): `text` UTF-8 (up to 255 bytes), `x`, `y` the top-left corner as fractions of the screen
 * (0..1), `color` 0xRRGGBBAA, `size` the font height in pixels (8..96). Returns 1, or 0 if invalid,
 * 64 elements exist, or the overlay is unavailable. hud_remove returns 1 if the element existed.
 * Elements are removed when the plugin is disabled. Main thread only. */
typedef int (BLSE_CALL *BLSE_HudSetFn)(void *context, const char *id, const char *text, float x, float y, uint32_t color, int size);
typedef int (BLSE_CALL *BLSE_HudRemoveFn)(void *context, const char *id);
/* HUD boxes, images and clicks (BLSE b0.5.24). Elements share the ids of hud_set and
 * are drawn in the order they were first created. hud_box: a filled box (color 0xRRGGBBAA; alpha 0
 * = no fill) with an optional 1-pixel outline (border; 0 = none); x, y, w, h are fractions of the
 * screen. hud_image: the material `material` (a path under materials/ without ".vmt", e.g.
 * "interface/mainmenu/vtm_title"; up to 32 different ones at once) stretched over the rectangle and
 * tinted by color (0xFFFFFFFF = unchanged). hud_clickable marks an element clickable (returns 0 if
 * it does not exist). hud_cursor turns cursor mode on or off for this plugin: the mouse stops
 * turning the view, a pointer is shown, mouse buttons do not reach the game, and clicks are
 * published as BLSE_MSG_HUD_CLICK. Cursor mode ends at level shutdown. Returns 0 if unavailable
 * (or, when turning it on, if no level runs). Main thread only. */
typedef int (BLSE_CALL *BLSE_HudBoxFn)(void *context, const char *id, float x, float y, float w, float h, uint32_t color, uint32_t border);
typedef int (BLSE_CALL *BLSE_HudImageFn)(void *context, const char *id, const char *material, float x, float y, float w, float h, uint32_t color);
typedef int (BLSE_CALL *BLSE_HudClickableFn)(void *context, const char *id, int clickable);
typedef int (BLSE_CALL *BLSE_HudCursorFn)(void *context, int on);
/* Input (BLSE b0.5.24). Keys are named as in the console's bind command: "a", "5",
 * "F5", "SPACE", "ENTER", "SHIFT", "MOUSE1".."MOUSE5", "MWHEELUP", "KP_ENTER", ... (case-insensitive);
 * keys without a name are "KEY<number>" (the engine's key number), as reported in key events.
 * key_state: 1 held, 0 not, -1 unknown key or input unavailable.
 * key_binding: the command bound to the key ("" if none) in buffer (NUL-terminated, truncated to
 * capacity - 1; *size receives the full length). Returns 1, or 0 for an unknown key.
 * block_key: while blocked (by any mod), a press of the key during play does not reach the game;
 * BLSE_MSG_KEY_DOWN is still published. Blocks are removed when the plugin is disabled.
 * press_key: simulates the key as if pressed (mode 1), released (0) or tapped (2: released after the
 * next game frame); its binding runs as for a real press. Simulated keys are not published. */
enum { BLSE_KEY_UP = 0, BLSE_KEY_DOWN = 1, BLSE_KEY_TAP = 2 };
/* Text entry fields, alignment and sizes (BLSE b0.5.25).
 * hud_input: a text entry field (a box with editable text): `text` (UTF-8) sets its text, NULL
 * keeps the current one; `size` the font height (8..96), `max_chars` 1..255; colors 0xRRGGBBAA
 * (background, outline, text). A left click in cursor mode focuses it; while it has focus, keys in
 * play type into it instead of reaching the game (BLSE_MSG_KEY_DOWN is sent with context "text").
 * hud_focus focuses an input (id NULL or "" ends text entry); returns 0 if there is no such input.
 * hud_input_text copies an input's text like key_binding copies a binding (0 if no such input).
 * hud_align: 0 left (x is the left edge), 1 center (x is the middle), 2 right (x is the right edge);
 * text elements only. hud_size: the element's width and height as fractions of the screen (text as
 * last drawn; 0 if not drawn yet or no such element). */
typedef int (BLSE_CALL *BLSE_HudInputFn)(void *context, const char *id, float x, float y, float w, float h, const char *text, int size,
                                         int max_chars, uint32_t background, uint32_t border, uint32_t text_color);
typedef int (BLSE_CALL *BLSE_HudFocusFn)(void *context, const char *id);
typedef int (BLSE_CALL *BLSE_HudInputTextFn)(void *context, const char *id, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_HudAlignFn)(void *context, const char *id, int align);
typedef int (BLSE_CALL *BLSE_HudSizeFn)(void *context, const char *id, float *w, float *h);
/* Versioned save-game records (BLSE b0.5.25), like save_set/save_get with a record version stored
 * next to the value: write your format's version, and
 * on load convert older ones. save_set_versioned with data NULL and size 0 removes the record and its
 * version. save_get_versioned also stores the version in *version (0 for values written with
 * save_set, which stays compatible). Keys up to 108 bytes. Returns as save_set / save_get. */
typedef int (BLSE_CALL *BLSE_SaveSetVersionedFn)(void *context, const char *key, uint32_t version, const void *data, size_t size);
/* Other plugins (BLSE b0.5.26). plugin_version: 1 if the plugin `name` (its BLSE_PLUGIN_INFO name or
 * file name, any case) is loaded and not disabled; its version ("" if it declares none) is copied
 * like key_binding copies a binding. list_plugins: the loaded plugins as "name version" lines
 * separated by '\n', copied the same way. Both work from BLSE_PluginLoad on (dependencies are loaded
 * first). Python: blse.plugin_version(name), blse.plugins(). */
/* Vertical alignment of a text element (BLSE b0.5.26): 0 top (y is the top edge), 1 middle, 2 bottom.
 * control_state: 1 while a key whose press ran `command` is held, else 0 (-1 if input is unavailable). */
typedef int (BLSE_CALL *BLSE_HudValignFn)(void *context, const char *id, int valign);
typedef int (BLSE_CALL *BLSE_ControlStateFn)(void *context, const char *command);
/* Main-thread tasks (BLSE b0.5.27). add_task may be called from ANY thread
 * (the one exception to the main-thread rule): `task(user)` then runs on the game's main thread at
 * the next frame (a game frame, or a painted frame while no level runs), in the order added, under
 * the plugin crash guard; from there it may use every host function. Returns 1 if queued, 0 if the
 * plugin was disabled or the queue is full (4096 tasks). Tasks of a plugin disabled before they run
 * are dropped. */
typedef void (BLSE_CALL *BLSE_TaskFn)(void *user);
typedef int (BLSE_CALL *BLSE_AddTaskFn)(void *context, BLSE_TaskFn task, void *user);
/* The entity the player targets (BLSE b0.5.27): kind 0 what the crosshair is on (the entity whose
 * name the game shows), 1 what the player is in the middle of using (a door knob being tried, a
 * sign being read, a carried prop), 2 the creature a targeted Discipline would hit, 3 (b0.5.34)
 * the console's "!picker": the entity straight ahead, else the one nearest the crosshair, any
 * entity (also ones the game does not name; main thread only). Copies
 * "classname\0targetname\0handle" into buffer like key_binding. Returns 1, or 0 if there is none or
 * no level runs. */
typedef int (BLSE_CALL *BLSE_PlayerTargetFn)(void *context, int kind, char *buffer, size_t capacity, size_t *size);
/* Hooks for plugins (BLSE b0.5.28). BLSE patches the game for the
 * plugin and records every patch, its own and the plugins': a patch that overlaps another one is
 * refused instead of silently breaking it. Main thread only (in BLSE_PluginLoad, callbacks, tasks).
 *
 * hook_function: makes `target` jump to `detour`. `expected` are the first `size` bytes (5..16)
 * of the target as the plugin was written for: whole instructions, without relative jumps or calls
 * (they are moved into the trampoline). *original receives a function to call the unhooked code.
 * hook_call: `call_site` is a 5-byte `call rel32` (E8); it is redirected to `detour`, and *original
 * receives the function it called. hook_pointer: `slot` holds a function pointer (a vtable or import
 * entry); it is set to `detour`, *original receives the old pointer.
 * unhook: undoes the calling plugin's patch at `target` (call site, slot); 1 if it had one.
 * Patches of a plugin that declines to load are undone; those of a plugin disabled after a crash
 * stay (the game may be inside them).
 * find_pattern: the unique address in `module`'s code ("engine.dll", "vampire.dll", ...; NULL: the
 * game's exe) matching `pattern`, hex bytes with ?? wildcards ("8B 0D ?? ?? ?? ?? 8B 01"). Returns 1
 * (found), 0 (not found), 2 (found more than once: make the pattern longer), BLSE_HOOK_INVALID for a
 * bad pattern or module; *address is NULL unless 1. */
#define BLSE_HOOK_OK 1
#define BLSE_HOOK_BYTES_DIFFER (-1) /* the code is not `expected`: another build, or another mod's patch */
#define BLSE_HOOK_CONFLICT (-2)     /* overlaps a patch of BLSE or another plugin (see the log) */
#define BLSE_HOOK_INVALID (-3)      /* bad arguments, not the main thread, or the plugin is disabled */
#define BLSE_HOOK_FAILED (-4)       /* memory could not be allocated or made writable */
typedef int (BLSE_CALL *BLSE_HookFunctionFn)(void *context, void *target, const unsigned char *expected, size_t size, void *detour, void **original);
typedef int (BLSE_CALL *BLSE_HookCallFn)(void *context, void *call_site, void *detour, void **original);
typedef int (BLSE_CALL *BLSE_HookPointerFn)(void *context, void **slot, void *detour, void **original);
typedef int (BLSE_CALL *BLSE_UnhookFn)(void *context, void *target);
typedef int (BLSE_CALL *BLSE_FindPatternFn)(void *context, const char *module, const char *pattern, void **address);
/* Console variables (BLSE b0.5.29). cvar_get copies the value as text like
 * key_binding; 0 if there is no such variable (commands are not variables). cvar_set sets it as the
 * console would (the variable converts and clamps the text); 1 if it exists. Main thread only. */
typedef int (BLSE_CALL *BLSE_CvarGetFn)(void *context, const char *name, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_CvarSetFn)(void *context, const char *name, const char *value);

/* Item information (BLSE b0.5.29), from the game's item data (vdata/items).
 * Set struct_size to sizeof(BLSE_ItemInfo) before calling item_info; it returns 1 and fills the
 * struct for a known item classname ("item_w_thirtyeight"), 0 otherwise. Items the game has not
 * loaded yet are unknown. list_items copies the known classnames, separated by '\n'. */
typedef struct BLSE_ItemInfo {
    uint32_t struct_size;
    char name[128];         /* "printname": the name the game shows (may be a #token) */
    char type[64];          /* "item_type": "armor", "ranged", "melee", ... */
    char model[128];        /* "infomodel" */
    int32_t price;          /* "item_worth" */
    int32_t sell_price;     /* "player_sell" */
    int32_t weight;
    int32_t flags;          /* "item_flags" */
    char description[2048]; /* truncated if longer */
} BLSE_ItemInfo;
typedef int (BLSE_CALL *BLSE_ItemInfoFn)(void *context, const char *classname, BLSE_ItemInfo *out);
typedef int (BLSE_CALL *BLSE_ListItemsFn)(void *context, char *buffer, size_t capacity, size_t *size);

/* Input filters (BLSE b0.5.29). Called on the main thread for every input an entity receives
 * (see BLSE_MSG_ENTITY_INPUT), before the entity acts on it. Set `block` to 1 to drop the input, or
 * write a replacement value into `new_value` (converted to the input's own type: a number, a
 * string, three numbers for a vector). Keep filters fast: they run very often. */
typedef struct BLSE_Input {
    uint32_t struct_size;
    const char *input, *target_class, *target_name, *value;
    const char *activator_class, *activator_name, *caller_class, *caller_name;
    int32_t block;
    char new_value[256];
} BLSE_Input;
typedef void (BLSE_CALL *BLSE_InputFilterFn)(BLSE_Input *input, void *user);
typedef int (BLSE_CALL *BLSE_AddInputFilterFn)(void *context, BLSE_InputFilterFn filter, void *user);

/* Game menus' contents (BLSE b0.5.30). Main thread only.
 * open_menus copies the open screens' names ("inventory", "character", ...; see BLSE_MSG_MENU_OPENED),
 * separated by '\n'. menu_panels copies the panels of an open screen, the screen first: records
 * separated by '\x1e', each with nine fields separated by '\x1f': path, class, visible (0/1), x, y,
 * w, h (pixels), label (0/1), text (UTF-8; a label's text). It returns 0 if the screen is not open.
 * A '\x1e' or '\x1f' inside a field (a label's text could hold one) is given as a space.
 * A path names a panel below the screen: names joined by '/', and any child can also be named
 * "#<index>" ("" is the screen itself). menu_set_text changes a label's text, menu_set_visible
 * shows or hides a panel, menu_move moves it (and resizes it unless w or h is negative); each
 * returns 1 or 0. The game may refill a screen when it opens or updates it. */
typedef int (BLSE_CALL *BLSE_OpenMenusFn)(void *context, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_MenuPanelsFn)(void *context, const char *screen, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_MenuSetTextFn)(void *context, const char *screen, const char *path, const char *text);
typedef int (BLSE_CALL *BLSE_MenuSetVisibleFn)(void *context, const char *screen, const char *path, int visible);
typedef int (BLSE_CALL *BLSE_MenuMoveFn)(void *context, const char *screen, const char *path, int x, int y, int w, int h);
/* Camera and sounds (BLSE b0.5.30). Main thread only.
 * camera_get: *third_person 1 or 0 (-1 unknown), *fov the player's field of view in degrees (-1
 * outside a level; 0 is the game's default); returns 1 if either is known. camera_set: third_person
 * 0/1 switches the view through the game's own toggle (-1 keeps it); fov 1..179 sets the field of
 * view, 0 restores the default (-1 keeps it);
 * returns 1 if everything asked was done.
 * play_sound plays a sound below sound/ ("player/pl_wade2.wav") without position, volume 0..1,
 * pitch 100 normal; stop_sound stops it. Both return 1 if the engine took the request. */
typedef int (BLSE_CALL *BLSE_CameraGetFn)(void *context, int *third_person, int *fov);
typedef int (BLSE_CALL *BLSE_CameraSetFn)(void *context, int third_person, int fov);
typedef int (BLSE_CALL *BLSE_PlaySoundFn)(void *context, const char *file, float volume, int pitch);
typedef int (BLSE_CALL *BLSE_StopSoundFn)(void *context, const char *file);
/* More of game menus, the camera's view, positional sounds (BLSE b0.5.31). Main thread only.
 * menu_panels records gain two fields: kind ("label", "button", "text_entry", "image", "panel" or
 * "") and image (an image panel's image). menu_set_text also changes buttons and text fields.
 * menu_set_image shows another image in an image panel (a name below materials/vgui/, as in the
 * game's .res files). menu_press presses a button as a click does. menu_command gives a panel
 * (usually the screen, path "") a command, the way its buttons do (Panel::OnCommand).
 * menu_add_label adds a label below the panel at parent_path (or updates the one this plugin added
 * with that name); menu_remove removes one this plugin added. Each returns 1 or 0.
 * camera_view: the position and angles (pitch, yaw, roll) the view was rendered from; 0 outside a
 * level. play_sound_at plays a sound from a point, play_sound_on from an entity (index or handle),
 * following it; attenuation 0.8 is normal, 0 heard everywhere. stop_sound_on stops one on an
 * entity. sound_length: the file's length in seconds from its WAV or MP3 header, or 0. */
typedef int (BLSE_CALL *BLSE_MenuSetImageFn)(void *context, const char *screen, const char *path, const char *image);
typedef int (BLSE_CALL *BLSE_MenuPressFn)(void *context, const char *screen, const char *path);
typedef int (BLSE_CALL *BLSE_MenuCommandFn)(void *context, const char *screen, const char *path, const char *command);
typedef int (BLSE_CALL *BLSE_MenuAddLabelFn)(void *context, const char *screen, const char *parent_path, const char *name,
                                             const char *text, int x, int y, int w, int h);
typedef int (BLSE_CALL *BLSE_MenuRemoveFn)(void *context, const char *screen, const char *path);
typedef int (BLSE_CALL *BLSE_CameraViewFn)(void *context, float origin[3], float angles[3]);
typedef int (BLSE_CALL *BLSE_PlaySoundAtFn)(void *context, const char *file, const float origin[3], float volume, int pitch, float attenuation);
typedef int (BLSE_CALL *BLSE_PlaySoundOnFn)(void *context, const char *file, uint32_t entity, float volume, int pitch, float attenuation);
typedef int (BLSE_CALL *BLSE_StopSoundOnFn)(void *context, const char *file, uint32_t entity);
typedef int (BLSE_CALL *BLSE_SoundLengthFn)(void *context, const char *file, float *seconds);
/* Added buttons and images, button commands, the camera override (BLSE b0.5.32). Main thread only.
 * menu_panels records gain a twelfth field: the command a button sends. menu_add_button adds a
 * button (clicking it publishes BLSE_MSG_MENU_BUTTON), menu_add_image an image panel (an image name
 * below materials/vgui/); both update the one this plugin added with that name (of the same kind);
 * menu_remove removes them too; 1 or 0.
 * camera_override renders the game from `origin` and/or with `angles` (NULL keeps the game's) until
 * called with both NULL, the level ends, or the plugin is disabled or unloaded; the player still
 * moves and aims as before. */
typedef int (BLSE_CALL *BLSE_MenuAddButtonFn)(void *context, const char *screen, const char *parent_path, const char *name,
                                              const char *text, int x, int y, int w, int h);
typedef int (BLSE_CALL *BLSE_MenuAddImageFn)(void *context, const char *screen, const char *parent_path, const char *name,
                                             const char *image, int x, int y, int w, int h);
typedef int (BLSE_CALL *BLSE_CameraOverrideFn)(void *context, const float *origin, const float *angles);
/* NPC fights and item changes (BLSE b0.5.33). Main thread only.
 * combat_npcs copies the NPCs fighting now (see BLSE_MSG_NPC_COMBAT): records separated by '',
 * each with six fields separated by '': NPC handle (decimal), classname, name, target handle,
 * classname, name. 0 outside a level.
 * item_set changes a field of an item ("name", "description", "price", "sell_price", "weight", and a
 * weapon's ammunition "ammo_name", "ammo_description", "ammo_price"; the value as text, numbers
 * 0..1000000000) in the game's records, both the server's and the client's
 * (the barter screen's prices): 1 changed now, 2 remembered (the item is not loaded yet; it gets the
 * value when the game loads it), 0 invalid. item_reset gives back the game's own values for every
 * field this plugin changed on the item (1, or 0 if none). Changes last until reset, the plugin is
 * disabled or unloaded, or the game exits; they are not saved. */
typedef int (BLSE_CALL *BLSE_CombatNpcsFn)(void *context, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_ItemSetFn)(void *context, const char *classname, const char *field, const char *value);
typedef int (BLSE_CALL *BLSE_ItemResetFn)(void *context, const char *classname);
/* Settings, files and items not loaded yet (BLSE b0.5.35). Main thread only; each copies its text
 * like key_binding and returns 1, or 0 if unavailable.
 * cvar_list: the console variables' names (commands too if `commands` is 1), separated by '\n'
 * (see also BLSE_MSG_CVAR_CHANGED). read_file: a file found the way the game finds its files (its
 * folders and .vpk packs, the mod's first), e.g. "cfg/config.cfg"; at most 4 MB; 0 if missing.
 * find_files: the names (not paths) matching a wildcard ("vdata/items/*.txt"), '\n'-separated.
 * item_files: every item there is a file for (classnames, loaded or not). item_info (b0.5.29) now
 * also answers for items the game has not loaded, from their file. */
typedef int (BLSE_CALL *BLSE_CvarListFn)(void *context, int commands, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_ReadFileFn)(void *context, const char *path, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_FindFilesFn)(void *context, const char *wildcard, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_ItemFilesFn)(void *context, char *buffer, size_t capacity, size_t *size);
/* Windows and text fields (BLSE b0.5.36). Main thread only; each returns 1 or 0.
 * window_create makes a window of the game's own kind (a vgui2 Frame: title bar, close button,
 * moved and resized by the player; a popup that takes the mouse and keyboard while shown), shown, or
 * changes its title, position and size if this plugin made one of that name before. The name (1-63
 * of A-Z a-z 0-9 _ -) then works as the screen for every menu_ function ("" is the window itself):
 * labels, buttons, images and text fields go in it. window_show shows or hides it, window_remove
 * deletes it with what is in it. A window closed with its button is hidden and BLSE_MSG_WINDOW_CLOSED
 * is published. A plugin's windows go when it is unloaded.
 * menu_add_text_entry adds a text field (several lines if multiline is 1); its text is read with
 * menu_panels and changed with menu_set_text.
 * hud_multiline makes a HUD input (hud_input) take several lines: words wrap, Enter starts a new
 * line, Ctrl+Enter submits; inputs now take up to 1023 characters. */
typedef int (BLSE_CALL *BLSE_WindowCreateFn)(void *context, const char *name, const char *title, int x, int y, int w, int h);
typedef int (BLSE_CALL *BLSE_WindowShowFn)(void *context, const char *name, int visible);
typedef int (BLSE_CALL *BLSE_WindowRemoveFn)(void *context, const char *name);
typedef int (BLSE_CALL *BLSE_MenuAddTextEntryFn)(void *context, const char *screen, const char *parent_path, const char *name,
                                                 const char *text, int x, int y, int w, int h, int multiline);
typedef int (BLSE_CALL *BLSE_HudMultilineFn)(void *context, const char *id, int multiline);
/* hud_hint: a hint shown faintly in a HUD input while it is empty ("" or NULL: none); 1 or 0. */
typedef int (BLSE_CALL *BLSE_HudHintFn)(void *context, const char *id, const char *hint);
/* Restyling and settings files (BLSE b0.5.37). Main thread only; each returns 1 or 0.
 * menu_set_colors: a panel's text color and background ("#RRGGBB" or "#RRGGBBAA"; NULL keeps it;
 * alpha 00: no background), any game panel or a mod's, kept while the panel exists (set again when
 * the game's scheme changes them; the latest change wins). menu_set_font: a font (a Windows or game
 * font face, size in pixels 6-128, weight 100-900) for a label, button or text field; fonts are made
 * through the game's surface, so only after it has drawn a frame. menu_reset_style: the game's own
 * look again for a panel this plugin restyled. menu_set_hint: a hint shown faintly while a text field
 * this plugin added is empty ("" none). A plugin's styles are undone when it is disabled or unloaded.
 * ini_get copies the value of `key` in [section] of <mod>.ini: Bin\loader\blse\mods\<mod>\<mod>.ini when a
 * mod folder of that name exists (0.6.1), else Bin\loader\blse\config\<mod>.ini (like
 * key_binding; 0 if the file or key is missing); ini_set writes it (NULL removes the key), keeping the
 * rest of the file. mod: 1-63 of A-Z a-z 0-9 _ - . */
typedef int (BLSE_CALL *BLSE_MenuSetColorsFn)(void *context, const char *screen, const char *path, const char *fg, const char *bg);
typedef int (BLSE_CALL *BLSE_MenuSetFontFn)(void *context, const char *screen, const char *path, const char *face, int size, int weight);
typedef int (BLSE_CALL *BLSE_MenuResetStyleFn)(void *context, const char *screen, const char *path);
typedef int (BLSE_CALL *BLSE_MenuSetHintFn)(void *context, const char *screen, const char *path, const char *hint);
typedef int (BLSE_CALL *BLSE_IniGetFn)(void *context, const char *mod, const char *section, const char *key, char *buffer,
                                       size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_IniSetFn)(void *context, const char *mod, const char *section, const char *key, const char *value);
/* menu_set_border: whether a panel draws its outline (1) or not (0); a window's frame lines. */
typedef int (BLSE_CALL *BLSE_MenuSetBorderFn)(void *context, const char *screen, const char *path, int visible);
/* Models, translated strings and frame rate (BLSE b0.5.38). Each returns 1 or 0.
 * entity: a server entity's index (below 8192; 1 = the player) or its handle. Models: main thread,
 * while a level runs, entities with a model (characters, props, items lying in the world).
 * bone_names copies the entity's bone names, each followed by '\n', in the model's order (like
 * list_plugins). bone_position: a bone's position and angles (pitch, yaw, roll) in the world, by name
 * (case is ignored). model_scale / set_model_scale: the model's scale (1 normal; 0 never set, drawn
 * as 1; 0.05-8) and the parts scaled with it (BLSE_SCALE_* bits; 0: only the
 * picture). Parts are options and last until the entity is gone or the level ends: VIEW the eye
 * height (for the player also crouched), HITBOXES the bones hits and attachments are tested with,
 * COLLISION the entity's box (for the player also its movement hulls). parts may be NULL.
 * translate copies the text for `key` (with or without '$') from
 * Bin\loader\blse\translations\<mod>_<language>.txt, else <mod>_english.txt, else the key itself
 * (UTF-8; like key_binding); 0 only for an invalid mod name or key. Any thread.
 * translation_language: the language translations use ("english", "german", ...).
 * frame_stats: set struct_size to sizeof(BLSE_FrameStats); the figures over the last second,
 * measured at the end of every frame the game draws. 0 until two frames were drawn. Any thread. */
typedef int (BLSE_CALL *BLSE_BoneNamesFn)(void *context, uint32_t entity, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_BonePositionFn)(void *context, uint32_t entity, const char *bone, float origin[3], float angles[3]);
#define BLSE_SCALE_VIEW 1u
#define BLSE_SCALE_HITBOXES 2u
#define BLSE_SCALE_COLLISION 4u
/* How long (b0.5.39). Without KEEP, a scale lasts until the level ends: saves hold the entity at the
 * size it had before (it loads at that size) and the parts end. KEEP remembers the scale and its
 * parts in the save and applies them all again after a load, when the level is entered again and
 * (the player) after every level change. */
#define BLSE_SCALE_KEEP 8u
typedef int (BLSE_CALL *BLSE_ModelScaleFn)(void *context, uint32_t entity, float *scale, unsigned *parts);
typedef int (BLSE_CALL *BLSE_SetModelScaleFn)(void *context, uint32_t entity, float scale, unsigned parts);
typedef int (BLSE_CALL *BLSE_TranslateFn)(void *context, const char *mod, const char *key, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_TranslationLanguageFn)(void *context, char *buffer, size_t capacity, size_t *size);
typedef struct BLSE_FrameStats {
    uint32_t struct_size; /* sizeof(BLSE_FrameStats) */
    double fps;           /* frames a second over the last second */
    double last_ms;       /* the last frame's time */
    double average_ms;    /* the average frame time over the last second */
    double worst_ms;      /* the longest frame over the last second */
    int frames;           /* frames drawn in the last second */
} BLSE_FrameStats;
typedef int (BLSE_CALL *BLSE_FrameStatsFn)(void *context, BLSE_FrameStats *out);
/* Values on entities, finding entities, JSON files (BLSE b0.5.40). Each returns 1 or 0.
 * entity_data_set stores a text value on an entity (index or handle) under `key` (1-63 of A-Z a-z
 * 0-9 _ - .; NULL value removes it), saved with the game and shared with Python mods (which read
 * it as a string); entity_data_get copies a value as text (numbers set from Python as digits);
 * entity_data_keys copies the keys, each followed by '\n'; entity_data_find fills `handles` with
 * this level's entities (and the player) that have `key` (*count: how many there are). Values
 * follow the entity across saves and level changes and go with it when it is removed. Main thread,
 * while a level runs.
 * entity_position: where an entity is. find_entities: entities within `radius` of `center`,
 * nearest first; classname "" any, "npc_*" a prefix or a class; flags BLSE_FIND_*. Main thread.
 * json_get copies the value at `path` ("" the whole file, "a.b", "list[2]") of
 * Bin\loader\blse\data\<file>.json as JSON text (0 if the file or value is missing or the file is
 * broken); json_set sets it from JSON text (NULL removes it), keeping the rest of the file. Any thread. */
#define BLSE_FIND_CHARACTERS 1u
#define BLSE_FIND_ALIVE 2u
#define BLSE_FIND_NO_PLAYER 4u
typedef int (BLSE_CALL *BLSE_EntityDataSetFn)(void *context, uint32_t entity, const char *key, const char *value);
typedef int (BLSE_CALL *BLSE_EntityDataGetFn)(void *context, uint32_t entity, const char *key, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_EntityDataKeysFn)(void *context, uint32_t entity, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_EntityDataFindFn)(void *context, const char *key, uint32_t *handles, size_t capacity, size_t *count);
typedef int (BLSE_CALL *BLSE_EntityPositionFn)(void *context, uint32_t entity, float origin[3]);
typedef int (BLSE_CALL *BLSE_FindEntitiesFn)(void *context, const float center[3], float radius, const char *classname, unsigned flags,
                                             uint32_t *handles, size_t capacity, size_t *count);
typedef int (BLSE_CALL *BLSE_JsonGetFn)(void *context, const char *file, const char *path, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_JsonSetFn)(void *context, const char *file, const char *path, const char *json);
/* Damage, velocity (BLSE b0.5.41). damage_entity: hurts `entity` through the game's own
 * damage (attacker BLSE_NO_ENTITY for none; type: damage type bits, 0 generic), 1 or 0. entity_velocity
 * / push_entity: units a second; add 1 adds, 0 sets. Main thread. fix_state: always -1 since b0.5.49
 * (BLSE applies no game fixes: they belong to a separate fixes mod); kept for the API layout. */
#define BLSE_NO_ENTITY 0xFFFFFFFFu
typedef int (BLSE_CALL *BLSE_DamageEntityFn)(void *context, uint32_t entity, float amount, uint32_t attacker, int type_bits);
typedef int (BLSE_CALL *BLSE_EntityVelocityFn)(void *context, uint32_t entity, float velocity[3]);
typedef int (BLSE_CALL *BLSE_PushEntityFn)(void *context, uint32_t entity, const float velocity[3], int add);
typedef int (BLSE_CALL *BLSE_FixStateFn)(void *context, const char *name);
/* console_command: runs a console command line as if typed (1-1023 characters, no line breaks;
 * queued, runs before the next frame). Main thread. */
typedef int (BLSE_CALL *BLSE_ConsoleCommandFn)(void *context, const char *line);
/* The shared database (BLSE b0.5.42): one JSON document saved with the game and shared with Python
 * mods (blse.db_get / db_set). db_get copies the value at `path` ("" all, "mymod.settings.speed",
 * "mymod.list[2]") as JSON text (0 if there is none); db_set sets it from JSON text (NULL removes it),
 * making the objects on the way. Main thread. */
typedef int (BLSE_CALL *BLSE_DbGetFn)(void *context, const char *path, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_DbSetFn)(void *context, const char *path, const char *json);
/* translate_text (BLSE b0.5.43): a "$KEY" (with "{param}" placeholders) translated from any mod's
 * translation files, or the text as given (like key_binding). BLSE's interface functions do this
 * for every text they get, so plugins need it only for text they show themselves. Any thread. */
typedef int (BLSE_CALL *BLSE_TranslateTextFn)(void *context, const char *text, char *buffer, size_t capacity, size_t *size);
/* NPC AI (BLSE b0.5.45). Entities: an index or a handle; BLSE_NO_ENTITY for none. Main thread,
 * while a level runs; 1 or 0 unless noted.
 * fire_input: sends an input as the map's I/O does (input filters and blse.entity_input see it);
 * value NULL or "" for none (else text, up to 1023 characters); activator BLSE_NO_ENTITY: the
 * player. 1 if the entity handled it.
 * set_relationship: how the NPC feels about one entity (disposition BLSE_DISPOSITION_*; priority:
 * higher wins among enemies), saved with the game. relationship reads it (also the game's own).
 * set_enemy: makes `target` (a character) the NPC's enemy and tells it where the target is; BLSE_NO_ENTITY
 * clears it and calms the NPC (back to idle, its schedule ended). An NPC keeps an enemy only while
 * it hates it. npc_enemy: the enemy's handle or BLSE_NO_ENTITY.
 * start_schedule: runs one of the NPC's schedules once ("SCHED_TROIKA_FLEE" or "TROIKA_FLEE"),
 * then its AI carries on; 0 if its kind has no such schedule. npc_schedule copies the name of the
 * schedule it runs now. npc_state: BLSE_NPC_STATE_* (-1 if not an NPC).
 * move_npc: sends the NPC to `goal`, or to `position` when goal is BLSE_NO_ENTITY (run 1 runs).
 * follow_npc: the NPC follows `leader` (any entity) until it dies, the level ends, a move or
 * BLSE_NO_ENTITY; not kept in saves. npc_leader: whom it follows, or BLSE_NO_ENTITY.
 * npc_senses: sight distance, field of view in degrees, the stealth vision and hearing multipliers
 * in effect, and the stealth level (1-10). set_npc_sense: "sight", "fov", "vision" or "hearing"
 * (a negative vision or hearing gives the game's own back). */
#define BLSE_DISPOSITION_HATE 1
#define BLSE_DISPOSITION_FEAR 2
#define BLSE_DISPOSITION_LIKE 3
#define BLSE_DISPOSITION_NEUTRAL 4
#define BLSE_NPC_STATE_NONE 0
#define BLSE_NPC_STATE_IDLE 1
#define BLSE_NPC_STATE_COMBAT 2
#define BLSE_NPC_STATE_ALERT 3
#define BLSE_NPC_STATE_SCRIPT 4
#define BLSE_NPC_STATE_PLAYDEAD 5
#define BLSE_NPC_STATE_PRONE 6
#define BLSE_NPC_STATE_DEAD 7
#define BLSE_NPC_STATE_FLEEING 8
#define BLSE_NPC_STATE_RETREATING 9
#define BLSE_NPC_STATE_COWERING 10
#define BLSE_NPC_STATE_HUNTING 11
#define BLSE_NPC_STATE_DIALOG 12
#define BLSE_NPC_STATE_OBLIVIOUS 13
#define BLSE_NPC_STATE_CRIMINAL_SUSPICION 14
typedef int (BLSE_CALL *BLSE_FireInputFn)(void *context, uint32_t entity, const char *input, const char *value, uint32_t activator);
typedef int (BLSE_CALL *BLSE_SetRelationshipFn)(void *context, uint32_t npc, uint32_t target, int disposition, int priority);
typedef int (BLSE_CALL *BLSE_RelationshipFn)(void *context, uint32_t npc, uint32_t target, int *disposition, int *priority);
typedef int (BLSE_CALL *BLSE_SetEnemyFn)(void *context, uint32_t npc, uint32_t target);
typedef uint32_t (BLSE_CALL *BLSE_NpcEnemyFn)(void *context, uint32_t npc);
typedef int (BLSE_CALL *BLSE_StartScheduleFn)(void *context, uint32_t npc, const char *schedule);
typedef int (BLSE_CALL *BLSE_NpcScheduleFn)(void *context, uint32_t npc, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_NpcStateFn)(void *context, uint32_t npc);
typedef int (BLSE_CALL *BLSE_MoveNpcFn)(void *context, uint32_t npc, uint32_t goal, const float position[3], int run);
typedef int (BLSE_CALL *BLSE_FollowNpcFn)(void *context, uint32_t npc, uint32_t leader);
typedef int (BLSE_CALL *BLSE_NpcSensesFn)(void *context, uint32_t npc, float values[5]);
typedef int (BLSE_CALL *BLSE_SetNpcSenseFn)(void *context, uint32_t npc, const char *sense, float value);
typedef uint32_t (BLSE_CALL *BLSE_NpcLeaderFn)(void *context, uint32_t npc);
/* World and inventories (BLSE b0.5.46). Main thread, while a level runs.
 * spawn_entity: creates and spawns an entity of `classname` at `origin`, turned by `yaw`, with the
 * map's keyvalues (`pairs`: key, value, key, value, ...; `pair_count` pairs; NULL for none); its
 * handle, or BLSE_NO_ENTITY. teleport_entity: `origin` and/or `angles` (NULL keeps each).
 * give_item / remove_item (how many were removed) / item_count (-1 if no inventory) / equip_item /
 * drop_item (one) / move_item (the item, a whole stack): the player, NPCs and containers. */
typedef uint32_t (BLSE_CALL *BLSE_SpawnEntityFn)(void *context, const char *classname, const float origin[3], float yaw,
                                                 const char *const *pairs, size_t pair_count);
typedef int (BLSE_CALL *BLSE_TeleportEntityFn)(void *context, uint32_t entity, const float *origin, const float *angles);
typedef int (BLSE_CALL *BLSE_ItemFn)(void *context, uint32_t owner, const char *classname);
typedef int (BLSE_CALL *BLSE_RemoveItemFn)(void *context, uint32_t owner, const char *classname, int count);
typedef int (BLSE_CALL *BLSE_MoveItemFn)(void *context, uint32_t from, uint32_t to, const char *classname);
/* Controls and screens (BLSE b0.5.46), like menu_add_button: the game's checkbox, slider (whole
 * numbers), drop-down (`items`: `item_count` strings), progress bar (0..1) and scrolling list (rows
 * are buttons: BLSE_MSG_MENU_BUTTON with "<list>/<row>"); changes by the player:
 * BLSE_MSG_MENU_CHANGED. menu_value copies a control's value as text; menu_set_value sets it.
 * menu_block keeps a game screen from opening (BLSE_MSG_MENU_BLOCKED). hud_in_menus: the HUD
 * element is also drawn over game screens and menus. */
typedef int (BLSE_CALL *BLSE_MenuAddCheckboxFn)(void *context, const char *screen, const char *parent_path, const char *name,
                                                const char *text, int checked, int x, int y, int w, int h);
typedef int (BLSE_CALL *BLSE_MenuAddSliderFn)(void *context, const char *screen, const char *parent_path, const char *name,
                                              int minimum, int maximum, int value, int x, int y, int w, int h);
typedef int (BLSE_CALL *BLSE_MenuAddDropdownFn)(void *context, const char *screen, const char *parent_path, const char *name,
                                                const char *const *items, size_t item_count, const char *selected, int x, int y, int w, int h);
typedef int (BLSE_CALL *BLSE_MenuAddProgressFn)(void *context, const char *screen, const char *parent_path, const char *name,
                                                float value, int x, int y, int w, int h);
typedef int (BLSE_CALL *BLSE_MenuAddListFn)(void *context, const char *screen, const char *parent_path, const char *name, int x, int y, int w, int h);
typedef int (BLSE_CALL *BLSE_MenuListAddFn)(void *context, const char *screen, const char *list_path, const char *row, const char *text, int height);
typedef int (BLSE_CALL *BLSE_MenuListClearFn)(void *context, const char *screen, const char *list_path);
typedef int (BLSE_CALL *BLSE_MenuValueFn)(void *context, const char *screen, const char *path, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_MenuSetValueFn)(void *context, const char *screen, const char *path, const char *value);
typedef int (BLSE_CALL *BLSE_MenuSetItemsFn)(void *context, const char *screen, const char *path, const char *const *items, size_t item_count);
typedef int (BLSE_CALL *BLSE_MenuBlockFn)(void *context, const char *screen, int blocked);
typedef int (BLSE_CALL *BLSE_HudInMenusFn)(void *context, const char *id, int on);
/* Appearance (BLSE b0.5.47). entity: a server entity's index (1 = the player) or handle; main
 * thread, while a level runs; each returns 1 or 0 (0 also on builds where it is unavailable).
 * Bones: a modifier per bone, applied in the bone's own space after the game built the skeleton:
 * scale (0.05-8 along the bone's axes), offset (model units in the bone's axes, -256..256), angles
 * (pitch, yaw, roll in degrees, -360..360). Children follow a modified bone unless BLSE_BONE_ALONE.
 * BLSE_BONE_HITBOXES: also the bones hits, attachments and bone_position use; BLSE_BONE_KEEP: kept
 * in the save and applied again after loads and level changes (the player on every level).
 * Otherwise a modifier lasts until the entity is gone or the level ends. set_bone_transform sets a
 * bone's whole modifier (scale 1 1 1, offset and angles 0 remove it); reset_bones removes one (NULL:
 * all of the entity's); bone_transform reads it (identity if none); modified_bones lists the bones
 * with modifiers, each followed by '\n'.
 * Flex: the model's flex controllers (facial expressions). flex_names lists them ('\n' after each);
 * flex_value reads one in its own units (minimum..maximum; any out pointer may be NULL; held: 1 while
 * BLSE holds it); set_flex holds it at a value (clamped to its range; sent to the client in 64
 * steps) against the game's expressions until release_flex, the entity is gone or the level ends;
 * BLSE_FLEX_KEEP: kept in the save. Blinking and lip-sync still play on top.
 * Skins, body groups, colour: entity_skin (count: the model's skins); set_entity_skin (0 <=
 * skin < count; BLSE_LOOK_FADE: the game's cross-fade); body_groups copies "name\tcount\tvalue\n"
 * per group; set_body_group by the group's name (0 <= value < count); render_color copies r, g, b, a
 * (0-255) and the render mode and FX (any out pointer but rgba may be NULL); set_render_color and
 * set_render_mode take -1 to keep a value; an alpha below 255 fades the entity (drawn see-through
 * by BLSE until it is gone or the level ends; with BLSE_LOOK_KEEP also after loads). Colour and mode work on any entity. The game keeps all of these in its saves itself;
 * BLSE_LOOK_KEEP applies them again after loads and level changes (the player on every level). */
typedef struct BLSE_BoneTransform {
    float scale[3];
    float offset[3];
    float angles[3];
    unsigned flags; /* BLSE_BONE_* */
} BLSE_BoneTransform;
#define BLSE_BONE_HITBOXES 2u
#define BLSE_BONE_KEEP 8u
#define BLSE_BONE_ALONE 16u
#define BLSE_FLEX_KEEP 8u
#define BLSE_LOOK_KEEP 8u
#define BLSE_LOOK_FADE 16u
typedef int (BLSE_CALL *BLSE_BoneTransformFn)(void *context, uint32_t entity, const char *bone, BLSE_BoneTransform *transform);
typedef int (BLSE_CALL *BLSE_SetBoneTransformFn)(void *context, uint32_t entity, const char *bone, const BLSE_BoneTransform *transform);
typedef int (BLSE_CALL *BLSE_ResetBonesFn)(void *context, uint32_t entity, const char *bone);
typedef int (BLSE_CALL *BLSE_FlexValueFn)(void *context, uint32_t entity, const char *name, float *value, float *minimum, float *maximum, int *held);
typedef int (BLSE_CALL *BLSE_SetFlexFn)(void *context, uint32_t entity, const char *name, float value, unsigned flags);
typedef int (BLSE_CALL *BLSE_ReleaseFlexFn)(void *context, uint32_t entity, const char *name);
typedef int (BLSE_CALL *BLSE_EntitySkinFn)(void *context, uint32_t entity, int *skin, int *count);
typedef int (BLSE_CALL *BLSE_SetEntitySkinFn)(void *context, uint32_t entity, int skin, unsigned flags);
typedef int (BLSE_CALL *BLSE_SetBodyGroupFn)(void *context, uint32_t entity, const char *group, int value, unsigned flags);
typedef int (BLSE_CALL *BLSE_RenderColorFn)(void *context, uint32_t entity, int rgba[4], int *mode, int *fx);
typedef int (BLSE_CALL *BLSE_SetRenderColorFn)(void *context, uint32_t entity, int r, int g, int b, int a, unsigned flags);
typedef int (BLSE_CALL *BLSE_SetRenderModeFn)(void *context, uint32_t entity, int mode, int fx, unsigned flags);
/* Materials: one entity's model drawn with other materials (other entities with the same model are
 * unaffected; drawing only). original: a material of the model, by its path or by its name in the
 * model; model_materials lists the paths ('\n' after each). replacement: a material path (a .vmt
 * under materials/, without "materials/" and ".vmt"), "" hides those meshes, NULL removes the
 * replacement (original NULL as well: all of the entity's). BLSE_LOOK_KEEP: kept in the save.
 * entity_materials copies "original\treplacement\n" for each replacement BLSE draws. */
typedef int (BLSE_CALL *BLSE_SetEntityMaterialFn)(void *context, uint32_t entity, const char *original, const char *replacement, unsigned flags);
/* Mod folders (BLSE b0.5.48; see docs/plugins/world-and-data.md). provide_file: the game reads `data`
 * (`size` bytes) as the game file `path` ("vdata/items/x.txt") from now on, over the game's and the
 * mods' files (merge files still apply); data NULL removes it. add_file_filter: `filter` is called
 * when a game file matching `path` ('*' and '?' allowed) is built, on the main thread, with the
 * file's bytes; to change them it calls set(set_context, new_data, new_size). add_map_filter: the
 * same for a map's entity text when the server parses it (a fresh map load: a new game, the map
 * command, the first visit of a map; not a loaded save or a revisit), with the map's name.
 * list_mods: "name\tfolder\n" for each enabled mod folder, in load order (the last one wins).
 * mod_file_owner: the name of the mod providing `path`, "(merged)" or "(provided)"; 0 if none.
 * The game keeps many files it read: a change reaches only files it opens later. */
typedef void (BLSE_CALL *BLSE_SetBytesFn)(void *set_context, const char *data, size_t size);
typedef void (BLSE_CALL *BLSE_TextFilterFn)(void *user, const char *name, const char *data, size_t size, BLSE_SetBytesFn set, void *set_context);
typedef int (BLSE_CALL *BLSE_ProvideFileFn)(void *context, const char *path, const void *data, size_t size);
typedef int (BLSE_CALL *BLSE_AddFileFilterFn)(void *context, const char *path, BLSE_TextFilterFn filter, void *user);
typedef int (BLSE_CALL *BLSE_AddMapFilterFn)(void *context, BLSE_TextFilterFn filter, void *user);
typedef int (BLSE_CALL *BLSE_ListModsFn)(void *context, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_ModFileOwnerFn)(void *context, const char *path, char *buffer, size_t capacity, size_t *size);
/* Conversations from mods (dialogue content; see docs/plugins/conversations.md).
 * Rows are .dlg text: per row 13 `{ }` columns (number, male text, female text, next line or '#'
 * for an NPC line, condition, action, 7 more texts: the last is a reply's Malkavian text).
 * add_dialog: a whole conversation under a virtual dialogue name ("mymod/guard.dlg"); an NPC with
 * that dialogname (spawn keyvalue or set_keyvalue) uses it. extend_dialog: rows numbered from
 * 10000 on added to a conversation (file matched like hide_dialog_reply's). add_dialog_reply: one
 * reply row (its number is ignored) offered under NPC line npc_line; first != 0 puts it first.
 * All return 1, or 0 for bad input (the reason is logged). */
typedef int (BLSE_CALL *BLSE_DialogRowsFn)(void *context, const char *name, const char *dlg_rows);
typedef int (BLSE_CALL *BLSE_DialogReplyFn)(void *context, const char *file, int npc_line, const char *dlg_row, int first);
/* set_keyvalue: one of an entity's keyvalues, as maps set them ("targetname" renames it,
 * "dialogname" gives an NPC another conversation); keys read only at spawn change nothing later. */
typedef int (BLSE_CALL *BLSE_SetKeyValueFn)(void *context, uint32_t entity, const char *key, const char *value);
/* Companions (BLSE b0.5.48). An NPC that goes with the player
 * through level changes with its whole saved state (the game carries it as it carries the player's
 * items), its inventory, BLSE entity values and kept looks. Main thread, while a level runs.
 * set_companion: on 1 makes the NPC (index or handle) a companion, 0 ends it. options: words,
 * NULL or "" for none: "follow" (BLSE's follow_npc(npc, player) again on every level and after
 * loads), "stay" (registered, does not go along), "range=<units>" (goes along only within that
 * distance of the player). Kept in the save.
 * list_companions copies "handle\tmap\tclass\tname\toptions\n" for each companion of this level
 * (all_maps 1: also those left on other maps, handle field empty).
 * block_companion_map: no companion goes to that map (block 1) or the block ends (0); not saved.
 * Event BLSE_MSG_COMPANION. */
typedef int (BLSE_CALL *BLSE_SetCompanionFn)(void *context, uint32_t npc, int on, const char *options);
typedef int (BLSE_CALL *BLSE_ListCompanionsFn)(void *context, int all_maps, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_BlockCompanionMapFn)(void *context, const char *map, int block);
/* (BLSE b0.5.48) npc_conditions: the AI condition ids set on the NPC now (ids 0-255; 64 no ammo,
 * 79 can fire its ranged weapon, 95/96 too close/too far for it, 97 not facing the enemy, 99 a
 * character it does not hate is in the line of fire, 71 lost its enemy), up to `capacity`, their
 * number in *count. npc_weapon: the class of the weapon in its hands and the rounds in its clip.
 * Both 0 if not an NPC (npc_weapon also without a weapon). */
typedef int (BLSE_CALL *BLSE_NpcConditionsFn)(void *context, uint32_t npc, int *ids, size_t capacity, size_t *count);
typedef int (BLSE_CALL *BLSE_NpcWeaponFn)(void *context, uint32_t npc, char *classname, size_t capacity, size_t *size, int *clip);
/* (BLSE b0.5.48) mod_info: one mod folder, loaded or not, as "key\tvalue\n" lines: folder, name,
 * version, author, description, requires, requires_blse (its mod.ini), position (1 = lowest
 * priority, 0 if not loaded), loaded (1 or 0), reason (why it is not loaded). 0 if no such folder. */
/* (BLSE b0.5.49) screen_size: the screen in pixels as the HUD overlay last drew (positions are
 * fractions of a screen that is usually not square: round shapes need the aspect). 0 before the
 * first frame. */
typedef int (BLSE_CALL *BLSE_ScreenSizeFn)(void *context, int *width, int *height);
/* (BLSE b0.5.49) Clans while a character is made (every way the clan is set: the questionnaire, the
 * sheet's clan list, the console; only while a character is being made). set_chargen_clan_rule: picking
 * `clan` (a name such as "Brujah") gives `replacement` instead; NULL blocks it (the current clan stays);
 * "" removes the rule. The later rule for the same clan wins. Names are matched without regard to case
 * when the clan is picked. add_chargen_clan_filter: `filter` sees every pick after the rules and may set
 * `clan` (a clan number) or `block`. Main thread. A blocked pick leaves the game's current clan, and the
 * sheet shows it. The sheet's arrows skip a blocked clan: the filter is then also asked about the next
 * clans that way. */
typedef struct BLSE_ChargenClan {
    int clan;              /* in/out: the clan number the game is about to apply */
    const char *clan_name; /* in: its name */
    int old_clan;          /* in: the clan applied last while this character is made, -1 none yet */
    int block;             /* out: 1 keeps the current clan */
} BLSE_ChargenClan;
typedef void (BLSE_CALL *BLSE_ChargenClanFilterFn)(BLSE_ChargenClan *c, void *user);
typedef int (BLSE_CALL *BLSE_SetChargenClanRuleFn)(void *context, const char *clan, const char *replacement);
typedef int (BLSE_CALL *BLSE_AddChargenClanFilterFn)(void *context, BLSE_ChargenClanFilterFn filter, void *user);
/* Drawing with Direct3D 9 (BLSE b0.5.49; see docs/plugins/drawing.md). For effects the HUD
 * overlay cannot draw: a blurred background behind a menu, glow, heat haze, 3D shapes.
 *
 * add_draw_callback: `callback` runs on every frame the game's interface is painted, on the main
 * thread, inside BLSE's paint hook, at `when`:
 *   BLSE_DRAW_BEFORE_OVERLAY: everything the game draws is done (the 3D view, its HUD, its menus and
 *     windows, mod panels); BLSE's overlay elements come next, so they stay sharp on top of anything
 *     drawn here (a blur behind a weapon wheel made of hud_* elements).
 *   BLSE_DRAW_AFTER_OVERLAY: after the overlay's elements and the cursor-mode pointer: on top of all.
 * It is called while a level runs, in game screens and the game menu, in the main menu (no level),
 * with the console open, and for the frames the engine paints while a level loads (the loading
 * screen; few of them). It is not called for frames the game does not paint (window minimized), and
 * not at all if BLSE's HUD overlay is unavailable (an unknown vguimatsurface.dll). `frame->flags`
 * say what is on screen: draw only when it suits you (e.g. BLSE_DRAW_HUD for something over play).
 * Callbacks run in the order they were added. The same function and user may be added once per
 * `when`; returns 1, or 0 if `when` is invalid, the callback is NULL or already added.
 * remove_draw_callback: removes it (1), 0 if it was not added. Callbacks go with a plugin that is
 * disabled (a crash, also inside a callback), unloaded or declines to load.
 *
 * The device: `frame->device` (also d3d9_device) is the game's IDirect3DDevice9*; NULL when it is
 * unavailable (another shaderapidx9.dll or MaterialSystem.dll than the analyzed ones, not created
 * yet, or lost: between BLSE_DEVICE_LOST and BLSE_DEVICE_RESET). The scene is open (between the
 * game's BeginScene and EndScene): do not call BeginScene, EndScene, Present or Reset. Render target
 * 0 is the frame being built (normally the back buffer: StretchRect from it to copy the screen).
 * Before each callback BLSE flushes the game's buffered drawing (so the render target holds all of
 * it) and saves the device state: a D3DSBT_ALL state block (render and sampler states, textures,
 * shaders and constants, streams, vertex declaration, viewport, scissor, transforms, clip planes)
 * and render targets 0-3 and the depth-stencil surface; after the callback it puts them all back.
 * So set every state you rely on yourself (the game leaves z, alpha test, cull, scissor, fog, clip
 * planes, shaders, texture stages in any state), and leave nothing else changed: no device or
 * game object may be released, and other device settings (gamma, dialog box mode, cursor, software
 * vertex processing) must stay. Drawing must not take long: it is in every frame. The state is saved
 * and restored around every call, also one that draws nothing: add a callback only while you draw.
 * Use only standard Direct3D 9 calls (DXVK and overlays such as ReShade then work too).
 *
 * Device loss: BLSE_DEVICE_LOST comes before the game resets the device (Alt+Tab in full screen, a
 * resolution or display change): release every D3DPOOL_DEFAULT resource you made (render targets,
 * dynamic buffers, textures in the default pool), state blocks, queries and additional swap chains,
 * at once, or the game's Reset fails. BLSE_DEVICE_RESET comes after it worked: make them again (or
 * when you next draw). D3DPOOL_MANAGED resources survive. Called on the main thread, from the game's
 * own release/restore step (the material system's; no hook). `device` is the device.
 * add_device_callback: 1, or 0 if the callback is NULL or already added. Plugins that use the device
 * must add one (or keep only managed resources).
 * Nothing is installed until a plugin calls add_draw_callback, add_device_callback or d3d9_device. */
enum { BLSE_DRAW_BEFORE_OVERLAY = 0, BLSE_DRAW_AFTER_OVERLAY = 1 };
enum {
    BLSE_DRAW_LEVEL = 1,    /* a level runs (as blse.level_init..level_shutdown; not the main menu) */
    BLSE_DRAW_HUD = 2,      /* BLSE's overlay shows over play: a level runs and no game screen covers it
                               (the console does not; the loading screen does until it fades out) */
    BLSE_DRAW_MENU = 4,     /* a game screen other than the console is open, or the game menu is up */
    BLSE_DRAW_CONSOLE = 8,  /* the console is open */
    BLSE_DRAW_LOADING = 16  /* the loading screen is up (also while it fades out) */
};
typedef struct BLSE_DrawFrame {
    uint32_t struct_size;  /* sizeof(BLSE_DrawFrame) when called */
    int32_t when;          /* BLSE_DRAW_BEFORE_OVERLAY or BLSE_DRAW_AFTER_OVERLAY */
    int32_t width, height; /* the screen in pixels */
    uint32_t flags;        /* BLSE_DRAW_* bits */
    int32_t overlay_alpha; /* 0..255: how much of the game's HUD and BLSE's overlay shows: 255 in play,
                              rising while the loading screen fades out, 0 when hidden */
    void *device;          /* IDirect3DDevice9*, or NULL (see above) */
    uint32_t frame;        /* counts painted frames (both calls of one frame have the same number) */
} BLSE_DrawFrame;
typedef void (BLSE_CALL *BLSE_DrawFn)(const BLSE_DrawFrame *frame, void *user);
typedef int (BLSE_CALL *BLSE_AddDrawCallbackFn)(void *context, int when, BLSE_DrawFn callback, void *user);
typedef int (BLSE_CALL *BLSE_RemoveDrawCallbackFn)(void *context, int when, BLSE_DrawFn callback, void *user);
/* d3d9_device: the game's IDirect3DDevice9* (no reference is added: do not Release it), or NULL as
 * frame->device. Main thread. Outside a draw callback the device is not prepared for drawing. */
typedef void *(BLSE_CALL *BLSE_D3D9DeviceFn)(void *context);
enum { BLSE_DEVICE_LOST = 1, BLSE_DEVICE_RESET = 2 };
typedef void (BLSE_CALL *BLSE_DeviceEventFn)(int event, void *device, void *user);
typedef int (BLSE_CALL *BLSE_AddDeviceCallbackFn)(void *context, BLSE_DeviceEventFn callback, void *user);
typedef int (BLSE_CALL *BLSE_ModInfoFn)(void *context, const char *folder, char *buffer, size_t capacity, size_t *size);
/* (BLSE b0.5.49) Menu sounds: sounds for the game's buttons and mods' own (see
 * docs/plugins/interface.md). set_button_sound: when a button on `screen` matching `button` does
 * `kind`, `file` (below sound/) plays with `volume` 0..1 and `pitch` (100 normal). screen: a screen
 * name ("game_menu" = the main menu and the Esc menu, "options", "character", ...; a mod's window
 * name) or a pattern; button: the path as menu_panels writes it, or the button's own name, or a
 * pattern ('*' any characters, '/' included; '?' one; case ignored); kind: "hover" (it lights up
 * under the pointer), "leave", "press" (left mouse button down on it) or "click" (it fires its
 * action: mouse release, keys, menu_press). For one button and kind, a rule naming it without
 * wildcards beats a wildcard one; otherwise the rule set later wins (from any plugin or Python).
 * file "" removes this plugin's rule. 1 or 0. clear_button_sounds: removes all of this plugin's
 * rules (also done when it is disabled or declines to load). play_ui_sound: plays an interface sound
 * the way the game's main menu plays its own (also on the main menu with no level loaded and over
 * the paused Esc menu); 1 if the engine took it. Main thread. */
typedef int (BLSE_CALL *BLSE_SetButtonSoundFn)(void *context, const char *screen, const char *button, const char *kind,
                                               const char *file, float volume, int pitch);
typedef int (BLSE_CALL *BLSE_ClearButtonSoundsFn)(void *context);
typedef int (BLSE_CALL *BLSE_PlayUiSoundFn)(void *context, const char *file, float volume, int pitch);
/* (BLSE b0.5.49) Animations added to models at run time, without editing their .mdl.
 * add_model_animations: the animation model `anim_mdl` (an animation-only .mdl compiled for the
 * target's skeleton, found through the game's search paths, mod folders included, e.g.
 * "models/mymod/male_idles.mdl") becomes an extra include of `target_mdl` when the engine loads it:
 * its sequences come after the model's own (original sequence numbers stay). A target that other
 * models include (e.g. "models/character/shared/male/npc_allsequences.mdl") means every model that
 * includes it directly. BLSE_ANIMS_REPLACE: the target's sequences with the same names as the
 * animation model's are hidden (renamed "<name>@orig", out of the activity choice), so the new
 * ones play instead; for a shared target in every file it includes (all characters using it), for
 * an exact model only in its own sequences. Two replacers of one name: the later registration
 * wins. Register at plugin load (before models load): a model already loaded gets it at its next
 * load. Paths: case and '/' or a backslash do not matter. Returns 1, 0 for invalid paths, -1 if not
 * available on this game build. A plugin's registrations end when it unloads (models loaded keep
 * theirs until their next load).
 * model_animations: what is registered and applied, "registered<TAB>target<TAB>file<TAB>add|replace<TAB>who"
 * lines, then "model<TAB>file" for each file added to a loaded model and "model<TAB>-name" for each
 * hidden sequence; only lines whose paths contain `filter` (NULL or "" all). Mods without code can
 * use a blse/animations.txt in their folder instead (docs/python/animation.md). */
#define BLSE_ANIMS_REPLACE 1u
typedef int (BLSE_CALL *BLSE_AddModelAnimationsFn)(void *context, const char *target_mdl, const char *anim_mdl, uint32_t flags);
typedef int (BLSE_CALL *BLSE_ModelAnimationsFn)(void *context, const char *filter, char *buffer, size_t capacity, size_t *size);
/* (BLSE b0.5.49) New item classes from mods (docs/python/items.md).
 * add_item_class: `classname` ("item_<name>", 6-63 characters of a-z 0-9 _) becomes an item class
 * the game can create everywhere it creates items (maps, give/GiveItem, merchants' stock, spawning,
 * loading saves): an entity of `template_class`'s C++ class (its behavior: a melee weapon, a gun,
 * a blood pack, a key...) named `classname`, which reads its own data (names, models, pictures,
 * prices, damage, ammunition, sounds) from vdata/items/<classname>.txt; the mod ships that file
 * (a mod folder). Templates: 184 of the game's 202 item classes (docs/python/items.md lists the
 * others: containers, item_g_keyring, item_w_remington_m_700, item_w_unarmed, ...). Register at
 * plugin load, before levels and saves load: a
 * save holding a mod's item needs the class registered (the game crashes loading an entity whose
 * class it cannot create). The same class again: the later registration wins. Returns 1, 0 if
 * refused (the reason is logged), -1 if not available on this game build. A plugin's classes end
 * when it declines to load (they stay if it is disabled after a crash: saves may hold its items). Mods without code can use a blse/items.txt in their folder instead.
 * item_classes: the registered classes, "classname<TAB>template<TAB>who<TAB>created" lines (created:
 * entities made this session). */
typedef int (BLSE_CALL *BLSE_AddItemClassFn)(void *context, const char *classname, const char *template_class);
typedef int (BLSE_CALL *BLSE_ItemClassesFn)(void *context, char *buffer, size_t capacity, size_t *size);
/* Player outfits (BLSE C34; docs/python/appearance.md, docs/plugins/world-and-data.md). The game
 * picks the player's model from clan + gender + armor every time a weapon or armor is wielded or put
 * away (also when first / third person changes), so a plain SetModel does not last. BLSE keeps one
 * player model slot: an outfit (registered by id) or a raw model. While it is filled, BLSE puts its
 * model back whenever the game picks the clan model; scripts' and cutscenes' own models and the
 * Protean beast form pass unchanged. Armor keeps its stats. All six: main thread only (0 from
 * other threads), as every host function.
 * add_outfit: registers outfit `id` (1-63 of a-z 0-9 _) from "key=value" lines, the keys of
 *   blse/outfits.txt: name, male, female, male.<clan>, female.<clan>, skin, body.<group>,
 *   material.<original>. The same id again: the later registration wins. 1 ok, 0 refused (the
 *   reason is logged), -1 not available on this build. A plugin's outfits end when it declines.
 * wear_outfit: puts the outfit on the player (id NULL or "": takes the outfit or model off, the
 *   game's model returns). Kept in the save. 1 ok, 0 refused (logged), -1 not available.
 * worn_outfit: the worn outfit's id ("" none).
 * list_outfits: "id<TAB>who<TAB>name<TAB>male<TAB>female" lines.
 * set_player_model: a raw model in the slot (NULL or "" removes it); options BLSE_LOOK_KEEP keeps it
 *   in the save (else it ends with the level). 1 / 0 / -1 as above.
 * player_model: the model the player has now ("" unknown). */
typedef int (BLSE_CALL *BLSE_AddOutfitFn)(void *context, const char *id, const char *fields);
typedef int (BLSE_CALL *BLSE_WearOutfitFn)(void *context, const char *id);
typedef int (BLSE_CALL *BLSE_WornOutfitFn)(void *context, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_ListOutfitsFn)(void *context, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_SetPlayerModelFn)(void *context, const char *model, unsigned options);
typedef int (BLSE_CALL *BLSE_PlayerModelFn)(void *context, char *buffer, size_t capacity, size_t *size);
/* Animation (BLSE b0.5.49; see docs/plugins/world-and-data.md). Main thread, while a level runs.
 * list_sequences: the entity's sequences in the game's numbering (its model's and the animation
 * models it includes), "index<TAB>name<TAB>activity<TAB>loops<TAB>duration<LF>"
 * each (activity "" if none,
 * loops 1 or 0, duration in seconds). lookup_sequence: a sequence by name (case ignored) or by its
 * number in decimal; -1 if the model has none.
 * play_animation: plays it on the entity, rate 0..16 (1 normal). An NPC keeps it against its own AI
 * until it ends (once; BLSE_ANIM_LOOP: until stop_animation) or the game changes its sequence itself
 * (death, a flinch, a conversation, a scripted scene, a task that sets an activity).
 * BLSE_ANIM_STILL: the NPC stops at once (the game's WAIT_FOR_SCRIPT schedule, started together
 * with the animation) and walks on afterwards. Other entities just get the
 * sequence. stop_animation: ends a mod's animation (0 if none plays).
 * animation_state: the entity's current animation (BLSE_Animation, struct_size set by the caller).
 * set_animation_rule: when an NPC's AI picks a sequence for `activity` (a name such as "ACT_WALK" or
 * a number), one of `sequences` (names separated by commas or spaces, one chosen at random) is used
 * instead, if the model has it. who: "*", "#<handle>", "model:<path part>", a classname or a
 * targetname. sequences NULL or "" removes the rule. A plugin's rules end when it unloads.
 * add_animation_filter: `filter` sees every pick after the rules (activity asked for, the activity
 * after the game's weapon translation, the sequence chosen so far) and returns the sequence to use
 * (an invalid number keeps the choice). Keep it fast: it runs for every NPC's activity change.
 * activity_id: an activity's number by name (-1 if unknown); activity_name copies its name.
 * Events: BLSE_MSG_ANIM_STARTED, BLSE_MSG_ANIM_ENDED, BLSE_MSG_ANIM_CHOSEN. */
#define BLSE_ANIM_LOOP 1u
#define BLSE_ANIM_STILL 2u
typedef struct BLSE_Animation {
    uint32_t struct_size;   /* sizeof(BLSE_Animation) */
    int sequence;           /* m_nSequence */
    int activity;           /* the sequence's activity, -1 none */
    int npc_activity;       /* characters: the activity it plays now (m_Activity), else -1 */
    int ideal_activity;     /* characters: the activity its AI wants (m_IdealActivity), else -1 */
    float cycle;            /* 0..1 through the sequence */
    float rate;             /* playback rate, 1 normal */
    int loops, finished;    /* 1 or 0 */
    int held;               /* 1 while a mod's animation plays (play_animation) */
    char name[64];          /* the sequence's name */
    char activity_name[64]; /* "" if none */
} BLSE_Animation;
typedef int (BLSE_CALL *BLSE_AnimationFilterFn)(void *user, uint32_t entity, int activity, int translated, int sequence);
typedef int (BLSE_CALL *BLSE_ListSequencesFn)(void *context, uint32_t entity, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_LookupSequenceFn)(void *context, uint32_t entity, const char *sequence);
typedef int (BLSE_CALL *BLSE_PlayAnimationFn)(void *context, uint32_t entity, const char *sequence, float rate, unsigned flags);
typedef int (BLSE_CALL *BLSE_StopAnimationFn)(void *context, uint32_t entity);
typedef int (BLSE_CALL *BLSE_AnimationStateFn)(void *context, uint32_t entity, BLSE_Animation *out);
typedef int (BLSE_CALL *BLSE_SetAnimationRuleFn)(void *context, const char *who, const char *activity, const char *sequences);
typedef int (BLSE_CALL *BLSE_AddAnimationFilterFn)(void *context, BLSE_AnimationFilterFn filter, void *user);
typedef int (BLSE_CALL *BLSE_ActivityIdFn)(void *context, const char *name);
typedef int (BLSE_CALL *BLSE_ActivityNameFn)(void *context, int activity, char *buffer, size_t capacity, size_t *size);
/* More places the game picks a sequence (C18b; see docs/python/animation.md). set_animation_rule's
 * rules apply at every one of them, by activity: an NPC's activity change (BLSE_ANIM_PICK_ACTIVITY,
 * the only kind before C18b), a gesture played over the body (GESTURE, CBaseAnimatingOverlay::
 * AddGesture), the melee attack a character picks (MELEE, ChooseMeleeAttackSequence: the NPCs' and
 * the player's), the sequences of a knockback flight that follow the one before it (KNOCKBACK:
 * ACT_KNOCKBACK_FLYING_IDLE, _LAND, _WALL_HIT, _WALL_FALL, _WALL_LAND), and the player's own
 * movement and weapon-layer animations (PLAYER; third person: the player's model is not drawn in
 * first person, whose arms are the weapon's view model). The player is matched only by its
 * classname "player", its name or its handle, never by "*" or "model:".
 * VIEWMODEL (C36): first person, the weapon's view model and the clan hands, picked when a weapon
 * sends an animation (draw, idle, fire, reload, swing, holster, ...). Only rules whose who is
 * "viewmodel" (any weapon), "viewmodel:<text>" (weapon classname, or text in its model path),
 * "hands" or "hands:<text>" (text in the hands' model path) apply there; those words keep their old
 * meaning (a classname or name) at kinds 0-4. Filter `entity`: the weapon (its sequence numbers are
 * its v_ model's) or the hands view model (classname "viewmodel"). A view model's sequence must be
 * below 2048 (11 network bits). Hooked only for such a rule, a filter with this bit, or Python's
 * report_viewmodel_picks; its picks are published as BLSE_MSG_ANIM_CHOSEN only then, never just
 * because someone listens.
 * add_animation_filter_ex: like add_animation_filter, at the kinds in `kinds` (BLSE_ANIM_PICK_*
 * bits; add_animation_filter is BLSE_ANIM_PICK_ACTIVITY only); `kind` tells the filter which one
 * (0 activity, 1 gesture, 2 melee, 3 knockback, 4 player, 5 viewmodel). Returning -1 or an invalid number keeps
 * the choice. NPC melee filters require an attack the game can make. Player weapon choices
 * (including swings at air) and by-name combo follow-ups are also kind 2; combo activity is
 * ACT_MELEE_ATTACK and translated is the weapon activity. add_animation_filter_ex and all six kinds
 * are new in 0.7 (check BLSE_HOST_HAS(host, add_animation_filter_ex)). */
#define BLSE_ANIM_PICK_ACTIVITY 0x01u
#define BLSE_ANIM_PICK_GESTURE 0x02u
#define BLSE_ANIM_PICK_MELEE 0x04u
#define BLSE_ANIM_PICK_KNOCKBACK 0x08u
#define BLSE_ANIM_PICK_PLAYER 0x10u
#define BLSE_ANIM_PICK_VIEWMODEL 0x20u /* C36 */
#define BLSE_ANIM_PICK_ALL 0x3fu
typedef int (BLSE_CALL *BLSE_AnimationFilterExFn)(void *user, uint32_t entity, int activity, int translated, int sequence, int kind);
typedef int (BLSE_CALL *BLSE_AddAnimationFilterExFn)(void *context, BLSE_AnimationFilterExFn filter, void *user, unsigned kinds);
/* Model animation events (C35; docs/python/animation.md#model-animation-events). A model's sequences
 * carry events at points of their cycle: NPC footsteps (2050-2053), script events (1000-1010:
 * scripted sequences' outputs, sounds), weapon events (3000-3999), VTMB's own (4005 a sequence, 4006/4007, 4020, 4100-4102
 * attached models, 2070/2071 physics chains). The server's events (numbers below 5000) of an
 * entity's sequence and its gesture layers pass BLSE before the game handles them; client events
 * (5000 and up: client sounds, muzzle flashes, the client's effects) never reach the server.
 * set_anim_event_rule: when an entity matching `who` (as animation rules, but "*" is every entity
 * with a model, the player included) fires `event`: action "block" (the game does not handle it),
 * "<number>" (handled as that event instead), "<number> <options>" (and with these options, at most
 * 63 characters), "* <options>" (the same event with these options); NULL or "" removes the rule.
 * add_anim_event_filter: `filter` sees every event after the rules (changes to event->event and
 * event->options are used; return 0 to block it). Filters and subscribers run inside the game's
 * think: keep them fast, never start animations or spawn entities there. Event:
 * BLSE_MSG_ANIM_EVENT. Hooks are installed with the first rule, filter or subscriber. */
typedef struct BLSE_AnimEvent {
    uint32_t struct_size;   /* sizeof(BLSE_AnimEvent), set by BLSE */
    int event;              /* the event's number; a filter may change it */
    float cycle;            /* where in the sequence it fires, 0..1 */
    int sequence;           /* the sequence that fires it */
    int layer;              /* -1 the entity's sequence, 0..3 a gesture layer */
    char options[64];       /* its options text; a filter may change it (NUL-terminated) */
} BLSE_AnimEvent;
typedef int (BLSE_CALL *BLSE_AnimEventFilterFn)(void *user, uint32_t entity, BLSE_AnimEvent *event);
typedef int (BLSE_CALL *BLSE_SetAnimEventRuleFn)(void *context, const char *who, int event, const char *action);
typedef int (BLSE_CALL *BLSE_AddAnimEventFilterFn)(void *context, BLSE_AnimEventFilterFn filter, void *user);
typedef int (BLSE_CALL *BLSE_PluginVersionFn)(void *context, const char *name, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_ListPluginsFn)(void *context, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_SaveGetVersionedFn)(void *context, const char *key, void *buffer, size_t capacity, size_t *size, uint32_t *version);
typedef int (BLSE_CALL *BLSE_KeyStateFn)(void *context, const char *key);
typedef int (BLSE_CALL *BLSE_KeyBindingFn)(void *context, const char *key, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_BlockKeyFn)(void *context, const char *key, int blocked);
typedef int (BLSE_CALL *BLSE_PressKeyFn)(void *context, const char *key, int mode);

/* HUD shapes, rotation and fonts (BLSE b0.5.49). Elements share the ids of hud_set; positions are
 * fractions of the screen, here -1..2 so shapes may reach past its edges (they are clipped).
 * hud_line: a line from (x0, y0) to (x1, y1), color 0xRRGGBBAA, width 1..32 pixels.
 * hud_polygon: `xy` holds `count` points as x, y pairs. strip 0: a filled convex polygon (3..128
 * points; concave shapes must be split); strip 1: a strip of quads (an even count 4..128, taken in
 * pairs: points 0 1 3 2 make the first quad, 2 3 5 4 the next, ...) for rings, arcs and thick curves.
 * color fills it (alpha 0 = no fill), border is a 1-pixel outline (0 = none). Clicks (hud_clickable)
 * hit the shape itself, a line within a few pixels.
 * hud_rotate: turns a box or image around its centre by `degrees` clockwise (0 = upright); kept
 * when it is set again. Returns 0 for other kinds.
 * hud_font: the font of a text element or input: a Windows or game font face (NULL or "" = the
 * default font), weight 100..900, BLSE_FONT_* flags; the size stays the element's. Returns 0 if
 * the element does not exist, is another kind or the values are invalid. Main thread only. */
enum { BLSE_FONT_ITALIC = 1, BLSE_FONT_UNDERLINE = 2, BLSE_FONT_STRIKEOUT = 4 };
typedef int (BLSE_CALL *BLSE_HudLineFn)(void *context, const char *id, float x0, float y0, float x1, float y1, uint32_t color, int width);
typedef int (BLSE_CALL *BLSE_HudPolygonFn)(void *context, const char *id, const float *xy, size_t count, uint32_t color, uint32_t border, int strip);
typedef int (BLSE_CALL *BLSE_HudRotateFn)(void *context, const char *id, float degrees);
typedef int (BLSE_CALL *BLSE_HudFontFn)(void *context, const char *id, const char *face, int weight, int flags);

/* 3D scenes in the HUD overlay (phase 2a; docs/plugins/scenes.md). A scene is a HUD element (the
 * same ids, drawing order, hud_in_menus, hud_clickable and hud_remove as the other elements) in
 * which game models (.mdl) are drawn with the scene's own camera and lights, over the world, every
 * rendered frame, without lag. Models are drawn client-side only: nothing is spawned or saved.
 * scene_set: the rectangle (fractions of the screen); makes the scene or moves it. 16 scenes in all.
 * scene_camera: position and angles (pitch, yaw, roll; Source axes, +X forward at yaw 0) in the
 *   scene's own space; position NULL = framed from all visible models (the default).
 * scene_projection: fov (horizontal, 1..170 degrees, default 40); ortho > 0 = orthographic, the view
 *   height in units; znear/zfar (default 1 and 4096).
 * scene_light: point light n (0..3) at `position` (NULL removes it), color 0xRRGGBBAA (alpha unused),
 *   range (0 = automatic), intensity 0..16 (1 normal). The first light set replaces the automatic one.
 * scene_color: BLSE_SCENE_AMBIENT (light from every side; black = none), BLSE_SCENE_CLEAR (fill the
 *   rectangle first), BLSE_SCENE_CLEAR_OFF (show what is behind, the default), BLSE_SCENE_TINT
 *   (colour modulation, alpha = opacity).
 * scene_model: adds or changes object `object` (1-63 characters; 16 a scene). model "models/....mdl"
 *   (NULL keeps the current one); origin/angles NULL keep theirs; scale 0.01..100 (<= 0 keeps it).
 *   The file is checked and loaded at once: 0 if it is missing or not a studio model (logged).
 * scene_animate: sequence by name or decimal number (NULL keeps it), rate -16..16 (0 = posed), cycle
 *   0..1 (< 0 keeps it; a new sequence starts at 0), mode BLSE_SCENE_MODEL (the sequence's own
 *   looping), BLSE_SCENE_LOOP or BLSE_SCENE_HOLD (stop at the end). Animation runs in real time
 *   while the scene is drawn. scene_cycle sets the cycle (0..1).
 * scene_state: the object's state (BLSE_SceneState, struct_size set by the caller).
 * scene_skin: skin (-1 keeps) and the raw body value (-1 keeps); scene_body_group sets one body group
 *   by name or decimal number.
 * scene_visible: object NULL = the whole scene. scene_remove: object NULL = the whole scene.
 * scene_sequences: a model's sequences, "index<TAB>name<TAB>duration<TAB>loops<LF>" each.
 * scene_available: 1 if scenes work on this game build; else 0 and the reason (copied like other
 *   text). All return 1 on success, 0 on failure. Main thread only. A plugin's scenes end with it. */
enum { BLSE_SCENE_AMBIENT = 0, BLSE_SCENE_CLEAR = 1, BLSE_SCENE_TINT = 2, BLSE_SCENE_CLEAR_OFF = 3 };
enum { BLSE_SCENE_MODEL = 0, BLSE_SCENE_LOOP = 1, BLSE_SCENE_HOLD = 2 };
typedef struct BLSE_SceneState {
    uint32_t struct_size;  /* sizeof(BLSE_SceneState) */
    int sequence;          /* number */
    float cycle;           /* 0..1 */
    float rate;
    int mode;              /* BLSE_SCENE_MODEL, _LOOP, _HOLD */
    int finished;          /* a held sequence reached its end */
    int skin, body;
    float scale;
    int visible;
    int loaded;            /* the model is loaded and its renderable made */
    char name[64];         /* the sequence's name ("" if unknown) */
    char model[128];
} BLSE_SceneState;
typedef int (BLSE_CALL *BLSE_SceneSetFn)(void *context, const char *id, float x, float y, float w, float h);
typedef int (BLSE_CALL *BLSE_SceneCameraFn)(void *context, const char *id, const float *position, const float *angles);
typedef int (BLSE_CALL *BLSE_SceneProjectionFn)(void *context, const char *id, float fov, float ortho, float znear, float zfar);
typedef int (BLSE_CALL *BLSE_SceneLightFn)(void *context, const char *id, int n, const float *position, uint32_t color, float range, float intensity);
typedef int (BLSE_CALL *BLSE_SceneColorFn)(void *context, const char *id, int what, uint32_t color);
typedef int (BLSE_CALL *BLSE_SceneModelFn)(void *context, const char *id, const char *object, const char *model, const float *origin, const float *angles, float scale);
typedef int (BLSE_CALL *BLSE_SceneAnimateFn)(void *context, const char *id, const char *object, const char *sequence, float rate, float cycle, int mode);
typedef int (BLSE_CALL *BLSE_SceneCycleFn)(void *context, const char *id, const char *object, float cycle);
typedef int (BLSE_CALL *BLSE_SceneStateFn)(void *context, const char *id, const char *object, BLSE_SceneState *out);
typedef int (BLSE_CALL *BLSE_SceneSkinFn)(void *context, const char *id, const char *object, int skin, int body);
typedef int (BLSE_CALL *BLSE_SceneBodyGroupFn)(void *context, const char *id, const char *object, const char *group, int value);
typedef int (BLSE_CALL *BLSE_SceneVisibleFn)(void *context, const char *id, const char *object, int visible);
typedef int (BLSE_CALL *BLSE_SceneRemoveFn)(void *context, const char *id, const char *object);
typedef int (BLSE_CALL *BLSE_SceneSequencesFn)(void *context, const char *model, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_SceneAvailableFn)(void *context, char *buffer, size_t capacity, size_t *size);

/* Light from the world (docs/plugins/scenes.md, "Light from the world"). Optional: a scene is lit like
 * a spot in the level, by the engine's own lighting there (the light a model standing there gets: an
 * ambient colour from each of 6 sides and the strongest nearby lights), sampled a few times a second
 * and turned so light from the player's left lights the scene from the camera's left.
 * scene_world_light: source BLSE_SCENE_WORLD_OFF (the default: only the scene's own lights),
 *   _PLAYER (where the player stands), _ENTITY (`entity`: index or handle, followed as it moves) or
 *   _POINT (`point`: world coordinates). mix 0..1: the world's share (1 = only the world; the
 *   scene's own lights, automatic light and ambient get 1 - mix). scale 0..16: brightness of the
 *   world's part (1 = as bright as a model standing there). rate 1..60: samples a second (10 is plenty). turn
 *   BLSE_SCENE_TURN_VIEW (directions follow the player's view, the default) or _WORLD (scene axes
 *   are the world's). 0 if the scene does not exist or a value is out of range.
 * scene_world_light_state: the settings and the last sample (BLSE_SceneWorldLightState,
 *   struct_size set by the caller). `active` 0 means the scene is lit by its own lights only (no
 *   level, the entity is gone, or this game build's lighting query was not recognized); `status`
 *   says why. `brightness` is about 0 (dark) to 1 (fully lit white), more under strong lights. */
enum { BLSE_SCENE_WORLD_OFF = 0, BLSE_SCENE_WORLD_PLAYER = 1, BLSE_SCENE_WORLD_ENTITY = 2, BLSE_SCENE_WORLD_POINT = 3 };
enum { BLSE_SCENE_TURN_VIEW = 0, BLSE_SCENE_TURN_WORLD = 1 };
typedef struct BLSE_SceneWorldLightState {
    uint32_t struct_size;  /* sizeof(BLSE_SceneWorldLightState) */
    int source;            /* BLSE_SCENE_WORLD_... */
    uint32_t entity;       /* _ENTITY */
    float mix, scale, rate;
    int turn;              /* BLSE_SCENE_TURN_... */
    int active;            /* a sample lights the scene */
    float cube[6][3];      /* the ambient light drawn from +X, -X, +Y, -Y, +Z, -Z (world axes, linear RGB) */
    int lights;            /* the nearby lights in the sample (the engine uses the 2 strongest by default) */
    float brightness;
    float point[3];        /* the world point sampled last */
    char status[128];      /* "" when active, else why not */
} BLSE_SceneWorldLightState;
typedef int (BLSE_CALL *BLSE_SceneWorldLightFn)(void *context, const char *id, int source, uint32_t entity, const float *point, float mix, float scale,
                                                float rate, int turn);
typedef int (BLSE_CALL *BLSE_SceneWorldLightStateFn)(void *context, const char *id, BLSE_SceneWorldLightState *out);
/* Particle effects and screen overlays (docs/plugins/effects.md).
 * An effect is a particle definition (particles/<name>.txt) shown at a place under an id of 1-63
 * characters, scoped to the plugin (like HUD ids). Places (BLSE_FxPlace, struct_size set by the caller):
 *   BLSE_FX_WORLD   position and angles in the level. A client-side emitter (the game's own pool); not
 *                   saved; ends with the level.
 *   BLSE_FX_ENTITY  on an entity (index or handle): attach BLSE_FX_ORIGIN (follows its origin),
 *                   _BONE (name = bone), _ATTACHMENT (name = model attachment), _HITBOX (name = a bone,
 *                   NULL = all), _TREE, _TREE_COLORS (the whole skeleton). Follows the bones; ends with
 *                   the level or the entity.
 *   BLSE_FX_SCREEN  on the screen: screen BLSE_FX_CENTER, _BORDER, _BOTTOM, _RANDOM. Made through the
 *                   player's PlayHUDParticle input (first person; third_person also draws it in third
 *                   person). Kept across level changes and loads (BLSE makes it again), saved switched off.
 * Options (BLSE_FxOptions, NULL = defaults): `set` says which of rate (0..16), size (0..4), alpha (0..1)
 * and rgb (0xRRGGBB colour scale) to apply; third_person (screen); life in seconds (0 = until stopped).
 * fx_start: makes or replaces the effect; 1 when made or waiting for the game to catch up (a definition
 *   precached this frame, an entity spawned this frame: up to 2 seconds; a screen effect while no level
 *   runs: at the next level start); 0 with the reason in blse.log (no level, no definition file, no
 *   bone, the precache table nearly full, 64 world/entity or 16 screen effects in all).
 * fx_move: a world effect moves; any other place starts it again there. fx_options: replaces the options.
 * fx_stop: now 0 stops spawning (the particles fade), 1 removes at once. fx_list: "id\n" per effect, then
 *   "overlay:id\n" per screen overlay.
 * precache_particle: puts a definition into the game's precache table (256 entries; BLSE leaves 16).
 * view_model: the handle of the player's view model 0 (the hands) or 1, 0 when none.
 * screen_effect: a full-screen material ("effects/redvision", "mymod/overlay"; materials/<name>.vmt)
 *   drawn the way the Disciplines' overlays are (CViewRender::ScreenFeedbackOverlay), after the world and
 *   the view model, under the HUD, at full resolution. alpha 0..1; blur: radius 0..0.05 in texture
 *   coordinates (materials with $blur; the game uses 0.002..0.005); expand 0..1 (texture coordinates
 *   pulled in: a zoom); rgb colour; frame: what a material reading the screen sees: BLSE_FX_FRAME_FULL
 *   (this frame at full size, as red vision), _DELAY (the game's 512x512 delayed copy, as Obfuscate and
 *   Auspex), _NONE (no copy); order -100..100 (lower draws first). material NULL or ""
 *   removes it. 16 in all, every mod's drawn. Kept across levels, loads and engine restarts; not saved.
 * screen_effect_alpha: changes only the alpha (fades every frame without a material lookup).
 * fx_available: kind BLSE_FX_WORLD/_ENTITY/_SCREEN, or BLSE_FX_OVERLAY for screen_effect: 1 if it works
 *   on this game build; else 0 and the reason (copied like other text).
 * All return 1 on success, 0 on failure. Main thread only. A plugin's effects end with it. */
enum { BLSE_FX_WORLD = 0, BLSE_FX_ENTITY = 1, BLSE_FX_SCREEN = 2, BLSE_FX_OVERLAY = 3 };
enum { BLSE_FX_ORIGIN = 0, BLSE_FX_BONE = 1, BLSE_FX_ATTACHMENT = 2, BLSE_FX_HITBOX = 3, BLSE_FX_TREE = 4, BLSE_FX_TREE_COLORS = 5 };
enum { BLSE_FX_CENTER = 0, BLSE_FX_BORDER = 1, BLSE_FX_BOTTOM = 2, BLSE_FX_RANDOM = 3 };
enum { BLSE_FX_SET_RATE = 1, BLSE_FX_SET_SIZE = 2, BLSE_FX_SET_ALPHA = 4, BLSE_FX_SET_COLOR = 8 };
typedef struct BLSE_FxPlace {
    uint32_t struct_size;  /* sizeof(BLSE_FxPlace) */
    int kind;              /* BLSE_FX_WORLD, _ENTITY, _SCREEN */
    float position[3];     /* world */
    float angles[3];       /* world: pitch, yaw, roll */
    uint32_t entity;       /* entity: index or handle */
    int attach;            /* entity: BLSE_FX_ORIGIN .. BLSE_FX_TREE_COLORS */
    const char *name;      /* entity: bone or attachment name (NULL for none) */
    int screen;            /* screen: BLSE_FX_CENTER .. BLSE_FX_RANDOM */
} BLSE_FxPlace;
typedef struct BLSE_FxOptions {
    uint32_t struct_size;  /* sizeof(BLSE_FxOptions) */
    uint32_t set;          /* BLSE_FX_SET_* bits: which of the next four apply */
    float rate, size, alpha;
    uint32_t rgb;          /* 0xRRGGBB */
    int third_person;      /* screen: also in third person */
    float life;            /* seconds; 0 = until stopped */
    int until_death;       /* entity: 1 = stops when the entity dies (0: follows the body, also a corpse) */
} BLSE_FxOptions;
typedef int (BLSE_CALL *BLSE_FxStartFn)(void *context, const char *id, const char *definition, const BLSE_FxPlace *place, const BLSE_FxOptions *options);
typedef int (BLSE_CALL *BLSE_FxMoveFn)(void *context, const char *id, const BLSE_FxPlace *place);
typedef int (BLSE_CALL *BLSE_FxOptionsFn)(void *context, const char *id, const BLSE_FxOptions *options);
typedef int (BLSE_CALL *BLSE_FxStopFn)(void *context, const char *id, int now);
typedef int (BLSE_CALL *BLSE_FxListFn)(void *context, char *buffer, size_t capacity, size_t *size);
typedef int (BLSE_CALL *BLSE_PrecacheParticleFn)(void *context, const char *definition);
typedef uint32_t (BLSE_CALL *BLSE_ViewModelFn)(void *context, int index);
enum { BLSE_FX_FRAME_NONE = 0, BLSE_FX_FRAME_FULL = 1, BLSE_FX_FRAME_DELAY = 2 };
typedef int (BLSE_CALL *BLSE_ScreenEffectFn)(void *context, const char *id, const char *material, float alpha, float blur, float expand, uint32_t rgb, int frame, int order);
typedef int (BLSE_CALL *BLSE_ScreenEffectAlphaFn)(void *context, const char *id, float alpha);
typedef int (BLSE_CALL *BLSE_FxAvailableFn)(void *context, int kind, char *buffer, size_t capacity, size_t *size);
/* Skill attempts: lockpicking taken over by mods (0.7; docs/plugins/world-and-data.md). Every lock
 * (prop_doorknob, prop_padlock, item_container_lock, prop_doorknob_electronic) is a skill entity. While a
 * plugin takes a class, the game still runs its own checks when the player uses a locked one (key,
 * requires key, "need lockpicks") and its start (the Intrusion alert, OnSkillAttemptBegin), but not its
 * timed attempt: BLSE publishes BLSE_MSG_SKILL_ATTEMPT and the plugin ends the attempt with
 * skill_result. Doors the player has the key for, and unlocked ones, are never taken. Main thread only.
 * take_skill_attempts: `classes` comma-separated class names ("prop_doorknob, prop_padlock"), "*" for
 *   every skill entity, NULL or "" to stop. Replaces the plugin's earlier call. The first owner (in the
 *   order they took) taking a class gets its attempts. 1 ok, 0 refused (logged), -1 not available.
 * skill_info: the lock's state; 1, 0 if `entity` (index or handle) is not a skill entity, -1 not available.
 * skill_result: "success" (unlocked; OnSkillSuccess), "fail" (OnSkillFail), "botch" (OnSkillBotch): the
 *   game's own handler with the player, and the lock's last roll set to 3 / 1 / 0; or "cancel" (no
 *   result). Each ends the player's attempt on that entity if there is one. 1 / 0 (logged) / -1.
 * hud_cursor_pointer: while this plugin has cursor mode on, shown 0 hides the mouse pointer BLSE draws
 *   (the mouse still moves freely and clicks are still reported). The pointer is drawn while any owner in
 *   cursor mode wants it. 1, or 0 if cursor mode is not available. */
typedef struct BLSE_SkillInfo {
    uint32_t handle;
    int difficulty;     /* the lock's difficulty (the maps use 0-10) */
    int skill;          /* the lock's skill type (1 for every lock in the game's maps) */
    int roll;           /* the last roll: below 3 = locked */
    int attempts;
    int locked, requires_key, has_key;  /* has_key: the player carries the key named in `key` */
    int active;         /* the player's current attempt is on this entity */
    int taken;          /* ... and a mod holds it */
    char classname[64];
    char name[128];
    char model[128];
    char key[128];
    char taken_by[64];  /* "python", a plugin's name, or "" */
} BLSE_SkillInfo;
typedef int (BLSE_CALL *BLSE_TakeSkillAttemptsFn)(void *context, const char *classes);
typedef int (BLSE_CALL *BLSE_SkillInfoFn)(void *context, uint32_t entity, BLSE_SkillInfo *out);
typedef int (BLSE_CALL *BLSE_SkillResultFn)(void *context, uint32_t entity, const char *result);
typedef int (BLSE_CALL *BLSE_HudCursorPointerFn)(void *context, int shown);

/* Game speed (0.7): slow motion and a near stop, shared by mods. BLSE sets the engine's
 * host_timescale: the whole world slows (thinking, physics, animation, timers, NPCs); the HUD overlay,
 * BLSE scenes and sounds keep real speed, and blse.game_frame keeps coming.
 * time_scale: asks for a speed, 0.001 (nearly stopped; the engine ignores 0) to 10; 1.0 withdraws the
 *   request. The slowest request of all mods applies; when none is left, the value from before the first
 *   one is set again. A request ends by itself when the plugin unloads or crashes, and at every level
 *   end (also loading a save). Main thread only. 1 ok, 0 refused (logged).
 * time_scale_current: the speed the game runs at now (1.0 if it cannot be read). */
typedef int (BLSE_CALL *BLSE_TimeScaleFn)(void *context, float scale);
typedef float (BLSE_CALL *BLSE_TimeScaleCurrentFn)(void *context);

/* Camera hold in first person, and hiding the game's HUD (0.7; for full-screen interfaces such as
 * a lock screen, photo modes and cut scenes).
 * camera_override_ex: camera_override with flags. BLSE_CAMERA_KEEP_MODE: a placed position does not switch
 *   to third person; the view mode stays as the player has it (in first person no character is drawn,
 *   and the hands and weapon are drawn at the placed view). flags 0 = camera_override. Both NULL: the
 *   game's view again (only if this plugin placed it).
 * hide_game_hud: 1 hides the game's own HUD (health, blood, stealth, Disciplines, the use icon, info texts,
 *   timers, the subtitles; not the dialogue, signs, credits or BLSE's HUD overlay, scenes and effects),
 *   at once, with no fade; 0 withdraws this plugin's request. The HUD comes back when no mod asks any
 *   more; requests end when the plugin unloads or crashes, and at every level end (also loading a save).
 *   Hiding needs a running level. Main thread only. 1 ok, 0 refused (logged).
 * game_hud_hidden: 1 if some mod has the game's HUD hidden. */
enum { BLSE_CAMERA_KEEP_MODE = 1 };
typedef int (BLSE_CALL *BLSE_CameraOverrideExFn)(void *context, const float *origin, const float *angles, int flags);
typedef int (BLSE_CALL *BLSE_HideGameHudFn)(void *context, int hide);
typedef int (BLSE_CALL *BLSE_GameHudHiddenFn)(void *context);

/* Memory Expansion's process split ([Split] enabled = 1 in memory_expansion.ini): the game's world
 * (vampire.dll: entities, AI, Python, skill attempts, saves) runs in a second Vampire.exe with no window,
 * the game window (client.dll: drawing, the HUD overlay, 3D scenes, keys, mouse, sounds, the camera) in
 * the first. Each loads BLSE and its plugins. A plugin that needs both sides runs its world part where
 * the world is and its screen part where the window is, and the two talk with split_send.
 * split_role: BLSE_SPLIT_NONE (one process: single player, everything here), BLSE_SPLIT_WORLD (this
 *   process has the world and no window), BLSE_SPLIT_SCREEN (this process has the window; the world is
 *   in the other process while split_linked says so: without it, e.g. when the world process could not
 *   start, this process runs its own world as in single player).
 * split_linked: 1 while the other process is connected.
 * split_send: publishes `topic` with the payload (copied) to the subscribers in the OTHER process, on its
 *   main thread at its next frame, in the order sent. Topics starting with "blse." are reserved. 1 sent,
 *   0 not linked (or a bad topic / more than 1 MB). In the screen process, while it shows a level of the
 *   world process, blse.level_init / blse.level_shutdown follow client.dll's level and blse.game_frame
 *   comes every painted frame; world events (game_loaded, new_game, before_save, skill attempts, entity
 *   events) stay in the world process. */
enum { BLSE_SPLIT_NONE = 0, BLSE_SPLIT_WORLD = 1, BLSE_SPLIT_SCREEN = 2 };
typedef int (BLSE_CALL *BLSE_SplitRoleFn)(void *context);
typedef int (BLSE_CALL *BLSE_SplitLinkedFn)(void *context);
typedef int (BLSE_CALL *BLSE_SplitSendFn)(void *context, const char *topic, const void *payload, size_t size);

typedef struct BLSE_HostAPI {
    uint32_t struct_size;
    uint32_t api_version;
    BLSE_LogFn log;
    /* --- messaging (BLSE 0.1) --- */
    void *context;
    BLSE_SubscribeFn subscribe;
    BLSE_UnsubscribeFn unsubscribe;
    BLSE_PublishFn publish;
    /* --- runtime information (BLSE 0.2) --- */
    const char *runtime_version; /* e.g. "b0.5.43"; valid for the process lifetime */
    const char *build_profile;   /* recognized binary profile ID, or NULL when the build is unknown */
    /* --- save-game data (BLSE 0.3) --- */
    BLSE_SaveSetFn save_set;
    BLSE_SaveGetFn save_get;
    /* --- console (BLSE 0.4) --- */
    BLSE_AddCommandFn add_command;
    BLSE_ConsolePrintFn console_print;
    /* --- game Python (BLSE b0.5.2) --- */
    BLSE_PythonExecFn python_exec;
    BLSE_PythonEvalFn python_eval;
    /* --- damage (BLSE b0.5.3) --- */
    BLSE_AddDamageFilterFn add_damage_filter;
    /* --- Python functions (BLSE b0.5.7) --- */
    BLSE_AddPythonFunctionFn add_python_function;
    /* --- inventory (BLSE b0.5.12) --- */
    BLSE_ListInventoryFn list_inventory;
    /* --- on-screen text (BLSE b0.5.16) --- */
    BLSE_ShowTextFn show_text;
    /* --- HUD overlay (BLSE b0.5.18) --- */
    BLSE_HudSetFn hud_set;
    BLSE_HudRemoveFn hud_remove;
    /* --- Disciplines (BLSE b0.5.20) --- */
    BLSE_AddDisciplineFilterFn add_discipline_filter;
    /* --- dialogue control (BLSE b0.5.22) --- */
    BLSE_AddDialogFilterFn add_dialog_filter;
    /* --- HUD boxes, images, clicks and input (BLSE b0.5.24) --- */
    BLSE_HudBoxFn hud_box;
    BLSE_HudImageFn hud_image;
    BLSE_HudClickableFn hud_clickable;
    BLSE_HudCursorFn hud_cursor;
    BLSE_KeyStateFn key_state;
    BLSE_KeyBindingFn key_binding;
    BLSE_BlockKeyFn block_key;
    BLSE_PressKeyFn press_key;
    /* --- text entry, alignment, sizes (BLSE b0.5.25) --- */
    BLSE_HudInputFn hud_input;
    BLSE_HudFocusFn hud_focus;
    BLSE_HudInputTextFn hud_input_text;
    BLSE_HudAlignFn hud_align;
    BLSE_HudSizeFn hud_size;
    /* --- versioned save records (BLSE b0.5.25) --- */
    BLSE_SaveSetVersionedFn save_set_versioned;
    BLSE_SaveGetVersionedFn save_get_versioned;
    /* --- plugin information (BLSE b0.5.26) --- */
    BLSE_PluginVersionFn plugin_version;
    BLSE_ListPluginsFn list_plugins;
    BLSE_HudValignFn hud_valign;
    BLSE_ControlStateFn control_state;
    /* --- main-thread tasks, player targets (BLSE b0.5.27) --- */
    BLSE_AddTaskFn add_task;
    BLSE_PlayerTargetFn player_target;
    /* --- hooks for plugins (BLSE b0.5.28) --- */
    BLSE_HookFunctionFn hook_function;
    BLSE_HookCallFn hook_call;
    BLSE_HookPointerFn hook_pointer;
    BLSE_UnhookFn unhook;
    BLSE_FindPatternFn find_pattern;
    /* --- console variables, items, input filters (BLSE b0.5.29) --- */
    BLSE_CvarGetFn cvar_get;
    BLSE_CvarSetFn cvar_set;
    BLSE_ItemInfoFn item_info;
    BLSE_ListItemsFn list_items;
    BLSE_AddInputFilterFn add_input_filter;
    /* --- game menus' contents, camera, sounds (BLSE b0.5.30) --- */
    BLSE_OpenMenusFn open_menus;
    BLSE_MenuPanelsFn menu_panels;
    BLSE_MenuSetTextFn menu_set_text;
    BLSE_MenuSetVisibleFn menu_set_visible;
    BLSE_MenuMoveFn menu_move;
    BLSE_CameraGetFn camera_get;
    BLSE_CameraSetFn camera_set;
    BLSE_PlaySoundFn play_sound;
    BLSE_StopSoundFn stop_sound;
    /* --- more of game menus, camera view, positional sounds (BLSE b0.5.31) --- */
    BLSE_MenuSetImageFn menu_set_image;
    BLSE_MenuPressFn menu_press;
    BLSE_MenuCommandFn menu_command;
    BLSE_MenuAddLabelFn menu_add_label;
    BLSE_MenuRemoveFn menu_remove;
    BLSE_CameraViewFn camera_view;
    BLSE_PlaySoundAtFn play_sound_at;
    BLSE_PlaySoundOnFn play_sound_on;
    BLSE_StopSoundOnFn stop_sound_on;
    BLSE_SoundLengthFn sound_length;
    /* --- added buttons and images, camera override (BLSE b0.5.32) --- */
    BLSE_MenuAddButtonFn menu_add_button;
    BLSE_MenuAddImageFn menu_add_image;
    BLSE_CameraOverrideFn camera_override;
    /* --- NPC fights, item changes (BLSE b0.5.33) --- */
    BLSE_CombatNpcsFn combat_npcs;
    BLSE_ItemSetFn item_set;
    BLSE_ItemResetFn item_reset;
    /* --- settings, files, items not loaded yet (BLSE b0.5.35) --- */
    BLSE_CvarListFn cvar_list;
    BLSE_ReadFileFn read_file;
    BLSE_FindFilesFn find_files;
    BLSE_ItemFilesFn item_files;
    /* --- windows and text fields (BLSE b0.5.36) --- */
    BLSE_WindowCreateFn window_create;
    BLSE_WindowShowFn window_show;
    BLSE_WindowRemoveFn window_remove;
    BLSE_MenuAddTextEntryFn menu_add_text_entry;
    BLSE_HudMultilineFn hud_multiline;
    BLSE_HudHintFn hud_hint;
    /* --- restyling and settings files (BLSE b0.5.37) --- */
    BLSE_MenuSetColorsFn menu_set_colors;
    BLSE_MenuSetFontFn menu_set_font;
    BLSE_MenuResetStyleFn menu_reset_style;
    BLSE_MenuSetHintFn menu_set_hint;
    BLSE_IniGetFn ini_get;
    BLSE_IniSetFn ini_set;
    BLSE_MenuSetBorderFn menu_set_border;
    /* --- models, translated strings, frame rate (BLSE b0.5.38) --- */
    BLSE_BoneNamesFn bone_names;
    BLSE_BonePositionFn bone_position;
    BLSE_ModelScaleFn model_scale;
    BLSE_SetModelScaleFn set_model_scale;
    BLSE_TranslateFn translate;
    BLSE_TranslationLanguageFn translation_language;
    BLSE_FrameStatsFn frame_stats;
    /* --- values on entities, finding entities, JSON files (BLSE b0.5.40) --- */
    BLSE_EntityDataSetFn entity_data_set;
    BLSE_EntityDataGetFn entity_data_get;
    BLSE_EntityDataKeysFn entity_data_keys;
    BLSE_EntityDataFindFn entity_data_find;
    BLSE_EntityPositionFn entity_position;
    BLSE_FindEntitiesFn find_entities;
    BLSE_JsonGetFn json_get;
    BLSE_JsonSetFn json_set;
    /* --- damage, velocity, fixes (BLSE b0.5.41) --- */
    BLSE_DamageEntityFn damage_entity;
    BLSE_EntityVelocityFn entity_velocity;
    BLSE_PushEntityFn push_entity;
    BLSE_FixStateFn fix_state;
    BLSE_ConsoleCommandFn console_command;
    /* --- the shared database (BLSE b0.5.42) --- */
    BLSE_DbGetFn db_get;
    BLSE_DbSetFn db_set;
    BLSE_TranslateTextFn translate_text;
    /* --- NPC AI (BLSE b0.5.45) --- */
    BLSE_FireInputFn fire_input;
    BLSE_SetRelationshipFn set_relationship;
    BLSE_RelationshipFn relationship;
    BLSE_SetEnemyFn set_enemy;
    BLSE_NpcEnemyFn npc_enemy;
    BLSE_StartScheduleFn start_schedule;
    BLSE_NpcScheduleFn npc_schedule;
    BLSE_NpcStateFn npc_state;
    BLSE_MoveNpcFn move_npc;
    BLSE_FollowNpcFn follow_npc;
    BLSE_NpcSensesFn npc_senses;
    BLSE_SetNpcSenseFn set_npc_sense;
    BLSE_NpcLeaderFn npc_leader;
    /* --- world, inventories, controls, screens (BLSE b0.5.46) --- */
    BLSE_SpawnEntityFn spawn_entity;
    BLSE_TeleportEntityFn teleport_entity;
    BLSE_ItemFn give_item;
    BLSE_RemoveItemFn remove_item;
    BLSE_ItemFn item_count;
    BLSE_ItemFn equip_item;
    BLSE_ItemFn drop_item;
    BLSE_MoveItemFn move_item;
    BLSE_MenuAddCheckboxFn menu_add_checkbox;
    BLSE_MenuAddSliderFn menu_add_slider;
    BLSE_MenuAddDropdownFn menu_add_dropdown;
    BLSE_MenuAddProgressFn menu_add_progress;
    BLSE_MenuAddListFn menu_add_list;
    BLSE_MenuListAddFn menu_list_add;
    BLSE_MenuListClearFn menu_list_clear;
    BLSE_MenuValueFn menu_value;
    BLSE_MenuSetValueFn menu_set_value;
    BLSE_MenuSetItemsFn menu_set_items;
    BLSE_MenuBlockFn menu_block;
    BLSE_HudInMenusFn hud_in_menus;
    /* --- appearance (BLSE b0.5.47) --- */
    BLSE_BoneTransformFn bone_transform;
    BLSE_SetBoneTransformFn set_bone_transform;
    BLSE_ResetBonesFn reset_bones;
    BLSE_BoneNamesFn modified_bones;
    BLSE_BoneNamesFn flex_names;
    BLSE_FlexValueFn flex_value;
    BLSE_SetFlexFn set_flex;
    BLSE_ReleaseFlexFn release_flex;
    BLSE_EntitySkinFn entity_skin;
    BLSE_SetEntitySkinFn set_entity_skin;
    BLSE_BoneNamesFn body_groups;
    BLSE_SetBodyGroupFn set_body_group;
    BLSE_RenderColorFn render_color;
    BLSE_SetRenderColorFn set_render_color;
    BLSE_SetRenderModeFn set_render_mode;
    BLSE_SetEntityMaterialFn set_entity_material;
    BLSE_BoneNamesFn model_materials;
    BLSE_BoneNamesFn entity_materials;
    /* --- mod folders (b0.5.48) --- */
    BLSE_ProvideFileFn provide_file;
    BLSE_AddFileFilterFn add_file_filter;
    BLSE_AddMapFilterFn add_map_filter;
    BLSE_ListModsFn list_mods;
    BLSE_ModFileOwnerFn mod_file_owner;
    /* --- conversations from mods, keyvalues (b0.5.48) --- */
    BLSE_DialogRowsFn add_dialog;
    BLSE_DialogRowsFn extend_dialog;
    BLSE_DialogReplyFn add_dialog_reply;
    BLSE_SetKeyValueFn set_keyvalue;
    BLSE_SetCompanionFn set_companion;
    BLSE_ListCompanionsFn list_companions;
    BLSE_BlockCompanionMapFn block_companion_map;
    /* --- why an NPC does not fire (b0.5.48) --- */
    BLSE_NpcConditionsFn npc_conditions;
    BLSE_NpcWeaponFn npc_weapon;
    /* --- mod folders: mod.ini (b0.5.48) --- */
    BLSE_ModInfoFn mod_info;
    /* --- HUD shapes, rotation and fonts (b0.5.49) --- */
    BLSE_HudLineFn hud_line;
    BLSE_HudPolygonFn hud_polygon;
    BLSE_HudRotateFn hud_rotate;
    BLSE_HudFontFn hud_font;
    /* --- animation (b0.5.49) --- */
    BLSE_ListSequencesFn list_sequences;
    BLSE_LookupSequenceFn lookup_sequence;
    BLSE_PlayAnimationFn play_animation;
    BLSE_StopAnimationFn stop_animation;
    BLSE_AnimationStateFn animation_state;
    BLSE_SetAnimationRuleFn set_animation_rule;
    BLSE_AddAnimationFilterFn add_animation_filter;
    BLSE_ActivityIdFn activity_id;
    BLSE_ActivityNameFn activity_name;
    /* --- the screen size (b0.5.49) --- */
    BLSE_ScreenSizeFn screen_size;
    /* --- clans while a character is made (b0.5.49) --- */
    BLSE_SetChargenClanRuleFn set_chargen_clan_rule;
    BLSE_AddChargenClanFilterFn add_chargen_clan_filter;
    /* --- menu sounds (b0.5.49) --- */
    BLSE_SetButtonSoundFn set_button_sound;
    BLSE_ClearButtonSoundsFn clear_button_sounds;
    BLSE_PlayUiSoundFn play_ui_sound;
    /* --- drawing with Direct3D 9 (b0.5.49) --- */
    BLSE_AddDrawCallbackFn add_draw_callback;
    BLSE_RemoveDrawCallbackFn remove_draw_callback;
    BLSE_D3D9DeviceFn d3d9_device;
    BLSE_AddDeviceCallbackFn add_device_callback;
    /* --- animations added to models at run time (b0.5.49) --- */
    BLSE_AddModelAnimationsFn add_model_animations;
    BLSE_ModelAnimationsFn model_animations;
    /* --- new item classes (b0.5.49) --- */
    BLSE_AddItemClassFn add_item_class;
    BLSE_ItemClassesFn item_classes;
    /* --- player outfits (C34) --- */
    BLSE_AddOutfitFn add_outfit;
    BLSE_WearOutfitFn wear_outfit;
    BLSE_WornOutfitFn worn_outfit;
    BLSE_ListOutfitsFn list_outfits;
    BLSE_SetPlayerModelFn set_player_model;
    BLSE_PlayerModelFn player_model;
    /* --- how the build was recognized (0.6.3) --- */
    /* NULL when build_profile is NULL. "" when the game DLLs are files BLSE knows. Otherwise the DLLs
     * a patch (such as a newer Unofficial Patch) edited in place, comma-separated, e.g. "vampire.dll":
     * the same build, so addresses are the profile's, but some code differs. Compare a function's
     * bytes before you hook, call or change it (hook_function does this for you). */
    const char *build_edited;
    /* --- more animation choices (C18b) and model animation events (C35) --- */
    BLSE_AddAnimationFilterExFn add_animation_filter_ex;
    BLSE_SetAnimEventRuleFn set_anim_event_rule;
    BLSE_AddAnimEventFilterFn add_anim_event_filter;
    /* --- 3D scenes in the HUD overlay (phase 2a) --- */
    BLSE_SceneSetFn scene_set;
    BLSE_SceneCameraFn scene_camera;
    BLSE_SceneProjectionFn scene_projection;
    BLSE_SceneLightFn scene_light;
    BLSE_SceneColorFn scene_color;
    BLSE_SceneModelFn scene_model;
    BLSE_SceneAnimateFn scene_animate;
    BLSE_SceneCycleFn scene_cycle;
    BLSE_SceneStateFn scene_state;
    BLSE_SceneSkinFn scene_skin;
    BLSE_SceneBodyGroupFn scene_body_group;
    BLSE_SceneVisibleFn scene_visible;
    BLSE_SceneRemoveFn scene_remove;
    BLSE_SceneSequencesFn scene_sequences;
    BLSE_SceneAvailableFn scene_available;
    /* --- 3D scenes: light from the world --- */
    BLSE_SceneWorldLightFn scene_world_light;
    BLSE_SceneWorldLightStateFn scene_world_light_state;
    /* --- particle effects and screen overlays --- */
    BLSE_FxStartFn fx_start;
    BLSE_FxMoveFn fx_move;
    BLSE_FxOptionsFn fx_options;
    BLSE_FxStopFn fx_stop;
    BLSE_FxListFn fx_list;
    BLSE_PrecacheParticleFn precache_particle;
    BLSE_ViewModelFn view_model;
    BLSE_ScreenEffectFn screen_effect;
    BLSE_ScreenEffectAlphaFn screen_effect_alpha;
    BLSE_FxAvailableFn fx_available;
    /* --- skill attempts: lockpicking taken over by mods (0.7) --- */
    BLSE_TakeSkillAttemptsFn take_skill_attempts;
    BLSE_SkillInfoFn skill_info;
    BLSE_SkillResultFn skill_result;
    BLSE_HudCursorPointerFn hud_cursor_pointer;
    /* --- game speed (0.7) --- */
    BLSE_TimeScaleFn time_scale;
    BLSE_TimeScaleCurrentFn time_scale_current;
    /* --- camera hold in first person, hiding the game's HUD (0.7) --- */
    BLSE_CameraOverrideExFn camera_override_ex;
    BLSE_HideGameHudFn hide_game_hud;
    BLSE_GameHudHiddenFn game_hud_hidden;
    /* --- Memory Expansion's process split (0.7.1) --- */
    BLSE_SplitRoleFn split_role;
    BLSE_SplitLinkedFn split_linked;
    BLSE_SplitSendFn split_send;
} BLSE_HostAPI;

typedef int (BLSE_CALL *BLSE_PluginLoadFn)(const BLSE_HostAPI *host);

/* Plugin information (BLSE b0.5.26, optional). Export it with
 * BLSE_PLUGIN_INFO; BLSE reads it before running any plugin code:
 *
 *     BLSE_PLUGIN_INFO("mymod", "1.2.0", "Author", "What it does", "b0.5.26", "otherplugin>=1.0, thirdplugin");
 *
 * name: the plugin's id for dependencies and queries (1-63 characters from [A-Za-z0-9_.-]; NULL uses
 * the file name). version: dotted numbers, e.g. "1.2.0". requires_blse: the oldest BLSE version it
 * works with (NULL: any). dependencies: plugins that must be loaded first, separated by commas, each
 * optionally with ">=version" (NULL: none). BLSE loads dependencies before the plugins that need
 * them and refuses a plugin (logging why) when BLSE is too old, a dependency is missing, too old,
 * refused itself or part of a cycle, or another plugin has the same name. Plugins without this
 * export load as before, named by their file name, with no version. */
typedef struct BLSE_PluginInfo {
    uint32_t struct_size;          /* sizeof(BLSE_PluginInfo) */
    const char *name;
    const char *version;
    const char *author;
    const char *description;
    const char *requires_blse;
    const char *dependencies;
} BLSE_PluginInfo;
#ifdef __cplusplus
#define BLSE_PLUGIN_INFO_LINKAGE extern "C" BLSE_EXPORT
#else
#define BLSE_PLUGIN_INFO_LINKAGE BLSE_EXPORT
#endif
#define BLSE_PLUGIN_INFO(name, version, author, description, requires_blse, dependencies) \
    BLSE_PLUGIN_INFO_LINKAGE const BLSE_PluginInfo BLSE_Plugin = \
        {sizeof(BLSE_PluginInfo), name, version, author, description, requires_blse, dependencies}

/* True when `host` is large enough to contain `field`. */
#define BLSE_HOST_HAS(host, field) \
    ((host)->struct_size >= offsetof(BLSE_HostAPI, field) + sizeof((host)->field))

/* Topics are case-sensitive, 1..128 bytes. Use a "yourmod." prefix for your own topics.
 * The "blse." prefix is reserved for messages published by BLSE itself: */

/* Published once after every plugin has been loaded. No payload. */
#define BLSE_MSG_POST_LOAD "blse.post_load"
/* Published after the game restarts its engine in-process (the console command `_restart`
 * does this). Payload: uint32_t restart number (1, 2, ...). Python
 * state from before the restart has been discarded by the game. */
#define BLSE_MSG_ENGINE_RESTART "blse.engine_restart"

/* Game events (BLSE 0.2). Delivered on the main thread from the server game interface.
 * Published after a level has been initialized (map entities created). Payload: four
 * NUL-separated text fields: map name, previous map, landmark, and "1" if the level is being
 * restored from a save game, otherwise "0". A level transition (door, exit) has the previous map
 * and the landmark set and "0"; a save load or a console `map` command has both empty.
 * In Python: map, old, landmark, loaded = data.split('\0') */
#define BLSE_MSG_LEVEL_INIT "blse.level_init"
/* Published just before the current level shuts down (entities still exist), exactly once per
 * level_init. No payload. */
#define BLSE_MSG_LEVEL_SHUTDOWN "blse.level_shutdown"
/* (b0.5.48) A level change was asked for (a door or exit, a taxi, a script): it happens on a later
 * frame. Payload: the map, NUL, the landmark ("" if none). Delivered to native subscribers at once,
 * to Python at the next safe point (still before the change). In Python: map, landmark = data.split('\0') */
#define BLSE_MSG_LEVEL_CHANGE "blse.level_change"
/* Published after every server frame. Payload: 1 byte, '1' if the game simulated this frame
 * (not paused), else '0'. Subscribers run every frame: keep them cheap. */
#define BLSE_MSG_GAME_FRAME "blse.game_frame"
/* Before the game writes a save (BLSE b0.5.9): a save game, an autosave, or the state of the
 * current level kept for a level transition. Payload: the map name. Subscribers run before the
 * game saves its script globals, so values written now (save_set, or G from Python) are in the
 * save. Python subscribers are called immediately, not deferred. Only if BLSE could verify the
 * game's save method (the analyzed build). */
#define BLSE_MSG_BEFORE_SAVE "blse.before_save"
/* Entity outputs (BLSE b0.5.1). Published after a game entity fires an output, for example an
 * NPC's OnDeath, a trigger's OnTrigger, a door's OnOpen, a dialog's OnDialogEnd or the player
 * events' OnPlayerTookDamage. Payload: five NUL-separated text fields: output name, classname and
 * targetname of the entity that fired it, classname and targetname of the activator (empty when
 * there is none). The output name is empty if it could not be determined. Fired often: keep
 * subscribers cheap. Available only on recognized game builds (host->build_profile != NULL);
 * on other builds the topic is never published.
 * Native subscribers are called immediately, from inside the game's entity code: do the minimum
 * there (copy the data, set a flag) and act later, e.g. on blse.game_frame. Anything published
 * from such a callback reaches Python subscribers only at the next safe point. Python
 * subscribers receive this topic at the end of the server frame (or before level_init /
 * level_shutdown), in order.
 * In Python: output, cls, name, act_cls, act_name = data.split('\0') */
#define BLSE_MSG_ENTITY_OUTPUT "blse.entity_output"
/* Damage report (BLSE b0.5.3), published after the game applied a hit. Payload: nine NUL-separated
 * text fields: victim class, victim name, attacker class, attacker name, inflictor class,
 * inflictor name, damage applied (after BLSE rules and filters, before the game's own scaling),
 * original damage, damage type bits. Native subscribers are called inside game code; Python
 * subscribers at the next safe point. Only on recognized game builds. */
#define BLSE_MSG_DAMAGE "blse.damage"
/* Stat change (BLSE b0.5.5), published after a character stat's base value changed:
 * blood pool, health, humanity, ... Payload: eight NUL-separated text fields: owner entity index,
 * stat index, old value, new value, the game method that changed it (IncBase, DecBase, AddBase,
 * SubBase, SetBase), the cause (the game functions that led to the change, innermost first,
 * separated by " < "), the entity name the innermost of them reported, and the stat's name from
 * the game's stat definitions (vdata/system/stats.txt), in lower case as the game stores it:
 * "bloodpool", "health", "humanity", ...
 * Filling a new character's stats with their defaults is not reported.
 * Native subscribers are called inside game code; Python subscribers at the next safe point.
 * Only on recognized game builds. */
#define BLSE_MSG_STAT_CHANGE "blse.stat_change"
/* Disciplines (BLSE b0.5.14): a character activated a Discipline, or its effect ended.
 * Payload: five NUL-separated fields: owner classname, owner targetname, owner entity index (the
 * player is 1), Discipline (the stats.txt name in lower case, e.g. "potence", "celerity",
 * "blood_healing"), level. Derived from the ActiveDisciplines stats (active_<name> 0 -> level and
 * back). Native subscribers are called inside game code; Python subscribers at the next safe
 * point. Recognized builds only. */
#define BLSE_MSG_DISCIPLINE_ACTIVATED "blse.discipline_activated"
#define BLSE_MSG_DISCIPLINE_ENDED "blse.discipline_ended"
/* A Discipline activation was blocked by a rule or filter (BLSE b0.5.20). Same payload. */
#define BLSE_MSG_DISCIPLINE_BLOCKED "blse.discipline_blocked"
/* Dialogue (BLSE b0.5.10). blse.dialog_line: the NPC is about to say a line.
 * Payload: six NUL-separated fields: dialogue file, NPC classname, NPC targetname, line number,
 * the line's text as written for a male player, and for a female player.
 * blse.dialog_choice: the player picked a reply. Payload: eight fields: dialogue file, NPC
 * classname, NPC targetname, choice (1-based, as shown), the reply's line number, the NPC line
 * that follows (0 when the conversation ends), the reply text for a male and for a female player. Native subscribers are called inside the game's
 * dialogue code; Python subscribers at the next safe point. Only on recognized game builds. */
#define BLSE_MSG_DIALOG_LINE "blse.dialog_line"
#define BLSE_MSG_DIALOG_CHOICE "blse.dialog_choice"
/* Inventory (BLSE b0.5.12): an item entered or left a character's inventory, or was
 * equipped or unequipped. Payload: four NUL-separated fields: owner classname, owner targetname,
 * item classname, item targetname. Published for the player and NPCs. Native subscribers are
 * called inside game code; Python subscribers at the next safe point. Recognized builds only. */
#define BLSE_MSG_ITEM_ADDED "blse.item_added"
#define BLSE_MSG_ITEM_REMOVED "blse.item_removed"
#define BLSE_MSG_ITEM_EQUIPPED "blse.item_equipped"
#define BLSE_MSG_ITEM_UNEQUIPPED "blse.item_unequipped"
/* Entities appearing and disappearing while a level runs (BLSE b0.5.13). Payload:
 * three NUL-separated fields: classname, targetname, handle (decimal; the same number in both
 * events for one entity). entity_spawned is published at the end of the frame the entity was
 * created in (set up by then, with its name); entity_removed when it is removed, before it is
 * destroyed. Not published while a level loads or shuts down. Recognized builds only. */
#define BLSE_MSG_ENTITY_SPAWNED "blse.entity_spawned"
#define BLSE_MSG_ENTITY_REMOVED "blse.entity_removed"
/* b0.5.41: an entity with health (a character, a breakable object) was killed by a hit: victim
 * classname, name, handle; attacker classname, name, handle; inflictor (the weapon or projectile)
 * classname, name, handle (empty fields when there is none); damage; damage type bits. NUL-separated,
 * delivered right after the hit (native subscribers; Python at the next safe point). */
#define BLSE_MSG_ENTITY_KILLED "blse.entity_killed"
/* Menus and screens (BLSE b0.5.17): a game screen was opened or closed. Payload: two
 * NUL-separated fields: the screen's class name and BLSE's name for it: "game_menu" (Esc),
 * "inventory" (also used for trading), "character", "journal" (a tab of the character screen),
 * "map", "hotkeys", "character_creation", "dialog", "load_game", "save_game", "options",
 * "new_game", "console", "loading", "help". A close is published only after an open; tabs close
 * with their screen. Requires vgui2.dll with the analyzed panel interface (code checked). */
#define BLSE_MSG_MENU_OPENED "blse.menu_opened"
#define BLSE_MSG_MENU_CLOSED "blse.menu_closed"
/* A button a mod added (menu_add_button, BLSE b0.5.32) was clicked. Payload: the screen's name and
 * the button's path, NUL-separated. Clicks are queued and published at the next menu tick (the
 * next painted frame), not inside the click. */
#define BLSE_MSG_MENU_BUTTON "blse.menu_button"
/* A button lit up under the pointer (BLSE b0.5.49): any button of an open screen or a mod's
 * window, the game's own included. Payload: the screen's name and the button's path (as
 * menu_panels writes it), NUL-separated. Published at the next paint, and only while someone
 * subscribes (BLSE watches the open screens' buttons only then, or while button sounds exist). */
#define BLSE_MSG_MENU_HOVER "blse.menu_hover"
/* A mod's window was closed with its close button (BLSE b0.5.36). Payload: the window's name. */
#define BLSE_MSG_WINDOW_CLOSED "blse.window_closed"
/* Keys and mouse buttons (BLSE b0.5.24): a key was pressed or released. Payload: two
 * NUL-separated fields: the key name (see key_state) and where it happened: "game" (playing),
 * "cursor" (a mouse button in cursor mode), "text" (typing into a focused HUD input, b0.5.25),
 * "console" (typing in the console) or "menu" (a game screen or the game menu is open, or no level
 * runs; keys in the game menu since b0.5.25). Auto-repeat is not reported. Subscribers (native and Python) are called from the engine's input
 * handling, between frames, right away. Recognized builds only. */
#define BLSE_MSG_KEY_DOWN "blse.key_down"
#define BLSE_MSG_KEY_UP "blse.key_up"
/* Input (BLSE b0.5.34). mouse_move: the mouse moved while the game window is in front, at most once
 * per rendered frame. Payload, NUL-separated: dx, dy (pixels since the last event; while the game
 * looks around with the mouse it puts the cursor back every frame, which is not counted), x, y (the
 * cursor in the window, 0..1, meaningful in menus and cursor mode) and the context ("game", "menu",
 * "console", "cursor"). key_simulated: press_key from a mod. Payload: key name, "down" or "up", and
 * the mod ("python" or the plugin's name). Simulated presses are not reported as key_down/key_up. */
#define BLSE_MSG_MOUSE_MOVE "blse.mouse_move"
#define BLSE_MSG_KEY_SIMULATED "blse.key_simulated"
/* Controls (BLSE b0.5.26): a key press in play ran its binding, whichever key
 * it is: "+jump", "+attack", "+use", "slot1", ... Payload: the command and the key name,
 * NUL-separated. control_up follows when that key is released (for every command, not only "+"
 * ones). Not sent for keys BLSE blocked, typed into a HUD input, or pressed in the console or menus. */
#define BLSE_MSG_CONTROL_DOWN "blse.control_down"
#define BLSE_MSG_CONTROL_UP "blse.control_up"
/* A click in cursor mode (BLSE b0.5.24). Payload: five NUL-separated fields: the id of the topmost
 * clickable HUD element under the pointer ("" if none), the button ("MOUSE1".."MOUSE3", or "MWHEELUP"/"MWHEELDOWN" for the wheel), x and y as
 * fractions of the screen, and the element's owner: "python" or the plugin's name ("" if none). */
#define BLSE_MSG_HUD_CLICK "blse.hud_click"
/* Hover in cursor mode (BLSE b0.5.25): the clickable element (or input) under the pointer changed.
 * Payload: id and owner, NUL-separated; both "" when the pointer left every element. Checked every
 * game frame. */
#define BLSE_MSG_HUD_HOVER "blse.hud_hover"
/* Text entry fields (BLSE b0.5.25). Payload: three NUL-separated fields: the input's id, its text
 * (UTF-8) and its owner ("python" or the plugin's name). changed: after each typed character or
 * Backspace; submit: Enter was pressed (focus ends); cancel: Escape, or a click outside the field
 * (focus ends, the text stays). */
#define BLSE_MSG_HUD_TEXT_CHANGED "blse.hud_text_changed"
#define BLSE_MSG_HUD_TEXT_SUBMIT "blse.hud_text_submit"
#define BLSE_MSG_HUD_TEXT_CANCEL "blse.hud_text_cancel"
/* Save-game data (BLSE 0.3).
 * Values written with save_set are kept with the game's own script variables: they are written
 * into every save game, replaced when a save is loaded, carried across level transitions, and
 * cleared when a new game starts. Write a value whenever it changes, or at blse.before_save
 * (BLSE b0.5.9), published just before the game writes a save. Keys follow the topic rules;
 * prefix them with your mod name.
 *
 * Published once after a save game has been loaded and restored, on the first server frame of
 * the loaded level: read your values here (at blse.level_init they may still be the old ones).
 * Payload: the map name. */
#define BLSE_MSG_GAME_LOADED "blse.game_loaded"
/* A new game (BLSE b0.5.25): the first level of a game state that
 * was not loaded from a save, such as a new character's first map (or a console `map` command from
 * the main menu). Save-game values are empty again: reset your in-memory state. Published at
 * that level's start, before blse.level_init reaches other subscribers. Payload: the map name.
 * Detected with a marker BLSE keeps in the save data (G.blse_data). */
#define BLSE_MSG_NEW_GAME "blse.new_game"
/* Save and load lifecycle (BLSE b0.5.27).
 * game_saved: a save was written (also quicksaves and autosaves). Payload: the save name ("quick",
 * "autosave", or the name the game menu chose) and "1" or "0" (it failed), NUL-separated.
 * before_load: a save was requested to be loaded (from the game menu, quickload or the `load`
 * command), before anything is unloaded. Payload: the save name.
 * load_failed: that load did not happen. Payload: the save name and the reason ("the save does not
 * exist", "the save could not be restored").
 * after_load: the save was restored, on the first frame of the loaded level (with game_loaded).
 * Payload: the save name and the map.
 * save_deleted: the player deleted a save. Payload: its file name without ".sav".
 * Delivered on the main thread right away. Recognized builds only. */
#define BLSE_MSG_GAME_SAVED "blse.game_saved"
#define BLSE_MSG_BEFORE_LOAD "blse.before_load"
#define BLSE_MSG_LOAD_FAILED "blse.load_failed"
#define BLSE_MSG_AFTER_LOAD "blse.after_load"
#define BLSE_MSG_SAVE_DELETED "blse.save_deleted"
/* What the player is looking at (BLSE b0.5.27): the entity under the
 * crosshair changed (checked every game frame). Payload: classname, targetname and handle (decimal),
 * NUL-separated; all three empty when it is on nothing. Recognized builds only. */
#define BLSE_MSG_LOOK_TARGET_CHANGED "blse.look_target_changed"
/* The same for what the player is using (kind 1) and the Discipline target (kind 2) (BLSE b0.5.34);
 * same payload. The Discipline target is the game's own pick, redone every frame. */
#define BLSE_MSG_USING_CHANGED "blse.using_changed"
#define BLSE_MSG_DISCIPLINE_TARGET_CHANGED "blse.discipline_target_changed"
/* Gameplay (BLSE b0.5.28). Recognized builds only; delivered to native subscribers right away and
 * to Python at the end of the frame, like entity outputs.
 * entity_input: an entity received an input, from a game script (pc.AwardExperience("x") is the
 * input AwardExperience sent to the player), from the map's I/O or from the game. Payload, nine
 * NUL-separated fields: input, target classname, target name, value (text; "" if none; the
 * replacement if a filter changed it), activator classname, activator name, caller classname,
 * caller name, and "1" if a filter or rule blocked it (else "0").
 * quest_changed: the player's quest was set (SetQuest). Payload: quest name, state (decimal).
 * attack: a character attacked. Payload: kind ("melee" or "ranged"), attacker classname, attacker
 * name, attacker handle (decimal), weapon classname (ranged; "" for melee), shots (ranged: bullets
 * fired this time; melee: 1). Melee covers every character's swings; ranged the player's guns and,
 * since b0.5.29, NPCs' guns (one event per burst: shots is the number of bullets).
 * Experience and Masquerade changes arrive as blse.stat_change ("experience", "masquerade"). */
#define BLSE_MSG_ENTITY_INPUT "blse.entity_input"
#define BLSE_MSG_QUEST_CHANGED "blse.quest_changed"
#define BLSE_MSG_ATTACK "blse.attack"
/* Combat and stealth (BLSE b0.5.29). Recognized builds only.
 * combat_state: "combat" (a living NPC has the player as its enemy and hates it; since b0.5.30
 * bystanders fleeing from the player do not count), "alert" (nobody fights, but the
 * stealth meter shows the player noticed or detected) or "normal"; checked ten times a second,
 * calming down only after 2 seconds; "normal" when a level ends during a fight or alert.
 * stealth_changed: the player's stealth level changed. Payload: old level, new level and the meter
 * (0..100, as the HUD shows it), decimal, NUL-separated. Levels: 0 unseen (green), 1 noticed
 * (yellow), 2 detected (red, "!").
 * An entity_input that a filter or rule blocked has a ninth field "1" (else "0"). */
#define BLSE_MSG_COMBAT_STATE "blse.combat_state"
#define BLSE_MSG_STEALTH_CHANGED "blse.stealth_changed"
/* An NPC's fight (BLSE b0.5.33): an NPC fights while it is alive, not idle, and has an enemy it
 * hates (the player or another character). Payload, NUL-separated: "start", "target" (it turned to
 * another enemy) or "stop" (3 seconds after it stopped; at once when it dies or is removed, and for
 * every fight when the level ends), the NPC's handle (decimal), classname, name, then its target's
 * handle, classname and name (for "stop": the last target). Checked ten times a second. */
#define BLSE_MSG_NPC_COMBAT "blse.npc_combat"
/* The game loaded an item's data (BLSE b0.5.34; items are read when first needed: a level, a
 * merchant's goods). Payload: the classname. Checked every game frame; items loaded before BLSE
 * started are listed by list_items instead. */
#define BLSE_MSG_ITEM_LOADED "blse.item_loaded"
/* A console variable changed (BLSE b0.5.35): from the console, the options screen, a config file or
 * a mod. Payload, NUL-separated: name, old value, new value. Values are compared twice a second
 * while anyone listens, so a variable changed and changed back in between is not reported. */
#define BLSE_MSG_CVAR_CHANGED "blse.cvar_changed"
/* NPC AI (BLSE b0.5.45). Recognized builds only; delivered to native subscribers right away and to
 * Python at the end of the frame.
 * npc_schedule: an NPC started a schedule (the game's SetSchedule: every change, also the game's
 * own). Payload, NUL-separated: the NPC's handle (decimal), classname, name, the old schedule's
 * name ("" if none) and the new one's ("SCHED_TROIKA_FLEE", ...).
 * npc_state: an NPC's state changed. Same first three fields, then the old and the new state:
 * "none", "idle", "combat", "alert", "script", "playdead", "prone", "dead", "fleeing", "retreating",
 * "cowering", "hunting", "dialog", "oblivious", "criminal_suspicion". Checked ten times a second
 * while anyone listens. */
/* Controls and screens (BLSE b0.5.46).
 * menu_changed: the player changed a checkbox, slider or drop-down a mod added. Payload,
 * NUL-separated: screen, path, the new value ("1"/"0", the slider's number, the drop-down's text).
 * Checked at every paint.
 * menu_blocked: the game tried to open a screen a mod blocks (menu_block); it stayed closed.
 * Payload: the screen's class name, then BLSE's name for it (as menu_opened). */
#define BLSE_MSG_MENU_CHANGED "blse.menu_changed"
#define BLSE_MSG_MENU_BLOCKED "blse.menu_blocked"
#define BLSE_MSG_NPC_SCHEDULE "blse.npc_schedule"
#define BLSE_MSG_NPC_STATE "blse.npc_state"
/* Character creation (BLSE b0.5.47): the new-game character sheet and its questionnaire.
 * chargen_started: the sheet opened to make a character. Payload: "1" (New Game).
 * chargen_step: the questionnaire shows a screen. Payload: its name from
 *   vdata/system/charcreatewizard.txt in lower case ("help_popup0", "trait_male_vs_female", ...).
 * chargen_answer: a button on that screen was clicked, before the game goes on. Payload: the
 *   screen's name, the button (1-4 as shown).
 * chargen_gender: "male" or "female". chargen_clan: the clan (its number in the game's clan table,
 *   then its name, e.g. "Player_Tremere"). chargen_history: the history's number (-1 none).
 * chargen_points: a point spent or taken back. Payload: "buy" or "sell", the stat, the count.
 * chargen_autolevel: the questionnaire spent the points by a template. Payload: its arguments.
 * chargen_finished: Accept; the game starts (the level change to sp_theatre follows). Payload:
 *   clan number, clan name, gender, the character's name, history, how it was made
 *   ("questionnaire", "manual" or "hunter"), "1". Experience is set to 0 just before: give any now.
 * chargen_cancelled: Payload: "cancel" (back to the main menu), or "level_shutdown" (the level
 *   ended otherwise). Choices made on the sheet are already the player's (the game applies each at
 *   once). All NUL-separated; delivered right away, Python too (the game is paused meanwhile).
 *   Recognized builds only. */
#define BLSE_MSG_CHARGEN_STARTED "blse.chargen_started"
#define BLSE_MSG_CHARGEN_STEP "blse.chargen_step"
#define BLSE_MSG_CHARGEN_ANSWER "blse.chargen_answer"
#define BLSE_MSG_CHARGEN_GENDER "blse.chargen_gender"
#define BLSE_MSG_CHARGEN_CLAN "blse.chargen_clan"
#define BLSE_MSG_CHARGEN_HISTORY "blse.chargen_history"
#define BLSE_MSG_CHARGEN_POINTS "blse.chargen_points"
#define BLSE_MSG_CHARGEN_AUTOLEVEL "blse.chargen_autolevel"
#define BLSE_MSG_CHARGEN_FINISHED "blse.chargen_finished"
#define BLSE_MSG_CHARGEN_CANCELLED "blse.chargen_cancelled"
/* Companions (BLSE b0.5.48). companion: payload, NUL-separated: what, the NPC's handle
 * (decimal; empty when it is not on this level), classname, name, a map. what: "arrived" (it came
 * along from the map given), "stayed" (it stayed on the map given: "stay", a blocked map, out of
 * range or dead), "lost" (it died or was removed; no longer a companion). Delivered right away to
 * native subscribers, to Python at the next safe point.
 * hit: every hit the game applies (as blse.damage), with handles. Payload: victim handle, victim
 * class, victim name, attacker handle, attacker class, attacker name, damage, damage type bits
 * (handles decimal, empty for none). Only built while anyone listens. */
#define BLSE_MSG_COMPANION "blse.companion"
#define BLSE_MSG_HIT "blse.hit"
/* Animation (BLSE b0.5.49). Delivered right away to native subscribers, to Python at the next safe
 * point; the hooks behind them are installed when the first subscriber appears. All NUL-separated;
 * every entity with a model (characters, props).
 * anim_started: an entity's sequence was (re)started. Payload: handle (decimal), classname, name,
 * sequence number, sequence name, the sequence's activity name ("" none), the previous sequence's
 * name ("" none).
 * anim_ended: a sequence ended. Same first six fields, then "finished" (it reached its end) or
 * "interrupted" (another sequence replaced it first). A looping sequence never finishes: it ends
 * when another one replaces it (b0.5.49).
 * anim_chosen: an NPC's AI picked a sequence for an activity. Payload: handle, classname, name, the
 * activity asked for, the activity after the game's translation (by weapon), the sequence's name
 * and "1" if a rule or filter changed the game's choice (else "0"). Frequent: listen only while
 * needed. Since C18b also for the other picks (gestures, melee attacks, knockback flights, the
 * player), with the same fields; for those the translated activity is the activity itself (the
 * player's: the activity asked for, then the one the game translated it to). C36, first person
 * (only while a first-person rule, a BLSE_ANIM_PICK_VIEWMODEL filter or a Python request exists):
 * the weapon (handle and classname of the weapon, activity = translated = the one sent) and the
 * hands view model (classname "viewmodel", activity = the weapon's, translated = the hands').
 * anim_event (C35): a model's animation event reached the server. Payload: handle, classname,
 * name, sequence number, sequence name, the event's number, its options text, the cycle ("0.500"),
 * and what BLSE did: "kept", "changed" or "blocked" (the number and options are the model's). */
#define BLSE_MSG_ANIM_STARTED "blse.anim_started"
#define BLSE_MSG_ANIM_ENDED "blse.anim_ended"
#define BLSE_MSG_ANIM_CHOSEN "blse.anim_chosen"
#define BLSE_MSG_ANIM_EVENT "blse.anim_event"
/* Player outfits (C34). outfit_changed: the player model slot changed. Payload, NUL-separated: the
 * outfit now ("" none, "*" a raw model from set_player_model), the one before, the model BLSE set
 * ("" the game's own), the reason: "wear", "remove", "load" (worn again from a save), "missing" (a
 * save's or the worn outfit's model or registration is gone: the game's model is back), "level" (a
 * raw model without keep ended with the level), "unregistered" (its plugin went away). */
#define BLSE_MSG_OUTFIT_CHANGED "blse.outfit_changed"
/* Skill attempts (0.7). skill_attempt: the player started an attempt on a lock a mod took. Payload,
 * NUL-separated: the entity's handle (decimal), classname, targetname, model, skill type, difficulty,
 * who holds it ("python" or a plugin's name). skill_attempt_end: that attempt ended. Payload: the
 * handle, the reason: "success", "fail", "botch", "cancel" (skill_result, or its owner went away),
 * "use" (the player pressed Use again) or "game" (the game ended it: damage, a cutscene, a level change, the lock
 * could no longer be tried, e.g. the lockpicks were taken away). */
#define BLSE_MSG_SKILL_ATTEMPT "blse.skill_attempt"
#define BLSE_MSG_SKILL_ATTEMPT_END "blse.skill_attempt_end"

#ifdef __cplusplus
}
#endif
#endif
