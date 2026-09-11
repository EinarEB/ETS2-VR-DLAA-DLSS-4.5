// SPDX-License-Identifier: MIT
// Dedicated native-resolution stereo DLAA integration. No neural-rendering route.
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <reshade.hpp>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>
#include "../standalone/native_sr.h"
#include "embedded_assets.h"
#include "control_sync.h"
#include "settings_overlay.h"
#include "producer_control.h"
#include "input_capture.h"
#include "../feeder/feed_scene_r_client.h"
#include "../feeder/feed_context_protection.h"
#include "../feeder/feed_d3d11_context.h"
#include "../feeder/stereo_motion_history.h"

BOOL DepthLifecycle(HMODULE module,DWORD reason,LPVOID reserved);
extern "C" __declspec(dllexport) const char* NAME="ETS2 VR DLAA — DLSS 4.5";
extern "C" __declspec(dllexport) const char* DESCRIPTION="Native-resolution anti-aliasing for Euro Truck Simulator 2 in VR.";
extern "C" __declspec(dllexport) const char* VERSION="1.0";

namespace {
using Microsoft::WRL::ComPtr;
using namespace reshade::api;
namespace fs=std::filesystem;
HMODULE self=nullptr;
constexpr const char* Effect="ETS2_DLAA.addonfx";
constexpr const char* MotionEffect="ETS2_VortStereo.addonfx";
struct Failure {const char* reason;HRESULT hr;};
void Check(HRESULT hr,const char* reason){if(FAILED(hr))throw Failure{reason,hr};}
void Need(bool ok,const char* reason){if(!ok)throw Failure{reason,E_INVALIDARG};}
bool Vr(effect_runtime* rt){return rt&&!rt->get_hwnd()&&rt->get_device()->get_api()==device_api::d3d11;}
const char* Bool(bool value){return value?"true":"false";}
DXGI_FORMAT ColorFormat(DXGI_FORMAT f){
    switch(f){
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:return DXGI_FORMAT_UNKNOWN;
    }
}
bool Uniform(effect_runtime* rt,const char* file,const char* name){
    bool value=false;const auto u=rt->find_uniform_variable(file,name);
    if(u.handle)rt->get_uniform_value_bool(u,&value,1);return u.handle&&value;
}
void MotionFlag(effect_runtime* rt,bool value){
    const auto u=rt->find_uniform_variable(Effect,"ETS2_MOTION_HISTORY_VALID");
    if(u.handle)rt->set_uniform_value_bool(u,&value,1);
}
constexpr char Shaders[]=R"HLSL(
cbuffer Settings:register(b0){uint eyeWidth,height;float blend;uint padding;};
float4 vs(uint id:SV_VertexID):SV_Position {
 float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
Texture2D<float4> source0:register(t0);
Texture2D<float4> source1:register(t1);
Texture2D<float4> source2:register(t2);
Texture2D<float> source3:register(t3);
struct Inputs {float4 color:SV_Target0;float depth:SV_Target1;};
Inputs inputs(float4 position:SV_Position){
 uint2 pixel=uint2(position.xy),p=pixel;uint eye=p.x/eyeWidth;p.x-=eye*eyeWidth;
 Inputs o;o.color=source0.Load(int3(pixel,0));
 o.depth=eye==0?source2.Load(int3(p,0)).x:source3.Load(int3(p,0));
 if(!all(isfinite(o.color))||!isfinite(o.depth))discard;return o;
}
float4 composite(float4 position:SV_Position):SV_Target {
 uint2 p=uint2(position.xy);float4 original=source0.Load(int3(p,0));
 if(!isfinite(blend)||blend<=0||blend>1)return original;
 uint lw,lh,rw,rh;source1.GetDimensions(lw,lh);source2.GetDimensions(rw,rh);
 if(lw!=eyeWidth||rw!=eyeWidth||lh!=height||rh!=height)return original;
 uint eye=p.x/eyeWidth;p.x-=eye*eyeWidth;
 float3 processed=eye==0?source1.Load(int3(p,0)).rgb:source2.Load(int3(p,0)).rgb;
 if(!all(isfinite(processed))||!all(isfinite(original.rgb)))return original;
 float3 result=lerp(original.rgb,processed,blend);
 if(!all(isfinite(result)))return original;return float4(saturate(result),original.a);
}
)HLSL";
struct Texture {
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11RenderTargetView> rtv;
};
struct State {
    bool desktopEffectsAllowed=true;
    effect_runtime* runtime=nullptr;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ets2_d3d11::ProtectionRegistry protection;ets2_d3d11::PrivateState privateState;
    ets2_scene_r_client::Client producer;
    ets2_scene_r_client::Sequence sequence;ets2_motion::HistoryGate history;
    ets2_scene_r_client::Pair pendingPair;
    ComPtr<ID3D11Texture2D> pendingTarget,pendingInputTarget;
    std::unique_ptr<ets2_native::NativeSr> sr;
    Texture color,depth,original,output;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> convert,compose;
    ComPtr<ID3D11Buffer> constants;
    ets2_native::EvaluationReceipt attempt{};
    ets2_input_capture::Capture inputCapture;
    uint64_t vortObservation=0,vortExpectedCallback=0;
    uint32_t vortFrame=0;
    unsigned eyeWidth=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    bool resolved=false,initialized=false,failed=false,reset=true,delivered=false,enabled=false,controlsAvailable=false;
    int requestedPreset=13,activePreset=-1;
    uint64_t callback=0,deliveredFrame=0,deliveredTick=0,deliveredSerial=0,serial=1,runtimeGeneration=0;
    float deliveredBlend=0;
    const char* detail="waiting-for-motion-and-depth";
};
struct Host {
    std::recursive_mutex mutex;std::unique_ptr<State> state;
    std::atomic<bool> missed{false};fs::path directory;
    uint64_t callback=0,session=0;bool initialized=false,quarantined=false;
};
Host& Global(){static Host* host=new Host;return *host;} // No loader-lock GPU teardown.
void Paths(){
    auto& h=Global();if(h.initialized)return;
    wchar_t module[32768]{};const DWORD n=GetModuleFileNameW(self,module,32768);
    Need(n&&n<32768,"module-path");h.directory=fs::path(module).parent_path();
    FILETIME created{},exited{},kernel{},user{};Need(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)!=FALSE,"process-session");
    h.session=(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;h.initialized=true;
}
void Log(const char* reason,HRESULT hr=S_OK){
    std::ofstream file(Global().directory/L"ets2-dlaa.log",std::ios::app);
    if(file)file<<GetTickCount64()<<" "<<reason<<" hr=0x"<<std::hex<<unsigned(hr)<<"\n";
}
void Status(State& s){
    const uint64_t now=GetTickCount64();
    const bool current=s.delivered&&s.attempt.success&&s.attempt.calls==2&&
        s.attempt.handles[0]&&s.attempt.handles[1]&&s.attempt.handles[0]!=s.attempt.handles[1];
    const char* reason=s.failed?"processing_failed":!s.enabled?"processing_disabled":
        current?"delivered":"waiting_for_neural_frame";
    std::ostringstream j;j<<std::setprecision(9)<<"{\"schema\":1,\"pid\":"<<GetCurrentProcessId()<<",\"session\":\""<<Global().session
      <<"\",\"tick\":"<<now<<",\"runtime_generation\":"<<s.runtimeGeneration
      <<",\"selected_blend\":"<<(s.enabled?1:0)<<",\"selected_serial\":"<<s.serial<<",\"temporary\":false"
      <<",\"delivered_blend\":"<<s.deliveredBlend<<",\"delivered_serial\":"<<s.deliveredSerial
      <<",\"delivered_frame\":"<<s.deliveredFrame<<",\"delivered_tick\":"<<s.deliveredTick
      <<",\"delivery_this_present\":"<<Bool(current)<<",\"reason\":\""<<reason<<"\",\"key\":0"
      <<",\"startup_remaining_ms\":0,\"startup_factor\":"<<(current?1:0)
      <<",\"nr_backend\":-1,\"requested_nr_backend\":-1,\"pipeline\":{\"neural_passes\":0,\"post_sr_active\":"<<Bool(s.initialized)
      <<",\"post_sr_ready\":"<<Bool(s.initialized&&!s.failed)<<",\"sr_only\":true,\"sr_current_pair\":"<<Bool(current)
      <<",\"sr_evaluation_calls\":"<<s.attempt.calls<<",\"sr_callback\":"<<s.callback
      <<",\"sr_feature_handles\":["<<s.attempt.handles[0]<<','<<s.attempt.handles[1]<<"],\"r_extent\":["<<s.eyeWidth<<','<<s.height
      <<"],\"h_extent\":["<<s.eyeWidth<<','<<s.height<<"],\"requested_sr_preset\":"<<s.requestedPreset
      <<",\"active_sr_preset_request\":"<<s.activePreset<<",\"effective_sr_preset\":-1,\"sr_quality\":5},\"detail\":"<<std::quoted(s.detail)<<"}\n";
    const auto pending=Global().directory/L"preview-status.json.part",target=Global().directory/L"preview-status.json";
    std::ofstream file(pending,std::ios::binary|std::ios::trunc);file<<j.str();file.close();
    if(file)MoveFileExW(pending.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING);
}
bool Retire(State& s){
    if(s.sr&&!s.sr->Shutdown()){s.failed=true;s.detail="unknown-retirement-retained";return false;}
    s.pendingPair.Reset();s.pendingTarget.Reset();s.pendingInputTarget.Reset();s.initialized=false;s.activePreset=-1;
    s.color={};s.depth={};s.original={};s.output={};s.vertex.Reset();s.convert.Reset();s.compose.Reset();s.constants.Reset();
    s.sr.reset();s.privateState.Release();s.eyeWidth=s.height=0;s.format=DXGI_FORMAT_UNKNOWN;return true;
}
void Skip(State& s,const char* reason){s.inputCapture.Interrupted(reason);s.reset=true;s.delivered=false;s.detail=reason;}
void Controls(State& s){
    const auto technique=s.runtime->find_technique(Effect,"ETS2_DLAA");
    const bool enabled=s.desktopEffectsAllowed&&technique.handle&&s.runtime->get_effects_state()&&s.runtime->get_technique_state(technique);
    const auto uniform=s.runtime->find_uniform_variable(Effect,"ETS2_DLAA_PRESET");
    int index=-1;if(uniform.handle)s.runtime->get_uniform_value_int(uniform,&index,1);
    s.controlsAvailable=uniform.handle&&index>=0&&index<=2;
    const int requested=s.controlsAvailable?13-index:s.requestedPreset;
    if(enabled!=s.enabled||requested!=s.requestedPreset){
        s.enabled=enabled;s.requestedPreset=requested;++s.serial;s.attempt={};
        s.sequence.InvalidateHistory();Skip(s,enabled?"model-or-technique-change-pending":"technique-disabled");
    }
    if(!s.controlsAvailable){s.attempt={};Skip(s,"preset-control-unavailable");}
    ets2_dlaa_producer::Request(s.enabled&&s.controlsAvailable&&!s.failed);
}
void MotionDependency(State& s){
    const auto motion=s.runtime->find_technique(MotionEffect,"ETS2_VortStereo");
    const auto dlaa=s.runtime->find_technique(Effect,"ETS2_DLAA");
    if(!motion.handle||!dlaa.handle)return;
    bool changed=false;
    if(s.runtime->get_technique_state(motion)!=s.enabled){s.runtime->set_technique_state(motion,s.enabled);changed=true;}
    std::vector<effect_technique> order;
    s.runtime->enumerate_techniques(nullptr,[](effect_runtime*,effect_technique technique,void* data){
        static_cast<std::vector<effect_technique>*>(data)->push_back(technique);
    },&order);
    const auto isMotion=[&](effect_technique t){return t.handle==motion.handle;};
    const auto isDlaa=[&](effect_technique t){return t.handle==dlaa.handle;};
    auto m=std::find_if(order.begin(),order.end(),isMotion),d=std::find_if(order.begin(),order.end(),isDlaa);
    if(order.size()>=2&&m!=order.end()&&d!=order.end()&&
       (order[0].handle!=motion.handle||order[1].handle!=dlaa.handle)){
        // Motion reads COLOR while SR reads the pinned pre-effects original.
        // Keep these first; erase preserves every other technique's order.
        order.erase(std::remove_if(order.begin(),order.end(),[&](effect_technique t){
            return isMotion(t)||isDlaa(t);
        }),order.end());
        order.insert(order.begin(),{motion,dlaa});
        s.runtime->reorder_techniques(order.size(),order.data());changed=true;
    }
    if(changed){s.history.Reset(reinterpret_cast<uintptr_t>(s.runtime));MotionFlag(s.runtime,false);
        s.attempt={};s.sequence.InvalidateHistory();Skip(s,"motion-prerequisite-synchronized");}
}
void MakeTexture(State& s,Texture& out,unsigned width,unsigned height,DXGI_FORMAT format){
    D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=format;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    Check(s.device->CreateTexture2D(&d,nullptr,&out.texture),"owned-texture");
    Check(s.device->CreateShaderResourceView(out.texture.Get(),nullptr,&out.srv),"owned-srv");
    Check(s.device->CreateRenderTargetView(out.texture.Get(),nullptr,&out.rtv),"owned-rtv");
}
ComPtr<ID3DBlob> Compile(const char* entry,const char* profile){
    ComPtr<ID3DBlob> code,error;Check(D3DCompile(Shaders,sizeof(Shaders)-1,"ETS2-DLAA",nullptr,nullptr,entry,profile,
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error),"native-shader-compile");return code;
}
void Resources(State& s,unsigned eyeWidth,unsigned height,DXGI_FORMAT format){
    if(s.initialized&&s.eyeWidth==eyeWidth&&s.height==height&&s.format==format){
        if(s.activePreset!=s.requestedPreset){
            // The host mutex excludes new output consumers. No borrowed output
            // view survives here; ChangePreset retires the prior composition.
            s.attempt={};s.initialized=false;s.activePreset=-1;Skip(s,"model-change-pending");
            const HRESULT changed=s.sr->ChangePreset(s.requestedPreset);
            if(FAILED(changed))throw Failure{s.sr->Reason(),changed};
            s.pendingPair.Reset();s.pendingTarget.Reset();s.pendingInputTarget.Reset();
            s.initialized=true;s.activePreset=s.requestedPreset;Log(s.sr->Reason());
        }return;
    }
    Need(Retire(s),"rebuild-retirement");s.eyeWidth=eyeWidth;s.height=height;s.format=format;s.reset=true;
    s.sr=std::make_unique<ets2_native::NativeSr>();
    const HRESULT initialized=s.sr->Initialize(s.device.Get(),eyeWidth,height,Global().directory.wstring(),s.requestedPreset);
    if(FAILED(initialized))throw Failure{s.sr->Reason(),initialized};s.initialized=true;s.activePreset=s.requestedPreset;
    MakeTexture(s,s.color,eyeWidth*2,height,DXGI_FORMAT_R16G16B16A16_FLOAT);
    MakeTexture(s,s.depth,eyeWidth*2,height,DXGI_FORMAT_R32_FLOAT);
    MakeTexture(s,s.original,eyeWidth*2,height,format);MakeTexture(s,s.output,eyeWidth*2,height,format);
    auto vs=Compile("vs","vs_5_0"),ps=Compile("inputs","ps_5_0"),compose=Compile("composite","ps_5_0");
    Check(s.device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&s.vertex),"vertex-shader");
    Check(s.device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&s.convert),"input-shader");
    Check(s.device->CreatePixelShader(compose->GetBufferPointer(),compose->GetBufferSize(),nullptr,&s.compose),"composition-shader");
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    Check(s.device->CreateBuffer(&cb,nullptr,&s.constants),"constants");Log(s.sr->Reason());
}
void DrawState(State& s,float blend){
    struct Constants {unsigned width,height;float blend;unsigned pad;} constants{s.eyeWidth,s.height,blend,0};
    auto* ctx=s.context.Get();ctx->UpdateSubresource(s.constants.Get(),0,nullptr,&constants,0,0);
    ID3D11Buffer* cb=s.constants.Get();ctx->PSSetConstantBuffers(0,1,&cb);
    D3D11_VIEWPORT viewport{0,0,float(s.eyeWidth*2),float(s.height),0,1};ctx->RSSetViewports(1,&viewport);
    ctx->OMSetBlendState(nullptr,nullptr,0xffffffff);ctx->OMSetDepthStencilState(nullptr,0);ctx->RSSetState(nullptr);
    ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(s.vertex.Get(),nullptr,0);ctx->GSSetShader(nullptr,nullptr,0);ctx->HSSetShader(nullptr,nullptr,0);ctx->DSSetShader(nullptr,nullptr,0);
}
ComPtr<ID3D11Texture2D> Guide(State& s,const char* name,DXGI_FORMAT format,unsigned width,unsigned height,const char* effect=Effect){
    const auto variable=s.runtime->find_texture_variable(effect,name);Need(variable.handle!=0,"guide-variable");
    resource_view normal{},srgb{};s.runtime->get_texture_binding(variable,&normal,&srgb);Need(normal.handle!=0,"guide-binding");
    ComPtr<ID3D11ShaderResourceView> view=reinterpret_cast<ID3D11ShaderResourceView*>(normal.handle);
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};view->GetDesc(&v);
    Need(v.Format==format&&v.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D&&!v.Texture2D.MostDetailedMip&&v.Texture2D.MipLevels==1,"guide-view");
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;Check(resource.As(&texture),"guide-texture");
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);ComPtr<IUnknown> device;
    Check(ets2_scene_r_client::detail::DeviceIdentity(s.device.Get(),device),"device-identity");
    Check(ets2_scene_r_client::detail::SameDevice(texture.Get(),device.Get()),"guide-device");
    Need(d.Width==width&&d.Height==height&&d.Format==format&&d.MipLevels==1&&d.ArraySize==1&&d.SampleDesc.Count==1&&
         d.SampleDesc.Quality==0&&d.Usage==D3D11_USAGE_DEFAULT&&!d.CPUAccessFlags,"guide-description");return texture;
}
void Process(State& s,command_list* commands,resource_view rtv){
    auto* ctx=reinterpret_cast<ID3D11DeviceContext*>(commands->get_native());
    Need(ctx==s.context.Get()&&ctx->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE,"callback-context");
    Controls(s);if(s.failed||!s.enabled||!s.controlsAvailable){
        Skip(s,s.failed?"processing-failed":!s.enabled?"technique-disabled":"preset-control-unavailable");return;}
    if(!s.history.ready||!Uniform(s.runtime,Effect,"ETS2_MOTION_HISTORY_VALID")||
       !Uniform(s.runtime,Effect,"ETS2_STEREO_DEPTH_VALID")){Skip(s,"current-motion-or-depth-unavailable");return;}
    Need(rtv.handle!=0,"current-output-view");ComPtr<ID3D11RenderTargetView> view=reinterpret_cast<ID3D11RenderTargetView*>(rtv.handle);
    D3D11_RENDER_TARGET_VIEW_DESC v{};view->GetDesc(&v);ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> target;Check(resource.As(&target),"current-output-texture");D3D11_TEXTURE2D_DESC d{};target->GetDesc(&d);
    const auto format=ColorFormat(d.Format);
    Need(format!=DXGI_FORMAT_UNKNOWN&&v.Format==format&&v.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D&&!v.Texture2D.MipSlice&&
         !(d.Width&1)&&d.Width>=256&&d.Height>=128&&d.MipLevels==1&&d.ArraySize==1&&d.SampleDesc.Count==1&&d.SampleDesc.Quality==0,"native-output-description");
    auto motion=Guide(s,"ETS2_DLAA_Motion",DXGI_FORMAT_R16G16_FLOAT,d.Width,d.Height);
    auto matchedDepth=Guide(s,"ETS2_DLAA_Depth",DXGI_FORMAT_R32_FLOAT,d.Width,d.Height);
    ets2_scene_r_client::Receipt acquired;
    if(!s.resolved){const HRESULT hr=s.producer.ResolveLoaded(acquired);if(FAILED(hr)){Skip(s,"producer-not-ready");return;}s.resolved=true;}
    const ETS2_SceneRRequestV1 request{sizeof(ETS2_SceneRRequestV1),ETS2_SCENE_R_PAIR_ABI_VERSION,
        reinterpret_cast<uint64_t>(s.runtime),ctx,s.device.Get(),target.Get()};
    ets2_scene_r_client::Pair pair;
    if(FAILED(s.producer.Acquire(request,false,pair,acquired))){Skip(s,"current-original-pair-unavailable");return;}
    const auto& p=pair.Get();Need(p.scene_width==p.final_eye_width&&p.scene_height==p.final_height,"native-R-must-equal-H");
    ets2_scene_r_client::Sequence::Ticket ticket;
    if(FAILED(s.sequence.Prepare(request.runtime,p,ticket))){Skip(s,"duplicate-or-discontinuous-pair");return;}
    // Native DLAA consumes the matched resolved image and measured image motion.
    // No unrelated HDR constant-buffer observation supplies jitter or resets.
    // The exact current pair and sequence ticket remain mandatory.
    // No private state/context lock has been entered before pair acquisition.
    Resources(s,p.scene_width,p.scene_height,format);s.runtimeGeneration=p.runtime_generation;
    const bool reset=s.reset||ticket.ResetNeeded();
    s.pendingPair=std::move(pair);s.pendingInputTarget=target;
    {
        ets2_d3d11::Scope scope(s.privateState,ctx);Check(scope.Status(),"input-state-scope");
        ctx->CopyResource(s.original.texture.Get(),target.Get());DrawState(s,0);
        constexpr float zero[4]{};ctx->ClearRenderTargetView(s.color.rtv.Get(),zero);ctx->ClearRenderTargetView(s.depth.rtv.Get(),zero);
        ID3D11RenderTargetView* outputs[]={s.color.rtv.Get(),s.depth.rtv.Get()};ctx->OMSetRenderTargets(2,outputs,nullptr);
        // Native R=H: use the completed SBS image, including late game overlays,
        // at the same boundary as Vort's COLOR input. The matched current pair
        // still supplies scene depth; it does not supply overlay-specific depth.
        const auto& owned=s.pendingPair.Get();ID3D11ShaderResourceView* sources[]={s.original.srv.Get(),nullptr,owned.eye[0].depth,owned.eye[1].depth};
        ctx->PSSetShader(s.convert.Get(),nullptr,0);ctx->PSSetShaderResources(0,4,sources);ctx->Draw(3,0);
    }
    // Diagnostic copies are before NativeSr's input Signal; readback is after
    // its successful retirement. The capture never changes the selected inputs.
    if(s.inputCapture.WantsEpoch(s.pendingPair.Get().epoch))try{
        const auto& owned=s.pendingPair.Get();ets2_input_capture::FrameIdentity identity;
        identity.callback=s.callback;identity.epoch=owned.epoch;identity.pairGeneration=owned.generation;
        identity.runtimeGeneration=owned.runtime_generation;identity.runtime=reinterpret_cast<uint64_t>(s.runtime);
        identity.context=reinterpret_cast<uint64_t>(ctx);identity.target=reinterpret_cast<uint64_t>(target.Get());
        identity.vortObservation=s.vortObservation;identity.vortExpectedCallback=s.vortExpectedCallback;identity.vortFrame=s.vortFrame;
        identity.eyeWidth=owned.scene_width;identity.height=owned.scene_height;identity.requestedReset=reset;
        const auto uniformFloats=[&](const char* name,float* values,size_t count){
            const auto u=s.runtime->find_uniform_variable(Effect,name);Need(u.handle!=0,"capture-live-float-uniform-missing");
            s.runtime->get_uniform_value_float(u,values,count);for(size_t i=0;i<count;++i)Need(std::isfinite(values[i]),"capture-live-float-uniform-invalid");};
        const auto uniformBool=[&](const char* name,bool& value){
            const auto u=s.runtime->find_uniform_variable(Effect,name);Need(u.handle!=0,"capture-live-bool-uniform-missing");
            s.runtime->get_uniform_value_bool(u,&value,1);};
        auto& settings=identity.motion;
        uniformFloats("MV_SIGN",settings.mvSign,2);uniformFloats("MV_SCALE",&settings.mvScale,1);
        uniformBool("MV_VALIDATE",settings.mvValidate);uniformBool("VALIDATE_STATIC",settings.validateStatic);
        uniformBool("STATIC_HYSTERESIS",settings.staticHysteresis);uniformFloats("STATIC_BIAS",&settings.staticBias,1);
        uniformFloats("STATIC_MIN_CONTRAST",&settings.staticMinContrast,1);uniformBool("VALIDATE_DEPTH",settings.validateDepth);
        uniformFloats("DEPTH_TOLERANCE",&settings.depthTolerance,1);uniformBool("VALIDATE_MV",settings.validateMv);
        uniformFloats("MV_CONSISTENCY",&settings.mvConsistency,1);settings.observed=true;
        std::array<ComPtr<ID3D11Texture2D>,2> eyeDepth;
        for(unsigned eye=0;eye<2;++eye){const auto& e=owned.eye[eye];identity.colorGeneration[eye]=e.color_generation;
            identity.depthGeneration[eye]=e.depth_generation;identity.resolveSequence[eye]=e.resolve_sequence;identity.candidate[eye]=e.candidate_index;
            ComPtr<ID3D11Resource> depthResource;e.depth->GetResource(&depthResource);Check(depthResource.As(&eyeDepth[eye]),"capture-eye-depth-resource");}
        auto rawMotion=Guide(s,"ETS2VortMotion",DXGI_FORMAT_R16G16_FLOAT,d.Width,d.Height,MotionEffect);
        auto vortCurrent=Guide(s,"ETS2VortDiagnosticCurrent",DXGI_FORMAT_R16G16_FLOAT,(d.Width/4)*2,d.Height/2,MotionEffect);
        auto vortPrevious=Guide(s,"ETS2VortDiagnosticPrevious",DXGI_FORMAT_R16G16_FLOAT,(d.Width/4)*2,d.Height/2,MotionEffect);
        auto diagnosticTests=Guide(s,"ETS2_DLAA_DiagnosticTests",DXGI_FORMAT_R16G16B16A16_FLOAT,d.Width,d.Height);
        const std::array<ID3D11Texture2D*,ets2_input_capture::PlaneCount> inputs={s.original.texture.Get(),s.color.texture.Get(),rawMotion.Get(),
            motion.Get(),s.depth.texture.Get(),eyeDepth[0].Get(),eyeDepth[1].Get(),vortCurrent.Get(),vortPrevious.Get(),diagnosticTests.Get()};
        if(!s.inputCapture.Begin(ctx,inputs,identity))Log(s.inputCapture.Reason());
    }catch(...){s.inputCapture.Abort("diagnostic-guide-acquisition-failed");Log(s.inputCapture.Reason());}
    const HRESULT evaluated=s.sr->Evaluate(ctx,s.color.texture.Get(),s.depth.texture.Get(),motion.Get(),reset);
    s.attempt=s.sr->LastEvaluation();if(FAILED(evaluated)){s.inputCapture.EvaluationFailed();throw Failure{s.sr->Reason(),evaluated};}
    Need(s.attempt.success&&s.attempt.calls==2&&s.attempt.handles[0]&&s.attempt.handles[1]&&
         s.attempt.handles[0]!=s.attempt.handles[1],"actual-two-eye-evaluation-receipt");
    if(s.inputCapture.Pending()){
        const bool read=s.inputCapture.ReadRetired(ctx,s.attempt.requested_reset,s.attempt.applied_reset,
            s.attempt.history_generation,s.attempt.evaluation_ordinal,s.attempt.handles);
        if(read){const auto directory=s.inputCapture.FrameDirectory()/L"ngx";
            s.inputCapture.Commit(SUCCEEDED(s.sr->CaptureLastInputs(directory.wstring())));}
        Log(s.inputCapture.Reason());
    }
    s.pendingPair.Reset();s.pendingTarget.Reset(); // NativeSr's fence retired input reads and previous output use.
    auto* left=s.sr->Output(0);auto* right=s.sr->Output(1);Need(left&&right,"current-two-eye-output");
    constexpr float factor=1.0f;
    s.pendingTarget=std::move(s.pendingInputTarget);
    {
        ets2_d3d11::Scope scope(s.privateState,ctx);Check(scope.Status(),"output-state-scope");DrawState(s,factor);
        auto* output=s.output.rtv.Get();ctx->OMSetRenderTargets(1,&output,nullptr);ctx->PSSetShader(s.compose.Get(),nullptr,0);
        ID3D11ShaderResourceView* sources[]={s.original.srv.Get(),left,right};ctx->PSSetShaderResources(0,3,sources);ctx->Draw(3,0);
        ctx->OMSetRenderTargets(0,nullptr,nullptr);ID3D11ShaderResourceView* empty[3]{};ctx->PSSetShaderResources(0,3,empty);
        ctx->CopyResource(target.Get(),s.output.texture.Get());
    }
    Check(s.device->GetDeviceRemovedReason(),"delivery-device-removed");
    Check(s.sequence.CommitDelivered(ticket),"current-pair-commit");
    s.reset=false;s.delivered=true;++s.deliveredFrame;s.deliveredTick=GetTickCount64();s.deliveredSerial=s.serial;
    s.deliveredBlend=factor;s.detail="current-native-two-eye-delivery";
    if(s.deliveredFrame<=3)Log("current-native-two-eye-delivery");
}
void Init(effect_runtime* rt){
    if(!rt||rt->get_device()->get_api()!=device_api::d3d11)return;
    auto& h=Global();std::lock_guard<std::recursive_mutex> lock(h.mutex);
    try{
        Paths();
        ets2_dlaa_settings::Initialize();
        const auto assets=ets2_dlaa_assets::EnsureAssets(self,rt);
        if(!assets.success){Log(assets.stage,HRESULT_FROM_WIN32(assets.win32Error?assets.win32Error:ERROR_INVALID_DATA));return;}
        if(!Vr(rt)||h.state||h.quarantined)return;
        HMODULE pinned=nullptr;Need(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Init),&pinned)!=FALSE,"callback-module-pin");
        auto s=std::make_unique<State>();s->runtime=rt;s->device=reinterpret_cast<ID3D11Device*>(rt->get_device()->get_native());
        Need(s->device!=nullptr,"runtime-device");s->device->GetImmediateContext(&s->context);Need(s->context!=nullptr,"runtime-context");
        Need(s->protection.Acquire(s->context.Get()),"runtime-context-protection");
        h.state=std::move(s);MotionFlag(rt,false);Log("ETS2-DLSS-4.5-1.0-native-only-attached");Status(*h.state);
    }catch(const Failure& f){Log(f.reason,f.hr);}catch(...){Log("runtime-initialization-exception",E_FAIL);}
}
void Render(effect_runtime* rt,effect_technique technique,command_list* commands,resource_view rtv,resource_view){
    if(!Vr(rt))return;auto& h=Global();std::unique_lock<std::recursive_mutex> lock(h.mutex,std::try_to_lock);
    if(!lock){h.missed=true;return;}if(!h.state||h.state->runtime!=rt)return;auto& s=*h.state;
    try{
        if(h.missed.exchange(false))Skip(s,"concurrent-callback-skipped");
        const auto provider=rt->find_technique(MotionEffect,"ETS2_VortStereo");
        if(provider.handle&&provider.handle==technique.handle){
            const auto frame=rt->find_uniform_variable(MotionEffect,"ETS2_VORT_FRAME");uint32_t index=0;
            if(frame.handle)rt->get_uniform_value_uint(frame,&index,1);
            s.vortFrame=index;++s.vortObservation;s.vortExpectedCallback=h.callback+1;
            MotionFlag(rt,s.history.Observe(reinterpret_cast<uintptr_t>(rt),provider.handle,index,
                frame.handle&&Uniform(rt,MotionEffect,"ETS2_STEREO_DEPTH_VALID"),Uniform(rt,MotionEffect,"ETS2_VORT_RESET")));return;
        }
        const auto feed=rt->find_technique(Effect,"ETS2_DLAA");if(!feed.handle||feed.handle!=technique.handle)return;
        s.callback=++h.callback;s.delivered=false;s.attempt={};Process(s,commands,rtv);
    }catch(const Failure& f){s.failed=true;ets2_dlaa_producer::Request(false);Skip(s,f.reason);Log(f.reason,f.hr);}
    catch(...){s.failed=true;ets2_dlaa_producer::Request(false);Skip(s,"render-exception");Log(s.detail,E_FAIL);}
}
void Present(effect_runtime* rt){
    if(rt&&rt->get_hwnd()){ets2_dlaa_controls::ObserveDesktop(rt);return;}
    if(!Vr(rt))return;auto& h=Global();std::lock_guard<std::recursive_mutex> lock(h.mutex);
    if(!h.state||h.state->runtime!=rt)return;auto& s=*h.state;
    try{s.desktopEffectsAllowed=ets2_dlaa_controls::ApplyVr(rt);Controls(s);MotionDependency(s);if(!s.delivered){s.reset=true;}Status(s);
        s.delivered=false;s.attempt={};s.history.Present(reinterpret_cast<uintptr_t>(rt));MotionFlag(rt,false);
    }catch(...){s.failed=true;ets2_dlaa_producer::Request(false);Skip(s,"present-status-exception");}
}
void Reload(effect_runtime* rt){
    if(rt&&rt->get_hwnd()){ets2_dlaa_controls::ObserveDesktop(rt);return;}
    if(!Vr(rt))return;auto& h=Global();std::lock_guard<std::recursive_mutex> lock(h.mutex);
    if(!h.state||h.state->runtime!=rt)return;auto& s=*h.state;
    try{s.history.Reset(reinterpret_cast<uintptr_t>(rt));MotionFlag(rt,false);Skip(s,"effect-history-reloaded");
        s.sequence.InvalidateHistory();s.desktopEffectsAllowed=ets2_dlaa_controls::ApplyVr(rt);Controls(s);MotionDependency(s);
    }catch(...){s.failed=true;ets2_dlaa_producer::Request(false);Skip(s,"effect-reload-exception");}
}
void Destroy(effect_runtime* rt){
    ets2_dlaa_controls::ForgetRuntime(rt);
    if(!Vr(rt))return;auto& h=Global();std::lock_guard<std::recursive_mutex> lock(h.mutex);
    if(!h.state||h.state->runtime!=rt)return;auto& s=*h.state;
    ets2_dlaa_producer::Request(false);
    try{Skip(s,"runtime-destroyed");s.failed=true;Status(s);
        if(!Retire(s)){h.quarantined=true;Log("runtime-retained-until-process-exit",E_FAIL);(void)h.state.release();return;}
        s.protection.ReleaseDevice(s.device.Get());h.state.reset();
    }catch(...){h.quarantined=true;(void)h.state.release();}
}
}
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID reserved){
    if(reason==DLL_PROCESS_ATTACH){self=module;DisableThreadLibraryCalls(module);if(!reshade::register_addon(module))return FALSE;
        if(!ets2_dlaa_settings::Register()){reshade::unregister_addon(module);return FALSE;}
        if(!DepthLifecycle(module,reason,reserved)){ets2_dlaa_settings::Unregister();reshade::unregister_addon(module);return FALSE;}
        reshade::register_event<reshade::addon_event::init_effect_runtime>(Init);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(Destroy);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(Reload);
        reshade::register_event<reshade::addon_event::reshade_render_technique>(Render);
        reshade::register_event<reshade::addon_event::reshade_present>(Present);
    }else if(reason==DLL_PROCESS_DETACH&&!reserved){
        ets2_dlaa_settings::Unregister();
        DepthLifecycle(module,reason,reserved);
        // Accepted VR initialization pins callback code; no GPU cleanup under loader lock.
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(Init);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(Destroy);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(Reload);
        reshade::unregister_event<reshade::addon_event::reshade_render_technique>(Render);
        reshade::unregister_event<reshade::addon_event::reshade_present>(Present);reshade::unregister_addon(module);
    }return TRUE;
}
