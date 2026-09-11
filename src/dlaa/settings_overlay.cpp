// SPDX-License-Identifier: MIT
#include "settings_overlay.h"
#include <windows.h>
#include <imgui.h>
#include <reshade.hpp>
#include <atomic>
#include <charconv>
#include <cstring>
#include <mutex>
#include <string_view>
#include <system_error>

namespace ets2_dlaa_settings {
namespace {
constexpr const char* Section = "ETS2_DLAA";
constexpr const char* EnabledKey = "Enabled";
constexpr const char* PresetKey = "ModelPreset";
std::atomic<bool> registered{false};
struct Shared {
    std::mutex valuesMutex;
    std::mutex configMutex;
    Snapshot value;
};
Shared& State() {
    // Created only by Initialize/Read/the UI, never by Register in DllMain.
    // Retained until process exit so late runtime teardown cannot race a static
    // mutex destructor. No graphics objects or runtime pointers are retained.
    static Shared* state = new Shared;
    return *state;
}
bool ValidPreset(int preset) noexcept { return preset >= 11 && preset <= 13; }
bool Space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool ConfigAvailable() noexcept {
    const HMODULE module = reshade::internal::get_reshade_module_handle();
    return module && GetProcAddress(module, "ReShadeGetConfigValue") &&
        GetProcAddress(module, "ReShadeSetConfigValue");
}

enum class ReadResult { Missing, Valid, Invalid };
ReadResult ReadInteger(const char* key, int& result) {
    size_t required = 0;
    if (!reshade::get_config_value(nullptr, Section, key, nullptr, &required))
        return ReadResult::Missing;
    char buffer[32] = {};
    if (required < 2 || required > sizeof(buffer)) return ReadResult::Invalid;
    size_t written = required;
    if (!reshade::get_config_value(nullptr, Section, key, buffer, &written) ||
        written != required - 1) return ReadResult::Invalid;
    // The config API returns a double-NUL-terminated array. Accept exactly one
    // decimal integer element, rejecting extra elements and truncated values.
    const size_t length = std::strlen(buffer);
    for (size_t i = length; i < required; ++i)
        if (buffer[i] != '\0') return ReadResult::Invalid;
    std::string_view token(buffer, length);
    while (!token.empty() && Space(token.front())) token.remove_prefix(1);
    while (!token.empty() && Space(token.back())) token.remove_suffix(1);
    if (token.empty()) return ReadResult::Invalid;
    int parsed = 0;
    const auto conversion = std::from_chars(token.data(), token.data() + token.size(), parsed, 10);
    if (conversion.ec != std::errc{} || conversion.ptr != token.data() + token.size())
        return ReadResult::Invalid;
    result = parsed;
    return ReadResult::Valid;
}

void Publish(bool valid, bool enabled, int preset, Stage stage) {
    auto& state = State();
    const std::lock_guard<std::mutex> lock(state.valuesMutex);
    auto& value = state.value;
    value.initialized = true;
    value.valid = valid;
    value.enabled = valid && enabled;
    value.requestedPreset = valid ? preset : 13;
    ++value.revision;
    value.stage = stage;
}

void Save(bool enabled, int preset) noexcept {
    try {
        auto& state = State();
        const std::lock_guard<std::mutex> serial(state.configMutex);
        if (!ConfigAvailable()) {
            Publish(false, false, 13, Stage::ConfigUnavailable);
            return;
        }
        if (!ValidPreset(preset)) {
            Publish(false, false, 13, Stage::SaveRejected);
            return;
        }
        // A nullptr runtime addresses ReShade's global config, not ReShadeVR.ini
        // or a Home effect preset. Only these two owned keys are changed. The
        // global config has no runtime load_config callback; do not hold the
        // snapshot mutex across these calls. ReShade owns eventual disk flush.
        reshade::set_config_value(nullptr, Section, EnabledKey, enabled ? "1" : "0");
        reshade::set_config_value(nullptr, Section, PresetKey,
            preset == 13 ? "13" : preset == 12 ? "12" : "11");
        int readEnabled = -1, readPreset = -1;
        const bool accepted = ReadInteger(EnabledKey, readEnabled) == ReadResult::Valid &&
            ReadInteger(PresetKey, readPreset) == ReadResult::Valid &&
            readEnabled == (enabled ? 1 : 0) && readPreset == preset;
        Publish(accepted, enabled, preset, accepted ? Stage::Ready : Stage::SaveRejected);
    } catch (...) {
        try { Publish(false, false, 13, Stage::ApiException); } catch (...) {}
    }
}

void Draw(reshade::api::effect_runtime* runtime) {
    if (!runtime || !runtime->get_hwnd()) return;
    Initialize();
    const auto selected = Read();
    ImGui::TextUnformatted("Native-resolution DLAA");
    ImGui::TextDisabled("DLSS library 310.9.1.0");
    bool enabled = selected.enabled;
    int index = selected.valid && ValidPreset(selected.requestedPreset) ? 13 - selected.requestedPreset : -1;
    bool changed = ImGui::Checkbox("Enable DLAA", &enabled);
    changed = ImGui::Combo("Model preset", &index,
        "M (DLSS 4.5)\0L (DLSS 4.5)\0K (DLSS 4)\0") || changed;
    if (changed) Save(enabled, index >= 0 ? 13 - index : 13);
    ImGui::TextWrapped("Changes apply to VR automatically and reset DLAA history.");
    if (!selected.valid) {
        ImGui::TextWrapped(selected.stage == Stage::InvalidSavedSettings ?
            "Saved DLAA settings are invalid. Processing is off. Select a model or change the checkbox to replace them." :
            "DLAA settings are unavailable or could not be saved. Processing is off.");
    }
}
}

Snapshot Read() noexcept {
    try {
        auto& state = State();
        const std::lock_guard<std::mutex> lock(state.valuesMutex);
        return state.value;
    } catch (...) {
        Snapshot failed;
        failed.stage = Stage::ApiException;
        return failed;
    }
}

void Initialize() noexcept {
    try {
        auto& state = State();
        const std::lock_guard<std::mutex> serial(state.configMutex);
        if (Read().initialized) return;
        if (!ConfigAvailable()) {
            Publish(false, false, 13, Stage::ConfigUnavailable);
            return;
        }
        int enabled = 0, preset = 13;
        const auto enabledResult = ReadInteger(EnabledKey, enabled);
        const auto presetResult = ReadInteger(PresetKey, preset);
        const bool valid = enabledResult != ReadResult::Invalid &&
            presetResult != ReadResult::Invalid && (enabled == 0 || enabled == 1) && ValidPreset(preset);
        Publish(valid, enabled == 1, preset, valid ? Stage::Ready : Stage::InvalidSavedSettings);
    } catch (...) {
        try { Publish(false, false, 13, Stage::ApiException); } catch (...) {}
    }
}

bool Register() noexcept {
    try {
        if (registered.load()) return true;
        const HMODULE module = reshade::internal::get_reshade_module_handle();
        if (!module) return false;
        using GetTable = const imgui_function_table* (*)(std::uint32_t);
        using AddOverlay = void (*)(const char*, void (*)(reshade::api::effect_runtime*));
        const auto getTable = reinterpret_cast<GetTable>(GetProcAddress(module, "ReShadeGetImGuiFunctionTable"));
        const auto addOverlay = reinterpret_cast<AddOverlay>(GetProcAddress(module, "ReShadeRegisterOverlay"));
        if (!getTable || !addOverlay || !GetProcAddress(module, "ReShadeUnregisterOverlay")) return false;
        const auto table = getTable(IMGUI_VERSION_NUM);
        if (!table) return false;
        imgui_function_table_instance() = table;
        addOverlay(nullptr, Draw);
        registered.store(true);
        return true;
    } catch (...) { return false; }
}

void Unregister() noexcept {
    try {
        if (!registered.exchange(false)) return;
        const HMODULE module = reshade::internal::get_reshade_module_handle();
        using RemoveOverlay = void (*)(const char*, void (*)(reshade::api::effect_runtime*));
        const auto removeOverlay = module ?
            reinterpret_cast<RemoveOverlay>(GetProcAddress(module, "ReShadeUnregisterOverlay")) : nullptr;
        if (removeOverlay) removeOverlay(nullptr, Draw);
    } catch (...) {}
}
}
