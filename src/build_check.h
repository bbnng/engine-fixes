// Build check for game DLLs, shared by the plugin and the startup companion.
//
// Unofficial Patch forward compatibility: a DLL is accepted when it is the analyzed file, or the
// same build that a newer patch edited in place: the same link time and exactly the same section
// table, and at most a tenth of its 4 KB blocks changed. A rebuilt or copy-protected DLL differs in
// its headers and is refused. This only says the DLL's layout (code and data addresses) is the
// analyzed one; every feature still checks the bytes of each hook site, called function and
// relied-on value before using it, and turns itself off if they differ.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#pragma comment(lib, "bcrypt.lib")

namespace build_check {
// As in the PE section table: name, VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData.
struct Section { char name[8]; std::uint32_t size, address, raw_size, raw_offset; };
struct Profile {
    const char* file;
    std::uint32_t timestamp;
    const Section* sections; unsigned section_count;
    const std::uint32_t* blocks; unsigned block_count; // first 4 bytes (LE) of each 4 KB block's SHA-256
};
enum Kind { unreadable, other_build, too_changed, analyzed, edited };
struct Result { Kind kind; unsigned changed, total; };
inline bool accepted(const Result& r) { return r.kind == analyzed || r.kind == edited; }

#include "build_profiles.inc"

inline std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0] | p[1] << 8 | p[2] << 16) | static_cast<std::uint32_t>(p[3]) << 24;
}
inline bool same_layout(const std::vector<unsigned char>& head, const Profile& p) {
    const std::size_t size = head.size();
    if (size < 0x40 || head[0] != 'M' || head[1] != 'Z') return false;
    const std::size_t pe = le32(&head[0x3c]);
    if (pe > size || size - pe < 24 || std::memcmp(&head[pe], "PE\0\0", 4) || head[pe + 4] != 0x4c || head[pe + 5] != 0x01) return false;
    const std::size_t count = static_cast<std::size_t>(head[pe + 6] | head[pe + 7] << 8);
    const std::size_t optional = static_cast<std::size_t>(head[pe + 20] | head[pe + 21] << 8);
    const std::size_t table = pe + 24 + optional;
    if (le32(&head[pe + 8]) != p.timestamp || count != p.section_count || table > size || (size - table) / 40 < count) return false;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char* h = &head[table + 40 * i];
        const Section& s = p.sections[i];
        if (std::memcmp(h, s.name, 8) || le32(h + 8) != s.size || le32(h + 12) != s.address || le32(h + 16) != s.raw_size ||
            le32(h + 20) != s.raw_offset) return false;
    }
    return true;
}
// Reads the file once: its first 4 KB and the fingerprint of each 4 KB block.
inline bool read_blocks(const wchar_t* path, std::vector<unsigned char>& head, std::vector<std::uint32_t>& blocks) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    static unsigned char block[4096]; // callers run on one thread (game startup)
    while (ok) {
        DWORD got = 0;
        while (ok && got < sizeof block) {
            DWORD part = 0;
            if (!ReadFile(file, block + got, static_cast<DWORD>(sizeof block - got), &part, nullptr)) ok = false;
            else if (!part) break;
            got += part;
        }
        if (!ok || !got) break;
        if (head.empty()) head.assign(block, block + got);
        BCRYPT_HASH_HANDLE one = nullptr;
        unsigned char digest[32];
        ok = BCryptCreateHash(alg, &one, nullptr, 0, nullptr, 0, 0) >= 0 && BCryptHashData(one, block, got, 0) >= 0 &&
             BCryptFinishHash(one, digest, sizeof digest, 0) >= 0;
        if (one) BCryptDestroyHash(one);
        if (ok) blocks.push_back(le32(digest));
    }
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return ok;
}
inline Result check_file(const wchar_t* path, const Profile& p) {
    std::vector<unsigned char> head;
    std::vector<std::uint32_t> blocks;
    if (!read_blocks(path, head, blocks)) return {unreadable, 0, p.block_count};
    if (!same_layout(head, p)) return {other_build, 0, p.block_count};
    unsigned changed = 0;
    const std::size_t n = blocks.size() > p.block_count ? blocks.size() : p.block_count;
    for (std::size_t i = 0; i < n; ++i)
        if (i >= p.block_count || i >= blocks.size() || blocks[i] != p.blocks[i]) ++changed;
    if (!changed) return {analyzed, 0, p.block_count};
    return {changed <= p.block_count / 10 ? edited : too_changed, changed, p.block_count};
}
inline void describe(const Profile& p, const Result& r, char* out, std::size_t size) {
    switch (r.kind) {
    case analyzed: sprintf_s(out, size, "%s: the analyzed build.", p.file); break;
    case edited: sprintf_s(out, size, "%s: the analyzed build, edited in place by a patch (%u of %u blocks of 4 KB differ); "
                           "each feature checks its code before using it.", p.file, r.changed, r.total); break;
    case too_changed: sprintf_s(out, size, "%s has the analyzed build's layout, but %u of its %u blocks of 4 KB differ: too much "
                                "for a patch that edits it in place, so the features that need it stay off.", p.file, r.changed, r.total); break;
    case other_build: sprintf_s(out, size, "%s is not the analyzed build: the features that need it stay off.", p.file); break;
    default: sprintf_s(out, size, "%s could not be read: the features that need it stay off.", p.file); break;
    }
}
// Cached per profile (the file does not change while the game runs); the result is logged once.
// An engine restart loads the same files again, so the cache stays valid.
inline Result check(HMODULE module, const Profile& p, void (*log)(const char*)) {
    struct Entry { const Profile* profile; Result result; };
    static Entry cache[8];
    static unsigned cached = 0;
    for (unsigned i = 0; i < cached; ++i) if (cache[i].profile == &p) return cache[i].result;
    wchar_t path[MAX_PATH];
    const DWORD length = module ? GetModuleFileNameW(module, path, MAX_PATH) : 0;
    const Result result = length && length < MAX_PATH ? check_file(path, p) : Result{unreadable, 0, p.block_count};
    if (cached < _countof(cache)) cache[cached++] = {&p, result};
    if (log) { char line[320]; describe(p, result, line, sizeof line); log(line); }
    return result;
}
inline bool known(HMODULE module, const Profile& p, void (*log)(const char*)) { return module && accepted(check(module, p, log)); }
} // namespace build_check
