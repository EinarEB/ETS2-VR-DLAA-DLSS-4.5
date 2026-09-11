// Private diagnostic helper. No hooks, application state mutation or readback.
// SPDX-License-Identifier: MIT
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <limits>

namespace ets2_scene_r_color {
using Microsoft::WRL::ComPtr;
enum class Stage { None, SlotOccupied, Thread, Context, Device, View, Resource, Shape, Predication, Allocate, CopyRecorded };
struct DeviceReceipt {
    uintptr_t raw=0,canonical=0;
    bool unwrapped=false;
    HRESULT unwrap_hr=E_PENDING,identity_hr=E_PENDING;
};
struct Receipt {
    uint64_t generation=0;
    Stage stage=Stage::None;
    HRESULT hr=E_PENDING;
    bool valid=false,copy_recorded=false;
    DWORD owner_thread=0;
    uintptr_t context=0,source_view=0,source_resource=0,owned_view=0,owned_resource=0;
    DeviceReceipt context_device{},view_device{},resource_device{};
    D3D11_TEXTURE2D_DESC source{};
    D3D11_SHADER_RESOURCE_VIEW_DESC source_srv{};
    int64_t copy_recorded_qpc=0; // CPU call timing, not GPU completion.
};
// Pure CPU contract validation. One base-mip SRGB view of the compatible RGBA8
// family only. No conversion is performed by CopyResource; destination view28
// later samples the original display-encoded bytes as UNORM values.
inline HRESULT Validate(const D3D11_TEXTURE2D_DESC& t,const D3D11_SHADER_RESOURCE_VIEW_DESC& v) noexcept {
    if(!t.Width||!t.Height||t.Width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||t.Height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||
       t.MipLevels!=1||t.ArraySize!=1||t.SampleDesc.Count!=1||t.SampleDesc.Quality!=0||
       (t.Usage!=D3D11_USAGE_DEFAULT&&t.Usage!=D3D11_USAGE_IMMUTABLE)||t.CPUAccessFlags||
       (t.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS&&t.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)||
       v.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||v.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||
       v.Texture2D.MostDetailedMip!=0||v.Texture2D.MipLevels!=1)return E_INVALIDARG;
    return S_OK;
}
namespace detail {
// Exact ReShade 6.8 public extension; never infer device identity from adapter.
inline constexpr GUID UnwrappedObject={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
inline HRESULT Identity(ID3D11Device* raw,DeviceReceipt& receipt,ComPtr<IUnknown>& identity,ComPtr<ID3D11Device>& normalized) noexcept {
    receipt.raw=reinterpret_cast<uintptr_t>(raw);if(!raw)return E_POINTER;
    normalized=raw;ComPtr<IUnknown> unwrapped;
    receipt.unwrap_hr=raw->QueryInterface(UnwrappedObject,reinterpret_cast<void**>(unwrapped.GetAddressOf()));
    if(SUCCEEDED(receipt.unwrap_hr)){
        if(!unwrapped)return E_NOINTERFACE;
        const HRESULT hr=unwrapped.As(&normalized);if(FAILED(hr))return hr;receipt.unwrapped=true;
    }else if(receipt.unwrap_hr!=E_NOINTERFACE)return receipt.unwrap_hr;
    receipt.identity_hr=normalized.As(&identity);receipt.canonical=reinterpret_cast<uintptr_t>(identity.Get());return receipt.identity_hr;
}
}

// Thread-confined slot. Capture consumes a generation even on failure. Call
// BeginReuse explicitly before any second attempt; failure publishes no view.
// Each successful generation allocates a fresh destination: previously acquired
// SRVs remain immutable even after reuse, for as long as their caller owns them.
// The caller serializes the immediate context and proves the source is unmapped.
// Bound predication is refused; this helper never changes predicate/app state.
// No Flush, query, Map, GPU wait, shader draw, or format conversion is performed.
class Slot final {
    uint64_t generation_=1;
    bool attempted_=false,valid_=false;
    DWORD owner_thread_=0;
    ComPtr<IUnknown> context_identity_,device_identity_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> view_;
public:
    Slot()=default;Slot(const Slot&)=delete;Slot& operator=(const Slot&)=delete;
    bool Valid()const noexcept{return valid_;}
    uint64_t Generation()const noexcept{return generation_;}
    HRESULT BeginReuse() noexcept {
        if(owner_thread_&&owner_thread_!=GetCurrentThreadId())return E_ACCESSDENIED;
        valid_=false;
        if(generation_==std::numeric_limits<uint64_t>::max())return E_FAIL;
        ++generation_;attempted_=false;return S_OK;
    }
    // AddRef publication; caller owns this reference. A failure always sets out
    // to null. Receipt identities are borrowed numeric provenance only.
    HRESULT AcquireView(ID3D11ShaderResourceView** out)const noexcept {
        if(!out)return E_POINTER;*out=nullptr;
        if(!valid_||!view_)return E_PENDING;
        return view_.CopyTo(out);
    }
    HRESULT Capture(ID3D11DeviceContext* context,ID3D11ShaderResourceView* source,Receipt& receipt) noexcept {
        receipt={};receipt.generation=generation_;receipt.owner_thread=owner_thread_;
        receipt.context=reinterpret_cast<uintptr_t>(context);receipt.source_view=reinterpret_cast<uintptr_t>(source);
        valid_=false;
        const auto fail=[&](Stage stage,HRESULT hr){receipt.stage=stage;receipt.hr=hr;return hr;};
        if(owner_thread_&&owner_thread_!=GetCurrentThreadId())return fail(Stage::Thread,E_ACCESSDENIED);
        if(attempted_)return fail(Stage::SlotOccupied,HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
        attempted_=true;
        if(!context||!source)return fail(Stage::Context,E_POINTER);
        if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return fail(Stage::Context,E_INVALIDARG);
        ComPtr<IUnknown> ci;HRESULT hr=context->QueryInterface(IID_PPV_ARGS(&ci));
        if(FAILED(hr)||!ci)return fail(Stage::Context,FAILED(hr)?hr:E_NOINTERFACE);
        if(context_identity_&&ci.Get()!=context_identity_.Get())return fail(Stage::Context,E_INVALIDARG);
        ComPtr<ID3D11Device> raw,device,viewDevice,resourceDevice;ComPtr<IUnknown> di,vi,ri;
        context->GetDevice(&raw);hr=detail::Identity(raw.Get(),receipt.context_device,di,device);
        if(FAILED(hr)||!di)return fail(Stage::Device,FAILED(hr)?hr:E_NOINTERFACE);
        if(device_identity_&&di.Get()!=device_identity_.Get())return fail(Stage::Device,E_INVALIDARG);
        source->GetDevice(raw.ReleaseAndGetAddressOf());hr=detail::Identity(raw.Get(),receipt.view_device,vi,viewDevice);
        if(FAILED(hr)||di.Get()!=vi.Get())return fail(Stage::View,FAILED(hr)?hr:E_INVALIDARG);
        ComPtr<ID3D11Resource> resource;source->GetResource(&resource);
        if(!resource)return fail(Stage::Resource,E_POINTER);
        receipt.source_resource=reinterpret_cast<uintptr_t>(resource.Get());
        ComPtr<ID3D11Texture2D> texture;hr=resource.As(&texture);if(FAILED(hr))return fail(Stage::Resource,hr);
        texture->GetDevice(raw.ReleaseAndGetAddressOf());hr=detail::Identity(raw.Get(),receipt.resource_device,ri,resourceDevice);
        if(FAILED(hr)||di.Get()!=ri.Get())return fail(Stage::Resource,FAILED(hr)?hr:E_INVALIDARG);
        texture->GetDesc(&receipt.source);source->GetDesc(&receipt.source_srv);
        hr=Validate(receipt.source,receipt.source_srv);if(FAILED(hr))return fail(Stage::Shape,hr);
        ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;context->GetPredication(&predicate,&predicateValue);
        if(predicate)return fail(Stage::Predication,E_ACCESSDENIED);
        hr=device->GetDeviceRemovedReason();if(FAILED(hr))return fail(Stage::Device,hr);
        D3D11_TEXTURE2D_DESC owned=receipt.source;owned.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
        owned.Usage=D3D11_USAGE_DEFAULT;owned.BindFlags=D3D11_BIND_SHADER_RESOURCE;owned.CPUAccessFlags=0;owned.MiscFlags=0;
        ComPtr<ID3D11Texture2D> destination;hr=device->CreateTexture2D(&owned,nullptr,&destination);
        if(FAILED(hr))return fail(Stage::Allocate,hr);
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> destinationView;hr=device->CreateShaderResourceView(destination.Get(),&view,&destinationView);
        if(FAILED(hr))return fail(Stage::Allocate,hr);
        // Last operation involving the application: read-only source copy. The
        // void API provides no execution/completion receipt; tests fence/readback.
        context->CopyResource(destination.Get(),texture.Get());
        LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);
        context_=context;context_identity_=ci;device_identity_=di;owner_thread_=GetCurrentThreadId();
        texture_=destination;view_=destinationView;valid_=true;
        receipt.owner_thread=owner_thread_;receipt.owned_resource=reinterpret_cast<uintptr_t>(texture_.Get());
        receipt.owned_view=reinterpret_cast<uintptr_t>(view_.Get());receipt.copy_recorded_qpc=qpc.QuadPart;
        receipt.valid=true;receipt.copy_recorded=true;receipt.stage=Stage::CopyRecorded;receipt.hr=S_OK;return S_OK;
    }
};
}
