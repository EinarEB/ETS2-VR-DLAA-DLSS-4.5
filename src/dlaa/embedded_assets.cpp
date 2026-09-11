// SPDX-License-Identifier: MIT
#include "embedded_assets.h"
#include <reshade.hpp>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace ets2_dlaa_assets {
namespace {
namespace fs = std::filesystem;
constexpr DWORD MaxAssetBytes = 2 * 1024 * 1024;
constexpr size_t MaxConfigBytes = 1024 * 1024;
constexpr size_t MaxPaths = 512;
struct Failure { DWORD error; };
void Require(bool good, DWORD error) { if (!good) throw Failure{error}; }
DWORD LastError() { const DWORD e = GetLastError(); return e ? e : ERROR_GEN_FAILURE; }
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    File() = default;
    File(const File &) = delete;
    File &operator=(const File &) = delete;
};
struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE value = nullptr;
    ~Hash() { if (value) BCryptDestroyHash(value); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
};
struct Asset { WORD id; const char *name; const BYTE *bytes = nullptr; DWORD size = 0; };
std::string Utf8(const fs::path &path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}
fs::path PathUtf8(const std::string &text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}
void HashBytes(Hash &hash, const void *bytes, DWORD size) {
    Require(BCryptHashData(hash.value, const_cast<PUCHAR>(static_cast<const BYTE *>(bytes)), size, 0) >= 0,
        ERROR_CRC);
}
std::string LoadAndHash(HMODULE module, std::array<Asset, 5> &assets) {
    Hash hash;
    Require(BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
        ERROR_NOT_SUPPORTED);
    Require(BCryptCreateHash(hash.algorithm, &hash.value, nullptr, 0, nullptr, 0, 0) >= 0,
        ERROR_NOT_SUPPORTED);
    for (auto &asset : assets) {
        const HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(asset.id), MAKEINTRESOURCEW(10));
        Require(found != nullptr, ERROR_RESOURCE_DATA_NOT_FOUND);
        asset.size = SizeofResource(module, found);
        Require(asset.size > 0 && asset.size <= MaxAssetBytes, ERROR_BAD_LENGTH);
        const HGLOBAL loaded = LoadResource(module, found);
        Require(loaded != nullptr, ERROR_RESOURCE_DATA_NOT_FOUND);
        asset.bytes = static_cast<const BYTE *>(LockResource(loaded));
        Require(asset.bytes != nullptr, ERROR_RESOURCE_DATA_NOT_FOUND);
        HashBytes(hash, asset.name, static_cast<DWORD>(std::strlen(asset.name) + 1));
        HashBytes(hash, &asset.size, sizeof(asset.size));
        HashBytes(hash, asset.bytes, asset.size);
    }
    std::array<BYTE, 32> digest{};
    Require(BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, ERROR_CRC);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const BYTE b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
    return result;
}
fs::path AssetRoot() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    Require(required > 1 && required < 32768, ERROR_BAD_ENVIRONMENT);
    std::vector<wchar_t> text(required);
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", text.data(), required);
    Require(copied > 0 && copied < required, ERROR_BAD_ENVIRONMENT);
    const fs::path local(text.data());
    Require(local.is_absolute(), ERROR_BAD_PATHNAME);
    return local / L"ETS2-DLAA" / L"assets";
}
void MakeOwnedDirectory(const fs::path &path) {
    // Validate each owned ancestor before creating its child, so an existing
    // redirect cannot cause writes outside the owned directory.
    const std::array<fs::path, 3> directories{
        path.parent_path().parent_path(), path.parent_path(), path};
    for (const auto &check : directories) {
        if (!CreateDirectoryW(check.c_str(), nullptr)) Require(GetLastError() == ERROR_ALREADY_EXISTS, LastError());
        const DWORD attributes = GetFileAttributesW(check.c_str());
        Require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0, ERROR_REPARSE_TAG_INVALID);
    }
}
void VerifyFile(const fs::path &path, const Asset &asset) {
    File file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Require(file.value != INVALID_HANDLE_VALUE, LastError());
    BY_HANDLE_FILE_INFORMATION info{};
    Require(GetFileInformationByHandle(file.value, &info) != FALSE, LastError());
    Require((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        info.nFileSizeHigh == 0 && info.nFileSizeLow == asset.size, ERROR_FILE_INVALID);
    std::array<BYTE, 16384> buffer{};
    DWORD offset = 0;
    while (offset < asset.size) {
        const DWORD wanted = (std::min)(static_cast<DWORD>(buffer.size()), asset.size - offset);
        DWORD got = 0;
        Require(ReadFile(file.value, buffer.data(), wanted, &got, nullptr) != FALSE, LastError());
        Require(got == wanted && std::memcmp(buffer.data(), asset.bytes + offset, got) == 0, ERROR_CRC);
        offset += got;
    }
}
bool ExtractFile(const fs::path &path, const Asset &asset) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) { VerifyFile(path, asset); return false; }
    const DWORD missing = GetLastError();
    Require(missing == ERROR_FILE_NOT_FOUND, missing);
    // A fresh private temporary file is published without replacement. Never
    // truncate a pre-existing file, including a user's modified owned asset.
    static unsigned serial = 0; // Entire EnsureAssets operation is serialized.
    const fs::path temporary = path.wstring() + L".part-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(++serial);
    bool created = false;
    try {
        {
            File file;
            file.value = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            Require(file.value != INVALID_HANDLE_VALUE, LastError());
            created = true;
            DWORD written = 0;
            Require(WriteFile(file.value, asset.bytes, asset.size, &written, nullptr) != FALSE, LastError());
            Require(written == asset.size, ERROR_WRITE_FAULT);
            Require(FlushFileBuffers(file.value) != FALSE, LastError());
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            Require(error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS, error);
            VerifyFile(path, asset); // Another process may have completed the same version.
            DeleteFileW(temporary.c_str());
            return false;
        }
        created = false;
        VerifyFile(path, asset);
        return true;
    } catch (...) {
        if (created) DeleteFileW(temporary.c_str());
        throw;
    }
}
void DecodeArray(const std::vector<char> &text, size_t copied, std::vector<std::string> &out) {
    Require(copied < text.size() && text[copied] == '\0', ERROR_INVALID_DATA);
    size_t start = 0;
    while (start < copied) {
        size_t end = start;
        while (end < copied && text[end] != '\0') ++end;
        Require(end < copied, ERROR_INVALID_DATA);
        Require(out.size() < MaxPaths, ERROR_BAD_LENGTH);
        out.emplace_back(text.data() + start, end - start);
        start = end + 1;
    }
}
bool ReadArray(reshade::api::effect_runtime *runtime, const char *key, std::vector<std::string> &out) {
    size_t required = 0;
    if (!reshade::get_config_value(runtime, "GENERAL", key, nullptr, &required)) return false;
    Require(required >= 2 && required <= MaxConfigBytes, ERROR_BAD_LENGTH);
    std::vector<char> text(required, '\0');
    size_t copied = required;
    Require(reshade::get_config_value(runtime, "GENERAL", key, text.data(), &copied) && copied < required,
        ERROR_RETRY);
    DecodeArray(text, copied, out);
    return true;
}
bool IsOwnedVersion(const std::string &text, const fs::path &root) {
    const fs::path path = PathUtf8(text).lexically_normal();
    if (!path.is_absolute() || _wcsicmp(path.parent_path().c_str(), root.c_str()) != 0) return false;
    const std::wstring leaf = path.filename().wstring();
    return leaf.size() == 67 && leaf.compare(0, 3, L"v1-") == 0 &&
        std::all_of(leaf.begin() + 3, leaf.end(), [](wchar_t c) {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
        });
}
std::vector<std::string> UpdatedPaths(const std::vector<std::string> &before, const fs::path &root,
    const std::string &directory) {
    // Only retire exact entries from our private content-addressed namespace.
    // Unrelated paths retain their original text and relative order.
    std::vector<std::string> after{directory};
    for (const auto &entry : before) if (!IsOwnedVersion(entry, root)) after.push_back(entry);
    Require(after.size() <= MaxPaths, ERROR_BAD_LENGTH);
    return after;
}
std::string PackArray(const std::vector<std::string> &values) {
    std::string packed;
    for (const auto &entry : values) { packed += entry; packed += '\0'; }
    Require(!packed.empty() && packed.size() <= MaxConfigBytes, ERROR_BAD_LENGTH);
    return packed;
}
bool NeedsReload(bool changed, std::uint32_t created, bool hasDlaa, bool hasMotion) noexcept {
    return changed || created != 0 || !hasDlaa || !hasMotion;
}
bool InstallPath(reshade::api::effect_runtime *runtime, const char *key, const fs::path &root,
    const std::string &directory) {
    std::vector<std::string> before;
    if (!ReadArray(runtime, key, before) && !ReadArray(nullptr, key, before)) before.emplace_back(".\\");
    const std::vector<std::string> after = UpdatedPaths(before, root, directory);
    if (before == after) return false;
    const std::string packed = PackArray(after);
    // Exclude the final delimiter; ReShadeSetConfigArray splits at internal NULs.
    reshade::set_config_value(runtime, "GENERAL", key, packed.data(), packed.size() - 1);
    std::vector<std::string> verified;
    Require(ReadArray(runtime, key, verified) && verified == after, ERROR_WRITE_FAULT);
    return true;
}
}
}

#if defined(ETS2_DLAA_EMBEDDED_ASSETS_CPU_TEST)
// Explicit CPU/file-only fixture. No EnsureAssets/ReShade API or graphics calls.
// Supply a NEW absolute directory whose parent already exists; evidence remains.
#include <cstdio>
int main(int argc, char **argv) {
    using namespace ets2_dlaa_assets;
    unsigned checks = 0;
    const auto check = [&checks](bool good) { ++checks; Require(good, ERROR_INVALID_DATA); };
    try {
        Require(argc == 2, ERROR_INVALID_PARAMETER);
        const fs::path base = PathUtf8(argv[1]);
        Require(base.is_absolute() && !fs::exists(base), ERROR_ALREADY_EXISTS);
        Require(CreateDirectoryW(base.c_str(), nullptr) != FALSE, LastError());
        std::array<Asset, 5> assets{{
            {45101, "ETS2_DLAA.addonfx"}, {45102, "ETS2_VortStereo.addonfx"},
            {45103, "ETS2_VortStereo_Eye.fxh"}, {45104, "ReShade.fxh"}, {45105, "vort_BlueNoise.png"}
        }};
        const std::string hash = LoadAndHash(GetModuleHandleW(nullptr), assets);
        check(hash.size() == 64);
        check(hash == LoadAndHash(GetModuleHandleW(nullptr), assets));
        const fs::path root = base / L"ETS2-DLAA" / L"assets";
        const fs::path directory = root / PathUtf8("v1-" + hash);
        MakeOwnedDirectory(directory);
        for (const auto &asset : assets) {
            const fs::path path = directory / PathUtf8(asset.name);
            check(ExtractFile(path, asset));
            check(fs::file_size(path) == asset.size);
            const auto timestamp = fs::last_write_time(path);
            check(!ExtractFile(path, asset));
            check(timestamp == fs::last_write_time(path));
        }
        const std::string current = Utf8(directory);
        const std::string old = Utf8(root / PathUtf8("v1-" + std::string(64, 'a')));
        const std::vector<std::string> before{".\\reshade-shaders\\Shaders\\**", old, "C:\\custom,comma\\Shaders", ".\\"};
        const auto after = UpdatedPaths(before, root, current);
        check(after == std::vector<std::string>({current, before[0], before[2], before[3]}));
        check(UpdatedPaths(after, root, current) == after);
        check(UpdatedPaths({".\\"}, root, current) == std::vector<std::string>({current, ".\\"}));
        check(!IsOwnedVersion(Utf8(root / L"v1-user-files"), root));
        check(!IsOwnedVersion(Utf8(base / PathUtf8("v1-" + hash)), root));
        const auto packed = PackArray(after);
        std::vector<char> returned(packed.begin(), packed.end());
        returned.push_back('\0');
        std::vector<std::string> decoded;
        DecodeArray(returned, packed.size(), decoded);
        check(decoded == after); // Includes comma without INI-serialization loss.
        bool refused = false;
        try { std::vector<std::string> out; DecodeArray({'x', '\0'}, 1, out); }
        catch (const Failure &) { refused = true; }
        check(refused);
        refused = false;
        try { (void)UpdatedPaths(std::vector<std::string>(MaxPaths, "other"), root, current); }
        catch (const Failure &) { refused = true; }
        check(refused);
        check(NeedsReload(false, 0, false, false));
        check(NeedsReload(false, 0, true, false));
        check(NeedsReload(false, 0, false, true));
        check(!NeedsReload(false, 0, true, true));
        check(NeedsReload(true, 0, true, true));
        check(NeedsReload(false, 1, true, true));
        const fs::path edited = directory / PathUtf8(assets[0].name);
        {
            File file;
            file.value = CreateFileW(edited.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Require(file.value != INVALID_HANDLE_VALUE, LastError());
            const BYTE changed = static_cast<BYTE>(assets[0].bytes[0] ^ 0xff);
            DWORD written = 0;
            Require(WriteFile(file.value, &changed, 1, &written, nullptr) && written == 1, ERROR_WRITE_FAULT);
        }
        refused = false;
        try { (void)ExtractFile(edited, assets[0]); }
        catch (const Failure &failure) { refused = failure.error == ERROR_CRC; }
        check(refused);
        for (size_t i = 1; i < assets.size(); ++i) {
            VerifyFile(directory / PathUtf8(assets[i].name), assets[i]);
            check(true);
        }
        std::printf("PASS %u CPU/file checks; no ReShade API, DLL load, device, or GPU calls.\n", checks);
        return 0;
    } catch (const Failure &failure) {
        std::printf("FAIL after %u checks, Win32 %lu\n", checks, failure.error);
    } catch (...) {
        std::printf("FAIL after %u checks, unexpected exception\n", checks);
    }
    return 1;
}
#endif

namespace ets2_dlaa_assets {
Result EnsureAssets(HMODULE module, reshade::api::effect_runtime *runtime) noexcept {
    Result result;
    try {
        static std::mutex mutex;
        const std::lock_guard<std::mutex> lock(mutex);
        result.stage = "arguments";
        Require(module != nullptr && runtime != nullptr, ERROR_INVALID_PARAMETER);
        const HMODULE reshadeModule = reshade::internal::get_reshade_module_handle();
        Require(reshadeModule && GetProcAddress(reshadeModule, "ReShadeGetConfigValue") &&
            GetProcAddress(reshadeModule, "ReShadeSetConfigArray"), ERROR_PROC_NOT_FOUND);
        result.stage = "embedded-resources";
        std::array<Asset, 5> assets{{
            {45101, "ETS2_DLAA.addonfx"}, {45102, "ETS2_VortStereo.addonfx"},
            {45103, "ETS2_VortStereo_Eye.fxh"}, {45104, "ReShade.fxh"}, {45105, "vort_BlueNoise.png"}
        }};
        const std::string version = "v1-" + LoadAndHash(module, assets);
        result.stage = "asset-directory";
        const fs::path root = AssetRoot();
        const fs::path directory = root / PathUtf8(version);
        result.directoryUtf8 = Utf8(directory);
        MakeOwnedDirectory(directory);
        result.stage = "asset-files";
        for (const auto &asset : assets) if (ExtractFile(directory / PathUtf8(asset.name), asset)) ++result.filesCreated;
        // v6.8 source/addon.cpp ignores the addon-module argument and addresses
        // the supplied runtime INI. Runtime::load_config falls back to global
        // GENERAL entries; mirror that fallback before preserving the array.
        result.stage = "effect-search-path";
        result.pathsChanged = InstallPath(runtime, "EffectSearchPaths", root, result.directoryUtf8);
        result.stage = "texture-search-path";
        result.pathsChanged = InstallPath(runtime, "TextureSearchPaths", root, result.directoryUtf8) || result.pathsChanged;
        // Existing cache/path entries do not establish discovery in this newly
        // initialized runtime (notably with NoReloadOnInit=1). The caller calls
        // EnsureAssets once per init, so this queues at most one discovery here.
        const bool hasDlaa = runtime->find_technique("ETS2_DLAA.addonfx", "ETS2_DLAA").handle != 0;
        const bool hasMotion = runtime->find_technique("ETS2_VortStereo.addonfx", "ETS2_VortStereo").handle != 0;
        if (NeedsReload(result.pathsChanged, result.filesCreated, hasDlaa, hasMotion)) {
            result.stage = "reload-request";
            // Named reload ignores effects not yet discovered in v6.8.
            runtime->reload_effect_next_frame(nullptr);
            result.reloadQueued = true;
        }
        result.stage = "ready";
        result.success = true;
    } catch (const Failure &failure) {
        result.win32Error = failure.error;
    } catch (const std::bad_alloc &) {
        result.win32Error = ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        result.win32Error = ERROR_GEN_FAILURE;
    }
    return result;
}
}
