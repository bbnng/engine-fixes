// Engine Fixes and Tweaks for Vampire: The Masquerade - Bloodlines.
// Copyright (c) 2026 bigbannng. GPL-3.0-only with additional terms: see LICENSE and NOTICE.
// Fix fixed-function water clip planes in shaderapidx9.dll.
//
// In Direct3D 9, IDirect3DDevice9::SetClipPlane interprets the plane in CLIP SPACE when a
// programmable vertex shader is bound (SetVertexShader != NULL), and in WORLD SPACE when the
// fixed-function pipeline is active (SetVertexShader == NULL, because D3D9 transforms the plane
// internally by (View * Proj)^-T).
//
// CShaderAPIDx8::CommitClipPlanes (shaderapidx9.dll+0x21C00, called from +0x1A5E1 right before
// mesh rendering) unconditionally transforms m_ClipPlanes[i] by Transpose(Inverse(View * Proj))
// into clip space before calling SetClipPlane, without checking whether a vertex shader is bound.
// Non-bumped LightmappedGeneric materials (such as CONCRETE/GANGBASE on the Santa Monica sewer
// canal floor) render through the fixed-function pipeline, so D3D9 transforms the clip-space
// plane a second time. The resulting bogus plane slices across the foreground canal floor during
// the _rt_WaterRefraction pass; because waterView.clearColor is 0, the un-overwritten pixels stay
// frozen on an earlier frame and move with the camera until mat_clipz is disabled.
//
// Hooking the call to CommitClipPlanes in DrawMesh (shaderapidx9.dll+0x1A5E1) lets us upload the
// world-space plane directly for fixed-function materials, and mark enabled planes dirty when
// switching back to a vertex shader so CommitClipPlanes re-uploads the clip-space plane. (The only
// other call, in the CommitStateChanges body at +0x21FC0, is not reached: DrawMesh holds an inlined
// copy of it.)
//
// DrawMesh commits its state BEFORE the material's passes bind their vertex shader, so the device
// still holds the previous draw's shader at this point. The kind of draw is therefore taken from the
// bound material, exactly as DrawMesh itself chooses between vertex-shader constants and
// fixed-function transforms a few instructions earlier (+0x1A438: material vertex format bit 0x100).

namespace water_clip_plane {

using CommitClipPlanesFn = void (__thiscall*)(void*);
using GetVertexFormatFn = std::uint32_t (__thiscall*)(void*);
using SetClipPlaneFn = long (__stdcall*)(void*, unsigned long, const float*);

constexpr std::uintptr_t kCallSiteRva = 0x1a5e1;
constexpr std::uintptr_t kCheckSiteRva = 0x1a5d4;
constexpr std::uintptr_t kMaterialCheckRva = 0x1a438;
constexpr std::uintptr_t kCommitFnRva = 0x21c00;
constexpr std::uint32_t kShaderApiTimestamp = 0x4164cbee;

constexpr std::size_t kDeviceOffset = 0x1c;
constexpr std::size_t kClipEnabledOffset = 0x1480;
constexpr std::size_t kClipDirtyOffset = 0x1484;
constexpr std::size_t kClipPlanesOffset = 0x1488;
constexpr std::size_t kLastClipPlanesOffset = 0x14e8;
constexpr std::size_t kMaterialOffset = 0x1a64;
constexpr unsigned kMaxClipPlanes = 6;
constexpr std::size_t kSetClipPlaneSlot = 55;   // IDirect3DDevice9 +0xdc
constexpr std::size_t kGetVertexFormatSlot = 20; // material +0x50
constexpr std::uint32_t kVertexShaderFormat = 0x100;

CommitClipPlanesFn original_commit = nullptr;
std::uint32_t fixed_function_mask = 0;
const unsigned char* hooked_module = nullptr;

bool plane_differs(const float a[4], const float b[4]) {
    return a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3];
}

void __fastcall commit_clip_planes_hook(void* shader_api, void*) {
    if (!shader_api || !original_commit) return;
    auto* base = static_cast<unsigned char*>(shader_api);
    void* device = *reinterpret_cast<void**>(base + kDeviceOffset);
    const std::uint32_t enabled_mask = *reinterpret_cast<std::uint32_t*>(base + kClipEnabledOffset);
    void* material = *reinterpret_cast<void**>(base + kMaterialOffset);
    if (!device || !material || !enabled_mask) {
        original_commit(shader_api);
        return;
    }

    auto* material_vt = *reinterpret_cast<void***>(material);
    const std::uint32_t format = reinterpret_cast<GetVertexFormatFn>(material_vt[kGetVertexFormatSlot])(material);
    const bool fixed_function = (format & kVertexShaderFormat) == 0;

    auto& dirty_mask = *reinterpret_cast<std::uint32_t*>(base + kClipDirtyOffset);
    if (fixed_function) {
        auto* dev_vt = *reinterpret_cast<void***>(device);
        const auto set_clip_plane = reinterpret_cast<SetClipPlaneFn>(dev_vt[kSetClipPlaneSlot]);
        for (unsigned i = 0; i < kMaxClipPlanes; ++i) {
            const std::uint32_t bit = 1u << i;
            if ((enabled_mask & bit) == 0) continue;
            const auto* world_plane = reinterpret_cast<const float*>(base + kClipPlanesOffset + i * 16);
            auto* last_plane = reinterpret_cast<float*>(base + kLastClipPlanesOffset + i * 16);
            if ((fixed_function_mask & bit) == 0 || (dirty_mask & bit) != 0 || plane_differs(world_plane, last_plane)) {
                set_clip_plane(device, i, world_plane);
                last_plane[0] = world_plane[0];
                last_plane[1] = world_plane[1];
                last_plane[2] = world_plane[2];
                last_plane[3] = world_plane[3];
            }
            fixed_function_mask |= bit;
            dirty_mask &= ~bit;
        }
        return;
    }

    const std::uint32_t to_reupload = fixed_function_mask & enabled_mask;
    if (to_reupload != 0) {
        dirty_mask |= to_reupload;
        fixed_function_mask &= ~to_reupload;
    }
    original_commit(shader_api);
}

bool module_matches(const unsigned char* base) {
    __try {
        if (base[0] != 'M' || base[1] != 'Z') return false;
        const std::uint32_t pe = le32(base + 0x3c);
        if (pe > 0x1000 || std::memcmp(base + pe, "PE\0\0", 4) != 0) return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + pe);
        if (nt->FileHeader.TimeDateStamp != kShaderApiTimestamp ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
        const unsigned char expected_call[] = {
            0x8b,0x86,0x80,0x14,0x00,0x00,
            0x85,0xc0,
            0x5f,
            0x74,0x07,
            0x8b,0xce,
            0xe8,0x1a,0x76,0x00,0x00
        };
        // mov ecx,[esi+1A64h]; mov eax,[ecx]; call [eax+50h]; shr eax,8; test al,1
        const unsigned char expected_material[] = {
            0x8b,0x8e,0x64,0x1a,0x00,0x00,
            0x8b,0x01,
            0xff,0x50,0x50,
            0xc1,0xe8,0x08,
            0xa8,0x01
        };
        const unsigned char expected_fn[] = {
            0x81,0xec,0x94,0x00,0x00,0x00,
            0x53,0x8b,0xd9,0x8b,0x43,0x04,0x56,0x8d,0x4b,0x04
        };
        const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
        if (kCheckSiteRva + sizeof(expected_call) > image_size ||
            kMaterialCheckRva + sizeof(expected_material) > image_size ||
            kCommitFnRva + sizeof(expected_fn) > image_size) return false;
        return std::memcmp(base + kCheckSiteRva, expected_call, sizeof(expected_call)) == 0 &&
               std::memcmp(base + kMaterialCheckRva, expected_material, sizeof(expected_material)) == 0 &&
               std::memcmp(base + kCommitFnRva, expected_fn, sizeof(expected_fn)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// True while the call site of this loaded module already goes to our hook (the module stayed loaded
// through an engine restart).
bool already_hooked(const unsigned char* base) {
    if (!original_commit || hooked_module != base) return false;
    __try {
        const unsigned char* site = base + kCallSiteRva;
        std::int32_t rel = 0;
        std::memcpy(&rel, site + 1, 4);
        return site[0] == 0xe8 && reinterpret_cast<std::uintptr_t>(site + 5) + rel == reinterpret_cast<std::uintptr_t>(commit_clip_planes_hook);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void install() {
    const HMODULE mod = GetModuleHandleW(L"shaderapidx9.dll");
    if (!mod) {
        host->log("Water clip plane fix disabled: shaderapidx9.dll not loaded.");
        return;
    }
    auto* base = reinterpret_cast<unsigned char*>(mod);
    if (already_hooked(base)) {
        host->log("Water clip plane fix still enabled (shaderapidx9.dll was not reloaded).");
        return;
    }
    if (!module_matches(base)) {
        host->log("Water clip plane fix disabled: unsupported shaderapidx9.dll build or byte check failed.");
        return;
    }
    void* original = nullptr;
    if (host->hook_call(host->context, base + kCallSiteRva,
            reinterpret_cast<void*>(commit_clip_planes_hook), &original) != BLSE_HOOK_OK) {
        host->log("Water clip plane fix disabled: call hook failed or conflicted.");
        return;
    }
    original_commit = reinterpret_cast<CommitClipPlanesFn>(original);
    hooked_module = base;
    fixed_function_mask = 0;
    host->log("Water clip plane fix enabled (shaderapidx9.dll+0x1A5E1: world-space SetClipPlane for fixed-function materials).");
}

} // namespace water_clip_plane
