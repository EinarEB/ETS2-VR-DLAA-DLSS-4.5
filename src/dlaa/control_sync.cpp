// SPDX-License-Identifier: MIT
#include "control_sync.h"
#include "settings_overlay.h"
#include <reshade.hpp>
#include <mutex>

namespace ets2_dlaa_controls {
namespace {
using reshade::api::effect_runtime;
constexpr const char* Effect="ETS2_DLAA.addonfx";
struct Shared {std::mutex mutex;Diagnostics value;};
Shared& State(){static Shared* state=new Shared;return *state;}
std::uintptr_t Identity(effect_runtime* runtime){return reinterpret_cast<std::uintptr_t>(runtime);}
void DesktopStage(Stage stage){auto& s=State();std::lock_guard<std::mutex> lock(s.mutex);s.value.desktopStage=stage;}
void VrStage(effect_runtime* runtime,Stage stage,std::uint64_t applied=0){
    auto& s=State();std::lock_guard<std::mutex> lock(s.mutex);
    s.value.lastVrRuntime=Identity(runtime);s.value.vrStage=stage;
    if(stage==Stage::Applied)s.value.lastAppliedRevision=applied;
}
}
Diagnostics Inspect() noexcept {
    auto& s=State();std::lock_guard<std::mutex> lock(s.mutex);return s.value;
}
void ObserveDesktop(effect_runtime* runtime) noexcept {
    try {
        if(!runtime||!runtime->get_hwnd())return;
        const bool allowed=runtime->get_effects_state();
        auto& s=State();std::lock_guard<std::mutex> lock(s.mutex);auto& value=s.value;
        if(!value.desktopObserved||value.desktopRuntime!=Identity(runtime)||
           value.desktopEffectsAllowed!=allowed){
            ++value.revision;value.desktopObserved=true;value.desktopRuntime=Identity(runtime);
            value.desktopEffectsAllowed=allowed;
        }
        value.desktopStage=Stage::Observed;
    }catch(...){DesktopStage(Stage::ApiException);}
}
bool ApplyVr(effect_runtime* runtime) noexcept {
    try {
        if(!runtime||runtime->get_hwnd())return false;
        const auto pending=Inspect();
        const auto settings=ets2_dlaa_settings::Read();
        const bool valid=settings.initialized&&settings.valid&&settings.requestedPreset>=11&&settings.requestedPreset<=13;
        const auto technique=runtime->find_technique(Effect,"ETS2_DLAA");
        const auto uniform=runtime->find_uniform_variable(Effect,"ETS2_DLAA_PRESET");
        if(!technique.handle||!uniform.handle){
            VrStage(runtime,Stage::ControlsUnavailable);return false;
        }
        int index=-1;runtime->get_uniform_value_int(uniform,&index,1);
        const int requested=valid?13-settings.requestedPreset:0;
        const bool enabled=valid&&settings.enabled;
        // Setters run without the shared mutex: ReShade may synchronously notify
        // its built-in runtime sync. Equality checks avoid repeated feedback.
        if(index!=requested)runtime->set_uniform_value_int(uniform,&requested,1);
        if(runtime->get_technique_state(technique)!=enabled)
            runtime->set_technique_state(technique,enabled);
        runtime->get_uniform_value_int(uniform,&index,1);
        if(index!=requested||runtime->get_technique_state(technique)!=enabled){
            VrStage(runtime,Stage::ApplyRejected);return false;
        }
        {
            auto& shared=State();std::lock_guard<std::mutex> lock(shared.mutex);
            shared.value.techniqueEnabled=enabled;shared.value.requestedPreset=valid?settings.requestedPreset:13;
        }
        VrStage(runtime,valid?Stage::Applied:Stage::InvalidPreset,settings.revision);
        return valid&&pending.desktopEffectsAllowed;
    }catch(...){VrStage(runtime,Stage::ApiException);return false;}
}
void ForgetRuntime(effect_runtime* runtime) noexcept {
    auto& s=State();std::lock_guard<std::mutex> lock(s.mutex);auto& value=s.value;
    if(value.desktopObserved&&value.desktopRuntime==Identity(runtime)){
        const auto revision=value.revision+1;
        value.desktopObserved=false;value.desktopRuntime=0;
        value.desktopEffectsAllowed=true;value.revision=revision;
        value.desktopStage=Stage::Waiting;
    }
    if(value.lastVrRuntime==Identity(runtime)){
        value.lastVrRuntime=0;value.lastAppliedRevision=0;value.vrStage=Stage::Waiting;
    }
}
}
