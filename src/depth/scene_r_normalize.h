#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <utility>
#include <cmath>

// Private, immediate-context helper. The caller holds the route/reentry guard
// AND the already-protected context lock across validation, replay and restore.
// No context-state swap: the game's actual VS, CB slices and IA stay bound.
namespace ets2_scene_r_normalize {
using Microsoft::WRL::ComPtr;
inline constexpr char VertexHash[]="c213179cf2954afc45f332dbe687ee0a94d91fe95359b3567819310ae9e36bb7";
inline constexpr char PixelHash[]="f8262196e3fe80dae9b7e8e10d635120e4c454b60f6b45b58bec38ad6731c22d";
inline constexpr char Source[]=R"HLSL(
Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
SamplerState Linear : register(s0);
SamplerState Point : register(s1);
struct Result { float4 color:SV_Target0; float depth:SV_Target1; };
Result normalize(float4 position:SV_Position, float2 uv:TEXCOORD0) {
 Result r;
 r.color=Color.SampleLevel(Linear,uv,0);
 r.depth=Depth.SampleLevel(Point,uv,0);
 return r;
}
)HLSL";
struct Contract {
    // Hashes must come from the caller's creation-time bytecode registry; the
    // API cannot recover bytecode from a shader object. Live pointers are checked.
    ID3D11VertexShader* verifiedVS=nullptr;
    ID3D11PixelShader* verifiedPS=nullptr;
    const char* vertexHash=nullptr;
    const char* pixelHash=nullptr;
    UINT finalWidth=0,finalHeight=0;
    UINT vertexCount=3,firstVertex=0,instanceCount=1,firstInstance=0;
    uint64_t generation=0;
    bool callerHoldsLocks=false;
};
inline bool ValidContract(const Contract& c) noexcept {
    return c.callerHoldsLocks&&c.verifiedVS&&c.verifiedPS&&c.vertexHash&&c.pixelHash&&
        std::strcmp(c.vertexHash,VertexHash)==0&&std::strcmp(c.pixelHash,PixelHash)==0&&
        c.finalWidth>0&&c.finalHeight>0&&c.finalWidth<=16384&&c.finalHeight<=16384&&
        c.vertexCount==3&&c.firstVertex==0&&c.instanceCount==1&&c.firstInstance==0&&c.generation!=0;
}
inline bool PlainTexture(const D3D11_TEXTURE2D_DESC& d) noexcept {
    return d.Width>0&&d.Height>0&&d.Width<=16384&&d.Height<=16384&&d.MipLevels==1&&d.ArraySize==1&&
        d.SampleDesc.Count==1&&d.SampleDesc.Quality==0;
}
inline bool PlainView(const D3D11_SHADER_RESOURCE_VIEW_DESC& d,DXGI_FORMAT format) noexcept {
    return d.Format==format&&d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D&&
        d.Texture2D.MostDetailedMip==0&&d.Texture2D.MipLevels==1;
}
inline bool FullViewport(const D3D11_VIEWPORT& v,UINT w,UINT h) noexcept {
    return v.TopLeftX==0&&v.TopLeftY==0&&v.Width==float(w)&&v.Height==float(h)&&v.MinDepth==0&&v.MaxDepth==1;
}
inline bool LinearClamp(const D3D11_SAMPLER_DESC& s) noexcept {
    // The separately enforced one-mip source/view can only select mip zero.
    // Reuse the actual sampler when its finite clamp interval contains zero;
    // the observed game state uses [-FLT_MAX,+FLT_MAX].
    return s.Filter==D3D11_FILTER_MIN_MAG_MIP_LINEAR&&s.AddressU==D3D11_TEXTURE_ADDRESS_CLAMP&&
        s.AddressV==D3D11_TEXTURE_ADDRESS_CLAMP&&s.AddressW==D3D11_TEXTURE_ADDRESS_CLAMP&&
        s.MipLODBias==0&&std::isfinite(s.MinLOD)&&s.MinLOD<=0&&std::isfinite(s.MaxLOD)&&s.MaxLOD>=0;
}
// Additive diagnosis of the LinearClamp predicate. Missing sampler
// is recorded separately; this function never changes the acceptance policy.
inline UINT SamplerRejectBits(const D3D11_SAMPLER_DESC& s) noexcept {
    return (s.Filter!=D3D11_FILTER_MIN_MAG_MIP_LINEAR?1u:0u)|
        (s.AddressU!=D3D11_TEXTURE_ADDRESS_CLAMP?2u:0u)|
        (s.AddressV!=D3D11_TEXTURE_ADDRESS_CLAMP?4u:0u)|
        (s.AddressW!=D3D11_TEXTURE_ADDRESS_CLAMP?8u:0u)|
        (s.MipLODBias!=0?16u:0u)|((!std::isfinite(s.MinLOD)||s.MinLOD>0)?32u:0u)|
        ((!std::isfinite(s.MaxLOD)||s.MaxLOD<0)?64u:0u);
}
// Diagnostic bits only; exactly the existing rasterizer refusal conditions.
inline UINT RasterRejectBits(const D3D11_RASTERIZER_DESC& d)noexcept{
    return (d.ScissorEnable?1u:0u)|(d.FillMode!=D3D11_FILL_SOLID?2u:0u)|
        ((d.CullMode!=D3D11_CULL_NONE&&(d.CullMode!=D3D11_CULL_BACK||d.FrontCounterClockwise))?4u:0u);
}
inline bool FullHScissor(UINT count,const D3D11_RECT* rectangles,UINT width,UINT height)noexcept{
    return count==1&&rectangles&&width>0&&height>0&&width<=16384&&height<=16384&&
        rectangles[0].left==0&&rectangles[0].top==0&&rectangles[0].right==LONG(width)&&rectangles[0].bottom==LONG(height);
}
struct Output {
    ComPtr<ID3D11Texture2D> color,depth;
    ComPtr<ID3D11RenderTargetView> colorRTV,depthRTV;
    ComPtr<ID3D11ShaderResourceView> colorSRV,depthSRV;
    ComPtr<ID3D11Query> coverageQuery;
    uint64_t generation=0;
    UINT width=0,height=0;
    bool submitted=false;
    bool coverageReady=false,coverageComplete=false;
    uint64_t coveredSamples=0;
    bool Empty()const noexcept{return !color&&!depth&&!colorRTV&&!depthRTV&&!colorSRV&&!depthSRV&&!coverageQuery&&!submitted&&!generation;}
    // No pool/reuse. Keep this cohort until all its downstream consumers retire.
    // D3D11 itself retains queued command references; this does not signal or wait.
    void ReleaseAfterRetired()noexcept{*this=Output{};}
};
struct DeviceReceipt {
    uintptr_t raw=0,canonical=0;
    bool unwrapped=false;
    HRESULT unwrapResult=E_PENDING,identityResult=E_PENDING;
};
struct Receipt {
    const char* stage="not_started";
    HRESULT result=E_FAIL;
    bool stateRestored=false,submitted=false;
    UINT sourceWidth=0,sourceHeight=0,vsFirstConstant=0,vsConstantCount=0;
    uintptr_t sourceView=0,sourceResource=0,depthView=0,depthResource=0,vertexShader=0,pixelShader=0;
    uint64_t generation=0;
    DeviceReceipt contextDevice{},colorViewDevice{},colorResourceDevice{},depthViewDevice{},depthResourceDevice{};
    uintptr_t rasterizer=0;
    D3D11_RASTERIZER_DESC rasterizerDesc{};
    UINT rasterRejectBits=0,forcedSamples=0,conservativeRaster=0;
    UINT scissorCount=0;D3D11_RECT scissors[16]{};
    D3D11_VIEWPORT finalViewport{};
    bool fullHScissorAccepted=false;
    bool inputMetadataObserved=false,samplerPresent=false;
    uintptr_t sampler=0,vsBuffer=0;
    D3D11_SAMPLER_DESC samplerDesc{};UINT samplerRejectBits=0;
    D3D11_SHADER_RESOURCE_VIEW_DESC sourceViewDesc{},depthViewDesc{};
    D3D11_TEXTURE2D_DESC sourceTextureDesc{},depthTextureDesc{};
    D3D11_BUFFER_DESC vsBufferDesc{};
    HRESULT vsSliceQuery=E_PENDING;
    HRESULT coverageResult=E_PENDING;
    uint64_t expectedSamples=0,coveredSamples=0;
    bool coverageReady=false,coverageComplete=false;
};
namespace detail {
// Same canonicalization as the independently tested scene-r-color helper:
// exact ReShade unwrap extension followed by canonical IUnknown identity.
// Never substitute adapter identity or accept a failed unwrap query.
inline constexpr GUID UnwrappedObject={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
inline HRESULT Identity(ID3D11Device* raw,DeviceReceipt& r,ComPtr<IUnknown>& identity,ComPtr<ID3D11Device>& normalized) noexcept {
    r.raw=reinterpret_cast<uintptr_t>(raw);if(!raw)return E_POINTER;normalized=raw;ComPtr<IUnknown> unwrapped;
    r.unwrapResult=raw->QueryInterface(UnwrappedObject,reinterpret_cast<void**>(unwrapped.GetAddressOf()));
    if(SUCCEEDED(r.unwrapResult)){if(!unwrapped)return E_NOINTERFACE;const HRESULT hr=unwrapped.As(&normalized);if(FAILED(hr))return hr;r.unwrapped=true;}
    else if(r.unwrapResult!=E_NOINTERFACE)return r.unwrapResult;
    r.identityResult=normalized.As(&identity);r.canonical=reinterpret_cast<uintptr_t>(identity.Get());return r.identityResult;
}
inline bool SameDevice(ID3D11DeviceChild* child,IUnknown* device,DeviceReceipt& receipt) noexcept {
    if(!child)return false;ComPtr<ID3D11Device> raw,normalized;ComPtr<IUnknown> identity;child->GetDevice(&raw);
    return SUCCEEDED(Identity(raw.Get(),receipt,identity,normalized))&&identity&&identity.Get()==device;
}
inline ComPtr<ID3D11Texture2D> Texture(ID3D11View* view) noexcept {
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;
    if(view){view->GetResource(&resource);resource.As(&texture);}return texture;
}
struct Saved {
    ID3D11DeviceContext* context;
    ID3D11RenderTargetView* rt[8]{};ID3D11DepthStencilView* dsv=nullptr;UINT rtCount=8;
    ComPtr<ID3D11PixelShader> ps;ID3D11ClassInstance* classes[256]{};UINT classCount=256;
    ID3D11ShaderResourceView* views[2]{};ID3D11SamplerState* samplers[2]{};
    ComPtr<ID3D11BlendState> blend;float factors[4]{};UINT mask=0;
    ComPtr<ID3D11DepthStencilState> depth;UINT stencil=0;
    D3D11_VIEWPORT viewport[16]{};UINT viewportCount=16;bool armed=false;
    explicit Saved(ID3D11DeviceContext* c) noexcept:context(c){
        c->OMGetRenderTargets(8,rt,&dsv);while(rtCount&&!rt[rtCount-1])--rtCount;
        c->PSGetShader(&ps,classes,&classCount);c->PSGetShaderResources(0,2,views);c->PSGetSamplers(0,2,samplers);
        c->OMGetBlendState(&blend,factors,&mask);c->OMGetDepthStencilState(&depth,&stencil);
        c->RSGetViewports(&viewportCount,viewport);
    }
    void Restore()noexcept{
        if(!armed)return;
        context->OMSetRenderTargets(0,nullptr,nullptr);
        context->PSSetShaderResources(0,2,views);context->PSSetSamplers(0,2,samplers);
        context->PSSetShader(ps.Get(),classes,classCount);
        context->OMSetRenderTargets(rtCount,rt,dsv);
        context->OMSetBlendState(blend.Get(),factors,mask);context->OMSetDepthStencilState(depth.Get(),stencil);
        context->RSSetViewports(viewportCount,viewport);armed=false;
    }
    ~Saved(){Restore();for(auto* r:rt)if(r)r->Release();if(dsv)dsv->Release();
        for(auto* v:views)if(v)v->Release();for(auto* s:samplers)if(s)s->Release();
        for(UINT i=0;i<classCount;++i)if(classes[i])classes[i]->Release();}
};
}
class Normalizer {
    ComPtr<ID3D11Device> device;
    ComPtr<IUnknown> deviceIdentity;
    ComPtr<ID3D11PixelShader> shader;
    ComPtr<ID3D11SamplerState> point;
    ComPtr<ID3D11DepthStencilState> noDepth;
    HRESULT Make(Output& o,UINT w,UINT h) noexcept {
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;d.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
        HRESULT hr=device->CreateTexture2D(&d,nullptr,&o.color);if(FAILED(hr))return hr;
        D3D11_RENDER_TARGET_VIEW_DESC rt{};rt.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;rt.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        hr=device->CreateRenderTargetView(o.color.Get(),&rt,&o.colorRTV);if(FAILED(hr))return hr;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;sv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        hr=device->CreateShaderResourceView(o.color.Get(),&sv,&o.colorSRV);if(FAILED(hr))return hr;
        d.Format=DXGI_FORMAT_R32_FLOAT;hr=device->CreateTexture2D(&d,nullptr,&o.depth);if(FAILED(hr))return hr;
        hr=device->CreateRenderTargetView(o.depth.Get(),nullptr,&o.depthRTV);if(FAILED(hr))return hr;
        hr=device->CreateShaderResourceView(o.depth.Get(),nullptr,&o.depthSRV);if(FAILED(hr))return hr;
        o.width=w;o.height=h;return S_OK;
    }
public:
    // Call only after the existing current-frame match synchronization, while
    // retaining the caller's route/reentry ownership. GetData takes the protected
    // immediate-context lock locally; no multi-call pipeline state is changed. A full
    // viewport does not prove that the pinned VS's dynamic triangle covered it.
    // This reads an 8-byte query with DONOTFLUSH; it never draws, maps or waits.
    HRESULT ConfirmCoverage(ID3D11DeviceContext* c,Output& out,Receipt& r)noexcept{
        r.coverageReady=r.coverageComplete=false;r.expectedSamples=r.coveredSamples=0;
        const auto fail=[&r](const char* stage,HRESULT hr){r.stage=stage;r.coverageResult=hr;return hr;};
        if(!device||!c||!out.submitted||!out.coverageQuery||!out.width||!out.height||
           !out.generation||r.generation!=out.generation)return fail("coverage_arguments",E_INVALIDARG);
        DeviceReceipt contextReceipt;
        if(c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!detail::SameDevice(c,deviceIdentity.Get(),contextReceipt))
            return fail("coverage_context_or_device",E_INVALIDARG);
        ComPtr<ID3D11Multithread> mt;if(FAILED(c->QueryInterface(IID_PPV_ARGS(&mt)))||!mt->GetMultithreadProtected())
            return fail("coverage_context_not_protected",E_INVALIDARG);
        r.expectedSamples=uint64_t(out.width)*out.height;
        if(!out.coverageReady){
            UINT64 samples=0;mt->Enter();
            const HRESULT hr=c->GetData(out.coverageQuery.Get(),&samples,sizeof(samples),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            mt->Leave();
            if(hr==S_FALSE)return fail("coverage_pending",E_PENDING);
            if(FAILED(hr))return fail("coverage_query_failed",hr);
            if(hr!=S_OK)return fail("coverage_query_unexpected",E_FAIL);
            out.coveredSamples=samples;out.coverageReady=true;out.coverageComplete=samples==r.expectedSamples;
        }
        r.coverageReady=out.coverageReady;r.coverageComplete=out.coverageComplete;r.coveredSamples=out.coveredSamples;
        if(!out.coverageComplete)return fail("coverage_incomplete",HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        r.stage="coverage_complete";r.coverageResult=S_OK;return S_OK;
    }
    HRESULT Create(ID3D11Device* d,const void* bytecode,size_t size) noexcept {
        if(device||!d||!bytecode||!size)return E_INVALIDARG;
        DeviceReceipt receipt;ComPtr<ID3D11Device> normalized;ComPtr<IUnknown> identity;
        HRESULT hr=detail::Identity(d,receipt,identity,normalized);if(FAILED(hr)||!identity)return FAILED(hr)?hr:E_NOINTERFACE;
        d=normalized.Get();if(d->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0)return E_INVALIDARG;
        ComPtr<ID3D11PixelShader> p;ComPtr<ID3D11SamplerState> s;ComPtr<ID3D11DepthStencilState> z;
        hr=d->CreatePixelShader(bytecode,size,nullptr,&p);if(FAILED(hr))return hr;
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        hr=d->CreateSamplerState(&sd,&s);if(FAILED(hr))return hr;
        D3D11_DEPTH_STENCIL_DESC zd{};zd.DepthFunc=D3D11_COMPARISON_ALWAYS;
        hr=d->CreateDepthStencilState(&zd,&z);if(FAILED(hr))return hr;
        device=d;deviceIdentity=std::move(identity);shader=std::move(p);point=std::move(s);noDepth=std::move(z);return S_OK;
    }
    HRESULT Replay(ID3D11DeviceContext* c,ID3D11ShaderResourceView* alignedDepth,const Contract& k,Output& out,Receipt& r) noexcept {
        r=Receipt{};r.generation=k.generation;
        const auto fail=[&r](const char* stage,HRESULT hr=E_INVALIDARG){r.stage=stage;r.result=hr;return hr;};
        if(!device||!c||!alignedDepth||!out.Empty()||!ValidContract(k))return fail("arguments");
        if(c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!detail::SameDevice(c,deviceIdentity.Get(),r.contextDevice)||
            !detail::SameDevice(alignedDepth,deviceIdentity.Get(),r.depthViewDevice))return fail("context_or_device");
        ComPtr<ID3D11Multithread> mt;if(FAILED(c->QueryInterface(IID_PPV_ARGS(&mt)))||!mt->GetMultithreadProtected())return fail("context_not_protected");
        detail::Saved save(c);
        // Read-only prerequisite inventory before individual pipeline guards.
        // This lets one refused draw expose sampler, source/view and VS slice
        // facts together. Retained COM bindings remain alive through validation;
        // these getters issue no copy, draw, query, map, flush or state mutation.
        r.inputMetadataObserved=true;r.samplerPresent=save.samplers[0]!=nullptr;
        r.sampler=reinterpret_cast<uintptr_t>(save.samplers[0]);
        if(save.samplers[0]){save.samplers[0]->GetDesc(&r.samplerDesc);r.samplerRejectBits=SamplerRejectBits(r.samplerDesc);}
        auto color=detail::Texture(save.views[0]);auto depth=detail::Texture(alignedDepth);
        D3D11_SHADER_RESOURCE_VIEW_DESC cv{},dv{};
        if(save.views[0])save.views[0]->GetDesc(&cv);alignedDepth->GetDesc(&dv);
        D3D11_TEXTURE2D_DESC cd{},dd{};if(color)color->GetDesc(&cd);if(depth)depth->GetDesc(&dd);
        r.sourceViewDesc=cv;r.depthViewDesc=dv;r.sourceTextureDesc=cd;r.depthTextureDesc=dd;
        r.sourceWidth=cd.Width;r.sourceHeight=cd.Height;
        r.sourceView=reinterpret_cast<uintptr_t>(save.views[0]);r.sourceResource=reinterpret_cast<uintptr_t>(color.Get());
        r.depthView=reinterpret_cast<uintptr_t>(alignedDepth);r.depthResource=reinterpret_cast<uintptr_t>(depth.Get());
        ComPtr<ID3D11DeviceContext1> c1;ComPtr<ID3D11Buffer> cb;UINT first=0,constants=0;D3D11_BUFFER_DESC cbd{};
        r.vsSliceQuery=c->QueryInterface(IID_PPV_ARGS(&c1));
        if(SUCCEEDED(r.vsSliceQuery)){c1->VSGetConstantBuffers1(0,1,&cb,&first,&constants);if(cb)cb->GetDesc(&cbd);}
        r.vsBuffer=reinterpret_cast<uintptr_t>(cb.Get());r.vsBufferDesc=cbd;
        r.vsFirstConstant=first;r.vsConstantCount=constants;
        if(save.classCount||save.ps.Get()!=k.verifiedPS)return fail("pixel_identity_or_classes");
        ComPtr<ID3D11VertexShader> vs;ID3D11ClassInstance* classes[256]{};UINT count=256;c->VSGetShader(&vs,classes,&count);
        for(UINT i=0;i<count;++i)if(classes[i])classes[i]->Release();
        if(count||vs.Get()!=k.verifiedVS)return fail("vertex_identity_or_classes");
        r.vertexShader=reinterpret_cast<uintptr_t>(vs.Get());r.pixelShader=reinterpret_cast<uintptr_t>(save.ps.Get());
        D3D11_PRIMITIVE_TOPOLOGY topology{};c->IAGetPrimitiveTopology(&topology);
        if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return fail("topology");
        ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
        c->GSGetShader(&gs,nullptr,nullptr);c->HSGetShader(&hs,nullptr,nullptr);c->DSGetShader(&ds,nullptr,nullptr);
        if(gs||hs||ds)return fail("extra_shader_stage");
        ID3D11Buffer* so[4]{};c->SOGetTargets(4,so);bool hasSO=false;for(auto* b:so)if(b){hasSO=true;b->Release();}
        if(hasSO)return fail("stream_output");
        ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;c->GetPredication(&predicate,&value);if(predicate)return fail("predication");
        // OMSetRenderTargets can unbind UAVs; refuse them instead of modifying
        // hidden counters or relying on restoration through an interposer.
        ID3D11UnorderedAccessView* uav[64]{};const UINT slots=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64:8;
        c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,slots,uav);
        bool hasUAV=false;for(auto* v:uav)if(v){hasUAV=true;v->Release();}if(hasUAV)return fail("pixel_uav");
        if(save.viewportCount!=1||!FullViewport(save.viewport[0],k.finalWidth,k.finalHeight))return fail("final_viewport");
        if(save.rtCount!=1||!save.rt[0])return fail("final_targets");
        auto target=detail::Texture(save.rt[0]);D3D11_TEXTURE2D_DESC targetDesc{};if(target)target->GetDesc(&targetDesc);
        D3D11_RENDER_TARGET_VIEW_DESC targetView{};save.rt[0]->GetDesc(&targetView);
        if(!target||!PlainTexture(targetDesc)||targetDesc.Width!=k.finalWidth||targetDesc.Height!=k.finalHeight||
            (targetDesc.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS&&targetDesc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)||
            targetView.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||targetView.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||targetView.Texture2D.MipSlice!=0)return fail("final_target_shape");
        ComPtr<ID3D11RasterizerState> raster;c->RSGetState(&raster);
        r.rasterizer=reinterpret_cast<uintptr_t>(raster.Get());r.finalViewport=save.viewport[0];
        r.scissorCount=16;c->RSGetScissorRects(&r.scissorCount,r.scissors);
        // A null state has the documented D3D11 defaults. Actual bound state
        // and rectangles are observed before refusing, without relaxing gates.
        r.rasterizerDesc.FillMode=D3D11_FILL_SOLID;r.rasterizerDesc.CullMode=D3D11_CULL_BACK;r.rasterizerDesc.DepthClipEnable=TRUE;
        if(raster){raster->GetDesc(&r.rasterizerDesc);r.rasterRejectBits=RasterRejectBits(r.rasterizerDesc);
            // Observed live: one scissor exactly equals the full H viewport.
            // The unchanged rectangle also covers the origin-aligned R targets
            // (validated R <= H below). Retain it; do not alter raster state.
            r.fullHScissorAccepted=r.rasterizerDesc.ScissorEnable&&FullHScissor(r.scissorCount,r.scissors,k.finalWidth,k.finalHeight);
            if(r.fullHScissorAccepted)r.rasterRejectBits&=~1u;
            ComPtr<ID3D11RasterizerState1> r1;if(SUCCEEDED(raster.As(&r1))){D3D11_RASTERIZER_DESC1 d{};r1->GetDesc1(&d);r.forcedSamples=d.ForcedSampleCount;}
            ComPtr<ID3D11RasterizerState2> r2;if(SUCCEEDED(raster.As(&r2))){D3D11_RASTERIZER_DESC2 d{};r2->GetDesc2(&d);r.conservativeRaster=UINT(d.ConservativeRaster);}
            if(r.rasterRejectBits)return fail("rasterizer");
            if(r.forcedSamples)return fail("forced_samples");
            if(r.conservativeRaster!=UINT(D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF))return fail("conservative_raster");
        }
        if(save.mask!=UINT(-1))return fail("sample_mask");
        if(save.blend){D3D11_BLEND_DESC b{};save.blend->GetDesc(&b);if(b.AlphaToCoverageEnable)return fail("alpha_to_coverage");
            for(UINT i=0;i<(b.IndependentBlendEnable?8u:1u);++i)if(b.RenderTarget[i].BlendEnable||b.RenderTarget[i].RenderTargetWriteMask!=D3D11_COLOR_WRITE_ENABLE_ALL)return fail("blend_or_write_mask");
            ComPtr<ID3D11BlendState1> b1;if(SUCCEEDED(save.blend.As(&b1))){D3D11_BLEND_DESC1 d{};b1->GetDesc1(&d);
                for(UINT i=0;i<(d.IndependentBlendEnable?8u:1u);++i)if(d.RenderTarget[i].LogicOpEnable)return fail("logic_op");}
        }
        if(!save.views[0]||!save.samplers[0])return fail("color_or_sampler_missing");
        if(!LinearClamp(r.samplerDesc))return fail("linear_clamp_sampler");
        if(!color||!depth||color.Get()==depth.Get()||color.Get()==target.Get()||depth.Get()==target.Get()||
            !detail::SameDevice(save.views[0],deviceIdentity.Get(),r.colorViewDevice)||
            !detail::SameDevice(color.Get(),deviceIdentity.Get(),r.colorResourceDevice)||!detail::SameDevice(depth.Get(),deviceIdentity.Get(),r.depthResourceDevice)||
            !PlainTexture(cd)||!PlainTexture(dd)||cd.Width!=dd.Width||cd.Height!=dd.Height||cd.Width>k.finalWidth||cd.Height>k.finalHeight||
            (cd.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS&&cd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)||
            (dd.Format!=DXGI_FORMAT_R32_FLOAT&&dd.Format!=DXGI_FORMAT_R32_TYPELESS)||
            !PlainView(cv,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)||!PlainView(dv,DXGI_FORMAT_R32_FLOAT))return fail("source_shape_or_encoding");
        if(FAILED(r.vsSliceQuery))return fail("vs_slice_unavailable");
        if(!cb||constants<2||(uint64_t(first)+2)*16>cbd.ByteWidth)return fail("vs_slice_bounds");
        Output fresh;const HRESULT made=Make(fresh,cd.Width,cd.Height);if(FAILED(made))return fail("create_outputs",made);
        const D3D11_QUERY_DESC queryDesc{D3D11_QUERY_OCCLUSION,0};
        const HRESULT queryCreated=device->CreateQuery(&queryDesc,&fresh.coverageQuery);
        if(FAILED(queryCreated))return fail("create_coverage_query",queryCreated);
        fresh.generation=k.generation;
        // All validation/allocation precedes any replay command. No app target is
        // written. Clears give deterministic black/far-depth outside a dynamic
        // position rectangle; they do not assert the rectangle was full screen.
        save.armed=true;c->OMSetRenderTargets(0,nullptr,nullptr);
        const float zero[4]{};c->ClearRenderTargetView(fresh.colorRTV.Get(),zero);c->ClearRenderTargetView(fresh.depthRTV.Get(),zero);
        ID3D11RenderTargetView* rt[]={fresh.colorRTV.Get(),fresh.depthRTV.Get()};c->OMSetRenderTargets(2,rt,nullptr);
        c->OMSetBlendState(nullptr,nullptr,UINT(-1));c->OMSetDepthStencilState(noDepth.Get(),0);
        const D3D11_VIEWPORT viewport{0,0,float(cd.Width),float(cd.Height),0,1};c->RSSetViewports(1,&viewport);
        ID3D11ShaderResourceView* inputs[]={save.views[0],alignedDepth};c->PSSetShaderResources(0,2,inputs);
        ID3D11SamplerState* samplers[]={save.samplers[0],point.Get()};c->PSSetSamplers(0,2,samplers);
        c->PSSetShader(shader.Get(),nullptr,0);
        // Count just this single nonoverlapping triangle. The guards above
        // enforce one sample/pixel, solid raster, no forced/conservative samples,
        // no extra geometry stages, no predication and full sample/write masks;
        // depth/stencil are disabled here and this PS does not discard. Thus an
        // area-sized count proves every output pixel was written exactly once.
        c->Begin(fresh.coverageQuery.Get());c->Draw(3,0);c->End(fresh.coverageQuery.Get());
        fresh.submitted=true;save.Restore();r.stateRestored=true;r.submitted=true;
        out=std::move(fresh);r.stage="submitted";r.result=S_OK;return S_OK;
    }
};
}
