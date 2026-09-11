// Private exact-color stereo depth experiment. See NOTICE.md for provenance.
// Public callback capture and custom semantic publication; no eye-order inference.
#include <reshade.hpp>
#include "depth_match.hpp"
#include "ets2_depth_route.hpp"
#include "../dlaa/producer_control.h"
#include <mutex>
#include <memory>
#include <unordered_map>
#include <set>
#include <cstdio>
#include <cstdarg>
#include <cstring>
using Microsoft::WRL::ComPtr;
using namespace reshade::api;
namespace
{
FILE* logfile = nullptr;
std::wstring outputDirectory;
size_t logBytes = 0;
ets2_scene_r_depth::Request sceneRequest;
ets2_scene_r_depth::Request ReadSceneRequest(){
    // DLAA always uses the actual VR eye extent; it needs no launch environment.
    return ets2_scene_r_depth::ParseRequest(nullptr,nullptr,"1");
}
constexpr size_t LogLimit = 4 * 1024 * 1024;
constexpr bool allowGenericFixture=false;
thread_local bool inside = false;
std::recursive_mutex routeMutex;
uint64_t runtimeGenerationSerial=0;
void Log(const char* f, ...)
{
    if (!logfile || logBytes >= LogLimit) return;
    char line[2048] = {};
    va_list args; va_start(args, f);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, f, args); va_end(args);
    const size_t bytes = strlen(line);
    if (logBytes + bytes + 96 > LogLimit)
    {
        fputs("LOG_CAP_REACHED: 4 MiB metadata budget; matching continues with the strict gate.\n", logfile);
        logBytes = LogLimit; fflush(logfile); return;
    }
    logBytes += fwrite(line, 1, bytes, logfile);
    logBytes += fwrite("\n", 1, 1, logfile); fflush(logfile);
}
// Fixed current-frame proof policy for native DLAA.
struct Config
{
    bool asyncProof = true;  // reuse a recent proof; slot reuse and fresh proofs may still block
    unsigned holdFrames = 2; // keep the last verified depth published this long when the route fails
};
Config config;
void LoadConfig()
{
    config.asyncProof=false;config.holdFrames=0;
}
struct Binding
{
    ComPtr<ID3D11Texture2D> color, depth;
    unsigned colorSub = 0, depthSub = 0, w = 0, h = 0;
    bool valid = false;
};
struct Command
{
    Binding bound;
    uint64_t draws = 0;
};
struct DeviceState
{
    ets2_route::Tracker route;
    std::unique_ptr<depth_match::Matcher> matcher;
    unsigned w = 0, h = 0, expectedW = 0, expectedH = 0;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
    std::string lastReason;
    uint64_t epoch = 0, serial = 0, vrFrames = 0;
    uint64_t runtimeGeneration=0,scenePairs=0,scenePairFailures=0;
    uint64_t captureRequest=0,routeWorkCalls=0,collectCalls=0,matchCalls=0,pausedFrames=0,lastCaptureReceiptTick=0;
    bool captureArmed=false;
    unsigned routeDiagnostics = 0;
    uint64_t heldFrames = 0, asyncFrames = 0, syncFrames = 0;
    double matchMsSum = 0, matchMsMax = 0;
    unsigned matchMsCount = 0;
    bool inEffects = false, mixedScope = false, previewSaved = false;
    std::unordered_map<command_list*, Command> commands;
    std::set<effect_runtime*> runtimes;
};
// Intentionally no static registry destructor. The OS reclaims this small
// registry at process termination; normal runtime/device callbacks release its
// owned resources earlier. A static map would still destroy COM objects during
// CRT detach even if the DllMain process-termination branch skipped clear().
using StateRegistry = std::unordered_map<device*, DeviceState>;
StateRegistry& states = *new StateRegistry;
struct Guard
{
    std::unique_lock<std::recursive_mutex> lock{routeMutex};
    Guard()
    {
        inside = true;
    }
    ~Guard()
    {
        inside = false;
    }
};
bool Immediate(command_list* cl)
{
    return cl->get_device()->get_api() == device_api::d3d11 &&
           reinterpret_cast<ID3D11DeviceContext*>(cl->get_native())->GetType() ==
               D3D11_DEVICE_CONTEXT_IMMEDIATE;
}
bool CaptureAllowed(const DeviceState& s) noexcept
{
    const auto requested=ets2_dlaa_producer::Read();
    return s.captureArmed&&ets2_dlaa_producer::Enabled(requested)&&requested==s.captureRequest;
}
void CaptureReceipt(DeviceState& s,const char* state)
{
    s.lastCaptureReceiptTick=GetTickCount64();
    Log("PRODUCER_CAPTURE tick=%llu state=%s request=%llu epoch=%llu paused_frames=%llu route_work_calls=%llu collect_calls=%llu match_calls=%llu",
        s.lastCaptureReceiptTick,state,s.captureRequest,s.epoch,s.pausedFrames,s.routeWorkCalls,s.collectCalls,s.matchCalls);
}
// ReShade reports layers=0 for a non-array texture_2d RTV/DSV. Only array views
// require an explicit one-layer range. Retain the native resource before unbind.
bool TextureFromView(device* d, resource_view v, ComPtr<ID3D11Texture2D>& tex, unsigned& sub, unsigned& w,
                     unsigned& h)
{
    if (!v.handle)
        return false;
    auto r = d->get_resource_from_view(v);
    auto desc = d->get_resource_desc(r);
    auto view = d->get_resource_view_desc(v);
    if (desc.type != resource_type::texture_2d || desc.texture.levels != 1 || desc.texture.samples != 1 ||
        view.texture.first_level != 0 ||
        (view.type != resource_view_type::texture_2d && view.texture.layers != 1))
        return false;
    w = desc.texture.width;
    h = desc.texture.height;
    sub = view.texture.first_layer;
    if (sub >= desc.texture.depth_or_layers)
        return false;
    return SUCCEEDED(reinterpret_cast<ID3D11Resource*>(r.handle)->QueryInterface(IID_PPV_ARGS(&tex)));
}
Binding Describe(device* d, uint32_t n, const resource_view* rt, resource_view dv)
{
    Binding b;
    if (n != 1 || !dv.handle)
        return b;
    unsigned dw = 0, dh = 0;
    if (!TextureFromView(d, rt[0], b.color, b.colorSub, b.w, b.h) ||
        !TextureFromView(d, dv, b.depth, b.depthSub, dw, dh) || dw != b.w || dh != b.h || b.w < 128 ||
        b.h < 128)
        return b;
    D3D11_TEXTURE2D_DESC color{}, depth{};
    b.color->GetDesc(&color);
    b.depth->GetDesc(&depth);
    if ((color.Format != DXGI_FORMAT_R8G8B8A8_UNORM && color.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         color.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) ||
        depth_match::DepthStorage(depth.Format) == DXGI_FORMAT_UNKNOWN)
        return b;
    b.valid = true;
    return b;
}
bool Same(const Binding& a, const Binding& b)
{
    return a.color.Get() == b.color.Get() && a.depth.Get() == b.depth.Get() && a.colorSub == b.colorSub &&
           a.depthSub == b.depthSub;
}
void Capture(device* owner, DeviceState& s, command_list* cl, Command& cmd, const char* reason)
try
{
    // Co-bound AA stencil/depth is not proven scene depth. Only a separately
    // compiled synthetic fixture may use the old generic candidate heuristic.
    if(!allowGenericFixture){cmd.draws=0;return;}
    if (!cmd.draws || !cmd.bound.valid)
        return;
    auto& b = cmd.bound;
    if (!s.expectedW || b.w != s.expectedW || b.h != s.expectedH)
    {
        cmd.draws = 0;
        return;
    }
    D3D11_TEXTURE2D_DESC nativeDepth{};
    b.depth->GetDesc(&nativeDepth);
    auto format = depth_match::DepthStorage(nativeDepth.Format);
    if (!s.matcher || (!s.matcher->Count() && (b.w != s.w || b.h != s.h || format != s.depthFormat)))
    {
        s.w = b.w;
        s.h = b.h;
        s.depthFormat = format;
        s.matcher = std::make_unique<depth_match::Matcher>(
            reinterpret_cast<ID3D11Device*>(owner->get_native()),
            reinterpret_cast<ID3D11DeviceContext*>(cl->get_native()), s.w, s.h, format);
    }
    if (b.w != s.w || b.h != s.h || format != s.depthFormat)
    {
        s.mixedScope = true;
        cmd.draws = 0;
        return;
    }
    bool ok = s.matcher->Capture(b.color.Get(), b.colorSub, b.depth.Get(), b.depthSub, s.epoch, ++s.serial);
    if (s.vrFrames < 4 || s.vrFrames % 120 == 0)
        Log("CAPTURE epoch=%llu serial=%llu index=%u reason=%s draws=%llu color=%p color_sub=%u depth=%p "
            "depth_sub=%u depth_family=%u extent=%ux%u captured=%d",
            s.epoch, s.serial, s.matcher->Count(), reason, cmd.draws, b.color.Get(), b.colorSub,
            b.depth.Get(), b.depthSub, unsigned(format), s.w, s.h, ok);
    cmd.draws = 0;
}
catch (const std::exception& e)
{
    if (s.lastReason != e.what())
        Log("CAPTURE exception: %s", e.what());
    s.lastReason = e.what();
    s.mixedScope = true;
    cmd.draws = 0;
}
// D3D11 emits this callback after native OM binding. The tracked old pair still
// owns its COM resources and is copied before the application can clear/reuse it.
void Bind(command_list* cl, uint32_t n, const resource_view* rt, resource_view ds)
{
    if (inside || !Immediate(cl))
        return;
    Guard guard;
    auto* owner = cl->get_device();
    auto& s = states[owner];
    if (s.inEffects || !CaptureAllowed(s))
        return;
    s.route.SetDevice(reinterpret_cast<ID3D11Device*>(owner->get_native()));
    ++s.routeWorkCalls;
    try {s.route.Bind(reinterpret_cast<ID3D11DeviceContext*>(cl->get_native()));}
    catch(const std::exception& e){s.route.Fail();if(s.lastReason!=e.what())Log("ETS2_ROUTE capture: %s",e.what());s.lastReason=e.what();}
    auto& cmd = s.commands[cl];
    auto next = Describe(owner, n, rt, ds);
    if (!Same(cmd.bound, next))
    {
        Capture(owner, s, cl, cmd, "attachment-change");
        cmd.draws = 0;
    }
    cmd.bound = std::move(next);
}
// The callers hold the device guard and have checked the immediate context.
// Indexed draws still enter the ordinary route with zero fullscreen vertices.
void RecordDraw(DeviceState& s,command_list* cl,uint32_t vertices,uint32_t instances,uint32_t firstVertex,uint32_t firstInstance
#ifdef ETS2_GEOMETRY_TAA_PROBE
    ,const ets2_geometry_taa::DrawArgs* probeArgs=nullptr
#endif
)
{
    auto* context=reinterpret_cast<ID3D11DeviceContext*>(cl->get_native());
    // routeMutex/inside are already held. One protected native interval encloses
    // all normalization getters, actual-VS replay and restoration. No state swap.
    // If protection is unavailable only the new early capture is refused; the
    // legacy depth route retains its existing behavior.
    struct Interval {
        ComPtr<ID3D11Multithread> mt;bool held=false;
        Interval(ID3D11DeviceContext* c,bool enabled){if(enabled&&SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&mt)))&&mt->GetMultithreadProtected()){mt->Enter();held=true;}}
        ~Interval(){if(held)mt->Leave();}
    } interval(context,sceneRequest.valid&&sceneRequest.enabled);
    s.route.ConstantsEpoch(s.epoch);
    s.route.WritableOutputs(context,false,false);
#ifdef ETS2_GEOMETRY_TAA_PROBE
    if(interval.held&&probeArgs)s.route.ProbeGeometry(context,*probeArgs);
#endif

    try {s.route.Draw(context,vertices,instances,firstVertex,firstInstance,interval.held);}
    catch(const std::exception& e){s.route.Fail();if(s.lastReason!=e.what())Log("ETS2_ROUTE draw: %s",e.what());s.lastReason=e.what();}
    auto& cmd=s.commands[cl];if(cmd.bound.valid)++cmd.draws;
}
bool Draw(command_list* cl, uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance)
{
    if(inside||!Immediate(cl))return false;
    Guard guard;auto it=states.find(cl->get_device());
    if(it!=states.end()&&!it->second.inEffects&&CaptureAllowed(it->second)){
        ++it->second.routeWorkCalls;RecordDraw(it->second,cl,vertices,instances,firstVertex,firstInstance);
    }
    return false;
}
bool Indexed(command_list* cl, uint32_t count, uint32_t instances, uint32_t first, int32_t base, uint32_t firstInstance)
{
#ifdef ETS2_GEOMETRY_TAA_PROBE
    if(inside||!Immediate(cl))return false;
    Guard guard;auto it=states.find(cl->get_device());
    if(it!=states.end()&&!it->second.inEffects&&CaptureAllowed(it->second)){
        ++it->second.routeWorkCalls;
        const ets2_geometry_taa::DrawArgs args{true,count,instances,first,base,firstInstance};
        RecordDraw(it->second,cl,0,0,0,0,&args);
    }
    return false;
#else
    (void)count;(void)instances;(void)first;(void)base;(void)firstInstance;
    return Draw(cl,0,0,0,0);
#endif
}
bool ClearDepth(command_list* cl, resource_view view, const float* depth, const uint8_t*, uint32_t,
                const rect*)
{
    // A device-loss path may clear a null DSV. Do not turn the original
    // device failure into a second exception while resolving a missing view.
    if (inside || !view.handle || !depth || !Immediate(cl))
        return false;
    Guard guard;
    auto* owner = cl->get_device();
    auto it = states.find(owner);
    if (it == states.end() || it->second.inEffects || !CaptureAllowed(it->second))
        return false;
    auto& cmd = it->second.commands[cl];
    auto resource = owner->get_resource_from_view(view);
    ++it->second.routeWorkCalls;
    try {it->second.route.ClearDepth(reinterpret_cast<ID3D11DeviceContext*>(cl->get_native()),reinterpret_cast<ID3D11Resource*>(resource.handle));}
    catch(const std::exception& e){it->second.route.Fail();if(it->second.lastReason!=e.what())Log("ETS2_ROUTE depth clear: %s",e.what());it->second.lastReason=e.what();}
    if (cmd.bound.depth.Get() == reinterpret_cast<ID3D11Texture2D*>(resource.handle))
        Capture(owner, it->second, cl, cmd, "before-depth-clear");
    return false;
}
bool ClearColor(command_list* cl, resource_view view, const float[4], uint32_t, const rect*)
{
    if (inside || !view.handle || !Immediate(cl))
        return false;
    Guard guard;
    auto* owner = cl->get_device();
    auto it = states.find(owner);
    if (it == states.end() || !CaptureAllowed(it->second))
        return false;
    auto& cmd = it->second.commands[cl];
    auto resource = owner->get_resource_from_view(view);
    ++it->second.routeWorkCalls;
    it->second.route.ClearColor(reinterpret_cast<ID3D11Resource*>(resource.handle));
    if (cmd.bound.color.Get() == reinterpret_cast<ID3D11Texture2D*>(resource.handle))
        Capture(owner, it->second, cl, cmd, "before-color-clear");
    return false;
}
// Explicit writes can run between an accepted pair and its consumer. Only the
// named destination is revoked, including during effects; internal producer
// replay is excluded by 'inside'. Reading a tracked resource is harmless.
void TextureWritten(device* owner,resource destination)
{
    if(inside||!destination.handle||owner->get_api()!=device_api::d3d11)return;
    Guard guard;auto it=states.find(owner);
    if(it==states.end()||!CaptureAllowed(it->second))return;
    ++it->second.routeWorkCalls;
    it->second.route.ResourceWritten(reinterpret_cast<ID3D11Resource*>(destination.handle));
}
void TextureWritten(command_list* cl,resource destination)
{
    if(inside||!Immediate(cl))return;
    TextureWritten(cl->get_device(),destination);
}
bool CopyResource(command_list* cl,resource,resource destination){TextureWritten(cl,destination);return false;}
bool CopyTexture(command_list* cl,resource,uint32_t,const subresource_box*,resource destination,uint32_t,const subresource_box*,filter_mode){TextureWritten(cl,destination);return false;}
bool ResolveTexture(command_list* cl,resource,uint32_t,const subresource_box*,resource destination,uint32_t,uint32_t,uint32_t,uint32_t,format){TextureWritten(cl,destination);return false;}
bool UpdateTexture(device* owner,const subresource_data&,resource destination,uint32_t,const subresource_box*){TextureWritten(owner,destination);return false;}
bool UpdateTextureCommand(command_list* cl,const subresource_data&,resource destination,uint32_t,const subresource_box*){TextureWritten(cl,destination);return false;}
void MapTexture(device* owner,resource destination,uint32_t,const subresource_box*,map_access access,subresource_data*)
{
    if(access!=map_access::read_only)TextureWritten(owner,destination);
}
bool ClearUavUint(command_list* cl,resource_view view,const uint32_t[4],uint32_t,const rect*)
{
    if(!inside&&view.handle&&Immediate(cl))TextureWritten(cl,cl->get_device()->get_resource_from_view(view));return false;
}
bool ClearUavFloat(command_list* cl,resource_view view,const float[4],uint32_t,const rect*)
{
    if(!inside&&view.handle&&Immediate(cl))TextureWritten(cl,cl->get_device()->get_resource_from_view(view));return false;
}
void OpaqueOutputs(command_list* cl,bool compute)
{
    if(inside||!Immediate(cl))return;
    Guard guard;auto it=states.find(cl->get_device());
    if(it==states.end()||!CaptureAllowed(it->second))return;
    ++it->second.routeWorkCalls;
    it->second.route.WritableOutputs(reinterpret_cast<ID3D11DeviceContext*>(cl->get_native()),compute,!compute);
}
bool Dispatch(command_list* cl,uint32_t,uint32_t,uint32_t){OpaqueOutputs(cl,true);return false;}
bool Indirect(command_list* cl,indirect_command kind,resource,uint64_t,uint32_t,uint32_t)
{
    OpaqueOutputs(cl,kind==indirect_command::dispatch);return false;
}
void ExecuteSecondary(command_list* cl,command_list*)
{
    if(inside||!Immediate(cl))return;
    Guard guard;auto it=states.find(cl->get_device());
    if(it==states.end()||!CaptureAllowed(it->second))return;
    // D3D11 secondary execution contains opaque recorded writes. Flush uses
    // execute_command_list instead and is deliberately not an invalidation.
    ++it->second.routeWorkCalls;it->second.route.Fail();
}
void Publish(effect_runtime* r, ID3D11ShaderResourceView* view, bool valid)
{
    const resource_view binding{reinterpret_cast<uint64_t>(view)};
    r->update_texture_bindings("ETS2_STEREO_DEPTH", binding, binding);
    auto dlaa = r->find_uniform_variable("ETS2_DLAA.addonfx", "ETS2_STEREO_DEPTH_VALID");
    if (dlaa.handle)
        r->set_uniform_value_bool(dlaa, &valid, 1);
    auto motion = r->find_uniform_variable("ETS2_VortStereo.addonfx", "ETS2_STEREO_DEPTH_VALID");
    if (motion.handle)
        r->set_uniform_value_bool(motion, &valid, 1);
}
// Publishes a proven output for a bounded route dropout. HeldView retires
// pending proofs without waiting and rejects a disproven speculative output.
bool HoldDepth(effect_runtime* r, DeviceState& s, const char* why)
{
    if (!config.holdFrames || !s.matcher)
        return false;
    auto* view = s.matcher->HeldView(s.epoch, config.holdFrames);
    if (!view)
        return false;
    Publish(r, view, true);
    ++s.heldFrames;
    if (s.heldFrames <= 4 || s.heldFrames % 120 == 0)
        Log("MATCH epoch=%llu held=1 age=%llu totals_held=%llu reason=%s", s.epoch, s.epoch - s.matcher->LastAcceptedEpoch(), s.heldFrames, why);
    return true;
}
// This event precedes every effect technique. The target is still untouched SBS
// color; sampling it later would include PNG/Kernel/Feed modifications.
void Begin(effect_runtime* r, command_list* cl, resource_view rtv, resource_view)
try
{
    if (inside || r->get_hwnd() != nullptr || !Immediate(cl))
        return;
    Guard guard;
    auto* owner = r->get_device();
    auto& s = states[owner];

    if(s.runtimes.insert(r).second){
        s.runtimeGeneration=runtimeGenerationSerial==UINT64_MAX?0:++runtimeGenerationSerial;
        Log("SCENE_R_RUNTIME runtime=%p generation=%llu epoch=%llu",r,s.runtimeGeneration,s.epoch);
    }
    Publish(r, nullptr, false);
    // Publish(false) above detached shader bindings before cleanup releases views.
    s.route.ClearScenePair();
    const auto requested=ets2_dlaa_producer::Read();
    const bool transition=requested!=s.captureRequest;
    if(transition){
        s.captureArmed=false;s.captureRequest=requested;s.route.Reset();
        if(s.matcher)s.matcher->Reset();
        s.commands.clear();s.mixedScope=false;
        CaptureReceipt(s,ets2_dlaa_producer::Enabled(requested)?"resume_waiting_complete_frame":"paused");
    }
    ++s.vrFrames;
    if(!ets2_dlaa_producer::Enabled(requested)){
        s.captureArmed=false;s.inEffects=true;++s.pausedFrames;
        if(!s.lastCaptureReceiptTick||GetTickCount64()-s.lastCaptureReceiptTick>=1000)CaptureReceipt(s,"paused");
        return;
    }
    if (s.vrFrames % 120 == 0)
        LoadConfig();
    uint32_t runtimeWidth = 0, runtimeHeight = 0;
    r->get_screenshot_width_and_height(&runtimeWidth, &runtimeHeight);
    if (runtimeWidth % 2 || runtimeWidth < 256 || runtimeHeight < 128 || s.runtimes.size() != 1)
    {
        s.inEffects = true;
        return;
    }
    s.expectedW = runtimeWidth / 2;
    s.expectedH = runtimeHeight;
    s.route.SetDevice(reinterpret_cast<ID3D11Device*>(owner->get_native()));
    s.route.Extent(s.expectedW,s.expectedH);
    const auto resolvedScene=ets2_scene_r_depth::ResolveRequest(sceneRequest,{s.expectedW,s.expectedH});
    if(!resolvedScene.valid||(resolvedScene.enabled&&!s.route.SceneExtent(resolvedScene.size.width,resolvedScene.size.height))){
        s.inEffects=true;
        const char* reason="private scene-R extent rejected; no depth publication";
        if(s.lastReason!=reason)Log("MATCH epoch=%llu rejected=%s",s.epoch,reason);
        s.lastReason=reason;return;
    }
    if(transition||!s.captureArmed){
        // Skip this callback: only a complete scene captured after Finish may
        // establish the first pair for this request generation.
        s.route.Reset();if(s.matcher)s.matcher->Reset();s.commands.clear();
        s.captureArmed=true;s.inEffects=true;return;
    }
    for (auto& [cmd, state] : s.commands)
        Capture(owner, s, cmd, state, "VR-begin");
    ++s.collectCalls;
#ifdef ETS2_GEOMETRY_TAA_PROBE
    s.route.PollGeometry(reinterpret_cast<ID3D11DeviceContext*>(cl->get_native()),s.epoch);
#endif
    const bool routeCollected=s.route.Collect(reinterpret_cast<ID3D11DeviceContext*>(cl->get_native()),s.matcher,s.w,s.h,s.depthFormat,s.epoch,s.serial);
    if(routeCollected) {
        s.mixedScope=false;
        if(s.vrFrames<5||s.vrFrames%120==0)Log("ETS2_ROUTE epoch=%llu two HDR depth snapshots followed actual fullscreen UV transforms; checking final eye colors",s.epoch);
    }
    else if(s.vrFrames<5||s.vrFrames%120==0)Log("ETS2_ROUTE epoch=%llu snapshots=%u outputs=%u",s.epoch,s.route.SnapshotCount(),s.route.OutputCount());
    s.inEffects = true;
    if(!routeCollected&&!allowGenericFixture){
        const char* reason="current-frame ETS2 scene-depth route unavailable; generic AA depth forbidden";
        if(HoldDepth(r,s,reason)){s.lastReason=reason;return;}
        if(s.vrFrames<5||s.vrFrames%120==0||s.lastReason!=reason)Log("MATCH epoch=%llu rejected=%s",s.epoch,reason);
        s.lastReason=reason;return; // Publish(false) above remains authoritative
    }
    if (s.vrFrames == 1)
    {
        Log("MATCH epoch=%llu rejected=bootstrap epoch freshness unknown", s.epoch);
        return;
    }
    if (!s.matcher || s.mixedScope)
    {
        const char* reason = s.mixedScope ? "mixed candidate extents or depth formats" : "no candidates";
        if (HoldDepth(r, s, reason))
        {
            s.lastReason = reason;
            return;
        }
        if (s.vrFrames < 5 || s.vrFrames % 120 == 0 || s.lastReason != reason)
            Log("MATCH epoch=%llu rejected=%s", s.epoch, reason);
        s.lastReason = reason;
        return;
    }
    auto raw = owner->get_resource_from_view(rtv);
    ComPtr<ID3D11Texture2D> target;
    if (FAILED(reinterpret_cast<ID3D11Resource*>(raw.handle)->QueryInterface(IID_PPV_ARGS(&target))))
        return;
    LARGE_INTEGER a{}, b{}, freq{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    ++s.matchCalls;
    auto result = s.matcher->Match(target.Get(), s.epoch, config.asyncProof);
    QueryPerformanceCounter(&b);
    if (result.accepted)
    {
        if (result.synchronous)
            ++s.syncFrames;
        else
            ++s.asyncFrames;
    }
    const double matchMs = 1000.0 * double(b.QuadPart - a.QuadPart) / double(freq.QuadPart);
    s.matchMsSum += matchMs;
    s.matchMsMax = matchMs > s.matchMsMax ? matchMs : s.matchMsMax;
    ++s.matchMsCount;
    if (s.vrFrames < 5 || s.vrFrames % 120 == 0 || s.lastReason != result.reason)
    {
        // window_* summarise every Match call since the previous periodic line.
        Log("MATCH epoch=%llu VR=%p extent=%ux%u accepted=%d choices=%d,%d counts=%u,%u comparison_ms=%.4f "
            "window_mean_ms=%.4f window_max_ms=%.4f window_n=%u "
            "sync=%d proof_age=%u totals_sync=%llu totals_async=%llu totals_held=%llu reason=%s",
            s.epoch, r, s.w * 2, s.h, result.accepted, result.eye[0], result.eye[1], result.matches[0],
            result.matches[1], matchMs, s.matchMsSum / double(s.matchMsCount), s.matchMsMax, s.matchMsCount,
            int(result.synchronous), result.proofAge, s.syncFrames, s.asyncFrames, s.heldFrames, result.reason);
        s.matchMsSum = s.matchMsMax = 0;
        s.matchMsCount = 0;
    }
    s.lastReason = result.reason;
    if (result.accepted)
    {
        Publish(r, s.matcher->View(), true);
        if(sceneRequest.valid&&sceneRequest.enabled){
            ets2_scene_r_pair::Publication proof;
            const HRESULT early=s.route.PublishScenePair(reinterpret_cast<uint64_t>(r),s.runtimeGeneration,
                reinterpret_cast<ID3D11DeviceContext*>(cl->get_native()),reinterpret_cast<ID3D11Device*>(owner->get_native()),
                target.Get(),result,s.epoch,proof);
            if(SUCCEEDED(early)){
                ++s.scenePairs;
                if(s.scenePairs<=4||s.scenePairs%120==0){
                    Log("SCENE_R_PAIR accepted=1 runtime=%p runtime_generation=%llu epoch=%llu pair_generation=%llu scene=%ux%u final=%ux%u proof_sync=%d proof_age=%u final_sbs=%p context=%p device=%p total=%llu",
                        r,s.runtimeGeneration,s.epoch,s.route.ScenePairGeneration(),resolvedScene.size.width,resolvedScene.size.height,
                        s.expectedW,s.expectedH,int(result.synchronous),result.proofAge,target.Get(),proof.context,proof.device,s.scenePairs);
                    for(unsigned eye=0;eye<2;++eye){const auto& e=proof.eye[eye];const auto coverage=s.route.NormalizedCoverage(e.candidate_index);
                        Log("SCENE_R_EYE epoch=%llu pair_generation=%llu eye=%u candidate=%u source=%llu resolve=%llu sequence=%llu color_generation=%llu depth_generation=%llu color=%p depth=%p coverage_expected=%llu coverage_actual=%llu coverage_ready=%d coverage_complete=%d coverage_hr=%08lx",
                            s.epoch,s.route.ScenePairGeneration(),eye,e.candidate_index,e.source_color,e.resolved_color,e.resolve_sequence,
                            e.color_generation,e.depth_generation,e.color,e.depth,coverage.expectedSamples,coverage.coveredSamples,int(coverage.coverageReady),int(coverage.coverageComplete),static_cast<unsigned long>(coverage.coverageResult));}
                }
            }else{
                ++s.scenePairFailures;
                if(s.scenePairFailures<=4||s.scenePairFailures%120==0)Log("SCENE_R_PAIR accepted=0 runtime=%p runtime_generation=%llu epoch=%llu hr=%08lx proof_sync=%d proof_age=%u failures=%llu; H_depth_remains_published=1",
                    r,s.runtimeGeneration,s.epoch,static_cast<unsigned long>(early),int(result.synchronous),result.proofAge,s.scenePairFailures);
            }
        }
    }
    else
        HoldDepth(r, s, result.reason);
}
catch (const std::exception& e)
{
    Guard guard;
    auto& s = states[r->get_device()];
    s.route.ClearScenePair();
    if (s.lastReason != e.what())
        Log("MATCH exception: %s", e.what());
    s.lastReason = e.what();
    s.inEffects = true;
    Publish(r, nullptr, false);
}
// Epochs follow VR effects only. A desktop present between eyes is not a boundary.
void Finish(effect_runtime* r, command_list* effectCommands, resource_view, resource_view)
{
    if (inside || r->get_hwnd() != nullptr)
        return;
    Guard guard;
    auto it = states.find(r->get_device());
    if (it == states.end())
        return;
    auto& s = it->second;
    s.route.ClearScenePair();
    (void)effectCommands;
    s.inEffects = false;
    ++s.epoch;
    s.route.Reset();
    s.mixedScope = false;
    if (s.matcher)
        s.matcher->Reset();
    for (auto& [cl, cmd] : s.commands)
        cmd.draws = 0;
}
void DestroyRuntime(effect_runtime* r)
{
    if (inside)
        return;
    Guard guard;
    auto it = states.find(r->get_device());
    if (it == states.end())
        return;
    auto& s = it->second;
    s.route.ClearScenePair();
    if (s.runtimes.erase(r))
    {
        Publish(r, nullptr, false);
        s.captureArmed=false;CaptureReceipt(s,"runtime_destroyed");
        s.matcher.reset();
        s.commands.clear();
        s.inEffects = false;
        s.mixedScope = false;
        s.previewSaved = false;
        s.expectedW = s.expectedH = s.w = s.h = 0;
        s.depthFormat = DXGI_FORMAT_UNKNOWN;
        s.route.Extent(0,0);
        s.vrFrames = 0; // Recreated runtime must establish a fresh bootstrap epoch.
        s.runtimeGeneration=0;s.scenePairs=s.scenePairFailures=0;
        ++s.epoch;
        s.lastReason.clear();
        Log("DESTROY_VR runtime=%p unbound custom depth", r);
    }
}
void DestroyCommand(command_list* cl)
{
    if (inside)
        return;
    Guard guard;
    auto it = states.find(cl->get_device());
    if (it != states.end()){
        it->second.route.ClearScenePair();
        it->second.commands.erase(cl);
    }
}
void InitPipeline(device* d,pipeline_layout,uint32_t count,const pipeline_subobject* subobjects,pipeline p)
{
    if(inside||d->get_api()!=device_api::d3d11)return;
    Guard guard;auto& s=states[d];s.route.SetDevice(reinterpret_cast<ID3D11Device*>(d->get_native()));
#ifdef ETS2_GEOMETRY_TAA_PROBE
    for(uint32_t i=0;i<count;++i)if(subobjects[i].type==pipeline_subobject_type::input_layout){
        const auto* elements=static_cast<const input_element*>(subobjects[i].data);
        ets2_geometry_taa::PositionLayout position{};unsigned found=0;
        if(elements)for(uint32_t e=0;e<subobjects[i].count;++e){const auto& element=elements[e];
            if(element.semantic&&_stricmp(element.semantic,"POSITION")==0&&element.semantic_index==0){
                ++found;position={true,uint32_t(element.format),element.buffer_binding,element.offset,element.instance_step_rate};
            }
        }
        position.valid=position.valid&&found==1;s.route.ProbeLayout(p.handle,position);
    }
#endif
    for(uint32_t i=0;i<count;++i)if(subobjects[i].type==pipeline_subobject_type::vertex_shader){
        auto* code=static_cast<const shader_desc*>(subobjects[i].data);
        if(code&&code->code&&code->code_size)s.route.VertexPipeline(p.handle,code->code,code->code_size);
    }
    for(uint32_t i=0;i<count;++i)if(subobjects[i].type==pipeline_subobject_type::pixel_shader){
        auto* code=static_cast<const shader_desc*>(subobjects[i].data);
        if(code&&code->code&&code->code_size)s.route.Pipeline(p.handle,code->code,code->code_size);
    }
}
void DestroyPipeline(device* d,pipeline p)
{
    if(inside)return;Guard guard;auto it=states.find(d);if(it!=states.end()){
        it->second.route.DestroyPipeline(p.handle);
    }
}
void DestroyDevice(device* d)
{
    if (inside)
        return;
    Guard guard;
    auto it = states.find(d);
    if (it == states.end())
        return;
    it->second.route.ClearScenePair();
    for (auto* r : it->second.runtimes)
        Publish(r, nullptr, false);
    states.erase(it);
    Log("DESTROY_DEVICE %p released owned depth matcher resources", d);
}
} // namespace
// CPU-only AddRef acquisition under the same route mutex. Deliberately no
// Guard: this export must not toggle the callback reentry flag. No GPU command,
// wait, ReShade callback, or opaque-runtime dereference occurs here.
extern "C" __declspec(dllexport) HRESULT WINAPI ETS2_AcquireSceneRPairV1(const ETS2_SceneRRequestV1* request,ETS2_SceneRPairV1* out)
{
    if(!out)return E_POINTER;*out={};
    if(inside)return HRESULT_FROM_WIN32(ERROR_BUSY);
    const HRESULT valid=ets2_scene_r_pair::ValidateRequest(request);if(FAILED(valid))return valid;
    try {
        std::unique_lock<std::recursive_mutex> lock(routeMutex,std::try_to_lock);
        if(!lock.owns_lock())return HRESULT_FROM_WIN32(ERROR_BUSY);
        if(inside)return HRESULT_FROM_WIN32(ERROR_BUSY);
        DeviceState* matched=nullptr;
        for(auto& entry:states)for(auto* runtime:entry.second.runtimes)if(reinterpret_cast<uint64_t>(runtime)==request->runtime){
            if(matched)return E_INVALIDARG;matched=&entry.second;
        }
        if(!matched||!CaptureAllowed(*matched)||!matched->inEffects||!matched->runtimeGeneration||matched->runtimes.size()!=1||!sceneRequest.valid||!sceneRequest.enabled)return E_PENDING;
        return matched->route.AcquireScenePair(request,out);
    }catch(...){*out={};return E_FAIL;}
}
BOOL DepthLifecycle(HMODULE module, DWORD reason, LPVOID reserved)
{
    // The state registry is already retained through process termination. Do
    // not enter another DLL or the CRT's file locks under the loader lock.
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr) return TRUE;
    if (reason == DLL_PROCESS_ATTACH)
    {
        std::wstring modulePath(32768, L'\0');
        const DWORD n = GetModuleFileNameW(module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        modulePath.resize(n); outputDirectory = modulePath.substr(0, modulePath.find_last_of(L'\\') + 1);
        _wfopen_s(&logfile, (outputDirectory + L"depth-match.log").c_str(), L"w");
        Log("DLAA matched-depth producer: operational log capped at 4 MiB; research capture disabled.");
        Log("ETS2 HDR stereo depth route v3-dev: save reused scene depth, replay pinned fullscreen shader UVs, "
            "then require unique current-epoch final eye colors. Reversed-Z eye input; no draw-order eye inference.");
        Log("Scene provenance required=%d; generic depth fallback is %s",!allowGenericFixture,allowGenericFixture?"enabled in diagnostic fixture build":"forbidden");
        sceneRequest=ReadSceneRequest();
        Log("PRIVATE_SCENE_R requested=%d valid=%d extent=%ux%u native_runtime_extent=%d snapshot_cap=H:4,R:16; original VS UV replay, final H matching retained",int(sceneRequest.enabled),int(sceneRequest.valid),sceneRequest.size.width,sceneRequest.size.height,int(sceneRequest.native));
        LoadConfig();
        Log("PRIVATE_SCENE_R_PAIR abi=1 normalized_MRT=1 current_proof_required=1 effective_async_proof=%d effective_hold_frames=%u config_file_unchanged=1",int(config.asyncProof),config.holdFrames);
        Log("DLAA depth policy: current synchronous proof, no held frames, no configuration file.");
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(Bind);
        reshade::register_event<reshade::addon_event::init_pipeline>(InitPipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(DestroyPipeline);
        reshade::register_event<reshade::addon_event::draw>(Draw);
        reshade::register_event<reshade::addon_event::draw_indexed>(Indexed);
        reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(ClearDepth);
        reshade::register_event<reshade::addon_event::clear_render_target_view>(ClearColor);
        reshade::register_event<reshade::addon_event::copy_resource>(CopyResource);
        reshade::register_event<reshade::addon_event::copy_texture_region>(CopyTexture);
        reshade::register_event<reshade::addon_event::resolve_texture_region>(ResolveTexture);
        reshade::register_event<reshade::addon_event::update_texture_region>(UpdateTexture);
        reshade::register_event<reshade::addon_event::update_texture_region_command>(UpdateTextureCommand);
        reshade::register_event<reshade::addon_event::map_texture_region>(MapTexture);
        reshade::register_event<reshade::addon_event::clear_unordered_access_view_uint>(ClearUavUint);
        reshade::register_event<reshade::addon_event::clear_unordered_access_view_float>(ClearUavFloat);
        reshade::register_event<reshade::addon_event::dispatch>(Dispatch);
        reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(Indirect);
        reshade::register_event<reshade::addon_event::execute_secondary_command_list>(ExecuteSecondary);
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(Begin);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(Finish);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(DestroyRuntime);
        reshade::register_event<reshade::addon_event::destroy_command_list>(DestroyCommand);
        reshade::register_event<reshade::addon_event::destroy_device>(DestroyDevice);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(Bind);
        reshade::unregister_event<reshade::addon_event::init_pipeline>(InitPipeline);
        reshade::unregister_event<reshade::addon_event::destroy_pipeline>(DestroyPipeline);
        reshade::unregister_event<reshade::addon_event::draw>(Draw);
        reshade::unregister_event<reshade::addon_event::draw_indexed>(Indexed);
        reshade::unregister_event<reshade::addon_event::clear_depth_stencil_view>(ClearDepth);
        reshade::unregister_event<reshade::addon_event::clear_render_target_view>(ClearColor);
        reshade::unregister_event<reshade::addon_event::copy_resource>(CopyResource);
        reshade::unregister_event<reshade::addon_event::copy_texture_region>(CopyTexture);
        reshade::unregister_event<reshade::addon_event::resolve_texture_region>(ResolveTexture);
        reshade::unregister_event<reshade::addon_event::update_texture_region>(UpdateTexture);
        reshade::unregister_event<reshade::addon_event::update_texture_region_command>(UpdateTextureCommand);
        reshade::unregister_event<reshade::addon_event::map_texture_region>(MapTexture);
        reshade::unregister_event<reshade::addon_event::clear_unordered_access_view_uint>(ClearUavUint);
        reshade::unregister_event<reshade::addon_event::clear_unordered_access_view_float>(ClearUavFloat);
        reshade::unregister_event<reshade::addon_event::dispatch>(Dispatch);
        reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(Indirect);
        reshade::unregister_event<reshade::addon_event::execute_secondary_command_list>(ExecuteSecondary);
        reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(Begin);
        reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(Finish);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(DestroyRuntime);
        reshade::unregister_event<reshade::addon_event::destroy_command_list>(DestroyCommand);
        reshade::unregister_event<reshade::addon_event::destroy_device>(DestroyDevice);
        if (!reserved)
        {
            // Dynamic unloading while a runtime survives must never leave its
            // custom semantic referring to a view that is about to be released.
            for (auto& [device, state] : states){
                state.route.ClearScenePair();
                for (auto* runtime : state.runtimes)
                    Publish(runtime, nullptr, false);
            }
            states.clear();
        }
        if (logfile)
        {
            fclose(logfile);
            logfile = nullptr;
        }
    }
    return TRUE;
}
