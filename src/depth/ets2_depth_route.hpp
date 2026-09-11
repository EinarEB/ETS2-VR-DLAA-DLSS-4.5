#pragma once
#ifdef ETS2_GEOMETRY_TAA_PROBE
#include "geometry_taa_probe.h"
#endif

#include <d3d11_1.h>
#include <bcrypt.h>
#include <map>
#include <memory>
#include <functional>
#include <sstream>
#include <set>
#include <cstdlib>
#include <cmath>
#include "ets2_rain_depth.hpp"
#include "scene_r_depth_contract.h"
#include "resolve_vs_metadata.h"
#include "scene_r_normalize.h"
#include "scene_r_pair.h"
#include "scene_r_publish_contract.h"

// Game-specific route observed in four real ETS2 VR frames. Save depth while
// co-bound with the final HDR scene, then replay ONLY depth into owned targets
// through the game's actual fullscreen geometry and vertex constants. Color
// is never replaced here; final eye order still requires exact pixel matching.
namespace ets2_route {
using Microsoft::WRL::ComPtr;
enum class Shader { unknown, tone, copy, ui, pre_taa, taa, post_taa };
inline std::string Hash(const void* code, size_t size) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return {};
    unsigned char hash[32]{};
    const auto status=BCryptHash(algorithm,nullptr,0,(PUCHAR)code,(ULONG)size,hash,sizeof(hash));
    BCryptCloseAlgorithmProvider(algorithm,0);
    if(status<0)return {};
    static const char hex[]="0123456789abcdef";
    std::string text; for(auto b:hash){text+=hex[b>>4];text+=hex[b&15];}return text;
}
inline Shader Classify(const void* code,size_t size) {
    if(size!=5484 && size!=5596 && size!=6012 && size!=6124 && size!=292 && size!=424 &&
       size!=600 && size!=9424 && size!=9740 && size!=9896 && size!=9580 && size!=1636 && size!=3616 && size!=3728 && size!=4144 && size!=4256 && size!=7496 && size!=6968 && size!=8712 && size!=9240 && size!=9352)return Shader::unknown;
    const auto hash=Hash(code,size);
    if(RainToneMode(hash))return Shader::tone;
    if(hash=="f98057b680db40ab5f7e0659937fea9189454a690ac2cda59208a8c27200b6f7")return Shader::tone;
    // Exact game-archive variants: shaft adds a same-UV light sample, col adds
    // pointwise RGB correction. The t0 coordinates and existing DRR sampling
    // branch are unchanged; replay still follows the actual bound vertex data.
    // Unreviewed DOF/distortion variants remain rejected. The explicitly reviewed rain
    // variants above use a separate coordinate replay, never this plain route.
    if(hash=="879c35aee7c1979dedc90a6ac99be39de3d0eec3c72ecfdd9e204d46e0fc5e8b")return Shader::tone;
    if(hash=="25bba17c86455f666702803c2b80a4f8385fe2fb63e5fdc5e226e5b473c265c4")return Shader::tone;
    if(hash=="b10fcba08c19508c03419bf9ab273eb690e3350f8260e6b0d1bf2c8b2d16420f")return Shader::tone;
    if(hash=="f8262196e3fe80dae9b7e8e10d635120e4c454b60f6b45b58bec38ad6731c22d")return Shader::copy;
    if(hash=="e10bb0dc2040752eb6b580ea60adc8842ae5f097513b03c6c7a5dab7d0c06925")return Shader::ui;
    if(hash=="14055a3b377bd037828f1b2b565194492e70ca7551eb19e023c77f324622f02c")return Shader::tone;
    // Exact non-DRR col variant adds pointwise RGB arithmetic only.
    if(hash=="42ab205e58a5590828574f91b1d56e86b11080024f2545439aa1bf659533cca9")return Shader::tone;
    // Exact non-DRR col+shaft variant observed in the 02:00 game scene.
    // Scene, bloom and shaft samples all use unchanged TEXCOORD0.xy;
    // exposure/color arithmetic does not move the scene's depth coordinates.
    if(hash=="e60c7f232bb8ac1a008d4e36a2d8de932f2a08657af57a6c73da0c246c643d77")return Shader::tone;
    // Exact non-DRR shaft variant: scene t0, bloom t1 and shaft t3 all
    // sample original TEXCOORD0.xy. Remaining operations are pointwise.
    if(hash=="d8858bcde3460ef4a3730ff5b68a4789ef50d58a0e75874dfea78df4678b4467")return Shader::tone;
    if(hash=="72ed8c252903b0c5f6a81a101432e73415401fa44b332e257d2be32c4eba44de")return Shader::pre_taa;
    if(hash=="b5288c9497cc816f8ff8d3e3c046921b6beb5b5cf6425a0979386002daea2bb4")return Shader::post_taa;
    // Ordinary stock TAA variants share current t1 and CB0[8].xy unjitter.
    // Debug variants have a different CB layout and remain unsupported.
    if(hash=="414b3cf32769472d606cb3bb979fa0d272df96a8d96ab47ce7e362d4effc0874"||
       hash=="f0fb529590d289cf76cd2ac0539bb8b27cece7788c2e143f746ad4f2cb457fc7"||
       hash=="88c696a72d1d01bd86bad5237f8433e0facbaef3fc0810353dc87285924af15e"||
       hash=="ee6add879d2288ea7cbdb76a2881b8350a76845842b743eb19ad3f02918f18e5"||
       hash=="cf6590b6305ce610a2c1ba540b60a5883e0e09e566286858f52915c206a2ee05"||
       hash=="d79cca79b6db1650a4d6d22f9e3c73d1d4489166da3d6747b76fd7c30dd5d2bb")return Shader::taa;
    return Shader::unknown;
}
struct Texture {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};
struct GraphicsSave {
    ID3D11DeviceContext* c;
    ID3D11RenderTargetView* rt[8]{};
    ID3D11DepthStencilView* ds=nullptr;
    ID3D11UnorderedAccessView* uav[64]{};
    UINT slots=8,rtCount=8;
    ComPtr<ID3D11PixelShader> ps;
    ID3D11ClassInstance* instances[256]{};UINT instanceCount=256;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;
    ComPtr<ID3D11DepthStencilState> depth;UINT stencil=0;
    explicit GraphicsSave(ID3D11DeviceContext* ctx):c(ctx) {
        ComPtr<ID3D11Device> d;c->GetDevice(&d);if(d->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1)slots=64;
        c->OMGetRenderTargets(8,rt,&ds);
        while(rtCount && !rt[rtCount-1])--rtCount;
        c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,slots,uav);
        c->PSGetShader(&ps,instances,&instanceCount);
        c->PSGetShaderResources(0,1,&srv);c->PSGetSamplers(0,1,&sampler);
        c->OMGetBlendState(&blend,factors,&mask);c->OMGetDepthStencilState(&depth,&stencil);
    }
    void UnbindOutputs(){ID3D11UnorderedAccessView* none[64]{};UINT keep[64];for(auto& n:keep)n=UINT(-1);c->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,slots,none,keep);}
    ~GraphicsSave(){
        UnbindOutputs();
        UINT keep[64];for(auto& n:keep)n=UINT(-1);
        // Render interposers can track OMSetRenderTargets independently of the
        // combined UAV setter. Restore the same binding through that entry point
        // before restoring the complete UAV state and preserving its counters.
        c->OMSetRenderTargets(rtCount,rt,ds);
        c->OMSetRenderTargetsAndUnorderedAccessViews(rtCount,rt,ds,rtCount,slots-rtCount,uav+rtCount,keep+rtCount);
        c->OMSetBlendState(blend.Get(),factors,mask);c->OMSetDepthStencilState(depth.Get(),stencil);
        c->PSSetShader(ps.Get(),instances,instanceCount);
        auto* restoreSRV=srv.Get();c->PSSetShaderResources(0,1,&restoreSRV);auto* s=sampler.Get();c->PSSetSamplers(0,1,&s);
        for(auto* v:rt)if(v)v->Release();if(ds)ds->Release();for(auto* v:uav)if(v)v->Release();
        for(UINT i=0;i<instanceCount;++i)if(instances[i])instances[i]->Release();
    }
};
class Tracker {
    struct Snapshot {ComPtr<ID3D11Texture2D> color;Texture depth;bool valid=false,colorCurrent=false;
    };
    struct Link {ComPtr<ID3D11Texture2D> color;Texture depth,resolveInputDepth;
        ets2_scene_r_depth::SourceStamp resolveInput{};
        ets2_scene_r_normalize::Output normalized;
        ets2_scene_r_normalize::Receipt normalizeReceipt;
        uint64_t normalizedEpoch=UINT64_MAX;
        int source=-1,stage=0;bool valid=false;};
    ets2_scene_r_depth::Extents extents;
    ets2_resolve_vs::Metadata resolveVs;
    std::unique_ptr<ets2_scene_r_normalize::Normalizer> normalizer;
    ets2_scene_r_pair::Publisher scenePublisher;
    DWORD scenePublicationThread=0;
    ID3D11Texture2D* publishedTarget=nullptr;
    uint64_t normalizedSerial=0;
    uint64_t constantsEpoch=UINT64_MAX;
    std::array<Link*,2> collected{};
    ComPtr<ID3D11Device> device;
    UINT writableUavSlots=8;
    ComPtr<ID3D11PixelShader> depthPS,taaDepthPS;
    ComPtr<ID3D11PixelShader> rainDepthPS[RainModeCount];
    ComPtr<ID3D11SamplerState> pointSampler;
    // Snowymoon A/B/C can render an extra full-size HDR scene for each eye.
    // Retain those independent candidates until the verified final routes
    // select their actual sources. Capacity is not an eye-order assumption.
    // Private R evidence contains eight HDR candidates. Preserve the legacy
    // four-snapshot bound in H-only mode; explicit R mode has a finite 16 cap.
    static constexpr size_t MaxSceneSnapshots=16;
    std::array<Snapshot,MaxSceneSnapshots> snapshots;
    std::array<Link,8> links;
    ComPtr<ID3D11Texture2D> boundColor,boundDepth;
#ifdef ETS2_GEOMETRY_TAA_PROBE
    ets2_geometry_taa::Probe geometryProbe;
    D3D11_DEPTH_STENCIL_VIEW_DESC geometryBoundDepthView{};
#endif
    bool boundHDR=false,broken=false;
    unsigned draws=0,w=0,h=0;
    struct ShaderInfo {Shader kind=Shader::unknown;std::string hash;size_t size=0;std::vector<unsigned char> bytes;};
    std::map<uint64_t,ShaderInfo> shaders;
    size_t cachedShaderBytes=0,diagnosticBytes=0;
    unsigned routeDraws[7]{},replays[2]{},invalidations=0,clears=0;
    std::map<std::string,unsigned> reasons;
    std::vector<std::string> events;
    std::string replayFailure;
    uint64_t currentPipeline=0,sequence=0;
    void Event(const char* reason,ID3D11Texture2D* source=nullptr,unsigned vertices=0,unsigned first=0) {
        (void)reason;(void)source;(void)vertices;(void)first;
    }
    bool RejectReplay(const char* reason){replayFailure=reason;return false;}
    void NormalizeResolve(ID3D11DeviceContext* context,ID3D11ShaderResourceView* alignedDepth,Link& destination,
        ID3D11PixelShader* pixel,const std::string& pixelHash,unsigned vertices,unsigned instances,
        unsigned firstVertex,unsigned firstInstance,bool contextLocked)noexcept try{
        destination.normalized={};destination.normalizedEpoch=UINT64_MAX;destination.normalizeReceipt={};
        auto& receipt=destination.normalizeReceipt;
        if(!contextLocked){receipt.stage="context_interval_unavailable";receipt.result=E_ACCESSDENIED;return;}
        if(normalizedSerial==UINT64_MAX){receipt.stage="generation_exhausted";receipt.result=E_FAIL;return;}
        const uint64_t generation=++normalizedSerial;
        ComPtr<ID3D11VertexShader> vertex;context->VSGetShader(&vertex,nullptr,nullptr);
        const auto* entry=resolveVs.Lookup(reinterpret_cast<uint64_t>(vertex.Get()));
        if(!entry||entry->hash!=ets2_scene_r_normalize::VertexHash){receipt.stage="vertex_registry_unproven";receipt.result=E_INVALIDARG;return;}
        if(!normalizer){
            ComPtr<ID3DBlob> code,error;
            HRESULT hr=D3DCompile(ets2_scene_r_normalize::Source,sizeof(ets2_scene_r_normalize::Source)-1,
                "scene-r-normalize",nullptr,nullptr,"normalize","ps_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
            if(FAILED(hr)){receipt.stage="normalize_shader_compile";receipt.result=hr;return;}
            auto created=std::make_unique<ets2_scene_r_normalize::Normalizer>();
            hr=created->Create(device.Get(),code->GetBufferPointer(),code->GetBufferSize());
            if(FAILED(hr)){receipt.stage="normalize_create";receipt.result=hr;return;}
            normalizer=std::move(created);
        }
        ets2_scene_r_normalize::Contract contract;
        contract.verifiedVS=vertex.Get();contract.verifiedPS=pixel;contract.vertexHash=entry->hash.c_str();contract.pixelHash=pixelHash.c_str();
        contract.finalWidth=w;contract.finalHeight=h;contract.vertexCount=vertices;contract.instanceCount=instances;
        contract.firstVertex=firstVertex;contract.firstInstance=firstInstance;contract.generation=generation;contract.callerHoldsLocks=true;
        if(SUCCEEDED(normalizer->Replay(context,alignedDepth,contract,destination.normalized,receipt)))destination.normalizedEpoch=constantsEpoch;
    }catch(...){destination.normalized={};destination.normalizedEpoch=UINT64_MAX;destination.normalizeReceipt.stage="normalize_exception";destination.normalizeReceipt.result=E_FAIL;}
    Texture Make(DXGI_FORMAT format,bool output,ets2_scene_r_depth::Size size) {
        Texture t;D3D11_TEXTURE2D_DESC td{};
        td.Width=size.width;td.Height=size.height;td.MipLevels=1;td.ArraySize=1;td.Format=format;td.SampleDesc.Count=1;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE|(output?D3D11_BIND_RENDER_TARGET:0);
        depth_match::Check(device->CreateTexture2D(&td,nullptr,&t.tex),"route texture");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
        sv.Format=output?DXGI_FORMAT_R32_FLOAT:depth_match::DepthViewFormat(format);
        depth_match::Check(device->CreateShaderResourceView(t.tex.Get(),&sv,&t.srv),"route SRV");
        if(output)depth_match::Check(device->CreateRenderTargetView(t.tex.Get(),nullptr,&t.rtv),"route RTV");
        return t;
    }
    static ets2_scene_r_depth::Size Shape(ID3D11Texture2D* t) {
        if(!t)return {};D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
        if(d.MipLevels!=1||d.ArraySize!=1||d.SampleDesc.Count!=1)return {};
        return {d.Width,d.Height};
    }
    bool Extent(ID3D11Texture2D* t) const {return extents.Accept(Shape(t));}
    bool FinalExtent(ID3D11Texture2D* t) const {return Shape(t)==extents.Final()&&Extent(t);}
    bool PairedExtent(ID3D11Texture2D* a,ID3D11Texture2D* b)const{
        return Extent(a)&&Shape(a)==Shape(b);
    }
    static ComPtr<ID3D11Texture2D> Resource(ID3D11View* view) {
        ComPtr<ID3D11Texture2D> t; if(view){ComPtr<ID3D11Resource> r;view->GetResource(&r);r.As(&t);}return t;
    }
    void SnapshotBound(ID3D11DeviceContext* c) {
        if(!boundHDR||!draws||!PairedExtent(boundColor.Get(),boundDepth.Get()))return;
        draws=0;
        ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;c->GetPredication(&predicate,&predicateValue);
        if(predicate){broken=true;Event("predicated_snapshot_not_proven");return;}
        Snapshot* found=nullptr;
        for(auto& s:snapshots)if(s.valid&&s.color.Get()==boundColor.Get()){found=&s;break;}
        if(found){
            const int identity=int(found-snapshots.data());
            for(const auto& link:links)if(link.valid&&link.source==identity){broken=true;Event("scene_snapshot_rewritten_after_use");return;}
        }
        if(!found)for(unsigned index=0;index<extents.SnapshotLimit();++index)if(!snapshots[index].valid){found=&snapshots[index];break;}
        if(!found){broken=true;Event("snapshot_overflow");return;}
        D3D11_TEXTURE2D_DESC desc{};boundDepth->GetDesc(&desc);auto fmt=depth_match::DepthStorage(desc.Format);
        if(fmt==DXGI_FORMAT_UNKNOWN){broken=true;Event("unsupported_depth_format");return;}
        D3D11_TEXTURE2D_DESC old{};if(found->depth.tex)found->depth.tex->GetDesc(&old);
        if(!found->depth.tex||old.Format!=fmt||Shape(found->depth.tex.Get())!=Shape(boundDepth.Get()))found->depth=Make(fmt,false,Shape(boundDepth.Get()));
        c->CopyResource(found->depth.tex.Get(),boundDepth.Get());found->color=boundColor;found->valid=found->colorCurrent=true;
#ifdef ETS2_GEOMETRY_TAA_PROBE
        geometryProbe.Snapshot(unsigned(found-snapshots.data()),boundColor.Get(),boundDepth.Get(),geometryBoundDepthView);
#endif
        Event("snapshot_saved");
    }
    bool Replay(ID3D11DeviceContext* c,ID3D11ShaderResourceView* source,Texture& dest,const std::function<void()>& draw,bool taa=false,int rain=0) {
        const auto destinationSize=Shape(boundColor.Get());
        if(!extents.Accept(destinationSize))return RejectReplay("destination_extent");
        UINT count=16;D3D11_VIEWPORT vp[16]{};c->RSGetViewports(&count,vp);
        if(count!=1||vp[0].TopLeftX!=0||vp[0].TopLeftY!=0||vp[0].Width!=float(destinationSize.width)||vp[0].Height!=float(destinationSize.height))return RejectReplay("viewport_mismatch");
        // Unusual geometry stages/stream output could have side effects on a replay.
        ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
        c->GSGetShader(&gs,nullptr,nullptr);c->HSGetShader(&hs,nullptr,nullptr);c->DSGetShader(&ds,nullptr,nullptr);
        ID3D11Buffer* so[4]{};c->SOGetTargets(4,so);bool hasSO=false;for(auto* b:so)if(b){hasSO=true;b->Release();}
        if(gs)return RejectReplay("geometry_shader_bound");if(hs)return RejectReplay("hull_shader_bound");if(ds)return RejectReplay("domain_shader_bound");if(hasSO)return RejectReplay("stream_output_bound");
        ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;c->GetPredication(&predicate,&predicateValue);
        if(predicate)return RejectReplay("predicated_draw_not_proven");
        if(rain){
            ComPtr<ID3D11DeviceContext1> c1;ComPtr<ID3D11Buffer> constants;UINT first=0,countConstants=0;
            if(FAILED(c->QueryInterface(IID_PPV_ARGS(&c1))))return RejectReplay("rain_constant_slice_unavailable");
            c1->PSGetConstantBuffers1(0,1,&constants,&first,&countConstants);
            const auto layout=RainLayoutFor(rain);
            if(!layout.constantRow)return RejectReplay("rain_mode_invalid");
            D3D11_BUFFER_DESC bd{};if(constants)constants->GetDesc(&bd);const UINT required=layout.requiredRows;
            if(!constants||countConstants<required||(uint64_t(first)+required)*16>bd.ByteWidth)return RejectReplay("rain_constant_slice_invalid");
            for(UINT slot:{2u,12u,14u}){
                ComPtr<ID3D11ShaderResourceView> view;c->PSGetShaderResources(slot,1,&view);
                auto texture=Resource(view.Get());D3D11_SHADER_RESOURCE_VIEW_DESC vd{};if(view)view->GetDesc(&vd);
                if(!texture||Shape(texture.Get())!=destinationSize||vd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||
                   vd.Texture2D.MostDetailedMip!=0||vd.Texture2D.MipLevels!=1)return RejectReplay("rain_coordinate_resource_unproven");
            }
            ComPtr<ID3D11SamplerState> sampler;c->PSGetSamplers(2,1,&sampler);
            if(!sampler)return RejectReplay("rain_distortion_sampler_missing");
        }
        if(taa){
            ComPtr<ID3D11DeviceContext1> c1;ComPtr<ID3D11Buffer> constants;UINT first=0,countConstants=0;
            if(FAILED(c->QueryInterface(IID_PPV_ARGS(&c1))))return RejectReplay("TAA_constant_slice_unavailable");
            c1->PSGetConstantBuffers1(0,1,&constants,&first,&countConstants);
            D3D11_BUFFER_DESC bd{};if(constants)constants->GetDesc(&bd);
            if(!constants||countConstants<14||uint64_t(first+14)*16>bd.ByteWidth)return RejectReplay("TAA_constant_slice_invalid");
        }
        if(!depthPS){
            const char* code="Texture2D<float> Depth:register(t0); SamplerState Point:register(s0); float main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target {return Depth.SampleLevel(Point,uv,0);}";
            ComPtr<ID3DBlob> blob,error;
            depth_match::Check(D3DCompile(code,strlen(code),"ets2-depth-route",nullptr,nullptr,"main","ps_5_0",0,0,&blob,&error),"route shader");
            depth_match::Check(device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&depthPS),"route PS");
            D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
            depth_match::Check(device->CreateSamplerState(&sd,&pointSampler),"route sampler");
        }
        if(!dest.tex||Shape(dest.tex.Get())!=destinationSize)dest=Make(DXGI_FORMAT_R32_FLOAT,true,destinationSize);
        if(taa&&!taaDepthPS){
            // Keep the game's active PS CB0 slice bound. Its unjitter offset
            // maps nominal TAA output geometry to the current scene sample.
            // No previous/packed TAA depth is substituted for raw scene depth.
            const char* code="Texture2D<float> Depth:register(t0); SamplerState Point:register(s0); cbuffer Taa:register(b0){float4 data[14];} float main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target {return Depth.SampleLevel(Point,uv+data[8].xy,0);}";
            ComPtr<ID3DBlob> blob,error;depth_match::Check(D3DCompile(code,strlen(code),"ets2-taa-current-depth",nullptr,nullptr,"main","ps_5_0",0,0,&blob,&error),"TAA depth shader");
            depth_match::Check(device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&taaDepthPS),"TAA depth PS");
        }
        if(rain&&!rainDepthPS[rain-1]){
            const D3D_SHADER_MACRO defines[]={{"RAIN_CB",RainLayoutFor(rain).constantRow},{nullptr,nullptr}};
            const char* code=RainDepthShader();ComPtr<ID3DBlob> blob,error;
            depth_match::Check(D3DCompile(code,strlen(code),"ets2-rain-nominal-depth",defines,nullptr,"main","ps_5_0",0,0,&blob,&error),"rain depth shader");
            depth_match::Check(device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&rainDepthPS[rain-1]),"rain depth PS");
        }
        GraphicsSave save(c);save.UnbindOutputs();
        // The recorded ETS2 eye depth is reversed Z and clears to zero.
        float farDepth[4]={0,0,0,0};c->ClearRenderTargetView(dest.rtv.Get(),farDepth);
        auto* r=dest.rtv.Get();c->OMSetRenderTargets(1,&r,nullptr);c->OMSetBlendState(nullptr,nullptr,UINT(-1));c->OMSetDepthStencilState(nullptr,0);
        c->PSSetShader(rain?rainDepthPS[rain-1].Get():taa?taaDepthPS.Get():depthPS.Get(),nullptr,0);c->PSSetShaderResources(0,1,&source);
        auto* sampler=pointSampler.Get();c->PSSetSamplers(0,1,&sampler);
        draw();return true;
    }
public:
    void Fail(){ClearScenePair();broken=true;for(auto& l:links)l.valid=false;}
    void ClearScenePair()noexcept{scenePublicationThread=0;publishedTarget=nullptr;
        scenePublisher.Clear();}
    uint64_t ScenePairGeneration()const noexcept{return scenePublisher.Generation();}
    HRESULT AcquireScenePair(const ETS2_SceneRRequestV1* request,ETS2_SceneRPairV1* out)const noexcept{
        if(!out)return E_POINTER;*out={};
        // Only the effect-callback thread that published may acquire. This
        // check precedes Publisher's context/resource COM getters, avoiding an
        // unproven routeMutex -> native-context lock order on foreign threads.
        if(scenePublicationThread&&scenePublicationThread!=GetCurrentThreadId())return E_ACCESSDENIED;
        return scenePublisher.Acquire(request,out);
    }
    HRESULT PublishScenePair(uint64_t runtime,uint64_t runtimeGeneration,ID3D11DeviceContext* context,ID3D11Device* owner,
        ID3D11Texture2D* finalSBS,const depth_match::Result& result,uint64_t epoch,ets2_scene_r_pair::Publication& receipt)noexcept{
        ClearScenePair();receipt={};
        if(broken||!extents.SceneSelected()||!collected[0]||!collected[1]||!normalizer||
           !result.accepted||!result.synchronous||result.proofAge)return E_PENDING;
        ets2_scene_r_publish::CaptureStamp captures[2]{};
        for(unsigned i=0;i<2;++i){auto& link=*collected[i];
            // Match has retired this frame's GPU work. A valid full viewport
            // alone cannot prove the dynamic resolve triangle covered every
            // pixel; require the replay's measured sample count as well.
            const HRESULT coverage=normalizer->ConfirmCoverage(context,link.normalized,link.normalizeReceipt);
            if(coverage!=S_OK)return coverage;
            captures[i]={link.normalizedEpoch,link.normalized.generation,link.valid&&link.stage==2&&link.resolveInput.Valid()&&link.normalized.submitted};}
        if(!ets2_scene_r_publish::FreshPair(result.accepted,result.synchronous,result.proofAge,result.eye,epoch,captures))return E_PENDING;
        ets2_scene_r_pair::Publication input;input.runtime=runtime;input.runtime_generation=runtimeGeneration;input.epoch=epoch;
        input.context=context;input.device=owner;input.final_sbs=finalSBS;
        for(unsigned eye=0;eye<2;++eye){const unsigned candidate=unsigned(result.eye[eye]);const auto& link=*collected[candidate];auto& out=input.eye[eye];
            out.source_color=link.resolveInput.sourceColor;out.resolved_color=link.resolveInput.targetColor;out.resolve_sequence=link.resolveInput.sequence;
            out.color_generation=out.depth_generation=link.normalized.generation;out.candidate_index=candidate;
            out.color=link.normalized.colorSRV.Get();out.depth=link.normalized.depthSRV.Get();}
        const HRESULT hr=scenePublisher.Publish(input);if(SUCCEEDED(hr)){
            scenePublicationThread=GetCurrentThreadId();publishedTarget=finalSBS;receipt=input;
#ifdef ETS2_GEOMETRY_TAA_PROBE
            unsigned sourceIndices[2]={unsigned(collected[unsigned(result.eye[0])]->source),unsigned(collected[unsigned(result.eye[1])]->source)};
            geometryProbe.Pair(context,epoch,runtimeGeneration,sourceIndices);
#endif

        }return hr;
    }
    unsigned SnapshotCount()const{unsigned n=0;for(auto& s:snapshots)n+=s.valid;return n;}
    unsigned OutputCount()const{unsigned n=0;for(auto& l:links)n+=l.valid&&l.stage==2;return n;}
    void SetDevice(ID3D11Device* d){if(!device&&d){device=d;writableUavSlots=d->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64u:8u;}}
    void Pipeline(uint64_t p,const void* code,size_t size){
        DestroyPipeline(p);if(shaders.size()>=8192||size>1024*1024)return;
        ShaderInfo info;info.kind=Classify(code,size);info.hash=Hash(code,size);info.size=size;
        shaders[p]=std::move(info);
    }
    void VertexPipeline(uint64_t p,const void* code,size_t size)noexcept{
        try {if(!code||!size||size>1024*1024){resolveVs.Erase(p);return;}resolveVs.Register(p,code,size,Hash(code,size));
#ifdef ETS2_GEOMETRY_TAA_PROBE
            geometryProbe.ShaderCode(Hash(code,size),code,size);
#endif
        }catch(...){}
    }
    void DestroyPipeline(uint64_t p){
#ifdef ETS2_GEOMETRY_TAA_PROBE
        geometryProbe.EraseLayout(p);
#endif
        resolveVs.Erase(p);auto it=shaders.find(p);if(it!=shaders.end()){cachedShaderBytes-=it->second.bytes.size();shaders.erase(it);}}
    void Extent(unsigned width,unsigned height){
        if(w==width&&h==height)return;
        ClearScenePair();
        w=width;h=height;extents.Final({width,height});snapshots={};links={};Reset();boundColor.Reset();boundDepth.Reset();boundHDR=false;
        if(!w||!h){normalizer.reset();depthPS.Reset();taaDepthPS.Reset();for(auto& shader:rainDepthPS)shader.Reset();pointSampler.Reset();device.Reset();}
    }
    // Explicit opt-in; the final extent and H matching ABI are unchanged.
    // Call before a scene epoch, not after its draws have already been observed.
    bool SceneExtent(unsigned width,unsigned height){
        const ets2_scene_r_depth::Size next{width,height};
        if(next==extents.Scene()&&extents.SceneSelected())return true;
        if(!extents.Scene(next))return false;
        ClearScenePair();
        snapshots={};links={};Reset();boundColor.Reset();boundDepth.Reset();boundHDR=false;
        return true;
    }
    // Borrowed until this epoch's Finish/Reset or any route invalidation. The
    // caller applies Matcher::Result.eye[] to candidate indices, never draw order.
    ID3D11ShaderResourceView* ResolveInputDepth(unsigned candidate)const{
        const auto* link=candidate<collected.size()?collected[candidate]:nullptr;
        return !broken&&link&&link->valid&&link->stage==2&&link->resolveInput.Valid()?link->resolveInputDepth.srv.Get():nullptr;
    }
    ets2_scene_r_depth::SourceStamp ResolveInputSource(unsigned candidate)const{
        return ResolveInputDepth(candidate)?collected[candidate]->resolveInput:ets2_scene_r_depth::SourceStamp{};
    }
    ets2_scene_r_normalize::Receipt NormalizedCoverage(unsigned candidate)const noexcept{
        const auto* link=candidate<collected.size()?collected[candidate]:nullptr;
        return link?link->normalizeReceipt:ets2_scene_r_normalize::Receipt{};
    }
    void ConstantsEpoch(uint64_t epoch){constantsEpoch=epoch;
#ifdef ETS2_GEOMETRY_TAA_PROBE
        geometryProbe.Epoch(epoch);
#endif
    }
#ifdef ETS2_GEOMETRY_TAA_PROBE
    void PollGeometry(ID3D11DeviceContext* context,uint64_t epoch){geometryProbe.Epoch(epoch);geometryProbe.Poll(context);}
    void ProbeLayout(uint64_t id,const ets2_geometry_taa::PositionLayout& layout){geometryProbe.Layout(id,layout);}
    void ProbeGeometry(ID3D11DeviceContext* context,const ets2_geometry_taa::DrawArgs& args){
        if(!extents.SceneSelected())return;
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
        context->VSGetShader(&vs,nullptr,nullptr);context->PSGetShader(&ps,nullptr,nullptr);
        const auto* vertex=resolveVs.Lookup(reinterpret_cast<uint64_t>(vs.Get()));
        const auto pixel=shaders.find(reinterpret_cast<uint64_t>(ps.Get()));
        if(vertex&&pixel!=shaders.end())geometryProbe.Geometry(context,sequence+1,vertex->hash,vertex->generation,
            pixel->second.hash,args,extents.Scene().width,extents.Scene().height);
    }
#endif

    void Reset(){
        ClearScenePair();
        collected={};
        for(auto& s:snapshots){s.valid=false;s.color.Reset();}for(auto& l:links){l.valid=false;l.color.Reset();l.resolveInput={};l.normalized={};l.normalizedEpoch=UINT64_MAX;l.normalizeReceipt={};}draws=0;broken=false;sequence=currentPipeline=0;std::fill(std::begin(routeDraws),std::end(routeDraws),0);std::fill(std::begin(replays),std::end(replays),0);invalidations=clears=0;reasons.clear();events.clear();}
    void Bind(ID3D11DeviceContext* c){
        ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11DepthStencilView> ds;
        c->OMGetRenderTargets(1,&rt,&ds);auto color=Resource(rt.Get());auto depth=Resource(ds.Get());
        ID3D11RenderTargetView* all[8]{};c->OMGetRenderTargets(8,all,nullptr);
        std::array<ComPtr<ID3D11Texture2D>,7> additional;
        bool one=true;for(unsigned i=0;i<8;++i){if(i&&all[i]){one=false;additional[i-1]=Resource(all[i]);}if(all[i])all[i]->Release();}
        bool hdr=false;
        if(PairedExtent(color.Get(),depth.Get())){
            D3D11_TEXTURE2D_DESC cd{};color->GetDesc(&cd);
            hdr=one&&cd.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
        const bool changed=color.Get()!=boundColor.Get()||depth.Get()!=boundDepth.Get()||hdr!=boundHDR;
        if(changed){SnapshotBound(c);
        }
#ifdef ETS2_GEOMETRY_TAA_PROBE
        geometryBoundDepthView={};if(ds)ds->GetDesc(&geometryBoundDepthView);
#endif
        // Only RT0 is followed. Binding an existing tracked color as an
        // additional writable output conservatively revokes its association,
        // even if that particular draw later masks writes or never executes.
        for(const auto& target:additional)if(target){
            for(auto& s:snapshots)if(s.color.Get()==target.Get())s.colorCurrent=false;
            for(auto& l:links)if(l.valid&&l.color.Get()==target.Get()){l.valid=false;Event("tracked_color_bound_as_additional_output",target.Get());}
        }
        if(!changed)return;
        boundColor=std::move(color);boundDepth=std::move(depth);boundHDR=hdr;draws=0;
    }
    void ClearDepth(ID3D11DeviceContext* c,ID3D11Resource* r){if(boundDepth.Get()==r){SnapshotBound(c);}
#ifdef ETS2_GEOMETRY_TAA_PROBE
        geometryProbe.Cleared(r);
#endif
    }
    void ClearColor(ID3D11Resource* r){
        // A consumed snapshot's depth is immutable for this epoch even when
        // the original color is reused as TAA scratch. Direct color lookup is
        // invalidated; derived current-frame links keep their source identity.
        for(auto& s:snapshots)if(s.color.Get()==r)s.colorCurrent=false;
        if(boundColor.Get()==r)draws=0;
        for(auto& l:links)if(l.color.Get()==r){if(l.valid){ClearScenePair();Event("derived_color_cleared",reinterpret_cast<ID3D11Texture2D*>(r),unsigned(l.stage),0);if(l.stage==2)++clears;}l.valid=false;}
        if(publishedTarget==r)ClearScenePair();
    }
    // Writes name resources, not historical source identities. Derived depth
    // already copied from an HDR source survives reuse of that original source.
    void ResourceWritten(ID3D11Resource* r){
        if(!r)return;
        if(boundHDR&&draws&&(boundColor.Get()==r||boundDepth.Get()==r)){
            // No observed paired geometry can account for this pending scene.
            draws=0;Fail();return;
        }
        ClearColor(r);
        for(auto& s:snapshots)if(s.depth.tex.Get()==r){s.valid=s.colorCurrent=false;Fail();return;}
        for(auto& l:links)if(l.valid&&(l.depth.tex.Get()==r||l.resolveInputDepth.tex.Get()==r||
            l.normalized.color.Get()==r||l.normalized.depth.Get()==r)){l.valid=false;ClearScenePair();}
    }
    void WritableOutputs(ID3D11DeviceContext* c,bool compute,bool colorAndDepth){
        const UINT slots=writableUavSlots;
        ID3D11UnorderedAccessView* views[64]{};
        if(compute)c->CSGetUnorderedAccessViews(0,slots,views);
        else c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,slots,views);
        for(auto* v:views)if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);v->Release();ResourceWritten(r.Get());}
        if(colorAndDepth){
            ID3D11RenderTargetView* targets[8]{};ComPtr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(8,targets,&depth);
            for(auto* v:targets)if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);v->Release();ResourceWritten(r.Get());}
            if(depth){ComPtr<ID3D11Resource> r;depth->GetResource(&r);ResourceWritten(r.Get());}
        }
    }
    void Draw(ID3D11DeviceContext* c,unsigned vertices,unsigned instances,unsigned firstVertex,unsigned firstInstance,bool normalizeLocked=false){
        ++sequence;
        if(boundHDR){
            ++draws;return;
        }
        if(!Extent(boundColor.Get()))return;
        Link* destination=nullptr;
        for(auto& l:links)if(l.valid&&l.color.Get()==boundColor.Get()){destination=&l;break;}
        ComPtr<ID3D11PixelShader> ps;c->PSGetShader(&ps,nullptr,nullptr);
        auto found=shaders.find(reinterpret_cast<uint64_t>(ps.Get()));
        auto kind=found==shaders.end()?Shader::unknown:found->second.kind;currentPipeline=reinterpret_cast<uint64_t>(ps.Get());
        ++routeDraws[unsigned(kind)];
        bool trackedCurrent=false;for(const auto& s:snapshots)trackedCurrent=trackedCurrent||(s.valid&&s.colorCurrent&&s.color.Get()==boundColor.Get());
        UINT writeMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(destination||trackedCurrent||kind!=Shader::unknown){
            ComPtr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT sampleMask=0;c->OMGetBlendState(&blend,factors,&sampleMask);
            if(blend){D3D11_BLEND_DESC bd{};blend->GetDesc(&bd);writeMask=bd.RenderTarget[0].RenderTargetWriteMask;}
            // The stock post-tone indexed pass has RT0 writes disabled. It
            // cannot invalidate scene color, regardless of its depth/stencil use.
            if(writeMask==0){if(destination)Event("color_writes_disabled_preserved");return;}
        }
        for(auto& s:snapshots)if(s.color.Get()==boundColor.Get())s.colorCurrent=false;
        // Known UI PS overlays color; it does not remap the scene underneath.
        if(destination&&ets2_scene_r_depth::PreserveOnWrite(unsigned(destination->stage),kind==Shader::ui,writeMask))return;
        if(destination&&kind==Shader::ui)Event("UI_invalidates_intermediate",nullptr,vertices,firstVertex);
        if(destination){Event("derived_color_draw_overwrite",nullptr,vertices,unsigned(destination->stage));if(destination->stage==2)++invalidations;destination->valid=false;}
        if(kind==Shader::unknown||kind==Shader::ui){if(!boundDepth&&vertices>=3&&vertices<=6)Event(found==shaders.end()?"shader_not_registered":"unknown_fullscreen_shader",nullptr,vertices,firstVertex);return;}
        if(writeMask!=D3D11_COLOR_WRITE_ENABLE_ALL){Event("partial_color_write_route_unsupported");return;}
        if(vertices>6||vertices<3||instances!=1||firstInstance!=0){Event("draw_arguments",nullptr,vertices,firstVertex);return;}
        if(boundDepth){Event("depth_still_bound",nullptr,vertices,firstVertex);return;}
        const UINT sourceSlot=(kind==Shader::taa||kind==Shader::post_taa)?1:0;
        ComPtr<ID3D11ShaderResourceView> sourceView;c->PSGetShaderResources(sourceSlot,1,&sourceView);
        auto source=Resource(sourceView.Get());
        const auto sourceSize=Shape(source.Get()),destinationSize=Shape(boundColor.Get());
        if(!extents.Replay(sourceSize,destinationSize,kind==Shader::copy)||source.Get()==boundColor.Get()){
            Event("source_extent_or_alias",source.Get(),vertices,firstVertex);return;
        }
        const bool resolving=extents.Resolves(sourceSize,destinationSize,kind==Shader::copy);
        D3D11_SHADER_RESOURCE_VIEW_DESC sourceDesc{};sourceView->GetDesc(&sourceDesc);
        if(sourceDesc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||sourceDesc.Texture2D.MostDetailedMip!=0||sourceDesc.Texture2D.MipLevels!=1){Event("source_view_subrange");return;}
        if(kind==Shader::taa){
            ID3D11RenderTargetView* views[8]{};c->OMGetRenderTargets(8,views,nullptr);
            auto history=Resource(views[1]);bool layout=views[0]&&views[1]&&Shape(history.Get())==destinationSize&&history.Get()!=boundColor.Get()&&history.Get()!=source.Get();
            for(unsigned i=2;i<8;++i)layout=layout&&!views[i];for(auto* view:views)if(view)view->Release();
            D3D11_TEXTURE2D_DESC hd{},cd{};if(history)history->GetDesc(&hd);boundColor->GetDesc(&cd);
            layout=layout&&hd.Format==DXGI_FORMAT_R16G16B16A16_FLOAT&&cd.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
            if(!layout){Event("TAA_two_output_layout");return;}
        }
        ID3D11ShaderResourceView* depth=nullptr;int identity=-1;
        if(kind==Shader::tone||kind==Shader::pre_taa){for(unsigned i=0;i<snapshots.size();++i)if(snapshots[i].valid&&snapshots[i].colorCurrent&&snapshots[i].color.Get()==source.Get()){depth=snapshots[i].depth.srv.Get();identity=int(i);}}
        const int required=kind==Shader::copy?1:kind==Shader::taa?3:kind==Shader::post_taa?4:kind==Shader::tone?5:-1;
        // With luma sharpening disabled, stock TAA's invert variant goes
        // directly into final tone. Both paths have the same nominal grid.
        if(!depth&&required>=0)for(auto& l:links)if(l.valid&&(l.stage==required||(kind==Shader::tone&&l.stage==4))&&l.color.Get()==source.Get()){depth=l.depth.srv.Get();identity=l.source;}
        if(!depth){Event("missing_current_scene_depth_link",source.Get(),vertices,firstVertex);return;}
        auto depthSource=Resource(depth);
        if(Shape(depthSource.Get())!=sourceSize){Event("source_depth_extent_mismatch",source.Get(),vertices,firstVertex);return;}
#ifdef ETS2_GEOMETRY_TAA_PROBE
        if(kind==Shader::taa&&identity>=0&&normalizeLocked){
            ComPtr<ID3D11VertexShader> vs;c->VSGetShader(&vs,nullptr,nullptr);
            const auto* vertex=resolveVs.Lookup(reinterpret_cast<uint64_t>(vs.Get()));
            if(vertex)geometryProbe.Taa(c,sequence,unsigned(identity),source.Get(),boundColor.Get(),vertex->hash,vertex->generation,found->second.hash);
        }
#endif
        if(!destination)for(auto& l:links)if(!l.valid){destination=&l;break;}
        if(!destination){broken=true;Event("link_overflow");return;}
        const int rain=kind==Shader::tone?RainToneMode(found->second.hash):0;
        destination->resolveInput={};
        destination->normalized={};destination->normalizedEpoch=UINT64_MAX;destination->normalizeReceipt={};
        if(resolving){
            // Preserve this generation before shared R intermediates are reused.
            // The source link has already mapped raw scene depth into R color UVs.
            D3D11_TEXTURE2D_DESC desc{};depthSource->GetDesc(&desc);
            if(!destination->resolveInputDepth.tex||Shape(destination->resolveInputDepth.tex.Get())!=sourceSize){
                destination->resolveInputDepth=Make(DXGI_FORMAT_R32_FLOAT,true,sourceSize);
            }
            if(desc.Format!=DXGI_FORMAT_R32_FLOAT){Event("resolve_depth_format");return;}
            c->CopyResource(destination->resolveInputDepth.tex.Get(),depthSource.Get());
            NormalizeResolve(c,depth,*destination,ps.Get(),found->second.hash,vertices,instances,firstVertex,firstInstance,normalizeLocked);
            Event(destination->normalized.submitted?"scene_R_normalized":destination->normalizeReceipt.stage,source.Get(),vertices,firstVertex);
        }
        if(Replay(c,depth,destination->depth,[&]{c->DrawInstanced(vertices,instances,firstVertex,firstInstance);},kind==Shader::taa,rain)){
            destination->color=boundColor;destination->source=identity;destination->stage=kind==Shader::tone?1:kind==Shader::copy?2:kind==Shader::pre_taa?3:kind==Shader::taa?4:5;destination->valid=true;
            if(resolving)destination->resolveInput={reinterpret_cast<uint64_t>(source.Get()),reinterpret_cast<uint64_t>(boundColor.Get()),sequence,sourceSize};
            if(kind==Shader::tone||kind==Shader::copy)++replays[kind==Shader::tone?0:1];
            Event(rain?"rain_tonemap_nominal_depth":kind==Shader::tone?"tonemap_replayed":kind==Shader::copy?"copy_replayed":kind==Shader::pre_taa?"pre_TAA_current_depth":kind==Shader::taa?"TAA_unjittered_current_depth":"post_TAA_nominal_depth",source.Get(),vertices,firstVertex);
        }
        else Event(replayFailure.c_str(),source.Get(),vertices,firstVertex);
    }
    bool Collect(ID3D11DeviceContext* c,std::unique_ptr<depth_match::Matcher>& matcher,unsigned& mw,unsigned& mh,DXGI_FORMAT& df,uint64_t epoch,uint64_t& serial){
        collected={};SnapshotBound(c);if(broken)return false;
        std::vector<Link*> outputs;for(auto& l:links)if(l.valid&&l.stage==2){
            if(!FinalExtent(l.color.Get())||!FinalExtent(l.depth.tex.Get()))return false;
            outputs.push_back(&l);
        }
        if(outputs.size()!=2||outputs[0]->source==outputs[1]->source)return false;
        const bool anyResolve=outputs[0]->resolveInput.Valid()||outputs[1]->resolveInput.Valid();
        if(anyResolve&&!ets2_scene_r_depth::Pair(outputs[0]->resolveInput,outputs[1]->resolveInput))return false;
        if(!matcher||mw!=w||mh!=h||df!=DXGI_FORMAT_R32_TYPELESS){matcher=std::make_unique<depth_match::Matcher>(device.Get(),c,w,h,DXGI_FORMAT_R32_FLOAT);mw=w;mh=h;df=DXGI_FORMAT_R32_TYPELESS;}
        matcher->Reset();
        // The final eye color rotates through the swapchain; the HDR scene target the
        // depth came from keeps its role between frames and names the candidate.
        for(unsigned outputIndex=0;outputIndex<outputs.size();++outputIndex){
            auto* l=outputs[outputIndex];
            const bool sourced=l->source>=0&&size_t(l->source)<snapshots.size()&&snapshots[size_t(l->source)].color;
            const uint64_t identity=sourced?reinterpret_cast<uint64_t>(snapshots[size_t(l->source)].color.Get()):0;
            if(!matcher->Capture(l->color.Get(),0,l->depth.tex.Get(),0,epoch,++serial,identity))return false;
        }
        collected={outputs[0],outputs[1]};
        return true;
    }
};
}
