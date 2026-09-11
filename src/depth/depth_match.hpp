#pragma once
// Private D3D11 experiment: full-pixel exact color correspondence, no eye-order inference.
// RGBA8 full-rect, equal eyes, single-sample D16/D24/D32/D32S8 depth. All native objects are owned.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include "feed_d3d11_context.h"
#ifdef ETS2_DEPTH_WAIT_PROFILE
#include "depth_wait_profile.hpp"
#endif
namespace depth_match
{
using Microsoft::WRL::ComPtr;
inline void Check(HRESULT h, const char* n)
{
    if (FAILED(h))
    {
        char b[160];
        sprintf_s(b, "%s:0x%08lX", n, (unsigned long)h);
        throw std::runtime_error(b);
    }
}
#ifndef DEPTH_MATCH_MAX_CANDIDATES
#define DEPTH_MATCH_MAX_CANDIDATES 8
#endif
constexpr unsigned Capacity = DEPTH_MATCH_MAX_CANDIDATES;
static_assert(Capacity >= 2 && Capacity <= 16, "Keep the proof pool bounded; overflow rejects.");
inline DXGI_FORMAT DepthStorage(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: // depth routed through the game's fullscreen UV transforms
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
inline DXGI_FORMAT DepthViewFormat(DXGI_FORMAT f)
{
    switch (DepthStorage(f))
    {
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
struct Candidate
{
    uint64_t epoch = 0, serial = 0;
    uint64_t identity = 0; // native color resource and subresource; stable while the game keeps its targets
};
struct Result
{
    bool accepted = false;
    int eye[2] = {-1, -1};
    unsigned matches[2] = {0, 0};
    std::vector<uint32_t> mismatch;
    const char* reason = "not matched";
    bool synchronous = true; // false: assigned by resource identity from an earlier exact proof
    unsigned proofAge = 0;   // frames between that proof and this frame (0 when synchronous)
};
class Matcher
{
    unsigned w, h;
    DXGI_FORMAT depthStorage;
    ComPtr<ID3D11Device> d;
    ComPtr<ID3D11DeviceContext> c;
    ComPtr<ID3D11Texture2D> colors, depths, sbs, output;
    ComPtr<ID3D11ShaderResourceView> colorsView, sbsView, outputView, depthsView;
    ComPtr<ID3D11Buffer> scores, constants;
    ComPtr<ID3D11UnorderedAccessView> scoresView, outputUav;
    ComPtr<ID3D11ComputeShader> compare, assemble;
    ets2_d3d11::PrivateState privateState;
    std::vector<Candidate> candidates;
    bool valid = false, overflow = false;
    // Every frame still runs the exact compare. Earlier 64-byte verdicts are
    // polled without waiting, but reusing the oldest ring slot may block. Current
    // assembly may reuse a proof for the same resources up to three epochs old.
    // A failed proof revokes the assignment; fresh proof recovery is synchronous.
    static constexpr unsigned kRing = 3;
    struct Proof
    {
        bool pending = false;
        uint64_t epoch = 0;
        unsigned count = 0;
        std::array<uint64_t, Capacity> identity{}, candidateEpoch{};
    };
    struct Verified
    {
        bool valid = false;
        uint64_t epoch = 0;
        unsigned count = 0;
        int index[2] = {-1, -1};
        uint64_t identity[2] = {0, 0};
        std::array<uint64_t, Capacity> identities{};
        const char* reason = "no proof yet";
    };
    ComPtr<ID3D11Buffer> readbacks[kRing];
    Proof proofs[kRing];
    unsigned ringNext = 0;
    Verified verified;
    uint64_t lastAcceptedEpoch = 0, lastMatchEpoch = 0;
    uint64_t outputIdentity[2] = {0, 0};
    bool outputVerified = false;
    static bool ColorFamily(DXGI_FORMAT f)
    {
        return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
               f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }

  public:
    Matcher(ID3D11Device* device, ID3D11DeviceContext* context, unsigned width, unsigned height,
            DXGI_FORMAT sourceDepth = DXGI_FORMAT_D32_FLOAT)
        : w(width), h(height), depthStorage(DepthStorage(sourceDepth)), d(device), c(context)
    {
        if (depthStorage == DXGI_FORMAT_UNKNOWN)
            throw std::runtime_error("unsupported native depth family");
        auto make =
            [&](unsigned width_, unsigned count, DXGI_FORMAT fmt, unsigned bind, ID3D11Texture2D** out)
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = width_;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = count;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.BindFlags = bind;
            Check(d->CreateTexture2D(&td, nullptr, out), "depth matcher texture");
        };
        make(w, Capacity, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &colors);
        make(w, Capacity, depthStorage, D3D11_BIND_SHADER_RESOURCE, &depths);
        make(2 * w, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &sbs);
        make(2 * w, 1, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
             &output);
        Check(d->CreateShaderResourceView(colors.Get(), nullptr, &colorsView), "candidate color view");
        Check(d->CreateShaderResourceView(sbs.Get(), nullptr, &sbsView), "SBS view");
        Check(d->CreateShaderResourceView(output.Get(), nullptr, &outputView), "combined depth view");
        D3D11_SHADER_RESOURCE_VIEW_DESC depthSrv{};
        depthSrv.Format = DepthViewFormat(sourceDepth);
        depthSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        depthSrv.Texture2DArray.MipLevels = 1;
        depthSrv.Texture2DArray.ArraySize = Capacity;
        Check(d->CreateShaderResourceView(depths.Get(), &depthSrv, &depthsView), "native depth-only view");
        Check(d->CreateUnorderedAccessView(output.Get(), nullptr, &outputUav), "R32 assembled depth UAV");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = Capacity * 2 * sizeof(uint32_t);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        Check(d->CreateBuffer(&bd, nullptr, &scores), "scores");
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uv.Buffer.NumElements = Capacity * 2;
        Check(d->CreateUnorderedAccessView(scores.Get(), &uv, &scoresView), "scores view");
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.MiscFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& readback : readbacks)
            Check(d->CreateBuffer(&bd, nullptr, &readback), "scores readback");
        bd = {};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(d->CreateBuffer(&bd, nullptr, &constants), "match constants");
#ifdef DEPTH_MATCH_LEGACY_COMPARE
        const char* shader = R"(
cbuffer Settings:register(b0){uint width,height,count,pad;};
Texture2DArray<float4> candidate:register(t0);
Texture2D<float4> combined:register(t1);
RWStructuredBuffer<uint> mismatches:register(u0);
groupshared uint groupMismatch;
[numthreads(16,16,1)] void main(uint3 id:SV_DispatchThreadID,uint lane:SV_GroupIndex){
 if(lane==0)groupMismatch=0;
 GroupMemoryBarrierWithGroupSync();
 if(id.x<width&&id.y<height&&id.z<2*count){
 uint index=id.z/2,eye=id.z%2;
 // Both owned views are UNORM, even when original RTVs were sRGB.
 uint4 a=(uint4)round(candidate.Load(int4(id.xy,index,0))*255.0);
 uint4 b=(uint4)round(combined.Load(int3(id.x+eye*width,id.y,0))*255.0);
 if(any(a!=b))InterlockedOr(groupMismatch,1);
 }
 GroupMemoryBarrierWithGroupSync();
 // Exact rejection flags, with at most one global atomic per 256-pixel group.
 if(lane==0&&groupMismatch!=0)InterlockedOr(mismatches[id.z],1);
})";
#else
        // Read each combined-eye pixel once, and each candidate once, while
        // preserving the exact all-pixel RGBA rejection flags for every pair.
        const char* shader=R"(
cbuffer Settings:register(b0){uint width,height,count,pad;};
Texture2DArray<float4> candidate:register(t0);
Texture2D<float4> combined:register(t1);
RWStructuredBuffer<uint> mismatches:register(u0);
groupshared uint groupMask;
[numthreads(16,16,1)] void main(uint3 id:SV_DispatchThreadID,uint lane:SV_GroupIndex){
 if(lane==0)groupMask=0;
 GroupMemoryBarrierWithGroupSync();
 if(id.x<width&&id.y<height){
  uint4 left=(uint4)round(combined.Load(int3(id.xy,0))*255.0);
  uint4 right=(uint4)round(combined.Load(int3(id.x+width,id.y,0))*255.0);
  uint flags=0;
  [loop]for(uint i=0;i<count;++i){
   uint4 a=(uint4)round(candidate.Load(int4(id.xy,i,0))*255.0);
   if(any(a!=left))flags|=1u<<(i*2);
   if(any(a!=right))flags|=1u<<(i*2+1);
  }
  if(flags)InterlockedOr(groupMask,flags);
 }
 GroupMemoryBarrierWithGroupSync();
 if(lane==0){[loop]for(uint i=0;i<count*2;++i)if(groupMask&(1u<<i))InterlockedOr(mismatches[i],1);}
})";
#endif
        ComPtr<ID3DBlob> b, e;
        HRESULT hr = D3DCompile(shader, strlen(shader), "exact-depth-color-match", nullptr, nullptr, "main",
                                "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e);
        if (FAILED(hr) && e)
            throw std::runtime_error(static_cast<const char*>(e->GetBufferPointer()));
        Check(hr, "compare compile");
        Check(d->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &compare),
              "compare shader");
        const char* assembly = R"(
cbuffer Settings:register(b0){uint width,height,leftIndex,rightIndex;};
Texture2DArray<float> rawDepth:register(t0);RWTexture2D<float> outputDepth:register(u0);
[numthreads(16,16,1)] void main(uint3 id:SV_DispatchThreadID){if(id.x>=2*width||id.y>=height)return;uint eye=id.x/width;uint layer=eye==0?leftIndex:rightIndex;outputDepth[id.xy]=rawDepth.Load(int4(id.x%width,id.y,layer,0));}
)";
        b.Reset();
        e.Reset();
        hr = D3DCompile(assembly, strlen(assembly), "assemble-native-depth", nullptr, nullptr, "main",
                        "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e);
        if (FAILED(hr) && e)
            throw std::runtime_error(static_cast<const char*>(e->GetBufferPointer()));
        Check(hr, "assembly compile");
        Check(d->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &assemble),
              "assembly shader");
    }
    void Reset()
    {
        candidates.clear();
        valid = false;
        overflow = false;
    }
    unsigned Count() const
    {
        return static_cast<unsigned>(candidates.size());
    }
    // `identity` names the resource whose role is expected to stay fixed across
    // frames (the scene target the depth was snapshotted from). The final color
    // target itself rotates through the OpenXR swapchain, so it is the fallback
    // only when the caller has nothing better.
    bool Capture(ID3D11Texture2D* color, unsigned colorSubresource, ID3D11Texture2D* depth,
                 unsigned depthSubresource, uint64_t epoch, uint64_t serial, uint64_t identity = 0)
    {
        if (candidates.size() >= Capacity)
        {
            overflow = true;
            return false;
        }
        if (!color || !depth)
            return false;
        D3D11_TEXTURE2D_DESC a{}, b{};
        color->GetDesc(&a);
        depth->GetDesc(&b);
        if (a.Width != w || a.Height != h || b.Width != w || b.Height != h || a.MipLevels != 1 ||
            b.MipLevels != 1 || a.SampleDesc.Count != 1 || b.SampleDesc.Count != 1 ||
            !ColorFamily(a.Format) || DepthStorage(b.Format) != depthStorage ||
            colorSubresource >= a.ArraySize || depthSubresource >= b.ArraySize)
            return false;
        const unsigned layer = Count();
        ets2_d3d11::Scope isolated(privateState,c.Get());
        if(!isolated)return false;
        // Full-subresource copies also preserve depth that will immediately be cleared/reused.
        c->CopySubresourceRegion(colors.Get(), layer, 0, 0, 0, color, colorSubresource, nullptr);
        c->CopySubresourceRegion(depths.Get(), layer, 0, 0, 0, depth, depthSubresource, nullptr);
        candidates.push_back({epoch, serial, identity ? identity : reinterpret_cast<uint64_t>(color) ^ (uint64_t(colorSubresource) << 56)});
        return true;
    }
    // Reads one recorded proof back and turns it into the verified eye
    // assignment, or its failure reason. A non-blocking read returns false while
    // the GPU still owns that slot; a blocking read throws on failure as before.
    bool Retire(unsigned slot, bool nonBlocking)
    {
        Proof& proof = proofs[slot];
        if (!proof.pending)
            return true;
        D3D11_MAPPED_SUBRESOURCE map{};
        HRESULT hr;
        {
#ifdef ETS2_DEPTH_WAIT_PROFILE
            ets2_depth_wait::MapSpan measure;
#endif
            hr = c->Map(readbacks[slot].Get(), 0, D3D11_MAP_READ, nonBlocking ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0, &map);
#ifdef ETS2_DEPTH_WAIT_PROFILE
            measure.result=hr;
#endif
        }
        if (nonBlocking && hr == DXGI_ERROR_WAS_STILL_DRAWING)
            return false;
        Check(hr, "match readback map");
        std::array<uint32_t, 2 * Capacity> flags{};
        memcpy(flags.data(), map.pData, sizeof(uint32_t) * 2 * proof.count);
        c->Unmap(readbacks[slot].Get(), 0);
        proof.pending = false;
        Verified next;
        next.epoch = proof.epoch;
        next.count = proof.count;
        next.identities = proof.identity;
        unsigned matches[2] = {0, 0};
        for (unsigned i = 0; i < proof.count; ++i)
        {
            if (proof.candidateEpoch[i] != proof.epoch)
                continue;
            for (unsigned eye = 0; eye < 2; ++eye)
                if (flags[i * 2 + eye] == 0)
                {
                    ++matches[eye];
                    next.index[eye] = static_cast<int>(i);
                }
        }
        if (matches[0] != 1 || matches[1] != 1 || next.index[0] == next.index[1])
        {
            next.valid = false;
            next.reason = (matches[0] > 1 || matches[1] > 1 || (next.index[0] >= 0 && next.index[0] == next.index[1]))
                              ? "ambiguous correspondence"
                              : "missing or stale correspondence";
        }
        else
        {
            next.valid = true;
            next.identity[0] = proof.identity[static_cast<size_t>(next.index[0])];
            next.identity[1] = proof.identity[static_cast<size_t>(next.index[1])];
            next.reason = "unique current-epoch exact color correspondence";
        }
        // A synchronous recovery may complete before an older pending slot is
        // retired. That older verdict must never resurrect a revoked assignment.
        if (next.epoch >= verified.epoch)
        {
            if (!next.valid)
                outputVerified = false;
            if (next.epoch == lastAcceptedEpoch)
                outputVerified = next.valid && next.identity[0] == outputIdentity[0] &&
                                 next.identity[1] == outputIdentity[1];
            verified = next;
        }
        return true;
    }
    // The verified assignment applies to the current candidates only when they
    // are the same resources, in any order, and each eye identity occurs once.
    bool AssignFromVerified(uint64_t epoch, int (&eye)[2]) const
    {
        if (!verified.valid || verified.count != Count() || verified.identity[0] == verified.identity[1] ||
            epoch < verified.epoch || epoch - verified.epoch > kRing)
            return false;
        std::array<uint64_t, Capacity> now{}, then = verified.identities;
        for (unsigned i = 0; i < Count(); ++i)
            now[i] = candidates[i].identity;
        std::sort(now.begin(), now.begin() + Count());
        std::sort(then.begin(), then.begin() + Count());
        if (!std::equal(now.begin(), now.begin() + Count(), then.begin()))
            return false;
        eye[0] = eye[1] = -1;
        for (unsigned e = 0; e < 2; ++e)
            for (unsigned i = 0; i < Count(); ++i)
                if (candidates[i].identity == verified.identity[e] && candidates[i].epoch == epoch)
                    eye[e] = eye[e] < 0 ? static_cast<int>(i) : -2;
        return eye[0] >= 0 && eye[1] >= 0 && eye[0] != eye[1];
    }
    Result Match(ID3D11Texture2D* combined, uint64_t epoch, bool asyncProof = false)
    {
        valid = false;
        // VR epochs also advance while the route is unavailable. A ring bounds
        // queued Match calls, not elapsed frames; recovery must establish truth
        // again even when the game's resource pointers happen to be unchanged.
        if (lastMatchEpoch && epoch != lastMatchEpoch + 1)
        {
            verified = {};
            for (auto& pending : proofs)
                pending = {};
            outputVerified = false;
        }
        lastMatchEpoch = epoch;
        Result out;
        if(!combined){out.reason="missing combined color";return out;}
        if (overflow)
        {
            out.reason = "candidate capacity exceeded";
            return out;
        }
        D3D11_TEXTURE2D_DESC desc{};
        combined->GetDesc(&desc);
        if (desc.Width != 2 * w || desc.Height != h || desc.MipLevels != 1 || desc.ArraySize != 1 ||
            desc.SampleDesc.Count != 1 || !ColorFamily(desc.Format))
        {
            out.reason = "unsupported SBS extent/format";
            return out;
        }
        if (candidates.empty())
        {
            out.reason = "no captured candidates";
            return out;
        }
        // One locked private state spans all commands and the existing proof
        // readback. A suppressing game predicate cannot publish stale scores.
        ets2_d3d11::Scope isolated(privateState,c.Get());
        if(!isolated){out.reason="protected private context unavailable";return out;}
        // Before recording epoch N, the reused slot contains N-kRing. Retiring
        // it guarantees a verdict no older than kRing epochs, while the two
        // newer proofs can remain pending. Requiring kRing-1 would drain the
        // queue again for the current proof whenever the GPU is three frames back.
#ifdef ETS2_DEPTH_WAIT_PROFILE
        ets2_depth_wait::Phase(ets2_depth_wait::Old);
#endif
        Retire(ringNext, false);
        c->CopyResource(sbs.Get(), combined);
        {
            const UINT zero[4]{};
            c->ClearUnorderedAccessViewUint(scoresView.Get(), zero);
            UINT values[] = {w, h, Count(), 0};
            c->UpdateSubresource(constants.Get(), 0, nullptr, values, 0, 0);
            auto* cb = constants.Get();
            c->CSSetConstantBuffers(0, 1, &cb);
            ID3D11ShaderResourceView* views[] = {colorsView.Get(), sbsView.Get()};
            c->CSSetShaderResources(0, 2, views);
            auto* uav = scoresView.Get();
            c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            c->CSSetShader(compare.Get(), nullptr, 0);
#ifdef DEPTH_MATCH_LEGACY_COMPARE
            c->Dispatch((w + 15) / 16, (h + 15) / 16, Count() * 2);
#else
            c->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
#endif
            ID3D11UnorderedAccessView* none = nullptr;
            c->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
            c->CopyResource(readbacks[ringNext].Get(), scores.Get());
        }
        Proof& proof = proofs[ringNext];
        proof.pending = true;
        proof.epoch = epoch;
        proof.count = Count();
        for (unsigned i = 0; i < Count(); ++i)
        {
            proof.identity[i] = candidates[i].identity;
            proof.candidateEpoch[i] = candidates[i].epoch;
        }
        const unsigned recorded = ringNext;
        ringNext = (ringNext + 1) % kRing;
        // Read back whatever earlier proofs have finished, oldest first, without waiting.
#ifdef ETS2_DEPTH_WAIT_PROFILE
        ets2_depth_wait::Phase(ets2_depth_wait::Poll);
#endif
        for (unsigned k = 1; k < kRing; ++k)
        {
            const unsigned slot = (recorded + k) % kRing;
            if (proofs[slot].pending && !Retire(slot, true))
                break;
        }
        if (asyncProof && AssignFromVerified(epoch, out.eye))
        {
            // Same resources as an exactly proven frame: assign by identity now and
            // let its proof confirm or revoke it when retired, up to three epochs later.
            out.synchronous = false;
            out.proofAge = static_cast<unsigned>(epoch - verified.epoch);
            out.reason = "identity assignment from an earlier exact proof";
        }
        else
        {
            // No proven assignment covers these resources: the original blocking proof.
#ifdef ETS2_DEPTH_WAIT_PROFILE
            ets2_depth_wait::Phase(ets2_depth_wait::Current);
#endif
            Retire(recorded, false);
            if (verified.epoch != epoch)
            {
                out.reason = "proof did not complete";
                return out;
            }
            if (!verified.valid)
            {
                out.reason = verified.reason;
                return out;
            }
            out.eye[0] = verified.index[0];
            out.eye[1] = verified.index[1];
            out.synchronous = true;
            out.reason = verified.reason;
        }
        out.matches[0] = out.matches[1] = 1;
        // Typed depth-only SRVs normalize D16/D24 and discard stencil; D32 remains raw float.
        {
            ID3D11ShaderResourceView* noViews[2]{};c->CSSetShaderResources(0,2,noViews);
            const UINT values[] = {w, h, static_cast<UINT>(out.eye[0]), static_cast<UINT>(out.eye[1])};
            c->UpdateSubresource(constants.Get(), 0, nullptr, values, 0, 0);
            auto* cb = constants.Get();
            c->CSSetConstantBuffers(0, 1, &cb);
            auto* view = depthsView.Get();
            c->CSSetShaderResources(0, 1, &view);
            auto* uav = outputUav.Get();
            c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            c->CSSetShader(assemble.Get(), nullptr, 0);
            c->Dispatch((2 * w + 15) / 16, (h + 15) / 16, 1);
        }
        valid = out.accepted = true;
        lastAcceptedEpoch = epoch;
        outputIdentity[0] = candidates[static_cast<unsigned>(out.eye[0])].identity;
        outputIdentity[1] = candidates[static_cast<unsigned>(out.eye[1])].identity;
        outputVerified = out.synchronous;
        return out;
    }
    ID3D11ShaderResourceView* View() const
    {
        return valid ? outputView.Get() : nullptr;
    }
    ID3D11Texture2D* Output() const
    {
        return valid ? output.Get() : nullptr;
    }
    // Hold only an output whose own proof confirmed the assignment used to
    // assemble it. Poll pending proofs during a route gap as Match is not called
    // there; a rejected or still-unverified speculative output is never a hold.
    ID3D11ShaderResourceView* HeldView(uint64_t epoch, unsigned frames)
    {
        if (!lastAcceptedEpoch || epoch <= lastAcceptedEpoch || epoch - lastAcceptedEpoch > frames)
            return nullptr;
        ets2_d3d11::Scope isolated(privateState, c.Get());
        if (!isolated)
            return nullptr;
#ifdef ETS2_DEPTH_WAIT_PROFILE
        ets2_depth_wait::Phase(ets2_depth_wait::Held);
#endif
        for (unsigned k = 0; k < kRing; ++k)
        {
            const unsigned slot = (ringNext + k) % kRing;
            if (proofs[slot].pending && !Retire(slot, true))
                break;
        }
        return outputVerified ? outputView.Get() : nullptr;
    }
    uint64_t LastAcceptedEpoch() const
    {
        return lastAcceptedEpoch;
    }
    bool ProofVerified() const
    {
        return verified.valid;
    }
#ifndef ETS2_DEPTH_EMBEDDED
    void SaveColorPreviews(uint64_t epoch, const std::wstring& outputDirectory = L"")
    {
        // Bounded, explicit diagnostic only: <=160x128 RGB previews, one event per runtime.
        // These are inspection aids; the matcher still compares every original pixel.
        const unsigned pw = (std::min)(w, 160u), ph = (std::min)(h, 128u), layers = Count() + 2;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = pw;
        td.Height = ph;
        td.MipLevels = 1;
        td.ArraySize = layers;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> preview, staging;
        Check(d->CreateTexture2D(&td, nullptr, &preview), "color preview");
        ComPtr<ID3D11UnorderedAccessView> view;
        Check(d->CreateUnorderedAccessView(preview.Get(), nullptr, &view), "color preview UAV");
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(d->CreateTexture2D(&td, nullptr, &staging), "color preview staging");
        std::string shader = "#define CANDIDATES " + std::to_string(Count()) + "\n#define WIDTH " +
                             std::to_string(w) + "\n#define HEIGHT " + std::to_string(h) + "\n#define PW " +
                             std::to_string(pw) + "\n#define PH " + std::to_string(ph) + R"(
Texture2DArray<float4> candidates:register(t0);Texture2D<float4> combined:register(t1);RWTexture2DArray<unorm float4> result:register(u0);
[numthreads(16,16,1)] void main(uint3 id:SV_DispatchThreadID){if(id.x>=PW||id.y>=PH)return;uint2 xy=(uint2)((float2(id.xy)+.5)*float2(WIDTH,HEIGHT)/float2(PW,PH));result[id]=id.z<CANDIDATES?candidates.Load(int4(xy,id.z,0)):combined.Load(int3(xy.x+(id.z-CANDIDATES)*WIDTH,xy.y,0));}
)";
        ComPtr<ID3DBlob> blob, error;
        Check(D3DCompile(shader.data(), shader.size(), "color-preview", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "color preview shader compile");
        ComPtr<ID3D11ComputeShader> cs;
        Check(d->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs),
              "color preview shader");
        {
            ets2_d3d11::Scope isolated(privateState,c.Get());
            Check(isolated.Status(),"color preview isolated context");
            ID3D11ShaderResourceView* inputs[] = {colorsView.Get(), sbsView.Get()};
            c->CSSetShaderResources(0, 2, inputs);
            auto* uav = view.Get();
            c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            c->CSSetShader(cs.Get(), nullptr, 0);
            c->Dispatch((pw + 15) / 16, (ph + 15) / 16, layers);
            ID3D11UnorderedAccessView* none = nullptr;
            c->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
            c->CopyResource(staging.Get(), preview.Get());
        }
        for (unsigned layer = 0; layer < layers; ++layer)
        {
            D3D11_MAPPED_SUBRESOURCE m{};
            Check(c->Map(staging.Get(), layer, D3D11_MAP_READ, 0, &m), "color preview map");
            std::string name = "depth-match-epoch-" + std::to_string(epoch) +
                               (layer < Count() ? "-candidate-" + std::to_string(layer)
                                                : "-sbs-eye-" + std::to_string(layer - Count())) +
                               ".ppm";
            std::ofstream f(std::filesystem::path(outputDirectory) / name, std::ios::binary);
            f << "P6\n" << pw << ' ' << ph << "\n255\n";
            for (unsigned y = 0; y < ph; ++y)
                for (unsigned x = 0; x < pw; ++x)
                    f.write(static_cast<const char*>(m.pData) + size_t(y) * m.RowPitch + x * 4, 3);
            c->Unmap(staging.Get(), layer);
        }
    }
#endif
};
} // namespace depth_match
