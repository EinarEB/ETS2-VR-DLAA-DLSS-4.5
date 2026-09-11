// Copyright (c) 2026 ETS2 VR preview contributors. SPDX-License-Identifier: MIT
#pragma once

namespace ets2_post_sr_model {
// NVIDIA DLSS SDK 310.9.1, nvsdk_ngx_defs.h at
// 374959484e79a640feaba44c93ac8cfb0a03f5b5: K=11, L=12, M=13. Legacy emits no hint.
enum class Preset : unsigned { Legacy=0, K=11, L=12, M=13 };
enum class CreationPolicy : unsigned { Legacy=0, QueriedOptimal=1, FixedInputMaximum=2 };
struct Request {
    Preset preset=Preset::Legacy;
    CreationPolicy creationPolicy=CreationPolicy::Legacy;
};
inline constexpr int EffectivePresetUnknown=-1;

inline constexpr bool ValidPresetForMode(Preset preset,int quality) noexcept {
    // Preserve the existing M Quality/DLAA paths. The native DLAA selector adds
    // K/L only for mode 5; these hints never change the selected quality mode.
    return preset==Preset::Legacy || (preset==Preset::M&&(quality==2||quality==5)) ||
           ((preset==Preset::K||preset==Preset::L)&&quality==5);
}
inline constexpr bool ValidRequest(Request request,int quality) noexcept {
    if(quality<-1||quality>5||!ValidPresetForMode(request.preset,quality))return false;
    switch(request.creationPolicy){
    case CreationPolicy::Legacy:return request.preset==Preset::Legacy;
    case CreationPolicy::QueriedOptimal:
    case CreationPolicy::FixedInputMaximum:return quality>=0;
    default:return false;
    }
}
inline constexpr const char* HintKey(Preset preset,int quality) noexcept {
    if(preset==Preset::Legacy||!ValidPresetForMode(preset,quality))return nullptr;
    return quality==2?"DLSS.Hint.Render.Preset.Quality":
           quality==5?"DLSS.Hint.Render.Preset.DLAA":nullptr;
}
// Caller contains driver exceptions. A successful Set only records a request;
// neither it nor a subsequent Get establishes the model that actually runs.
template<class Parameters> inline bool ApplyHint(Parameters* parameters,Preset preset,int quality) {
    if(!parameters||!ValidPresetForMode(preset,quality))return false;
    if(preset==Preset::Legacy)return true;
    parameters->Set(HintKey(preset,quality),static_cast<unsigned>(preset));
    return true;
}
// The fixed-input policy uses this owner's immutable actual input extent.
// It is an explicit experiment for the guide's L/M allocation caveat, not proof
// that the conflicting queried-optimum and maximum-input rules are reconciled.
inline constexpr bool UsesFixedInputMaximum(CreationPolicy policy) noexcept {
    return policy==CreationPolicy::FixedInputMaximum;
}
}
