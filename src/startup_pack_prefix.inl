// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Sound-scheme pack prefix index for FileSystem_Stdio's pack scan (FS+0x7c40).
// Included inside restart_startup::detail (needs thread, host, read_client, le32, now).
//
// Native: FindFirst (FS+0x3c10) / FindNext (FS+0x4090) call
//   int __stdcall scan(path_object, pattern, start_node)        (retn 0Ch, ECX/EDX unused)
// which walks the pack's CUtlRBTree in order from start_node and returns the
// first node whose name passes the pattern filter, else -1. The tree is ordered
// by SYMBOL ID (less function FS+0x12b0: `a.symbol < b.symbol`), not by name, so
// no lexical lower bound exists; every listing walks the whole pack (71k nodes
// across the shipped packs) once per pack search path.
//
// This file replaces nothing. Inside the sound-scheme single-pass FS block only
// (Active, main thread, inside a Lifetime), for the exact pattern shape
// "<P>*.*" (P non-empty, printable ASCII, no '.' or '*', folds to "sound/..."),
// a plugin-owned per-pack index built from the SAME in-order chain (same node
// accessor semantics, names read through the native FS+0xcb60) maps the native
// successor order to ordinals. The wrapper finds the first matching ordinal at or
// after the supplied start and calls the unchanged native scan with that node
// (native re-checks it and returns it immediately) or with -1 when no match
// remains (native returns -1 after its normal parse/lazy init). A disagreement
// disables the fast path for the trace and re-runs the native scan from the
// original start. Everything else goes straight to the native scan.
//
// Extension patterns "<P>*.<E>" (worldspawn's "sound/Surfaces/*.wav" etc.) use the same index plus
// the native extension test (first '.' of the name, C-locale _stricmp at FS+0x16c90, verified
// separately). They are answered only inside an Extensions window, opened solely by the loading
// sound-precache scope (src/loading_sound_precache.inl); elsewhere they stay native as before.
#ifndef ENGINE_FIXES_PACK_PREFIX_INL
#define ENGINE_FIXES_PACK_PREFIX_INL
bool call_hook(std::uintptr_t base,unsigned site,unsigned target,void* detour,void** original,const char* label);
namespace pack_prefix {
using Scan=int(__stdcall*)(void*,const char*,int);
using Text=const char*(__thiscall*)(const void*);
struct Fixup { unsigned short offset; unsigned rva; };
struct Body { unsigned rva; const unsigned char* bytes; unsigned size; const Fixup* fixups; unsigned fixup_count; };

// IDA-verified bodies (FileSystem_Stdio.dll image base 0x10000000, SHA-256 below);
// The IDA export these bytes were generated from is the source of truth.
// FS+0x7c40 pack scan (stdcall path, pattern, start).
const unsigned char body_scan[]={
0x55,0x8b,0xec,0x83,0xec,0x14,0x53,0x8b,0x5d,0x0c,0x56,0x83,0xce,0xff,0x57,0x8b,0xfb,0x8b,0xce,0x33,
0xc0,0xf2,0xae,0xf7,0xd1,0x49,0x89,0x75,0xec,0x8b,0xc1,0x83,0xc0,0x04,0x24,0xfc,0xe8,0x17,0x6c,0x00,
0x00,0x8b,0xfc,0x8b,0xc3,0x8b,0xd7,0x89,0x7d,0xf8,0x2b,0xd3,0x8a,0x08,0x88,0x0c,0x02,0x40,0x84,0xc9,
0x75,0xf6,0x6a,0x2e,0x57,0xe8,0x7a,0x6c,0x00,0x00,0x8b,0xd8,0x83,0xc4,0x08,0x85,0xdb,0x75,0x0e,0x8b,
0xc6,0x8d,0x65,0xe0,0x5f,0x5e,0x5b,0x8b,0xe5,0x5d,0xc2,0x0c,0x00,0xc6,0x03,0x00,0x43,0x6a,0x2a,0x57,
0x89,0x5d,0x0c,0xe8,0x54,0x6c,0x00,0x00,0x83,0xc4,0x08,0x85,0xc0,0x74,0x03,0xc6,0x00,0x00,0x8b,0xce,
0x33,0xc0,0xf2,0xae,0xf7,0xd1,0x49,0x89,0x4d,0xfc,0x75,0x14,0x80,0x3b,0x2a,0x75,0x0f,0x8b,0x45,0x10,
0x8d,0x65,0xe0,0x5f,0x5e,0x5b,0x8b,0xe5,0x5d,0xc2,0x0c,0x00,0x8b,0x5d,0x10,0xa0,0x9f,0x3a,0x02,0x10,
0xa8,0x01,0x75,0x23,0x0c,0x01,0x89,0x35,0x6c,0x36,0x02,0x10,0xa2,0x9f,0x3a,0x02,0x10,0x89,0x35,0x70,
0x36,0x02,0x10,0x89,0x35,0x74,0x36,0x02,0x10,0xc7,0x05,0x78,0x36,0x02,0x10,0x01,0x00,0x00,0x00,0x3b,
0xde,0x8b,0x75,0x08,0x74,0x11,0x8b,0x4e,0x30,0x8d,0x04,0xdd,0x00,0x00,0x00,0x00,0x2b,0xc3,0x8d,0x04,
0x81,0xeb,0x05,0xb8,0x6c,0x36,0x02,0x10,0x39,0x18,0x0f,0x84,0x4e,0x01,0x00,0x00,0x8b,0x46,0x30,0x8d,
0x14,0xdd,0x00,0x00,0x00,0x00,0x2b,0xd3,0x8b,0x4c,0x90,0x10,0x8d,0x44,0x90,0x10,0x89,0x4d,0xec,0x8d,
0x4d,0xec,0x8b,0x50,0x04,0x89,0x55,0xf0,0x8b,0x40,0x08,0x89,0x45,0xf4,0xe8,0x09,0x4e,0x00,0x00,0x8b,
0xf8,0x8b,0x45,0xfc,0x85,0xc0,0x74,0x12,0x8b,0x4d,0xf8,0x50,0x51,0x57,0xe8,0x35,0x3f,0x01,0x00,0x83,
0xc4,0x0c,0x85,0xc0,0x75,0x31,0x8b,0x55,0x0c,0x80,0x3a,0x2a,0x0f,0x84,0xfc,0x00,0x00,0x00,0x6a,0x2e,
0x57,0xe8,0x7a,0x6b,0x00,0x00,0x83,0xc4,0x08,0x85,0xc0,0x74,0x16,0x8b,0x4d,0x0c,0x40,0x51,0x50,0xe8,
0xf8,0xee,0x00,0x00,0x83,0xc4,0x08,0x85,0xc0,0x0f,0x84,0xd7,0x00,0x00,0x00,0x83,0xc6,0x2c,0x53,0x8b,
0xce,0xe8,0x62,0x15,0x00,0x00,0x8b,0x48,0x04,0x53,0x83,0xf9,0xff,0x8b,0xce,0x74,0x36,0xe8,0x52,0x15,
0x00,0x00,0x8b,0x78,0x04,0x8b,0xce,0x57,0xe8,0x47,0x15,0x00,0x00,0x83,0x38,0xff,0x74,0x17,0x57,0x8b,
0xce,0xe8,0x3a,0x15,0x00,0x00,0x8b,0x38,0x8b,0xce,0x57,0xe8,0x30,0x15,0x00,0x00,0x83,0x38,0xff,0x75,
0xe9,0x8b,0xdf,0x83,0xce,0xff,0xe9,0xec,0xfe,0xff,0xff,0xe8,0x1c,0x15,0x00,0x00,0x8b,0x78,0x08,0x53,
0x8b,0xce,0xe8,0x11,0x15,0x00,0x00,0x8b,0x40,0x08,0x8b,0xce,0x50,0xe8,0x06,0x15,0x00,0x00,0x39,0x58,
0x04,0x75,0x61,0x83,0xc8,0xff,0x8b,0xdf,0x3b,0xf8,0x74,0x58,0x8a,0x0d,0x9f,0x3a,0x02,0x10,0xf6,0xc1,
0x01,0x75,0x22,0x80,0xc9,0x01,0xa3,0x6c,0x36,0x02,0x10,0x88,0x0d,0x9f,0x3a,0x02,0x10,0xa3,0x70,0x36,
0x02,0x10,0xa3,0x74,0x36,0x02,0x10,0xc7,0x05,0x78,0x36,0x02,0x10,0x01,0x00,0x00,0x00,0x8b,0x46,0x04,
0x8d,0x14,0xfd,0x00,0x00,0x00,0x00,0x2b,0xd7,0x53,0x8b,0x7c,0x90,0x08,0x8b,0xce,0x8d,0x04,0x90,0xe8,
0xb0,0x14,0x00,0x00,0x8b,0x40,0x08,0x8b,0xce,0x50,0xe8,0xa5,0x14,0x00,0x00,0x39,0x58,0x04,0x74,0x9f,
0x8b,0xdf,0x83,0xce,0xff,0xe9,0x61,0xfe,0xff,0xff,0x8b,0xc3,0x8d,0x65,0xe0,0x5f,0x5e,0x5b,0x8b,0xe5,
0x5d,0xc2,0x0c,0x00};
const Fixup fix_scan[]={{156,0x23a9f},{168,0x2366c},{173,0x23a9f},{179,0x23670},{185,0x23674},{191,0x23678},{224,0x2366c},
    {474,0x23a9f},{487,0x2366c},{493,0x23a9f},{498,0x23670},{503,0x23674},{509,0x23678}};
// FS+0x9310 tree node accessor (thiscall tree, index): -1 -> static sentinel {-1,-1,-1,1}.
const unsigned char body_node[]={
0x53,0x8a,0x1d,0x9f,0x3a,0x02,0x10,0xb8,0x01,0x00,0x00,0x00,0x83,0xca,0xff,0x84,0xd8,0x75,0x1f,0x0a,
0xd8,0x89,0x15,0x6c,0x36,0x02,0x10,0x88,0x1d,0x9f,0x3a,0x02,0x10,0x89,0x15,0x70,0x36,0x02,0x10,0x89,
0x15,0x74,0x36,0x02,0x10,0xa3,0x78,0x36,0x02,0x10,0x8b,0x44,0x24,0x08,0x5b,0x3b,0xc2,0x74,0x12,0x8d,
0x14,0xc5,0x00,0x00,0x00,0x00,0x2b,0xd0,0x8b,0x41,0x04,0x8d,0x04,0x90,0xc2,0x04,0x00,0xb8,0x6c,0x36,
0x02,0x10,0xc2,0x04,0x00};
const Fixup fix_node[]={{3,0x23a9f},{23,0x2366c},{29,0x23a9f},{35,0x23670},{41,0x23674},{46,0x23678},{78,0x2366c}};
// FS+0xcb60 record -> C string (thiscall record; symbol table +0xcb20 -> +0xd060).
const unsigned char body_text[]={0x51,0x8b,0x09,0x8b,0xc4,0x89,0x08,0xe8,0xb4,0xff,0xff,0xff,0x8b,0xc8,0xe8,0xed,0x04,0x00,0x00,0xc3};
const unsigned char body_table[]={0xe8,0x4b,0xff,0xff,0xff,0xa1,0xb8,0x36,0x02,0x10,0xc3};
const Fixup fix_table[]={{6,0x236b8}};
// FS+0xca70 lazy symbol-table creation (never taken: the ready byte is required).
const unsigned char body_lazy[]={
0xa0,0xbc,0x36,0x02,0x10,0x84,0xc0,0x75,0x39,0x6a,0x34,0xe8,0x4a,0x10,0x00,0x00,0x83,0xc4,0x04,0x85,
0xc0,0x74,0x1a,0x6a,0x00,0x6a,0x20,0x6a,0x00,0x8b,0xc8,0xe8,0x3c,0x02,0x00,0x00,0xa3,0xb8,0x36,0x02,
0x10,0xc6,0x05,0xbc,0x36,0x02,0x10,0x01,0xc3,0xc7,0x05,0xb8,0x36,0x02,0x10,0x00,0x00,0x00,0x00,0xc6,
0x05,0xbc,0x36,0x02,0x10,0x01,0xc3};
const Fixup fix_lazy[]={{1,0x236bc},{37,0x236b8},{43,0x236bc},{51,0x236b8},{61,0x236bc}};
const unsigned char body_string[]={
0x8b,0x44,0x24,0x04,0x83,0xf8,0xff,0x75,0x08,0xb8,0x68,0x36,0x02,0x10,0xc2,0x04,0x00,0x8b,0x51,0x04,
0x8d,0x04,0x80,0x8b,0x44,0x82,0x10,0x8b,0x51,0x20,0x03,0xc2,0xc2,0x04,0x00};
const Fixup fix_string[]={{10,0x23668}};
// FS+0x1bca0 static-CRT _strnicmp: ASCII A-Z fold when the LC_CTYPE handle (FS+0x239d8) is 0.
const unsigned char body_strnicmp[]={
0x55,0x8b,0xec,0x57,0x56,0x53,0x8b,0x4d,0x10,0x0b,0xc9,0x0f,0x84,0xe9,0x00,0x00,0x00,0x8b,0x75,0x08,
0x8b,0x7d,0x0c,0x8d,0x05,0xd0,0x39,0x02,0x10,0x83,0x78,0x08,0x00,0x75,0x4e,0xb7,0x41,0xb3,0x5a,0xb6,
0x20,0x8d,0x49,0x00,0x8a,0x26,0x0a,0xe4,0x8a,0x07,0x74,0x21,0x0a,0xc0,0x74,0x1d,0x46,0x47,0x38,0xfc,
0x72,0x06,0x38,0xdc,0x77,0x02,0x02,0xe6,0x38,0xf8,0x72,0x06,0x38,0xd8,0x77,0x02,0x02,0xc6,0x38,0xc4,
0x75,0x0d,0x49,0x75,0xd7,0x33,0xc9,0x38,0xc4,0x0f,0x84,0x9b,0x00,0x00,0x00,0xb9,0xff,0xff,0xff,0xff,
0x0f,0x82,0x90,0x00,0x00,0x00,0xf7,0xd9,0xe9,0x89,0x00,0x00,0x00,0xf0,0xff,0x05,0xac,0x3a,0x02,0x10,
0x83,0x3d,0xa8,0x3a,0x02,0x10,0x00,0x7f,0x04,0x6a,0x00,0xeb,0x19,0xf0,0xff,0x0d,0xac,0x3a,0x02,0x10,
0x8b,0xd9,0x6a,0x13,0xe8,0xc1,0x48,0xff,0xff,0xc7,0x04,0x24,0x01,0x00,0x00,0x00,0x8b,0xcb,0x33,0xc0,
0x33,0xdb,0x8b,0xff,0x8a,0x06,0x0b,0xc0,0x8a,0x1f,0x74,0x23,0x0b,0xdb,0x74,0x1f,0x46,0x47,0x51,0x50,
0x53,0xe8,0xce,0x98,0xff,0xff,0x8b,0xd8,0x83,0xc4,0x04,0xe8,0xc4,0x98,0xff,0xff,0x83,0xc4,0x04,0x59,
0x3b,0xc3,0x75,0x09,0x49,0x75,0xd5,0x33,0xc9,0x3b,0xc3,0x74,0x09,0xb9,0xff,0xff,0xff,0xff,0x72,0x02,
0xf7,0xd9,0x58,0x0b,0xc0,0x75,0x09,0xf0,0xff,0x0d,0xac,0x3a,0x02,0x10,0xeb,0x0e,0x8b,0xd9,0x6a,0x13,
0xe8,0xc2,0x48,0xff,0xff,0x83,0xc4,0x04,0x8b,0xcb,0x8b,0xc1,0x5b,0x5e,0x5f,0xc9,0xc3};
const Fixup fix_strnicmp[]={{25,0x239d0},{116,0x23aac},{122,0x23aa8},{136,0x23aac},{230,0x23aac}};
// FS+0xe8f0..0xe9bc static-CRT strchr, including the shared exit stub at +0xe8f0
// that its first loop jumps back to.
const unsigned char body_strchr[]={
0x8d,0x42,0xff,0x5b,0xc3,0x8d,0xa4,0x24,0x00,0x00,0x00,0x00,0x8d,0x64,0x24,0x00,
0x33,0xc0,0x8a,0x44,0x24,0x08,0x53,0x8b,0xd8,0xc1,0xe0,0x08,0x8b,0x54,0x24,0x08,0xf7,0xc2,0x03,0x00,
0x00,0x00,0x74,0x13,0x8a,0x0a,0x42,0x38,0xd9,0x74,0xd1,0x84,0xc9,0x74,0x51,0xf7,0xc2,0x03,0x00,0x00,
0x00,0x75,0xed,0x0b,0xd8,0x57,0x8b,0xc3,0xc1,0xe3,0x10,0x56,0x0b,0xd8,0x8b,0x0a,0xbf,0xff,0xfe,0xfe,
0x7e,0x8b,0xc1,0x8b,0xf7,0x33,0xcb,0x03,0xf0,0x03,0xf9,0x83,0xf1,0xff,0x83,0xf0,0xff,0x33,0xcf,0x33,
0xc6,0x83,0xc2,0x04,0x81,0xe1,0x00,0x01,0x01,0x81,0x75,0x1c,0x25,0x00,0x01,0x01,0x81,0x74,0xd3,0x25,
0x00,0x01,0x01,0x01,0x75,0x08,0x81,0xe6,0x00,0x00,0x00,0x80,0x75,0xc4,0x5e,0x5f,0x5b,0x33,0xc0,0xc3,
0x8b,0x42,0xfc,0x38,0xd8,0x74,0x36,0x84,0xc0,0x74,0xef,0x38,0xdc,0x74,0x27,0x84,0xe4,0x74,0xe7,0xc1,
0xe8,0x10,0x38,0xd8,0x74,0x15,0x84,0xc0,0x74,0xdc,0x38,0xdc,0x74,0x06,0x84,0xe4,0x74,0xd4,0xeb,0x96,
0x5e,0x5f,0x8d,0x42,0xff,0x5b,0xc3,0x8d,0x42,0xfe,0x5e,0x5f,0x5b,0xc3,0x8d,0x42,0xfd,0x5e,0x5f,0x5b,
0xc3,0x8d,0x42,0xfc,0x5e,0x5f,0x5b,0xc3};
const Body bodies[]={{0x7c40,body_scan,sizeof body_scan,fix_scan,13},{0x9310,body_node,sizeof body_node,fix_node,7},
    {0xcb60,body_text,sizeof body_text,nullptr,0},{0xcb20,body_table,sizeof body_table,fix_table,1},{0xca70,body_lazy,sizeof body_lazy,fix_lazy,5},
    {0xd060,body_string,sizeof body_string,fix_string,1},{0x1bca0,body_strnicmp,sizeof body_strnicmp,fix_strnicmp,5},{0xe8f0,body_strchr,sizeof body_strchr,nullptr,0}};
// FS+0x16c90 static-CRT _stricmp, reached only for an extension other than "*" (scan +0x7d93:
// _stricmp(strchr(name,'.')+1, extension)). C locale (FS+0x239d8 == 0): ASCII A-Z fold, equal
// only through the terminating NUL. Verified separately: it gates only "<P>*.<E>" fast answers, so
// the "*.*" path and its install gate are unchanged. Provenance:
// (IDA export of the native _strnicmp).
const unsigned char body_stricmp[]={
0x55,0x8b,0xec,0x57,0x56,0x53,0x8b,0x75,0x0c,0x8b,0x7d,0x08,0x8d,0x05,0xd0,0x39,0x02,0x10,0x83,0x78,
0x08,0x00,0x75,0x3b,0xb0,0xff,0x8b,0xff,0x0a,0xc0,0x74,0x2e,0x8a,0x06,0x46,0x8a,0x27,0x47,0x38,0xc4,
0x74,0xf2,0x2c,0x41,0x3c,0x1a,0x1a,0xc9,0x80,0xe1,0x20,0x02,0xc1,0x04,0x41,0x86,0xe0,0x2c,0x41,0x3c,
0x1a,0x1a,0xc9,0x80,0xe1,0x20,0x02,0xc1,0x04,0x41,0x38,0xe0,0x74,0xd2,0x1a,0xc0,0x1c,0xff,0x0f,0xbe,
0xc0,0xeb,0x78,0xf0,0xff,0x05,0xac,0x3a,0x02,0x10,0x83,0x3d,0xa8,0x3a,0x02,0x10,0x00,0x7f,0x04,0x6a,
0x00,0xeb,0x15,0xf0,0xff,0x0d,0xac,0x3a,0x02,0x10,0x6a,0x13,0xe8,0xf1,0x98,0xff,0xff,0xc7,0x04,0x24,
0x01,0x00,0x00,0x00,0xb8,0xff,0x00,0x00,0x00,0x33,0xdb,0x90,0x0a,0xc0,0x74,0x27,0x8a,0x06,0x46,0x8a,
0x1f,0x47,0x38,0xd8,0x74,0xf2,0x50,0x53,0xe8,0xff,0xe8,0xff,0xff,0x8b,0xd8,0x83,0xc4,0x04,0xe8,0xf5,
0xe8,0xff,0xff,0x83,0xc4,0x04,0x38,0xc3,0x74,0xda,0x1b,0xc0,0x83,0xd8,0xff,0x8b,0xd8,0x58,0x0b,0xc0,
0x75,0x09,0xf0,0xff,0x0d,0xac,0x3a,0x02,0x10,0xeb,0x0a,0x6a,0x13,0xe8,0x01,0x99,0xff,0xff,0x83,0xc4,
0x04,0x8b,0xc3,0x5b,0x5e,0x5f,0xc9,0xc3};
const Fixup fix_stricmp[]={{14,0x239d0},{86,0x23aac},{92,0x23aa8},{106,0x23aac},{185,0x23aac}};
const Body extension_body={0x16c90,body_stricmp,sizeof body_stricmp,fix_stricmp,5};
const char module_sha256[]="ff69aa1ada4bcef2c5fb2277502718fb45ea0d7429f9240b9472eb62753a6194";
// Unofficial Patch forward compatibility (as for client.dll): the analyzed file by hash, or the
// same build edited in place (same link time and section table, at most a tenth of its 4 KB
// blocks changed). Every body above is still byte-checked in memory before use.
struct Section { char name[8]; std::uint32_t size,address,raw_size,raw_offset; };
const Section module_sections[]={{".text",0x1b37e,0x1000,0x1c000,0x1000},{".rdata",0x1b27,0x1d000,0x2000,0x1d000},
    {".data",0x5e5c,0x1f000,0x5000,0x1f000},{".reloc",0x1866,0x25000,0x2000,0x24000}};
const std::uint32_t module_timestamp=0x4164cc44;
const std::uint32_t module_blocks[39]={ // first 4 bytes (LE) of each 4 KB block's SHA-256
    0x729f3b1du,0x8f99a2ebu,0x3b1f9fefu,0xb9645efau,0xedad0c8cu,0x3ab74262u,0x2a4cf4a4u,0x4310e7a0u,0x0adb83a8u,0x9d76acf0u,
    0x9d5cb584u,0x7635d5fcu,0x78e6982au,0xacb21e59u,0x2c8cbf0fu,0xcd930c46u,0xa70cfbefu,0x79c43894u,0xbe5f1791u,0xe0b221eeu,
    0xa6c9f139u,0xb4ccbc08u,0x9fc825e8u,0x0e3062a6u,0xa7f664c9u,0xaf91b54du,0xff975778u,0xe87d332eu,0xfa429c8bu,0xea0463a8u,
    0x7ff0760eu,0x0fb175b9u,0x8f23d1e8u,0xb2ac7fadu,0x0f72f1b3u,0xd26894b1u,0xa57b113fu,0x48d83a04u,0xf3535394u};
bool same_layout(const unsigned char* head,std::size_t size) {
    if (size<0x40 || head[0]!='M' || head[1]!='Z') return false;
    const std::size_t pe=le32(head+0x3c);
    if (pe>size || size-pe<24 || std::memcmp(head+pe,"PE\0\0",4) || head[pe+4]!=0x4c || head[pe+5]!=0x01) return false;
    const std::size_t sections=static_cast<std::size_t>(head[pe+6]|head[pe+7]<<8),optional=static_cast<std::size_t>(head[pe+20]|head[pe+21]<<8);
    const std::size_t table=pe+24+optional;
    if (le32(head+pe+8)!=module_timestamp || sections!=_countof(module_sections) || table>size || (size-table)/40<sections) return false;
    for (std::size_t i=0;i<sections;++i) {
        const unsigned char* h=head+table+40*i; const Section& s=module_sections[i];
        if (std::memcmp(h,s.name,8) || le32(h+8)!=s.size || le32(h+12)!=s.address || le32(h+16)!=s.raw_size || le32(h+20)!=s.raw_offset) return false;
    }
    return true;
}
// 1 = analyzed file, 2 = analyzed build edited in place (`changed` blocks), 0 = other/unreadable.
int build_of(const wchar_t* path,unsigned& changed) {
    unsigned char digest[32]={}; std::vector<unsigned char> head; std::vector<std::uint32_t> blocks; changed=0;
    if (!read_client(path,digest,head,blocks)) return 0;
    char hex[65]; for (unsigned i=0;i<32;++i) sprintf_s(hex+2*i,3,"%02x",digest[i]);
    if (!std::strcmp(hex,module_sha256)) return 1;
    if (!same_layout(head.data(),head.size())) return 0;
    const std::size_t total=_countof(module_blocks);
    for (std::size_t i=0;i<(blocks.size()>total?blocks.size():total);++i) if (i>=total || i>=blocks.size() || blocks[i]!=module_blocks[i]) ++changed;
    return changed<=total/10?2:0;
}
const unsigned scan_rva=0x7c40,text_rva=0xcb60,locale_rva=0x239d8,symbols_ready_rva=0x236bc;
const unsigned sites[3]={0x3d2b,0x41b1,0x42b8}; // FindFirst, FindNext (current pack), FindNext (next pack)
const unsigned tree_offset=0x2c;                 // path object: CUtlRBTree (less,nodes,alloc,grow,root,count,free,last)
const unsigned arena_capacity=8u<<20,max_packs=64,max_name=1024,max_pattern=259,max_alloc=1u<<20,max_extension=32;

struct Node { int left,right,parent,tag; unsigned record[3]; };
struct Tree { const void* less; Node* nodes; int alloc,grow,root,count,first_free,last_alloc; };
static_assert(sizeof(Node)==28 && sizeof(Tree)==32,"FileSystem_Stdio x86 layouts");
struct Key { unsigned ordinal,offset,length; };
struct Pack {
    const void* path; Tree tree; bool usable;
    int* ordinal_of; int* node_at; unsigned* symbol_at;   // [alloc], [count], [count]
    Key* keys; unsigned key_count; const char* text;      // folded "sound/" names, sorted
    unsigned* memo; unsigned memo_count,memo_length; bool memo_valid; char memo_prefix[max_pattern+1];
    unsigned memo_extension_length; char memo_extension[max_extension+1]; // 0 = "*" (any)
};
const Node sentinel={-1,-1,-1,1,{0,0,0}};

struct Counter { std::uint64_t calls=0; LONGLONG ticks=0; };
bool requested=false,ready=false,disabled=false,extension_ok=false; unsigned hooks=0; std::uintptr_t base=0;
Scan original=nullptr; Text text_of=nullptr;
unsigned lifetime=0; volatile LONG active=0; unsigned extensions=0;
char* arena=nullptr; unsigned used=0,peak=0; Pack packs[max_packs]; unsigned pack_count=0;
// calls = in-scope main-thread scans; fast_hit/fast_none = answered via the index;
// native_* = in-scope scans passed through unchanged, by reason.
std::uint64_t calls=0,fast_hit=0,fast_none=0,native_pattern=0,native_start=0,native_guard=0,native_index=0,native_disabled=0;
std::uint64_t stale=0,mismatches=0,builds=0,build_failed=0,indexed_nodes=0,indexed_keys=0,memo_builds=0,memo_entries=0;
std::uint64_t avoided_nodes=0,visited_nodes=0; volatile LONG off_thread=0;
// "<P>*.<E>" share: fast answers (also counted in fast_hit/fast_none) and pass-throughs because the
// _stricmp body was not verified (also counted in native_pattern). Not in the startup report line.
std::uint64_t extension_fast=0,extension_unverified=0;
Counter fast_time,native_time,build_time;

struct Timer {
    Counter& counter; LONGLONG begin;
    explicit Timer(Counter& value):counter(value),begin(now()) {}
    ~Timer() { ++counter.calls; counter.ticks+=now()-begin; }
};
void release() {
    if (arena) VirtualFree(arena,0,MEM_RELEASE);
    arena=nullptr; used=0; pack_count=0;
}
// Index lifetime (the client data-table Window). Indices never outlive it.
struct Lifetime {
    bool live=false;
    Lifetime() { if (GetCurrentThreadId()==thread) { live=true; if (!lifetime++) release(); } }
    ~Lifetime() { if (live && !--lifetime) release(); }
    Lifetime(const Lifetime&)=delete; Lifetime& operator=(const Lifetime&)=delete;
};
// Fast-path window (the sound-scheme single-pass FS listing). Main thread only.
struct Active {
    bool live=false;
    Active() { if (GetCurrentThreadId()==thread && lifetime) { live=true; InterlockedIncrement(&active); } }
    ~Active() { if (live) InterlockedDecrement(&active); }
    Active(const Active&)=delete; Active& operator=(const Active&)=delete;
};

// Extension window: "<P>*.<E>" patterns are answered only inside it (and inside Active). It is
// opened only by the loading sound-precache scope; startup and server-sound scopes never open it, so
// their extension patterns stay native exactly as before. Main thread only.
struct Extensions {
    bool live=false;
    Extensions() { if (GetCurrentThreadId()==thread) { live=true; ++extensions; } }
    ~Extensions() { if (live) --extensions; }
    Extensions(const Extensions&)=delete; Extensions& operator=(const Extensions&)=delete;
};
bool body_ok(std::uintptr_t at,const Body& b) {
    unsigned char expected[600]; if (b.size>sizeof expected) return false;
    std::memcpy(expected,b.bytes,b.size);
    for (unsigned i=0;i<b.fixup_count;++i) { const auto value=static_cast<std::uint32_t>(at+b.fixups[i].rva); std::memcpy(expected+b.fixups[i].offset,&value,4); }
    return !std::memcmp(reinterpret_cast<const void*>(at+b.rva),expected,b.size);
}
bool bodies_ok(std::uintptr_t at) {
    for (const auto& b:bodies) if (!body_ok(at,b)) return false;
    return true;
}
// C locale (exact ASCII A-Z fold in the verified _strnicmp) and a created symbol table.
bool guards_ok() { return !*reinterpret_cast<const volatile int*>(base+locale_rva) && *reinterpret_cast<const volatile unsigned char*>(base+symbols_ready_rva); }
inline unsigned char fold(unsigned char c) { return c>='A' && c<='Z'?static_cast<unsigned char>(c+32):c; }

// Accepts exactly "<P>*.*" or "<P>*.<E>". P is non-empty printable ASCII without '.' or '*', so
// the native parse (scan +0x7c7e..+0x7cbf) splits at the "*." it ends with: prefix P
// (MaxCount=|P|>0) and extension "*" or E.
//   "*"  : match <=> _strnicmp(name,P,|P|)==0                                  (extension_length 0)
//   E    : that, and the name has a '.', and _stricmp(after the name's FIRST '.', E)==0.
// E is 1..32 printable ASCII without '.', '*' or '?', so no extension is "*"-led and both CRT
// routines compare plain bytes with an ASCII A-Z fold. P and E are returned folded.
bool parse(const char* pattern,char* prefix,unsigned& length,char* extension,unsigned& extension_length) {
    const size_t n=strnlen(pattern,max_pattern+1);
    if (n>max_pattern) return false;
    size_t star=0; while (star<n && pattern[star]!='*' && pattern[star]!='.') ++star;
    if (star+2>=n || pattern[star]!='*' || pattern[star+1]!='.') return false;
    length=static_cast<unsigned>(star);
    for (unsigned i=0;i<length;++i) {
        const auto c=static_cast<unsigned char>(pattern[i]);
        if (c<0x20 || c>0x7e) return false;
        prefix[i]=static_cast<char>(fold(c));
    }
    prefix[length]=0;
    if (length<6 || std::memcmp(prefix,"sound/",6)) return false;
    const char* e=pattern+star+2; const size_t e_length=n-star-2; // >= 1 (star+2 < n)
    if (e_length==1 && e[0]=='*') { extension_length=0; extension[0]=0; return true; }
    if (e_length>max_extension) return false;
    for (size_t i=0;i<e_length;++i) {
        const auto c=static_cast<unsigned char>(e[i]);
        if (c<0x20 || c>0x7e || c=='.' || c=='*' || c=='?') return false;
        extension[i]=static_cast<char>(fold(c));
    }
    extension_length=static_cast<unsigned>(e_length); extension[e_length]=0;
    return true;
}
// Native extension test on a folded key: first '.' of the whole name, then fold-equal through NUL.
bool extension_matches(const char* name,unsigned name_length,const char* extension,unsigned extension_length) {
    if (!extension_length) return true;
    const void* dot=std::memchr(name,'.',name_length); if (!dot) return false;
    const char* after=static_cast<const char*>(dot)+1; const auto rest=static_cast<unsigned>(name+name_length-after);
    return rest==extension_length && !std::memcmp(after,extension,extension_length);
}
const Tree& live_tree(const void* path) { return *reinterpret_cast<const Tree*>(static_cast<const char*>(path)+tree_offset); }
bool same_tree(const Pack& p) { return !std::memcmp(&live_tree(p.path),&p.tree,sizeof(Tree)); }
void* take(unsigned bytes) {
    bytes=(bytes+7u)&~7u;
    if (bytes>arena_capacity-used) return nullptr;
    void* at=arena+used; used+=bytes; if (used>peak) peak=used; return at;
}
// No <algorithm> here (this file is included inside a namespace): bounded heap
// sort and a partition-point search over plugin-owned arrays.
template<class T,class Less> void heap_sort(T* a,unsigned n,Less less) {
    auto sift=[&](unsigned root,unsigned end) {
        for (unsigned child;(child=2*root+1)<end;root=child) {
            if (child+1<end && less(a[child],a[child+1])) ++child;
            if (!less(a[root],a[child])) return;
            const T swap=a[root]; a[root]=a[child]; a[child]=swap;
        }
    };
    for (unsigned i=n/2;i-->0;) sift(i,n);
    for (unsigned end=n;end>1;) { --end; const T swap=a[0]; a[0]=a[end]; a[end]=swap; sift(0,end); }
}
template<class Before> unsigned first_not(unsigned n,Before before) { // first i with !before(i)
    unsigned low=0,high=n;
    while (low<high) { const unsigned mid=low+(high-low)/2; if (before(mid)) low=mid+1; else high=mid; }
    return low;
}
bool key_less(const char* text,const Key& a,const Key& b) {
    const int c=std::memcmp(text+a.offset,text+b.offset,a.length<b.length?a.length:b.length);
    return c?c<0:a.length<b.length;
}
// One in-order walk with the native successor rules (7c40) and the callers'
// leftmost start (3c10/4090), bounds-checked; names through native FS+0xcb60.
bool index(Pack& p) {
    const Tree t=p.tree; const auto alloc=t.alloc,elements=t.count;
    if (!t.nodes || alloc<0 || elements<0 || elements>alloc || static_cast<unsigned>(alloc)>max_alloc || t.root<-1 || t.root>=alloc) return false;
    p.ordinal_of=static_cast<int*>(take(4u*alloc)); p.node_at=static_cast<int*>(take(4u*elements)); p.symbol_at=static_cast<unsigned*>(take(4u*elements));
    if ((alloc && !p.ordinal_of) || (elements && (!p.node_at || !p.symbol_at))) return false;
    for (int i=0;i<alloc;++i) p.ordinal_of[i]=-1;
    auto at=[&](int i)->const Node* { return i==-1?&sentinel:(i>=0 && i<alloc?t.nodes+i:nullptr); };
    std::uint64_t budget=64ull*alloc+64; // bounds the inner loops of a corrupt tree
    int v=t.root;
    for (const Node* n=at(v);;n=at(v)) { if (!n || !budget--) return false; if (n->left==-1) break; v=n->left; }
    const unsigned text_begin=used; unsigned keys=0;
    for (int ordinal=0;v!=-1;++ordinal) {
        const Node* n=at(v);
        if (!n || n->left==v || ordinal>=elements || p.ordinal_of[v]!=-1) return false;
        p.ordinal_of[v]=ordinal; p.node_at[ordinal]=v; p.symbol_at[ordinal]=n->record[0];
        unsigned record[3]; std::memcpy(record,n->record,sizeof record); // native passes a stack copy
        const char* name=text_of(record);
        if (!name) return false;
        if (fold(name[0])=='s' && fold(name[1])=='o' && fold(name[2])=='u' && fold(name[3])=='n' && fold(name[4])=='d' && name[5]=='/') {
            const size_t length=strnlen(name,max_name+1); if (length>max_name) return false;
            auto slot=static_cast<unsigned*>(take(8u+static_cast<unsigned>(length))); if (!slot) return false;
            slot[0]=static_cast<unsigned>(ordinal); slot[1]=static_cast<unsigned>(length);
            auto out=reinterpret_cast<char*>(slot+2); for (size_t i=0;i<length;++i) out[i]=static_cast<char>(fold(static_cast<unsigned char>(name[i])));
            ++keys;
        }
        if (n->right==-1) { // climb while v is a right child
            int parent=n->parent; const Node* up=at(parent); if (!up) return false;
            if (up->right==v) {
                int child;
                do {
                    child=parent; if (parent==-1) break;
                    const Node* c=at(parent); if (!c || !budget--) return false; parent=c->parent;
                    up=at(parent); if (!up) return false;
                } while (up->right==child);
            }
            v=parent;
        } else {
            int i=n->right;
            for (const Node* c=at(i);;c=at(i)) { if (!c || !budget--) return false; if (c->left==-1) break; i=c->left; }
            v=i;
        }
        if (!budget--) return false;
    }
    p.key_count=keys; p.keys=static_cast<Key*>(take(sizeof(Key)*keys)); p.memo=static_cast<unsigned*>(take(4u*keys));
    if (keys && (!p.keys || !p.memo)) return false;
    // Ordinal count must equal the tree's element count (whole tree reached once).
    unsigned seen=0; for (int i=0;i<alloc;++i) if (p.ordinal_of[i]!=-1) ++seen;
    if (seen!=static_cast<unsigned>(elements)) return false;
    p.text=arena; unsigned cursor=text_begin;
    for (unsigned k=0;k<keys;++k) {
        auto slot=reinterpret_cast<const unsigned*>(arena+cursor);
        p.keys[k]={slot[0],cursor+8,slot[1]}; cursor+=(8u+slot[1]+7u)&~7u;
    }
    const char* text=p.text; heap_sort(p.keys,keys,[text](const Key& a,const Key& b){ return key_less(text,a,b); });
    indexed_nodes+=static_cast<unsigned>(elements); indexed_keys+=keys;
    return true;
}
Pack* find(const void* path) {
    for (unsigned i=0;i<pack_count;++i) if (packs[i].path==path) return packs[i].usable?&packs[i]:nullptr;
    if (pack_count==max_packs) return nullptr;
    if (!arena) arena=static_cast<char*>(VirtualAlloc(nullptr,arena_capacity,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if (!arena) return nullptr;
    Pack& p=packs[pack_count++]; p=Pack{}; p.path=path; p.tree=live_tree(path);
    const unsigned mark=used; Timer t(build_time); ++builds;
    p.usable=index(p);
    if (!p.usable) { used=mark; ++build_failed; return nullptr; }
    return &p;
}
// Sorted ordinals of every node whose folded name starts with the folded prefix and, for an
// extension pattern, passes the native extension test.
void memo(Pack& p,const char* prefix,unsigned length,const char* extension,unsigned extension_length) {
    if (p.memo_valid && p.memo_length==length && !std::memcmp(p.memo_prefix,prefix,length) &&
        p.memo_extension_length==extension_length && !std::memcmp(p.memo_extension,extension,extension_length)) return;
    const char* text=p.text;
    const Key* first=p.keys+first_not(p.key_count,[&](unsigned i) { const Key& k=p.keys[i];
        const int c=std::memcmp(text+k.offset,prefix,k.length<length?k.length:length); return c?c<0:k.length<length; });
    unsigned n=0;
    for (const Key* k=first;k!=p.keys+p.key_count && k->length>=length && !std::memcmp(text+k->offset,prefix,length);++k)
        if (extension_matches(text+k->offset,k->length,extension,extension_length)) p.memo[n++]=k->ordinal;
    heap_sort(p.memo,n,[](unsigned a,unsigned b){ return a<b; });
    std::memcpy(p.memo_prefix,prefix,length); p.memo_length=length; p.memo_count=n; p.memo_valid=true;
    std::memcpy(p.memo_extension,extension,extension_length); p.memo_extension_length=extension_length;
    ++memo_builds; memo_entries+=n;
}
int native(void* path,const char* pattern,int from_node,std::uint64_t& reason) { ++reason; Timer t(native_time); return original(path,pattern,from_node); }
int __stdcall scan(void* path,const char* pattern,int from_node) {
    // Other threads must not read mutable main-thread index/trace state.
    if (GetCurrentThreadId()!=thread) {
        if (InterlockedCompareExchange(&active,0,0)) InterlockedIncrement(&off_thread);
        return original(path,pattern,from_node);
    }
    if (!ready || !active) return original(path,pattern,from_node);
    ++calls;
    if (disabled) return native(path,pattern,from_node,native_disabled);
    char prefix[max_pattern+1],extension[max_extension+1]; unsigned length=0,extension_length=0;
    if (!pattern || !parse(pattern,prefix,length,extension,extension_length)) return native(path,pattern,from_node,native_pattern);
    if (extension_length && !extensions) return native(path,pattern,from_node,native_pattern);
    if (extension_length && !extension_ok) { ++extension_unverified; return native(path,pattern,from_node,native_pattern); }
    if (from_node==-1) return native(path,pattern,from_node,native_start);
    if (!guards_ok()) return native(path,pattern,from_node,native_guard);
    Pack* p=find(path);
    if (!p) return native(path,pattern,from_node,native_index);
    if (!same_tree(*p)) { p->usable=false; return native(path,pattern,from_node,stale); }
    if (from_node<0 || from_node>=p->tree.alloc || p->ordinal_of[from_node]<0) return native(path,pattern,from_node,native_start);
    const auto from=static_cast<unsigned>(p->ordinal_of[from_node]);
    if (p->tree.nodes[from_node].record[0]!=p->symbol_at[from]) { p->usable=false; return native(path,pattern,from_node,stale); }
    Timer t(fast_time);
    memo(*p,prefix,length,extension,extension_length);
    const unsigned* next=p->memo+first_not(p->memo_count,[&](unsigned i){ return p->memo[i]<from; });
    const int expected=next==p->memo+p->memo_count?-1:p->node_at[*next];
    if (expected!=-1 && p->tree.nodes[expected].record[0]!=p->symbol_at[*next]) { p->usable=false; return native(path,pattern,from_node,stale); }
    const int result=original(path,pattern,expected);
    if (result!=expected) { disabled=true; ++mismatches; return original(path,pattern,from_node); }
    if (extension_length) ++extension_fast;
    if (expected==-1) { ++fast_none; avoided_nodes+=static_cast<unsigned>(p->tree.count)-from; }
    else { ++fast_hit; ++visited_nodes; avoided_nodes+=*next-from; }
    return result;
}

void install() {
    ready=false; hooks=0;
    if (!requested) return;
    const HMODULE module=GetModuleHandleW(L"filesystem_stdio.dll");
    wchar_t path[MAX_PATH]; unsigned changed=0; const DWORD length=module?GetModuleFileNameW(module,path,MAX_PATH):0;
    const int build=length && length<MAX_PATH?build_of(path,changed):0;
    if (!build) { host->log("Startup pack prefix off: FileSystem_Stdio.dll is not the analyzed build (nor that build edited in place)."); return; }
    base=reinterpret_cast<std::uintptr_t>(module);
    if (!bodies_ok(base)) { host->log("Startup pack prefix off: a verified FileSystem_Stdio body differs in memory."); return; }
    const char* labels[3]={"pack_prefix_find_first","pack_prefix_find_next","pack_prefix_find_next_pack"};
    for (unsigned i=0;i<3;++i) if (call_hook(base,sites[i],scan_rva,reinterpret_cast<void*>(scan),reinterpret_cast<void**>(&original),labels[i])) ++hooks;
    text_of=reinterpret_cast<Text>(base+text_rva);
    extension_ok=body_ok(base,extension_body); // gates only "<P>*.<E>" answers
    ready=hooks && reinterpret_cast<std::uintptr_t>(original)==base+scan_rva;
    char line[200]; sprintf_s(line,"Startup pack prefix requested=1 enabled=%d hooks=%u/3 locale_c=%d (sound-scheme single-pass listings only)",ready?1:0,hooks,
        *reinterpret_cast<const int*>(base+locale_rva)?0:1); host->log(line);
}
// Trace begin (also after an engine restart). Hooks stay; the next install re-verifies.
void reset() {
    release(); ready=disabled=extension_ok=false; hooks=0; text_of=nullptr; lifetime=0; InterlockedExchange(&active,0); extensions=0; peak=0;
    calls=fast_hit=fast_none=native_pattern=native_start=native_guard=native_index=native_disabled=0;
    stale=mismatches=builds=build_failed=indexed_nodes=indexed_keys=memo_builds=memo_entries=avoided_nodes=visited_nodes=0;
    off_thread=0; extension_fast=extension_unverified=0; fast_time=native_time=build_time=Counter{};
}
void report(double scale) {
    if (!requested) return;
    char line[900];
    sprintf_s(line,"Startup pack prefix requested=%d enabled=%d hooks=%u/3 disabled=%d calls=%llu fast_hit=%llu fast_none=%llu"
        " native_pattern=%llu native_start=%llu native_guard=%llu native_index=%llu native_disabled=%llu stale=%llu mismatches=%llu off_thread=%ld"
        " builds=%llu build_failed=%llu indexed_nodes=%llu indexed_keys=%llu memo_builds=%llu memo_entries=%llu peak_bytes=%u"
        " avoided_nodes=%llu native_visited_fast=%llu fast_ms=%.3f native_ms=%.3f/%llu build_ms=%.3f",
        requested?1:0,ready?1:0,hooks,disabled?1:0,calls,fast_hit,fast_none,native_pattern,native_start,native_guard,native_index,native_disabled,stale,mismatches,
        static_cast<long>(off_thread),builds,build_failed,indexed_nodes,indexed_keys,memo_builds,memo_entries,peak,avoided_nodes,visited_nodes,
        fast_time.ticks*scale,native_time.ticks*scale,native_time.calls,build_time.ticks*scale); host->log(line);
}
}
#endif
