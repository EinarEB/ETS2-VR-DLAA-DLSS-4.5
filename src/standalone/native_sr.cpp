// SPDX-License-Identifier: MIT
#include "native_sr.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <winver.h>
#include <cwchar>
#include <mutex>
#include <limits>
#include <vector>
#include <filesystem>
#include <fstream>
#include <cmath>
#include "../feeder/feed_post_sr.h"
#include "../feeder/feed_context_protection.h"

namespace ets2_native {
namespace {
using Microsoft::WRL::ComPtr;
constexpr DWORD FenceTimeoutMs=10000;
struct Failure {HRESULT hr;const char* reason;};
void Check(HRESULT hr,const char* reason){if(FAILED(hr))throw Failure{hr,reason};}
void Require(bool yes,const char* reason,HRESULT hr=E_INVALIDARG){if(!yes)throw Failure{hr,reason};}
bool ValidPreset(int preset){return preset==13||preset==12||preset==11;}
bool Same(IUnknown* first,IUnknown* second){
    if(!first||!second)return false;
    ComPtr<IUnknown> a,b;
    return SUCCEEDED(first->QueryInterface(IID_PPV_ARGS(&a)))&&
        SUCCEEDED(second->QueryInterface(IID_PPV_ARGS(&b)))&&a.Get()==b.Get();
}
ComPtr<IUnknown> DeviceIdentity(ID3D11Device* device){
    Require(device!=nullptr,"device-identity-null",E_POINTER);
    // ReShade 6.8's documented-in-source proxy query. A texture's GetDevice
    // may return the proxy while effect_runtime::get_native returns its original.
    constexpr GUID unwrappedObject={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
    ComPtr<ID3D11Device> native=device;ComPtr<IUnknown> unwrapped,identity;
    const HRESULT hr=device->QueryInterface(unwrappedObject,reinterpret_cast<void**>(unwrapped.GetAddressOf()));
    if(SUCCEEDED(hr)){
        Require(unwrapped!=nullptr,"device-identity-empty",E_NOINTERFACE);
        Check(unwrapped.As(&native),"device-identity-interface");
    }else if(hr!=E_NOINTERFACE)Check(hr,"device-identity-query");
    Check(native.As(&identity),"device-identity-iunknown");return identity;
}
struct InitResult {int code=NVSDK_NGX_Result_Fail;DWORD exception=0;};
InitResult InitializeCore(feed_driver::Api* api,const wchar_t* path,ID3D12Device* device) noexcept {
    InitResult result;
    __try {
        result.code=api->init(0x1000000ULL,path,device,NVSDK_NGX_Version_API,nullptr);
        if(NVSDK_NGX_FAILED(result.code))result.code=api->project(
            "a0f57b54-1daf-4934-90ae-c4035c19df04",NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "1.0",path,device,NVSDK_NGX_Version_API,nullptr);
    } __except(EXCEPTION_EXECUTE_HANDLER){result.exception=GetExceptionCode();}
    return result;
}
struct DiagnosticParameters {
    float jitterX=0,jitterY=0,scaleX=0,scaleY=0;
    int reset=0;bool success=false;DWORD exception=0;
};
DiagnosticParameters ReadDiagnosticParameters(const NVSDK_NGX_Parameter* p) noexcept {
    DiagnosticParameters r;
    __try {
        r.success=p&&p->Get("Jitter.Offset.X",&r.jitterX)==NVSDK_NGX_Result_Success&&
            p->Get("Jitter.Offset.Y",&r.jitterY)==NVSDK_NGX_Result_Success&&
            p->Get("MV.Scale.X",&r.scaleX)==NVSDK_NGX_Result_Success&&
            p->Get("MV.Scale.Y",&r.scaleY)==NVSDK_NGX_Result_Success&&
            p->Get("Reset",&r.reset)==NVSDK_NGX_Result_Success;
    } __except(EXCEPTION_EXECUTE_HANDLER){r.exception=GetExceptionCode();}
    return r;
}
class LocalHandle {
public:
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit LocalHandle(HANDLE handle):value(handle){}
    ~LocalHandle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
    LocalHandle(const LocalHandle&)=delete;LocalHandle& operator=(const LocalHandle&)=delete;
};
void RequireModelVersion(const wchar_t* path,bool loaded){
    DWORD ignored=0;
    const DWORD size=GetFileVersionInfoSizeW(path,&ignored);
    Require(size!=0,loaded?"loaded-nvngx_dlss.dll-version-unreadable":
            "nvngx_dlss.dll-version-unreadable",HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    std::vector<BYTE> bytes(size);
    Require(GetFileVersionInfoW(path,0,size,bytes.data())!=FALSE,
            loaded?"loaded-nvngx_dlss.dll-version-unreadable":"nvngx_dlss.dll-version-unreadable",
            HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    void* raw=nullptr;UINT length=0;
    Require(VerQueryValueW(bytes.data(),L"\\",&raw,&length)!=FALSE&&raw&&length>=sizeof(VS_FIXEDFILEINFO),
            loaded?"loaded-nvngx_dlss.dll-version-invalid":"nvngx_dlss.dll-version-invalid",
            HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    const auto* version=static_cast<const VS_FIXEDFILEINFO*>(raw);
    Require(version->dwSignature==0xfeef04bdu,
            loaded?"loaded-nvngx_dlss.dll-version-invalid":"nvngx_dlss.dll-version-invalid",
            HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    Require(version->dwFileVersionMS==MAKELONG(9,310)&&version->dwFileVersionLS==MAKELONG(0,1),
            loaded?"loaded-nvngx_dlss.dll-wrong-version-required-310.9.1.0":
            "nvngx_dlss.dll-wrong-version-required-310.9.1.0",HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
}
std::wstring ModelFinalPath(const wchar_t* path){
    LocalHandle file(CreateFileW(path,FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    Require(file.value!=INVALID_HANDLE_VALUE,"nvngx_dlss.dll-path-unverifiable",E_ACCESSDENIED);
    wchar_t finalPath[32768]{};
    const DWORD length=GetFinalPathNameByHandleW(file.value,finalPath,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    Require(length>0&&length<32768,"nvngx_dlss.dll-path-unverifiable",HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME));
    return std::wstring(finalPath,length);
}
void RejectConflictingLoadedModel(const std::wstring& required){
    const std::wstring expected=ModelFinalPath(required.c_str());
    HANDLE handle=INVALID_HANDLE_VALUE;
    // A loader mutation can invalidate a Toolhelp snapshot; retry only that
    // documented transient failure, then fail closed before calling NGX.
    for(unsigned attempt=0;attempt<3;++attempt){
        handle=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,GetCurrentProcessId());
        if(handle!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_BAD_LENGTH)break;
    }
    LocalHandle snapshot(handle);
    Require(snapshot.value!=INVALID_HANDLE_VALUE,"nvngx_dlss.dll-loaded-module-snapshot-failed",E_FAIL);
    MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
    BOOL found=Module32FirstW(snapshot.value,&entry);
    Require(found!=FALSE,"nvngx_dlss.dll-loaded-module-enumeration-failed",E_FAIL);
    do {
        if(_wcsicmp(entry.szModule,L"nvngx_dlss.dll")==0){
            // GetModuleFileName supports long paths unlike szExePath. This is
            // a pre-init snapshot and on-disk version check, not a receipt of
            // the module NGX will select later or its effective model preset.
            wchar_t loadedPath[32768]{};
            const DWORD length=GetModuleFileNameW(entry.hModule,loadedPath,32768);
            Require(length>0&&length<32768,"loaded-nvngx_dlss.dll-path-unverifiable",E_FAIL);
            const std::wstring actual=ModelFinalPath(loadedPath);
            Require(_wcsicmp(expected.c_str(),actual.c_str())==0,
                    "loaded-nvngx_dlss.dll-conflicts-with-process-directory",HRESULT_FROM_WIN32(ERROR_DLL_INIT_FAILED));
            RequireModelVersion(loadedPath,true);
        }
        found=Module32NextW(snapshot.value,&entry);
    }while(found);
    Require(GetLastError()==ERROR_NO_MORE_FILES,"nvngx_dlss.dll-loaded-module-enumeration-failed",E_FAIL);
}
std::wstring ModelBesideProcess(){
    wchar_t path[32768]{};
    const DWORD length=GetModuleFileNameW(nullptr,path,32768);
    Require(length>0&&length<32768,"process-executable-path",HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME));
    wchar_t* slash=wcsrchr(path,L'\\');
    Require(slash!=nullptr,"process-executable-directory");
    *(slash+1)=0;
    const std::wstring model=std::wstring(path)+L"nvngx_dlss.dll";
    const DWORD attributes=GetFileAttributesW(model.c_str());
    Require(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_DIRECTORY),
            "nvngx_dlss.dll-missing-beside-process",HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    RequireModelVersion(model.c_str(),false);
    RejectConflictingLoadedModel(model);
    return model;
}
class ContextLock {
    ComPtr<ID3D11Multithread> multithread_;
public:
    explicit ContextLock(ID3D11DeviceContext* context){
        Check(context->QueryInterface(IID_PPV_ARGS(&multithread_)),"context-multithread-interface");
        Require(multithread_->GetMultithreadProtected()!=FALSE,"context-protection-lost",E_ACCESSDENIED);
        multithread_->Enter();
    }
    ~ContextLock(){multithread_->Leave();}
    ContextLock(const ContextLock&)=delete;ContextLock& operator=(const ContextLock&)=delete;
};
}

struct NativeSr::Impl {
    struct Input {
        ComPtr<ID3D11Texture2D> texture11;
        ComPtr<ID3D12Resource> texture12;
        HANDLE shared=nullptr;
    };
    mutable std::mutex mutex;
    bool attempted=false,ready=false,outputValid=false,closed=false;
    bool recording=false,commands=false,inFlight=false,unknown=false,poisoned=false;
    bool coreCalled=false,coreReady=false;
    unsigned eyeWidth=0,height=0;
    UINT64 fenceValue=0;
    const char* reason="not-initialized";
    EvaluationReceipt lastEvaluation{};
    std::uint64_t historyGeneration=0,evaluationOrdinal=0;
    std::wstring dataDirectory;
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Fence> fence11;
    HANDLE event=nullptr;
    Input input[3];
    // Retain caller resources before the first D3D11 copy, through retirement.
    ComPtr<ID3D11Texture2D> borrowed[3];
    ets2_d3d11::ProtectionRegistry protection;
    ets2_post_sr::Owner owner;

    ~Impl(){
        // Destruction is reached only after Shutdown proves retirement, or
        // before any GPU/core activity. Unknown states detach the entire Impl.
        for(auto& i:input)if(i.shared)CloseHandle(i.shared);
        if(event)CloseHandle(event);
    }
    UINT64 NextFence(){
        Require(fenceValue<std::numeric_limits<UINT64>::max()-1,"fence-value-exhausted",E_UNEXPECTED);
        return ++fenceValue;
    }
    void Wait(UINT64 value){
        auto completed=fence12->GetCompletedValue();
        Require(completed!=UINT64_MAX,"fence-device-removed",DXGI_ERROR_DEVICE_REMOVED);
        if(completed<value){
            Check(fence12->SetEventOnCompletion(value,event),"fence-event-registration");
            const DWORD result=WaitForSingleObject(event,FenceTimeoutMs);
            Require(result==WAIT_OBJECT_0,"fence-wait-unconfirmed",
                    result==WAIT_TIMEOUT?HRESULT_FROM_WIN32(ERROR_TIMEOUT):E_FAIL);
        }
        completed=fence12->GetCompletedValue();
        Check(device12->GetDeviceRemovedReason(),"d3d12-device-removed");
        Check(device11->GetDeviceRemovedReason(),"d3d11-device-removed");
        Require(completed!=UINT64_MAX&&completed>=value,"fence-retirement-unconfirmed",E_FAIL);
    }
    void Begin(){
        Require(!recording&&!commands&&!inFlight&&!unknown,"commands-not-retired",E_UNEXPECTED);
        Check(allocator->Reset(),"allocator-reset");
        Check(list->Reset(allocator.Get(),nullptr),"list-reset");recording=true;
    }
    void SubmitAndWait(){
        Require(recording&&!poisoned,"submission-state",E_UNEXPECTED);
        // A Close/submission/signal/wait failure makes retirement unknown.
        unknown=true;
        Check(list->Close(),"list-close");recording=false;
        ID3D12CommandList* lists[]={list.Get()};
        inFlight=true;queue->ExecuteCommandLists(1,lists);
        const UINT64 value=NextFence();Check(queue->Signal(fence12.Get(),value),"queue-signal");
        Wait(value);inFlight=false;commands=false;unknown=false;
        for(auto& resource:borrowed)resource.Reset();
    }
    void Barrier(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
        commands=true;list->ResourceBarrier(1,&barrier);
    }
    void ShareInput(unsigned slot,DXGI_FORMAT format,ID3D11Device1* dev1){
        auto& value=input[slot];
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=UINT64(eyeWidth)*2;d.Height=height;d.DepthOrArraySize=1;d.MipLevels=1;
        d.Format=format;d.SampleDesc.Count=1;
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        HRESULT hr=device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&d,D3D12_RESOURCE_STATE_COMMON,
            nullptr,IID_PPV_ARGS(&value.texture12));
        if(SUCCEEDED(hr))hr=device12->CreateSharedHandle(value.texture12.Get(),nullptr,GENERIC_ALL,nullptr,&value.shared);
        if(SUCCEEDED(hr))hr=dev1->OpenSharedResource1(value.shared,IID_PPV_ARGS(&value.texture11));
        if(FAILED(hr)){
            value.texture11.Reset();value.texture12.Reset();
            if(value.shared){CloseHandle(value.shared);value.shared=nullptr;}
            D3D11_TEXTURE2D_DESC d11{};d11.Width=eyeWidth*2;d11.Height=height;
            d11.MipLevels=1;d11.ArraySize=1;d11.Format=format;d11.SampleDesc.Count=1;
            d11.Usage=D3D11_USAGE_DEFAULT;d11.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            d11.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
            Check(device11->CreateTexture2D(&d11,nullptr,&value.texture11),"shared-input-create11");
            ComPtr<IDXGIResource1> dxgi;Check(value.texture11.As(&dxgi),"shared-input-dxgi-interface");
            Check(dxgi->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,
                                          nullptr,&value.shared),"shared-input-handle11");
            Check(device12->OpenSharedHandle(value.shared,IID_PPV_ARGS(&value.texture12)),"shared-input-open12");
        }
        D3D11_TEXTURE2D_DESC actual{};value.texture11->GetDesc(&actual);
        const auto actual12=value.texture12->GetDesc();
        Require(actual.Width==eyeWidth*2&&actual.Height==height&&actual.Format==format&&
                actual.MipLevels==1&&actual.ArraySize==1&&actual.SampleDesc.Count==1&&
                actual12.Width==UINT64(eyeWidth)*2&&actual12.Height==height&&actual12.Format==format&&
                actual12.MipLevels==1&&actual12.DepthOrArraySize==1&&actual12.SampleDesc.Count==1,
                "shared-input-description");
    }
    void ValidateInput(ID3D11Texture2D* texture,DXGI_FORMAT format){
        Require(texture!=nullptr,"missing-real-input",E_POINTER);
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
        Require(d.Width==eyeWidth*2&&d.Height==height&&d.Format==format&&d.MipLevels==1&&d.ArraySize==1&&
                d.SampleDesc.Count==1&&d.SampleDesc.Quality==0&&d.Usage==D3D11_USAGE_DEFAULT&&d.CPUAccessFlags==0,
                "input-texture-description");
        ComPtr<ID3D11Device> sourceDevice;texture->GetDevice(&sourceDevice);
        Require(DeviceIdentity(sourceDevice.Get()).Get()==DeviceIdentity(device11.Get()).Get(),"input-device-mismatch");
        for(const auto& destination:input)Require(!Same(texture,destination.texture11.Get()),"input-alias");
    }
    void ConsumeFence(){
        // Caller has stopped scheduling use of Output(). Signal follows every
        // previously issued game-context read of the owner's shared outputs.
        ContextLock lock(context.Get());
        const UINT64 value=NextFence();unknown=true;inFlight=true;
        Check(context4->Signal(fence11.Get(),value),"output-consumption-signal");
        context->Flush();Wait(value);inFlight=false;unknown=false;
    }
    void RetireOwner(){
        Require(!unknown&&!poisoned&&!owner.Quarantined(),"retirement-unknown-retained",E_UNEXPECTED);
        if(recording)SubmitAndWait();
        Require(!commands&&!inFlight,"work-not-retired",E_UNEXPECTED);
        if(owner.HasResources()){
            Require(context4&&fence11,"output-retirement-unavailable",E_UNEXPECTED);
            ConsumeFence();
            const auto released=owner.ReleaseAfterRetired();
            if(!released.success){poisoned=true;throw Failure{E_FAIL,released.stage};}
        }
    }
    void CreateOwner(int preset){
        Require(coreReady&&ValidPreset(preset),"native-preset-or-core",E_UNEXPECTED);
        const auto requested=static_cast<ets2_post_sr_model::Preset>(preset);
        // Use the same public hint path for the dedicated query map and each
        // creation map. A setter/request is not evidence of an effective model.
        const ets2_post_sr_model::Request model{requested,ets2_post_sr_model::CreationPolicy::FixedInputMaximum};
        Require(ets2_post_sr_model::ValidRequest(model,5),"native-preset-request",E_INVALIDARG);
        Begin();commands=true;
        const auto created=owner.Initialize(device12.Get(),device11.Get(),list.Get(),eyeWidth,height,eyeWidth,height,
            true,5,false,false,model);
        if(created.exception||owner.Quarantined()){poisoned=true;throw Failure{E_FAIL,created.stage};}
        // A failed create may still record commands or return one eye handle.
        // Retire its submission before permitting a different preset request.
        SubmitAndWait();
        Require(created.success,created.stage,FAILED(created.hresult)?created.hresult:E_FAIL);
        const auto& contract=owner.SettingsContract();
        Require(contract.valid&&contract.quality==5&&contract.requestedPreset==requested&&
                contract.creationPolicy==ets2_post_sr_model::CreationPolicy::FixedInputMaximum&&
                contract.createWidth==eyeWidth&&contract.createHeight==height&&
                contract.evaluateWidth==eyeWidth&&contract.evaluateHeight==height&&contract.outputWidth==eyeWidth&&
                contract.outputHeight==height&&owner.CreateFlags()==74,"native-create-contract",E_FAIL);
        ready=true;
        ++historyGeneration;evaluationOrdinal=0;
        reason=preset==13?"native-M-request-DLAA-ready":preset==12?"native-L-request-DLAA-ready":"native-K-request-DLAA-ready";
    }
    bool Close(){
        ready=false;outputValid=false;lastEvaluation={};
        if(closed)return true;
        if(unknown||poisoned||owner.Quarantined()){reason="retirement-unknown-retained";return false;}
        try {
            RetireOwner();
            protection.ReleaseDevice(device11.Get());
            for(auto& value:input){value.texture11.Reset();value.texture12.Reset();
                if(value.shared){CloseHandle(value.shared);value.shared=nullptr;}}
            for(auto& value:borrowed)value.Reset();
            context4.Reset();context.Reset();fence11.Reset();fence12.Reset();
            list.Reset();allocator.Reset();queue.Reset();device11.Reset();device12.Reset();
            if(event){CloseHandle(event);event=nullptr;}
            coreReady=false;closed=true;reason="released-after-retirement";return true;
        }catch(const Failure& failure){reason=failure.reason;return false;}
        catch(...){poisoned=true;reason="shutdown-exception-retained";return false;}
    }
};

NativeSr::NativeSr():impl_(std::make_unique<Impl>()){}
NativeSr::~NativeSr(){if(impl_&&!Shutdown())(void)impl_.release();}

HRESULT NativeSr::Initialize(ID3D11Device* device,unsigned eyeWidth,unsigned height,const std::wstring& dataDirectory,int preset){
    std::lock_guard<std::mutex> lock(impl_->mutex);auto& s=*impl_;
    if(s.attempted||s.closed){s.reason="initialize-already-attempted-or-closed";return E_UNEXPECTED;}
    s.attempted=true;
    try {
        Require(ValidPreset(preset),"unsupported-preset");
        Require(device&&eyeWidth>=128&&eyeWidth<=8192&&height>=128&&height<=16384,"native-geometry-or-device");
        Require(!dataDirectory.empty()&&dataDirectory.find(L'\0')==std::wstring::npos,"data-directory");
        const DWORD attributes=GetFileAttributesW(dataDirectory.c_str());
        Require(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY),"data-directory-missing");
        const std::wstring model=ModelBesideProcess();
        s.dataDirectory=dataDirectory;s.eyeWidth=eyeWidth;s.height=height;s.device11=device;
        device->GetImmediateContext(&s.context);Require(s.context!=nullptr,"immediate-context",E_NOINTERFACE);
        Require(s.protection.Acquire(s.context.Get()),"context-protection",E_ACCESSDENIED);
        Check(s.context.As(&s.context4),"context4-interface");
        ComPtr<ID3D11Device1> dev1;Check(device->QueryInterface(IID_PPV_ARGS(&dev1)),"device1-interface");
        ComPtr<ID3D11Device5> dev5;Check(device->QueryInterface(IID_PPV_ARGS(&dev5)),"device5-interface");
        ComPtr<IDXGIDevice> dxgi;Check(device->QueryInterface(IID_PPV_ARGS(&dxgi)),"dxgi-device");
        ComPtr<IDXGIAdapter> adapter;Check(dxgi->GetAdapter(&adapter),"game-adapter");
        DXGI_ADAPTER_DESC description{};Check(adapter->GetDesc(&description),"adapter-description");
        Require(description.VendorId==0x10de,"nvidia-adapter-required",DXGI_ERROR_UNSUPPORTED);
        Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&s.device12)),"d3d12-device");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(s.device12->CreateCommandQueue(&q,IID_PPV_ARGS(&s.queue)),"direct-queue");
        Check(s.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator)),"command-allocator");
        Check(s.device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list)),"command-list");
        Check(s.list->Close(),"initial-list-close");
        s.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(s.event!=nullptr,"fence-event",HRESULT_FROM_WIN32(GetLastError()));
        Check(s.device12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&s.fence12)),"shared-fence");
        HANDLE shared=nullptr;
        Check(s.device12->CreateSharedHandle(s.fence12.Get(),nullptr,GENERIC_ALL,nullptr,&shared),"shared-fence-handle");
        const HRESULT opened=dev5->OpenSharedFence(shared,IID_PPV_ARGS(&s.fence11));CloseHandle(shared);
        Check(opened,"open-fence11");
        constexpr DXGI_FORMAT formats[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT};
        for(unsigned i=0;i<3;++i)s.ShareInput(i,formats[i],dev1.Get());
        auto& api=feed_driver::Get();Require(api.ready&&api.init&&api.project,"ngx-driver-api",E_NOINTERFACE);
        // Recheck after loading the driver core, before its first Init call.
        // Neither check loads or executes the model DLL itself.
        RequireModelVersion(model.c_str(),false);
        RejectConflictingLoadedModel(model);
        // The direct core has no Shutdown entry point. Retain the initialized
        // device for process lifetime, including a failed/exceptional Init.
        s.device12->AddRef();s.coreCalled=true;
        const auto initialized=InitializeCore(&api,s.dataDirectory.c_str(),s.device12.Get());
        if(initialized.exception){s.poisoned=true;throw Failure{E_FAIL,"ngx-init-exception-retained"};}
        Require(initialized.code==NVSDK_NGX_Result_Success,"ngx-init-failed",E_FAIL);
        s.coreReady=true;s.CreateOwner(preset);return S_OK;
    }catch(const Failure& failure){s.reason=failure.reason;return failure.hr;}
    catch(...){s.poisoned=s.coreCalled||s.commands||s.inFlight;s.reason="initialize-exception";return E_FAIL;}
}

HRESULT NativeSr::ChangePreset(int preset){
    std::lock_guard<std::mutex> lock(impl_->mutex);auto& s=*impl_;
    s.outputValid=false;s.lastEvaluation={};
    if(!ValidPreset(preset)){s.reason="unsupported-preset";return E_INVALIDARG;}
    s.ready=false;
    if(!s.coreReady||s.closed||s.unknown||s.poisoned||s.owner.Quarantined()){
        s.reason="preset-change-unavailable-or-retirement-unknown";return E_UNEXPECTED;
    }
    try {
        // The caller's session mutex prevents new consumers from obtaining/using an
        // output while this call retires and replaces the two feature histories.
        s.RetireOwner();
        s.CreateOwner(preset);return S_OK;
    }catch(const Failure& failure){s.reason=failure.reason;return failure.hr;}
    catch(...){s.poisoned=true;s.reason="preset-change-exception-retained";return E_FAIL;}
}

HRESULT NativeSr::Evaluate(ID3D11DeviceContext* context,ID3D11Texture2D* color,ID3D11Texture2D* depth,
                           ID3D11Texture2D* motion,bool reset){
    std::lock_guard<std::mutex> lock(impl_->mutex);auto& s=*impl_;s.outputValid=false;s.lastEvaluation={};
    if(!s.ready||s.closed||s.unknown||s.poisoned){s.reason="evaluate-not-ready";return E_UNEXPECTED;}
    try {
        Require(context&&context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE&&Same(context,s.context.Get()),"context-mismatch");
        s.ValidateInput(color,DXGI_FORMAT_R16G16B16A16_FLOAT);
        s.ValidateInput(depth,DXGI_FORMAT_R32_FLOAT);s.ValidateInput(motion,DXGI_FORMAT_R16G16_FLOAT);
        Require(!Same(color,depth)&&!Same(color,motion)&&!Same(depth,motion),"input-alias");
        s.Begin();
        s.borrowed[0]=color;s.borrowed[1]=depth;s.borrowed[2]=motion;
        {
            ContextLock guard(context);
            // Also orders all earlier D3D11 consumption of the previous SR pair.
            const UINT64 value=s.NextFence();s.unknown=true;s.inFlight=true;
            for(unsigned i=0;i<3;++i)context->CopyResource(s.input[i].texture11.Get(),s.borrowed[i].Get());
            Check(s.context4->Signal(s.fence11.Get(),value),"input-signal11");context->Flush();
            Check(s.queue->Wait(s.fence12.Get(),value),"input-wait12");
        }
        for(auto& input:s.input)s.Barrier(input.texture12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto evaluated=s.owner.Evaluate(s.list.Get(),s.input[0].texture12.Get(),s.input[1].texture12.Get(),
            s.input[2].texture12.Get(),reset,0,0,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        s.lastEvaluation.calls=evaluated.evaluation_calls;s.lastEvaluation.success=evaluated.success;
        s.lastEvaluation.requested_reset=reset;s.lastEvaluation.applied_reset=evaluated.applied_reset;
        s.lastEvaluation.history_generation=s.historyGeneration;
        for(unsigned eye=0;eye<2;++eye)s.lastEvaluation.handles[eye]=
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(s.owner.Handle(eye)));
        if(evaluated.exception||s.owner.Quarantined()){s.poisoned=true;throw Failure{E_FAIL,evaluated.stage};}
        for(auto& input:s.input)s.Barrier(input.texture12.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        s.SubmitAndWait();
        Require(evaluated.success&&evaluated.evaluation_calls==2,evaluated.stage,
                FAILED(evaluated.hresult)?evaluated.hresult:E_FAIL);
        s.lastEvaluation.evaluation_ordinal=++s.evaluationOrdinal;
        s.outputValid=true;s.reason="current-native-two-eye-SR-retired";return S_OK;
    }catch(const Failure& failure){s.ready=false;s.reason=failure.reason;return failure.hr;}
    catch(...){s.ready=false;s.poisoned=true;s.reason="evaluate-exception-retained";return E_FAIL;}
}

ID3D11ShaderResourceView* NativeSr::Output(unsigned eye)const{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->ready&&impl_->outputValid?impl_->owner.OutputSrv(eye):nullptr;
}
EvaluationReceipt NativeSr::LastEvaluation()const{
    std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->lastEvaluation;
}
HRESULT NativeSr::CaptureLastInputs(const std::wstring& directory){
    std::lock_guard<std::mutex> lock(impl_->mutex);auto& s=*impl_;
    if(!s.ready||!s.outputValid||!s.lastEvaluation.success||s.lastEvaluation.calls!=2||
       s.closed||s.unknown||s.poisoned)return E_UNEXPECTED;
    try {
        Require(!directory.empty(),"diagnostic-directory");
        const std::filesystem::path path(directory);
        std::filesystem::create_directories(path);
        std::ofstream metadata(path/L"native-inputs.json",std::ios::binary);
        Require(metadata.good(),"diagnostic-metadata-open",E_FAIL);
        const auto& receipt=s.lastEvaluation;
        metadata<<"{\"width\":"<<s.eyeWidth<<",\"height\":"<<s.height
            <<",\"requested_reset\":"<<(receipt.requested_reset?"true":"false")
            <<",\"applied_reset\":"<<(receipt.applied_reset?"true":"false")
            <<",\"history_generation\":"<<receipt.history_generation
            <<",\"evaluation_ordinal\":"<<receipt.evaluation_ordinal<<",\"parameters\":[";
        for(unsigned eye=0;eye<2;++eye){
            const auto* parameters=s.owner.Parameters(eye);
            Require(parameters!=nullptr,"diagnostic-parameters-unavailable",E_UNEXPECTED);
            const auto values=ReadDiagnosticParameters(parameters);
            Require(values.success&&!values.exception&&std::isfinite(values.jitterX)&&
                std::isfinite(values.jitterY)&&std::isfinite(values.scaleX)&&std::isfinite(values.scaleY),
                "diagnostic-parameter-readback",E_FAIL);
            if(eye)metadata<<',';
            metadata<<"{\"eye\":"<<eye<<",\"jitter_x\":"<<values.jitterX<<",\"jitter_y\":"<<values.jitterY
                <<",\"motion_scale_x\":"<<values.scaleX<<",\"motion_scale_y\":"<<values.scaleY<<",\"reset\":"<<values.reset<<'}';
        }
        metadata<<"],\"planes\":[";
        constexpr const char* names[]={"color.rgba16f","depth.r32f","motion.rg16f"};
        constexpr unsigned pixelBytes[]={8,4,4};
        for(unsigned eye=0;eye<2;++eye)for(unsigned slot=0;slot<3;++slot){
            ID3D12Resource* input=s.owner.InputResource(eye,slot);
            Require(input!=nullptr,"diagnostic-input-unavailable",E_UNEXPECTED);
            const auto desc=input->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT rows=0;UINT64 rowBytes=0,totalBytes=0;
            s.device12->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&totalBytes);
            Require(rows==s.height&&rowBytes==UINT64(s.eyeWidth)*pixelBytes[slot],"diagnostic-input-footprint",E_FAIL);
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width=totalBytes;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;
            buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> readback;
            Check(s.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
                D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"diagnostic-readback-create");
            s.Begin();
            s.Barrier(input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=input;
            from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=readback.Get();
            to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
            s.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            s.Barrier(input,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // If retirement fails, retain the pending copy destination as well.
            try{s.SubmitAndWait();}catch(...){(void)readback.Detach();throw;}
            void* mapped=nullptr;D3D12_RANGE readRange{0,static_cast<SIZE_T>(totalBytes)};
            Check(readback->Map(0,&readRange,&mapped),"diagnostic-readback-map");
            const std::string name="eye"+std::to_string(eye)+"-"+names[slot];
            std::ofstream out(path/name,std::ios::binary);
            for(UINT row=0;row<rows&&out.good();++row)out.write(
                static_cast<const char*>(mapped)+footprint.Offset+UINT64(row)*footprint.Footprint.RowPitch,
                static_cast<std::streamsize>(rowBytes));
            D3D12_RANGE noWrites{0,0};readback->Unmap(0,&noWrites);out.close();
            Require(out.good(),"diagnostic-input-write",E_FAIL);
            if(eye||slot)metadata<<',';
            metadata<<"{\"eye\":"<<eye<<",\"slot\":"<<slot<<",\"file\":\""<<name
                <<"\",\"bytes\":"<<rowBytes*rows<<",\"resource\":"<<reinterpret_cast<std::uintptr_t>(input)
                <<",\"parameters\":"<<reinterpret_cast<std::uintptr_t>(s.owner.Parameters(eye))
                <<",\"feature\":"<<receipt.handles[eye]<<'}';
        }
        metadata<<"]}\n";metadata.close();Require(metadata.good(),"diagnostic-metadata-write",E_FAIL);
        return S_OK;
    }catch(const Failure& failure){if(s.unknown||s.poisoned)s.ready=false;return failure.hr;}
    catch(...){if(s.recording||s.commands||s.inFlight){s.poisoned=true;s.ready=false;}return E_FAIL;}
}
bool NativeSr::Shutdown(){std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->Close();}
const char* NativeSr::Reason()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->reason;}
}
