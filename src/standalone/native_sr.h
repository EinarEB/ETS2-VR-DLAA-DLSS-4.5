// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <string>

namespace ets2_native {
struct EvaluationReceipt {
    std::uint64_t handles[2]{};
    unsigned calls=0;
    bool success=false; // Owner API result; Evaluate() must also succeed for retired output.
    bool requested_reset=false;
    bool applied_reset=false;
    std::uint64_t history_generation=0;
    std::uint64_t evaluation_ordinal=0; // Successful, retired evaluations in this owner generation.
};
// Native-resolution, two-eye DLAA only. Input textures are owned by the caller,
// SBS, one mip/sample/layer: RGBA16F color, R32F inverted depth, RG16F motion in
// render-pixel units. The immediate context must belong to the supplied device.
// The implementation owns its D3D12 device/queue, both feature histories, shared
// inputs/outputs and synchronization. No ReShade or game-mod runtime is used.
class NativeSr {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativeSr();
    ~NativeSr();
    NativeSr(const NativeSr&)=delete;
    NativeSr& operator=(const NativeSr&)=delete;
    // Per-feature request only: M=13, L=12, K=11; the effective preset is unknown.
    // Quality stays DLAA (5), with native input/output dimensions for every preset.
    HRESULT Initialize(ID3D11Device* device, unsigned eyeWidth, unsigned height,
                       const std::wstring& dataDirectory, int preset=13);
    // Call at a serialized frame boundary, after stopping new Output() use.
    // Retires prior D3D11 consumption and D3D12 work, then recreates fresh eye
    // histories on the existing initialized core/device. No stale output or
    // evaluation receipt survives this call, including a rejected/failed change.
    HRESULT ChangePreset(int preset);
    HRESULT Evaluate(ID3D11DeviceContext* context, ID3D11Texture2D* color,
                     ID3D11Texture2D* depth, ID3D11Texture2D* motion, bool reset);
    ID3D11ShaderResourceView* Output(unsigned eye) const;
    EvaluationReceipt LastEvaluation() const;
    // Opt-in diagnostic of the actual retired NGX input planes. The caller owns
    // the per-frame directory/identity and must serialize this with Output use.
    HRESULT CaptureLastInputs(const std::wstring& directory);
    // Wait for recorded work and D3D11 output consumption before releasing.
    // False means dependencies must remain retained until process teardown.
    bool Shutdown();
    const char* Reason() const;
};
}
