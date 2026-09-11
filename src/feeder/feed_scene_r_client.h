// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <limits>
#include <utility>
#include "feed_scene_r_pair_abi.h"

namespace ets2_scene_r_client {
using Microsoft::WRL::ComPtr;
static_assert(sizeof(void*)==8 && sizeof(ETS2_SceneRRequestV1)==40 &&
    sizeof(ETS2_SceneREyeV1)==64 && sizeof(ETS2_SceneRPairV1)==176,"Exact x64 ABI V1 required");

enum class Stage { None, Boundary, Request, Resolve, Pin, Acquire, FailureContract,
    Header, Device, Context, Final, View, Identity, Ready };
struct Receipt {
    Stage stage=Stage::None;
    HRESULT hr=E_PENDING,acquire_hr=E_PENDING;
    bool acquired=false,validated=false,module_pinned=false;
    uintptr_t module=0,entry=0,device=0,context=0,final_resource=0;
    uintptr_t view[4]{},resource[4]{}; // canonical IUnknown identities, borrowed numbers
    D3D11_TEXTURE2D_DESC texture[4]{};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv[4]{};
};

inline HRESULT ValidateHeader(const ETS2_SceneRPairV1& p) noexcept {
    if(p.struct_size!=sizeof(p)||p.abi_version!=ETS2_SCENE_R_PAIR_ABI_VERSION||
       !p.generation||!p.runtime_generation||p.scene_width<128||p.scene_height<128||
       p.scene_width>p.final_eye_width||p.scene_height>p.final_height||
       p.final_eye_width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/2||
       p.final_height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)return E_INVALIDARG;
    for(const auto& e:p.eye)if(!e.source_color||!e.resolved_color||e.source_color==e.resolved_color||
        !e.resolve_sequence||!e.color_generation||!e.depth_generation||e.candidate_index>1||e.reserved||
        !e.color||!e.depth)return E_INVALIDARG;
    // The game can reuse one raw color source sequentially. The immutable
    // resolved copies and their resolve stamps must distinguish the two eyes.
    if(p.eye[0].resolved_color==p.eye[1].resolved_color||
       p.eye[0].resolve_sequence==p.eye[1].resolve_sequence||
       p.eye[0].candidate_index==p.eye[1].candidate_index)return E_INVALIDARG;
    return S_OK; // Epoch zero is allowed by ABI V1; validity is not inferred from it.
}
inline bool Single(const D3D11_TEXTURE2D_DESC& d) noexcept {
    return d.Width>=128&&d.Height>=128&&d.Width<=D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION&&
        d.Height<=D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION&&d.MipLevels==1&&d.ArraySize==1&&
        d.SampleDesc.Count==1&&d.SampleDesc.Quality==0&&!d.CPUAccessFlags&&
        (d.Usage==D3D11_USAGE_DEFAULT||d.Usage==D3D11_USAGE_IMMUTABLE);
}
inline HRESULT ValidateView(const D3D11_TEXTURE2D_DESC& d,const D3D11_SHADER_RESOURCE_VIEW_DESC& v,
                            bool color,UINT width,UINT height) noexcept {
    if(!Single(d)||d.Width!=width||d.Height!=height||!(d.BindFlags&D3D11_BIND_SHADER_RESOURCE)||
       v.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||v.Texture2D.MostDetailedMip||v.Texture2D.MipLevels!=1)return E_INVALIDARG;
    if(color)return v.Format==DXGI_FORMAT_R8G8B8A8_UNORM&&
        (d.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS||d.Format==DXGI_FORMAT_R8G8B8A8_UNORM)?S_OK:E_INVALIDARG;
    return v.Format==DXGI_FORMAT_R32_FLOAT&&
        (d.Format==DXGI_FORMAT_R32_TYPELESS||d.Format==DXGI_FORMAT_R32_FLOAT)?S_OK:E_INVALIDARG;
}

class Pair final {
    ETS2_SceneRPairV1 value_{};
    bool valid_=false;
    friend class Client;
public:
    Pair()=default;
    ~Pair(){Reset();}
    Pair(const Pair&)=delete;Pair& operator=(const Pair&)=delete;
    Pair(Pair&& other) noexcept:value_(other.value_),valid_(other.valid_){other.value_={};other.valid_=false;}
    Pair& operator=(Pair&& other) noexcept {
        if(this!=&other){Reset();value_=other.value_;valid_=other.valid_;other.value_={};other.valid_=false;}return *this;
    }
    void Reset() noexcept {
        // One Release for each AddRef transferred by the ABI, including aliases
        // in a malformed successful result. No references are taken from stamps.
        for(auto& e:value_.eye){if(e.color)e.color->Release();if(e.depth)e.depth->Release();}
        value_={};valid_=false;
    }
    bool Valid()const noexcept{return valid_;}
    const ETS2_SceneRPairV1& Get()const noexcept{return value_;} // borrowed, lifetime is this Pair
};

namespace detail {
inline constexpr GUID UnwrappedObject={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
inline HRESULT DeviceIdentity(ID3D11Device* raw,ComPtr<IUnknown>& identity) noexcept {
    if(!raw)return E_POINTER;
    ComPtr<ID3D11Device> native=raw;ComPtr<IUnknown> unwrapped;
    HRESULT hr=raw->QueryInterface(UnwrappedObject,reinterpret_cast<void**>(unwrapped.GetAddressOf()));
    if(SUCCEEDED(hr)){if(!unwrapped)return E_NOINTERFACE;hr=unwrapped.As(&native);if(FAILED(hr))return hr;}
    else if(hr!=E_NOINTERFACE)return hr;
    hr=native.As(&identity);return FAILED(hr)?hr:identity?S_OK:E_NOINTERFACE;
}
inline HRESULT SameDevice(ID3D11DeviceChild* child,IUnknown* expected) noexcept {
    if(!child)return E_POINTER;
    ComPtr<ID3D11Device> raw;child->GetDevice(&raw);ComPtr<IUnknown> identity;
    const HRESULT hr=DeviceIdentity(raw.Get(),identity);return FAILED(hr)?hr:identity.Get()==expected?S_OK:E_INVALIDARG;
}
inline bool Zero(const ETS2_SceneRPairV1& p) noexcept {
    const auto* b=reinterpret_cast<const unsigned char*>(&p);
    for(size_t i=0;i<sizeof(p);++i)if(b[i])return false;return true;
}
}

// Single-caller owner. ResolveLoaded never loads an addon. Acquire must be called
// before entering any feeder private D3D11 context state scope; the required
// boolean is a caller receipt of that boundary, not a hidden state query.
// No rendering, copying, state swap, wait, Flush, Map or context protection change.
class Client final {
    ETS2_AcquireSceneRPairV1Fn acquire_=nullptr;
    HMODULE module_=nullptr;
    bool pinned_=false;
public:
    Client()=default;Client(const Client&)=delete;Client& operator=(const Client&)=delete;
    // Injection is explicit and only for tests; it performs no module API call.
    static Client InjectedForTest(ETS2_AcquireSceneRPairV1Fn fn) noexcept {return Client(fn);}
    HRESULT ResolveLoaded(Receipt& receipt) noexcept {
        receipt={};acquire_=nullptr;module_=nullptr;pinned_=false;
        receipt.stage=Stage::Resolve;
        HMODULE held=nullptr;
        // Acquire a temporary reference to prevent unload between lookup/pin.
#ifdef ETS2_DEPTH_EMBEDDED
        constexpr auto producerModule=L"ets2-dlaa.addon64";
#else
        constexpr auto producerModule=L"ets2-stereo-depth.addon64";
#endif
        if(!GetModuleHandleExW(0,producerModule,&held))return receipt.hr=HRESULT_FROM_WIN32(GetLastError());
        const auto address=GetProcAddress(held,"ETS2_AcquireSceneRPairV1");
        HRESULT hr=address?S_OK:HRESULT_FROM_WIN32(GetLastError());
        HMODULE addressOwner=nullptr;
        if(SUCCEEDED(hr)){
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(address),&addressOwner))hr=HRESULT_FROM_WIN32(GetLastError());
            else if(addressOwner!=held)hr=E_INVALIDARG; // refuse forwarded export from another module
        }
        receipt.stage=Stage::Pin;HMODULE pinned=nullptr;
        if(SUCCEEDED(hr)&&!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(address),&pinned))hr=HRESULT_FROM_WIN32(GetLastError());
        if(SUCCEEDED(hr)&&pinned!=held)hr=E_UNEXPECTED;
        if(SUCCEEDED(hr)){acquire_=reinterpret_cast<ETS2_AcquireSceneRPairV1Fn>(address);module_=pinned;pinned_=true;}
        if(addressOwner)FreeLibrary(addressOwner);FreeLibrary(held);
        receipt.module=reinterpret_cast<uintptr_t>(module_);receipt.entry=reinterpret_cast<uintptr_t>(acquire_);
        receipt.module_pinned=pinned_;receipt.hr=hr;return hr;
    }
    HRESULT Acquire(const ETS2_SceneRRequestV1& request,bool privateContextActive,Pair& out,Receipt& receipt) noexcept {
        out.Reset();receipt={};receipt.module=reinterpret_cast<uintptr_t>(module_);
        receipt.entry=reinterpret_cast<uintptr_t>(acquire_);receipt.module_pinned=pinned_;
        const auto fail=[&](Stage stage,HRESULT hr){out.Reset();receipt.stage=stage;receipt.hr=hr;return hr;};
        if(privateContextActive)return fail(Stage::Boundary,E_ACCESSDENIED);
        if(request.struct_size!=sizeof(request)||request.abi_version!=ETS2_SCENE_R_PAIR_ABI_VERSION||
           !request.runtime||!request.context||!request.device||!request.final_sbs)return fail(Stage::Request,E_INVALIDARG);
        if(!acquire_)return fail(Stage::Resolve,E_NOINTERFACE);
        ETS2_SceneRPairV1 transferred{};
        receipt.acquire_hr=acquire_(&request,&transferred);
        if(FAILED(receipt.acquire_hr)){
            // Failure carries no ownership under V1. Never dereference illegal
            // nonzero failure output: it is a provider contract violation.
            if(!detail::Zero(transferred))return fail(Stage::FailureContract,E_UNEXPECTED);
            return fail(Stage::Acquire,receipt.acquire_hr);
        }
        out.value_=transferred;receipt.acquired=true; // adopt immediately; validate before publishing
        HRESULT hr=ValidateHeader(out.value_);if(FAILED(hr))return fail(Stage::Header,hr);
        ComPtr<IUnknown> device,context,target,views[4],resources[4];
        hr=detail::DeviceIdentity(request.device,device);if(FAILED(hr))return fail(Stage::Device,hr);
        receipt.device=reinterpret_cast<uintptr_t>(device.Get());
        if(request.context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return fail(Stage::Context,E_INVALIDARG);
        hr=detail::SameDevice(request.context,device.Get());if(FAILED(hr))return fail(Stage::Context,hr);
        hr=request.context->QueryInterface(IID_PPV_ARGS(&context));if(FAILED(hr)||!context)return fail(Stage::Context,FAILED(hr)?hr:E_NOINTERFACE);
        receipt.context=reinterpret_cast<uintptr_t>(context.Get());
        hr=detail::SameDevice(request.final_sbs,device.Get());if(FAILED(hr))return fail(Stage::Final,hr);
        hr=request.final_sbs->QueryInterface(IID_PPV_ARGS(&target));if(FAILED(hr)||!target)return fail(Stage::Final,FAILED(hr)?hr:E_NOINTERFACE);
        receipt.final_resource=reinterpret_cast<uintptr_t>(target.Get());
        D3D11_TEXTURE2D_DESC finalDesc{};request.final_sbs->GetDesc(&finalDesc);
        const bool finalFormat=finalDesc.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS||finalDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||
            finalDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||finalDesc.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS||
            finalDesc.Format==DXGI_FORMAT_B8G8R8A8_UNORM||finalDesc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if(!Single(finalDesc)||!finalFormat||(finalDesc.Width&1)||finalDesc.Width/2!=out.value_.final_eye_width||
            finalDesc.Height!=out.value_.final_height)return fail(Stage::Final,E_INVALIDARG);
        for(unsigned i=0;i<4;++i){
            auto* view=(i&1)?out.value_.eye[i/2].depth:out.value_.eye[i/2].color;
            hr=detail::SameDevice(view,device.Get());if(FAILED(hr))return fail(Stage::View,hr);
            hr=view->QueryInterface(IID_PPV_ARGS(&views[i]));if(FAILED(hr)||!views[i])return fail(Stage::Identity,FAILED(hr)?hr:E_NOINTERFACE);
            ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
            hr=detail::SameDevice(resource.Get(),device.Get());if(FAILED(hr))return fail(Stage::View,hr);
            hr=resource.As(&resources[i]);if(FAILED(hr)||!resources[i])return fail(Stage::Identity,FAILED(hr)?hr:E_NOINTERFACE);
            ComPtr<ID3D11Texture2D> texture;hr=resource.As(&texture);if(FAILED(hr))return fail(Stage::View,hr);
            view->GetDesc(&receipt.srv[i]);texture->GetDesc(&receipt.texture[i]);
            receipt.view[i]=reinterpret_cast<uintptr_t>(views[i].Get());receipt.resource[i]=reinterpret_cast<uintptr_t>(resources[i].Get());
            hr=ValidateView(receipt.texture[i],receipt.srv[i],!(i&1),out.value_.scene_width,out.value_.scene_height);
            if(FAILED(hr))return fail(Stage::View,hr);
            if(resources[i].Get()==target.Get())return fail(Stage::Identity,E_INVALIDARG);
            for(unsigned j=0;j<i;++j)if(views[i].Get()==views[j].Get()||resources[i].Get()==resources[j].Get())return fail(Stage::Identity,E_INVALIDARG);
        }
        out.valid_=true;receipt.validated=true;receipt.stage=Stage::Ready;return receipt.hr=S_OK;
    }
private:
    explicit Client(ETS2_AcquireSceneRPairV1Fn fn) noexcept:acquire_(fn){}
};

// A candidate never consumes a cohort. Commit only after accepted paired final
// delivery/processing; acquisition, allocation grace and abandoned builds do not
// call CommitDelivered. Skipped delivery creates an epoch/generation gap on the
// next candidate, requesting reset. InvalidateHistory retains anti-replay IDs.
// This publisher's runtime generations come from a process-global monotonic
// serial. An older generation cannot re-enter by presenting another raw address.
class Sequence final {
public:
    class Ticket {
        friend class Sequence;
        const Sequence* owner_=nullptr;
        uint64_t revision_=0,runtime_=0,runtime_generation_=0,generation_=0,epoch_=0;
        bool valid_=false,reset_=false;
    public:
        bool Valid()const noexcept{return valid_;}
        bool ResetNeeded()const noexcept{return reset_;}
        uint64_t Generation()const noexcept{return generation_;}
        uint64_t Epoch()const noexcept{return epoch_;}
        uint64_t Runtime()const noexcept{return runtime_;}
        uint64_t RuntimeGeneration()const noexcept{return runtime_generation_;}
    };
private:
    uint64_t revision_=1,runtime_=0,runtime_generation_=0,generation_=0,epoch_=0;
    bool delivered_=false,history_=false;
public:
    Sequence()=default;Sequence(const Sequence&)=delete;Sequence& operator=(const Sequence&)=delete;
    HRESULT Prepare(uint64_t runtime,const ETS2_SceneRPairV1& pair,Ticket& ticket)const noexcept {
        ticket={};if(!runtime||FAILED(ValidateHeader(pair)))return E_INVALIDARG;
        if(revision_==std::numeric_limits<uint64_t>::max())return E_FAIL;
        const bool sameRuntime=delivered_&&runtime==runtime_;
        if(delivered_&&(pair.runtime_generation<runtime_generation_||
            (pair.runtime_generation==runtime_generation_&&!sameRuntime)))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const bool sameGeneration=sameRuntime&&pair.runtime_generation==runtime_generation_;
        if(sameGeneration&&(pair.generation<=generation_||pair.epoch<=epoch_))return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        const bool continuous=sameGeneration&&generation_!=std::numeric_limits<uint64_t>::max()&&
            epoch_!=std::numeric_limits<uint64_t>::max()&&pair.generation==generation_+1&&pair.epoch==epoch_+1;
        ticket.owner_=this;ticket.revision_=revision_;ticket.runtime_=runtime;ticket.runtime_generation_=pair.runtime_generation;
        ticket.generation_=pair.generation;ticket.epoch_=pair.epoch;ticket.valid_=true;ticket.reset_=!history_||!continuous;return S_OK;
    }
    HRESULT CommitDelivered(const Ticket& ticket) noexcept {
        if(!ticket.valid_||ticket.owner_!=this||ticket.revision_!=revision_||revision_==std::numeric_limits<uint64_t>::max())return E_INVALIDARG;
        runtime_=ticket.runtime_;runtime_generation_=ticket.runtime_generation_;generation_=ticket.generation_;epoch_=ticket.epoch_;
        delivered_=history_=true;++revision_;return S_OK;
    }
    HRESULT InvalidateHistory() noexcept {
        history_=false;if(revision_==std::numeric_limits<uint64_t>::max())return E_FAIL;++revision_;return S_OK;
    }
    uint64_t DeliveredGeneration()const noexcept{return delivered_?generation_:0;}
};
}

