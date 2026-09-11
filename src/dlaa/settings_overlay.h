// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace ets2_dlaa_settings {
enum class Stage : unsigned {
    Waiting, Ready, InvalidSavedSettings, ConfigUnavailable, SaveRejected, ApiException
};
struct Snapshot {
    bool initialized = false;
    bool valid = false;
    bool enabled = false;
    int requestedPreset = 13; // Requested M=13, L=12, K=11; not effective-model proof.
    std::uint64_t revision = 0;
    Stage stage = Stage::Waiting;
};

// Register after register_addon; unregister before unregister_addon. These only
// bind the supported ImGui table/register the Add-ons settings callback, and do
// not initialize settings or perform file/graphics work under DllMain.
bool Register() noexcept;
void Unregister() noexcept;

// Call outside DllMain, before the first runtime consumes Read(). Reads once
// from the global ReShade configuration, owned [ETS2_DLAA] section. Missing
// keys mean OFF/M; malformed values fail closed. Never reads a Home preset.
void Initialize() noexcept;

// Value-only snapshot, safe on the VR callback thread. The rendering owner
// applies it on its own thread; this module never modifies a runtime resource,
// technique, uniform, effect order, or global effects switch.
Snapshot Read() noexcept;
}
