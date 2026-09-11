// SPDX-License-Identifier: MIT
#pragma once
#include "scene_r_pair_abi.h"
#include "scene_r_color.h"
#include <limits>
#include <type_traits>

namespace ets2_scene_r_pair {
using Microsoft::WRL::ComPtr;
static_assert(sizeof(void*)==8,"Scene R pair ABI requires x64");
static_assert(std::is_standard_layout_v<ETS2_SceneRRequestV1> && sizeof(ETS2_SceneRRequestV1)==40);
static_assert(std::is_standard_layout_v<ETS2_SceneREyeV1> && sizeof(ETS2_SceneREyeV1)==64);
static_assert(std::is_standard_layout_v<ETS2_SceneRPairV1> && sizeof(ETS2_SceneRPairV1)==176);

struct Publication {
    uint64_t runtime=0,runtime_generation=0,epoch=0;
    ID3D11DeviceContext* context=nullptr;
    ID3D11Device* device=nullptr;
    ID3D11Texture2D* final_sbs=nullptr;
    ETS2_SceneREyeV1 eye[2]={}; // already ordered by Matcher::Result.eye[]
};

inline HRESULT ValidateRequest(const ETS2_SceneRRequestV1* request) noexcept {
    if(!request)return E_POINTER;
    if(request->struct_size!=sizeof(*request)||request->abi_version!=ETS2_SCENE_R_PAIR_ABI_VERSION)return E_INVALIDARG;
    return request->runtime&&request->context&&request->device&&request->final_sbs?S_OK:E_POINTER;
}
inline bool Single(const D3D11_TEXTURE2D_DESC& d) noexcept {
    return d.Width>=128&&d.Height>=128&&d.Width<=D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION&&
        d.Height<=D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION&&d.MipLevels==1&&d.ArraySize==1&&
        d.SampleDesc.Count==1&&d.SampleDesc.Quality==0&&d.CPUAccessFlags==0&&
        (d.Usage==D3D11_USAGE_DEFAULT||d.Usage==D3D11_USAGE_IMMUTABLE);
}
inline HRESULT ValidateView(const D3D11_TEXTURE2D_DESC& d,const D3D11_SHADER_RESOURCE_VIEW_DESC& v,bool color) noexcept {
    if(!Single(d)||!(d.BindFlags&D3D11_BIND_SHADER_RESOURCE)||v.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||
       v.Texture2D.MostDetailedMip!=0||v.Texture2D.MipLevels!=1)return E_INVALIDARG;
    if(color)return v.Format==DXGI_FORMAT_R8G8B8A8_UNORM&&
        (d.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS||d.Format==DXGI_FORMAT_R8G8B8A8_UNORM)?S_OK:E_INVALIDARG;
    return v.Format==DXGI_FORMAT_R32_FLOAT&&(d.Format==DXGI_FORMAT_R32_TYPELESS||d.Format==DXGI_FORMAT_R32_FLOAT)?S_OK:E_INVALIDARG;
}
inline HRESULT ValidateFinal(const D3D11_TEXTURE2D_DESC& d) noexcept {
    if(!Single(d)||(d.Width&1)||d.Width<256)return E_INVALIDARG;
    switch(d.Format){
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return S_OK;
    default:return E_INVALIDARG;
    }
}
inline HRESULT ValidateStamps(const ETS2_SceneREyeV1 (&eye)[2]) noexcept {
    for(const auto& e:eye)if(!e.source_color||!e.resolved_color||e.source_color==e.resolved_color||
        !e.resolve_sequence||!e.color_generation||!e.depth_generation||e.candidate_index>=2||e.reserved)return E_INVALIDARG;
    return eye[0].resolved_color!=eye[1].resolved_color&&eye[0].resolve_sequence!=eye[1].resolve_sequence&&
        eye[0].candidate_index!=eye[1].candidate_index?S_OK:E_INVALIDARG;
}
namespace detail {
inline HRESULT DeviceIdentity(ID3D11Device* raw,ComPtr<IUnknown>& identity) noexcept {
    ets2_scene_r_color::DeviceReceipt ignored;ComPtr<ID3D11Device> normalized;
    const HRESULT hr=ets2_scene_r_color::detail::Identity(raw,ignored,identity,normalized);
    return FAILED(hr)?hr:identity?S_OK:E_NOINTERFACE;
}
inline HRESULT SameDevice(ID3D11Device* raw,IUnknown* expected) noexcept {
    ComPtr<IUnknown> identity;const HRESULT hr=DeviceIdentity(raw,identity);
    return FAILED(hr)?hr:identity.Get()==expected?S_OK:E_INVALIDARG;
}
inline HRESULT ContextIdentity(ID3D11DeviceContext* context,IUnknown* expectedDevice,ComPtr<IUnknown>& identity) noexcept {
    if(!context)return E_POINTER;
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return E_INVALIDARG;
    ComPtr<ID3D11Device> raw;context->GetDevice(&raw);HRESULT hr=SameDevice(raw.Get(),expectedDevice);
    if(FAILED(hr))return hr;
    hr=context->QueryInterface(IID_PPV_ARGS(&identity));return FAILED(hr)?hr:identity?S_OK:E_NOINTERFACE;
}
inline HRESULT ResourceIdentity(ID3D11Resource* resource,IUnknown* expectedDevice,ComPtr<IUnknown>& identity) noexcept {
    if(!resource)return E_POINTER;
    ComPtr<ID3D11Device> raw;resource->GetDevice(&raw);HRESULT hr=SameDevice(raw.Get(),expectedDevice);
    if(FAILED(hr))return hr;
    hr=resource->QueryInterface(IID_PPV_ARGS(&identity));return FAILED(hr)?hr:identity?S_OK:E_NOINTERFACE;
}
inline HRESULT InspectView(ID3D11ShaderResourceView* view,bool color,IUnknown* expectedDevice,
    D3D11_TEXTURE2D_DESC& desc,ComPtr<IUnknown>& identity) noexcept {
    if(!view)return E_POINTER;
    ComPtr<ID3D11Device> raw;view->GetDevice(&raw);HRESULT hr=SameDevice(raw.Get(),expectedDevice);
    if(FAILED(hr))return hr;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    hr=ResourceIdentity(resource.Get(),expectedDevice,identity);if(FAILED(hr))return hr;
    ComPtr<ID3D11Texture2D> texture;hr=resource.As(&texture);if(FAILED(hr))return hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};view->GetDesc(&srv);texture->GetDesc(&desc);
    return ValidateView(desc,srv,color);
}
}

// Caller holds its publication/route mutex for EVERY method. No internal lock,
// ReShade callback, context-state change, GPU command, wait or export lookup.
// Publish requires fresh immutable color AND depth copies; AddRef cannot enforce
// the promise that an external producer will never write their pixels again.
// Clear belongs before Begin work, before Finish/route reset, and destruction.
class Publisher final {
    bool ready_=false;
    uint64_t generation_=0;
    ETS2_SceneRPairV1 published_{};
    uint64_t runtime_=0;
    ComPtr<IUnknown> device_identity_,context_identity_,target_identity_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> target_;
    ComPtr<ID3D11ShaderResourceView> color_[2],depth_[2];
public:
    Publisher()=default;
    Publisher(const Publisher&)=delete;Publisher& operator=(const Publisher&)=delete;
    bool Ready()const noexcept{return ready_;}
    uint64_t Generation()const noexcept{return generation_;}
    void Clear() noexcept {
        ready_=false;published_={};runtime_=0;
        for(unsigned i=0;i<2;++i){color_[i].Reset();depth_[i].Reset();}
        target_.Reset();context_.Reset();target_identity_.Reset();context_identity_.Reset();device_identity_.Reset();
    }
    HRESULT Publish(const Publication& input) noexcept {
        Clear();
        if(generation_==std::numeric_limits<uint64_t>::max())return E_FAIL;
        ++generation_;
        if(!input.runtime||!input.runtime_generation||!input.context||!input.device||!input.final_sbs||
           !input.eye[0].color||!input.eye[1].color||!input.eye[0].depth||!input.eye[1].depth)return E_POINTER;
        HRESULT hr=ValidateStamps(input.eye);if(FAILED(hr))return hr;
        ComPtr<IUnknown> deviceIdentity,contextIdentity,targetIdentity,resources[4];
        hr=detail::DeviceIdentity(input.device,deviceIdentity);if(FAILED(hr))return hr;
        hr=detail::ContextIdentity(input.context,deviceIdentity.Get(),contextIdentity);if(FAILED(hr))return hr;
        hr=detail::ResourceIdentity(input.final_sbs,deviceIdentity.Get(),targetIdentity);if(FAILED(hr))return hr;
        D3D11_TEXTURE2D_DESC targetDesc{},desc[4]{};input.final_sbs->GetDesc(&targetDesc);
        hr=ValidateFinal(targetDesc);if(FAILED(hr))return hr;
        for(unsigned eye=0;eye<2;++eye){
            hr=detail::InspectView(input.eye[eye].color,true,deviceIdentity.Get(),desc[eye*2],resources[eye*2]);if(FAILED(hr))return hr;
            hr=detail::InspectView(input.eye[eye].depth,false,deviceIdentity.Get(),desc[eye*2+1],resources[eye*2+1]);if(FAILED(hr))return hr;
        }
        for(unsigned i=0;i<4;++i){
            if(desc[i].Width!=desc[0].Width||desc[i].Height!=desc[0].Height||resources[i].Get()==targetIdentity.Get())return E_INVALIDARG;
            for(unsigned j=0;j<i;++j)if(resources[i].Get()==resources[j].Get())return E_INVALIDARG;
        }
        if(desc[0].Width>targetDesc.Width/2||desc[0].Height>targetDesc.Height)return E_INVALIDARG;
        // Commit only after complete validation. No partial pair becomes visible.
        published_.struct_size=sizeof(published_);published_.abi_version=ETS2_SCENE_R_PAIR_ABI_VERSION;
        published_.epoch=input.epoch;published_.generation=generation_;published_.runtime_generation=input.runtime_generation;
        published_.scene_width=desc[0].Width;published_.scene_height=desc[0].Height;
        published_.final_eye_width=targetDesc.Width/2;published_.final_height=targetDesc.Height;
        for(unsigned eye=0;eye<2;++eye){
            published_.eye[eye]=input.eye[eye];color_[eye]=input.eye[eye].color;depth_[eye]=input.eye[eye].depth;
        }
        device_identity_=deviceIdentity;context_identity_=contextIdentity;target_identity_=targetIdentity;
        context_=input.context;target_=input.final_sbs;runtime_=input.runtime;ready_=true;return S_OK;
    }
    HRESULT Acquire(const ETS2_SceneRRequestV1* request,ETS2_SceneRPairV1* out)const noexcept {
        if(!out)return E_POINTER;
        *out={};
        HRESULT hr=ValidateRequest(request);if(FAILED(hr))return hr;
        if(!ready_)return E_PENDING;
        if(request->runtime!=runtime_)return E_INVALIDARG;
        hr=detail::SameDevice(request->device,device_identity_.Get());if(FAILED(hr))return hr;
        ComPtr<IUnknown> contextIdentity,targetIdentity;
        hr=detail::ContextIdentity(request->context,device_identity_.Get(),contextIdentity);if(FAILED(hr))return hr;
        if(contextIdentity.Get()!=context_identity_.Get())return E_INVALIDARG;
        hr=detail::ResourceIdentity(request->final_sbs,device_identity_.Get(),targetIdentity);if(FAILED(hr))return hr;
        if(targetIdentity.Get()!=target_identity_.Get())return E_INVALIDARG;
        // All failure paths precede reference transfer. COM AddRef cannot fail.
        *out=published_;
        for(unsigned eye=0;eye<2;++eye){out->eye[eye].color->AddRef();out->eye[eye].depth->AddRef();}
        return S_OK;
    }
};
}
