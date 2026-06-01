#include "fh6/logo_hook.hpp"
#include "fh6/log.hpp"

#include <windows.h>
#include <winnt.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>

namespace fh6 {

namespace {

// The original CreateFileW imported by the game's main module.
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD,
                                      LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static std::atomic<CreateFileW_t> g_orig{nullptr};
static wchar_t g_custom_path[MAX_PATH * 2]{};   // fh6-radio\custom\Anthem.zip

// ---------------------------------------------------------------------------
// Hook: intercept opens of ..\Textures\Anthem.zip (standard, not HiRes or
// Data_Bound) and redirect them to our pre-built custom copy.
// ---------------------------------------------------------------------------
static HANDLE WINAPI hook_CreateFileW(
    LPCWSTR             lpFileName,
    DWORD               dwDesiredAccess,
    DWORD               dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD               dwCreationDisposition,
    DWORD               dwFlagsAndAttributes,
    HANDLE              hTemplateFile)
{
    auto* orig = g_orig.load(std::memory_order_acquire);
    if (lpFileName && g_custom_path[0]) {
        // Case-insensitive suffix match: path ends with Textures/Anthem.zip
        // and does NOT contain HiRes or Data_Bound
        std::wstring p(lpFileName);
        auto lower = p;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](wchar_t c) noexcept { return static_cast<wchar_t>(towlower(c)); });

        constexpr std::wstring_view kSuffix{L"\\textures\\anthem.zip"};
        constexpr std::wstring_view kHiRes{L"\\hires\\"};
        constexpr std::wstring_view kDataBound{L"\\data_bound\\"};

        if (lower.size() >= kSuffix.size() &&
            lower.compare(lower.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0 &&
            lower.find(kHiRes)      == std::wstring::npos &&
            lower.find(kDataBound)  == std::wstring::npos)
        {
            return orig(g_custom_path, dwDesiredAccess, dwShareMode,
                        lpSecurityAttributes, dwCreationDisposition,
                        dwFlagsAndAttributes, hTemplateFile);
        }
    }
    return orig(lpFileName, dwDesiredAccess, dwShareMode,
                lpSecurityAttributes, dwCreationDisposition,
                dwFlagsAndAttributes, hTemplateFile);
}

// ---------------------------------------------------------------------------
// Patch one IAT slot in hMod: find dllName!funcName and swap the pointer.
// Returns the original function pointer, or nullptr on failure.
// ---------------------------------------------------------------------------
static void* patch_iat(HMODULE hMod,
                        const char* dllName,
                        const char* funcName,
                        void*       replacement) noexcept
{
    auto* base = reinterpret_cast<const std::byte*>(hMod);
    auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;

    auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* name = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(name, dllName) != 0) continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            const_cast<std::byte*>(base) + desc->FirstThunk);
        auto* orig_thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(
            base + desc->OriginalFirstThunk);

        for (; thunk->u1.Function; ++thunk, ++orig_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal)) continue;
            const auto* imp = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + orig_thunk->u1.AddressOfData);
            if (strcmp(imp->Name, funcName) != 0) continue;

            void* old_fn = reinterpret_cast<void*>(thunk->u1.Function);
            DWORD old_prot{};
            if (!VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR),
                                PAGE_READWRITE, &old_prot))
                return nullptr;
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR),
                           old_prot, &old_prot);
            return old_fn;
        }
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void install_logo_hook(const std::filesystem::path& game_dir) noexcept
{
    try {
        const auto custom = game_dir / "fh6-radio" / "custom" / "Anthem.zip";
        if (!std::filesystem::exists(custom)) {
            log::info("[logo] custom Anthem.zip not found — logo hook inactive");
            log::info("[logo] run pack-logo.ps1 to create fh6-radio/custom/Anthem.zip");
            return;
        }

        // Store path for the hook function
        const auto ws = custom.wstring();
        if (ws.size() >= std::size(g_custom_path)) {
            log::warn("[logo] custom path too long, logo hook inactive");
            return;
        }
        std::copy(ws.begin(), ws.end(), g_custom_path);
        g_custom_path[ws.size()] = L'\0';

        // Patch the game executable's IAT
        HMODULE exe = GetModuleHandleW(nullptr);
        void* orig = patch_iat(exe, "KERNEL32.DLL", "CreateFileW",
                               reinterpret_cast<void*>(&hook_CreateFileW));
        if (!orig) {
            // Some builds import as "kernel32.dll" (lowercase)
            orig = patch_iat(exe, "kernel32.dll", "CreateFileW",
                             reinterpret_cast<void*>(&hook_CreateFileW));
        }
        if (!orig) {
            log::warn("[logo] IAT patch for CreateFileW failed — logo hook inactive");
            g_custom_path[0] = L'\0';
            return;
        }

        g_orig.store(reinterpret_cast<CreateFileW_t>(orig),
                     std::memory_order_release);

        log::info("[logo] logo hook active -> {}", custom.string());
    } catch (...) {
        log::warn("[logo] exception in install_logo_hook, hook inactive");
    }
}

} // namespace fh6
