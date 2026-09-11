// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace reshade::api { struct effect_runtime; }
namespace ets2_dlaa_controls {
enum class Stage : unsigned { Waiting, Observed, Applied, ControlsUnavailable, InvalidPreset, ApplyRejected, ApiException };
struct Diagnostics {
    bool desktopObserved=false;
    bool techniqueEnabled=false;
    bool desktopEffectsAllowed=true;
    int requestedPreset=13; // Requested M=13, L=12, K=11; no effective-model claim.
    std::uintptr_t desktopRuntime=0,lastVrRuntime=0;
    std::uint64_t revision=0,lastAppliedRevision=0;
    Stage desktopStage=Stage::Waiting,vrStage=Stage::Waiting;
};
// Observe only ReShade's desktop global-effects switch on its callback thread.
// Add-on settings are owned separately; Home/preset values are never authority.
void ObserveDesktop(reshade::api::effect_runtime* runtime) noexcept;
// Call only on the live VR runtime's callback thread, before reading its DLAA
// controls. Applies the settings panel's values to the hidden effect. Never changes
// any global effects switch or another technique. The return value separately
// gates DLAA processing by the last observed desktop global-effects setting;
// it is true before a desktop authority exists, and false on API exceptions.
// Invalid or uninitialized add-on settings disable the owned technique.
bool ApplyVr(reshade::api::effect_runtime* runtime) noexcept;
// Call from destroy_effect_runtime, after that runtime's callbacks are quiescent.
// No API calls or retained ownership of the supplied runtime are involved.
void ForgetRuntime(reshade::api::effect_runtime* runtime) noexcept;
Diagnostics Inspect() noexcept;
}
