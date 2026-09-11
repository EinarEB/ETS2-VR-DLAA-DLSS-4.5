// Copyright (c) 2026 ETS2 VR preview contributors. SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <cstdint>
#include <cmath>
#include <cstring>
#include "feed_driver_api.h"
#include "feed_post_sr_settings.h"

// Experimental post-NR SR transport. The caller owns the initialized NGX core,
// command list, submission, cross-API fences and presentation. No queue is owned.
// Initialize records feature creation: submit/fence it before Evaluate, including
// a partially failed Initialize. Never destroy or reuse an owner until every use
// has retired. Unknown retirement or an NGX exception retains all dependencies.
namespace ets2_post_sr {
// NVIDIA DLSS SDK flag value; guide section 3.6.2 requires evaluation jitter
// when supplied motion already includes the rasterization jitter displacement.
constexpr int NVSDK_NGX_DLSS_Feature_Flags_MVJittered=1<<2;
// Static-view diagnostic only. The upload allocation is filled once and never
// modified again. Its GENERIC_READ state includes COPY_SOURCE on every use.
// As with Owner, scope destruction is not evidence of GPU retirement.
class ZeroMotionUpload {
    ID3D12Resource* resource_=nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    UINT64 bytes_=0;
    bool filled_=false;
public:
    ZeroMotionUpload()=default;~ZeroMotionUpload()=default;
    ZeroMotionUpload(const ZeroMotionUpload&)=delete;ZeroMotionUpload& operator=(const ZeroMotionUpload&)=delete;
    static bool ValidFootprint(unsigned width,unsigned height,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f,
                               UINT rows,UINT64 rowBytes,UINT64 bytes) noexcept {
        if(!width||!height||width>16384||height>16384||f.Offset!=0||
           f.Footprint.Format!=DXGI_FORMAT_R16G16_FLOAT||f.Footprint.Width!=width||
           f.Footprint.Height!=height||f.Footprint.Depth!=1||rows!=height||rowBytes!=UINT64(width)*4||
           f.Footprint.RowPitch<rowBytes||f.Footprint.RowPitch%D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)return false;
        const UINT64 required=UINT64(f.Footprint.RowPitch)*(height-1)+rowBytes;
        return bytes>=required&&bytes<=UINT64(f.Footprint.RowPitch)*height&&bytes<=SIZE_MAX;
    }
    HRESULT Create(ID3D12Device* device,unsigned width,unsigned height) noexcept {
        if(resource_)return E_UNEXPECTED;
        if(!device||!width||!height||width>16384||height>16384)return E_INVALIDARG;
        D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width=width;texture.Height=height;texture.DepthOrArraySize=1;texture.MipLevels=1;
        texture.Format=DXGI_FORMAT_R16G16_FLOAT;texture.SampleDesc.Count=1;
        UINT rows=0;UINT64 rowBytes=0;
        device->GetCopyableFootprints(&texture,0,1,0,&footprint_,&rows,&rowBytes,&bytes_);
        if(!ValidFootprint(width,height,footprint_,rows,rowBytes,bytes_))return E_INVALIDARG;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes_;
        buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        HRESULT hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&resource_));
        if(FAILED(hr))return hr;
        void* mapped=nullptr;const D3D12_RANGE noRead{0,0};hr=resource_->Map(0,&noRead,&mapped);
        if(FAILED(hr))return hr;
        if(!mapped){resource_->Unmap(0,&noRead);return E_POINTER;}
        std::memset(mapped,0,size_t(bytes_));const D3D12_RANGE written{0,size_t(bytes_)};resource_->Unmap(0,&written);filled_=true;
        return S_OK;
    }
    HRESULT CopyTo(ID3D12GraphicsCommandList* list,ID3D12Resource* target)const noexcept {
        if(!filled_||!resource_||!list||!target||list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
        const auto d=target->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Width!=footprint_.Footprint.Width||
           d.Height!=footprint_.Footprint.Height||d.Format!=DXGI_FORMAT_R16G16_FLOAT||
           d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||d.SampleDesc.Quality!=0)return E_INVALIDARG;
        ID3D12Device *own=nullptr,*destination=nullptr,*commands=nullptr;
        const HRESULT a=resource_->GetDevice(__uuidof(ID3D12Device),reinterpret_cast<void**>(&own));
        const HRESULT b=target->GetDevice(__uuidof(ID3D12Device),reinterpret_cast<void**>(&destination));
        const HRESULT c=list->GetDevice(__uuidof(ID3D12Device),reinterpret_cast<void**>(&commands));
        const bool same=SUCCEEDED(a)&&SUCCEEDED(b)&&SUCCEEDED(c)&&own==destination&&own==commands;
        if(own)own->Release();if(destination)destination->Release();if(commands)commands->Release();if(!same)return E_INVALIDARG;
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={target,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST};
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=resource_;from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint_;
        to.pResource=target;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1,&barrier);return S_OK;
    }
    ID3D12Resource* Resource()const noexcept{return resource_;}
    UINT64 Bytes()const noexcept{return bytes_;}
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& Footprint()const noexcept{return footprint_;}
    void ReleaseAfterRetired()noexcept{if(resource_){resource_->Release();resource_=nullptr;}footprint_={};bytes_=0;filled_=false;}
};
struct Result {
    bool success=false;
    NVSDK_NGX_Result ngx_result=NVSDK_NGX_Result_Success;
    HRESULT hresult=S_OK;
    DWORD exception=0;
    const char* stage="not-started";
    unsigned eye=2; // 2 denotes a whole-owner operation.
    bool commands_recorded=false;
    unsigned evaluation_calls=0; // successful or failed driver calls in this Evaluate only
    bool applied_reset=false; // meaningful only when evaluation_calls is nonzero
};

class Owner {
    struct Eye {
        NVSDK_NGX_Parameter* parameters=nullptr;
        NVSDK_NGX_Handle* feature=nullptr;
        ID3D12Resource* input[3]={}; // RGBA16F color, R32F depth, RG16F motion.
        ID3D12Resource* output12=nullptr;
        ID3D11Texture2D* output11=nullptr;
        ID3D11ShaderResourceView* output_srv=nullptr;
        HANDLE shared=nullptr;
        const char* shared_direction="none";
    };
    Eye eyes_[2];
    ID3D12Device* device12_=nullptr;
    ID3D11Device* device11_=nullptr;
    feed_driver::Api* api_=nullptr;
    unsigned input_width_=0,input_height_=0,output_width_=0,output_height_=0;
    unsigned create_width_=0,create_height_=0;
    int requested_quality_=-1,create_quality_=NVSDK_NGX_PerfQuality_Value_MaxPerf;
    ets2_post_sr_model::Request model_request_{};
    ets2_post_sr_settings::QueryOwner settings_query_;
    ets2_post_sr_settings::Receipt settings_receipt_{};
    ets2_post_sr_settings::Contract settings_contract_{};
    ets2_post_sr_settings::Call settings_acquire_{},settings_destroy_{};
    bool ready_=false,output_valid_=false,ever_evaluated_=false;
    bool commands_recorded_=false,quarantined_=false;
    bool zero_motion_=false;
    ZeroMotionUpload zero_motion_upload_;
    int flags_=0;

    template<class T> static void Drop(T*& value) noexcept {
        if(value){value->Release();value=nullptr;}
    }
    Result Status(const char* stage,HRESULT hr=S_OK,int ngx=NVSDK_NGX_Result_Success,
                  DWORD exception=0,unsigned eye=2) const noexcept {
        return {SUCCEEDED(hr)&&ngx==NVSDK_NGX_Result_Success&&exception==0,
                ngx,hr,exception,stage,eye,commands_recorded_};
    }
    static ets2_post_sr_settings::Call SettingsFailure(const ets2_post_sr_settings::Receipt& r) noexcept {
        const ets2_post_sr_settings::Call calls[]={r.availabilityGet,r.callbackGet,r.setRequest,r.callback};
        for(const auto& c:calls)if(c.exception)return c;
        for(const auto& field:r.fields)if(field.call.exception)return field.call;
        if(r.sharpness.call.exception)return r.sharpness.call;
        for(const auto& c:calls)if(!ets2_post_sr_settings::Success(c))return c;
        for(const auto& field:r.fields)if(!ets2_post_sr_settings::Success(field.call))return field.call;
        return {NVSDK_NGX_Result_Success,0,true};
    }
    Result NegotiateSettings() noexcept {
        // No feature commands exist yet. The capability map is independent of
        // both eye parameter maps and can be destroyed immediately after query.
        settings_acquire_=settings_query_.Acquire(*api_);
        if(settings_query_.Quarantined()){
            quarantined_=true;
            return Status("settings-capabilities",E_FAIL,settings_acquire_.code,settings_acquire_.exception);
        }
        const bool acquired=ets2_post_sr_settings::Success(settings_acquire_)&&settings_query_.HasParameters();
        if(acquired){
            settings_receipt_=settings_query_.Read(output_width_,output_height_,requested_quality_,model_request_.preset);
            settings_contract_=ets2_post_sr_settings::MakeContract(settings_receipt_,input_width_,input_height_,model_request_.creationPolicy);
        }
        if(settings_query_.Quarantined()){
            quarantined_=true;const auto failure=SettingsFailure(settings_receipt_);
            return Status("settings-query-exception",E_FAIL,failure.code,failure.exception);
        }
        // Also destroy a partially returned map on an ordinary acquisition
        // failure. A failed or exceptional destroy retains the entire owner.
        if(settings_query_.HasParameters()){
            settings_destroy_=settings_query_.Destroy(*api_);
            if(!ets2_post_sr_settings::Success(settings_destroy_)){
                quarantined_=true;
                return Status("settings-destroy-capabilities",E_FAIL,settings_destroy_.code,settings_destroy_.exception);
            }
        }
        if(!acquired)return Status("settings-capabilities",E_POINTER,settings_acquire_.code);
        if(!settings_receipt_.valid){const auto failure=SettingsFailure(settings_receipt_);
            return Status("settings-query-invalid",E_INVALIDARG,failure.code,failure.exception);}
        if(!settings_contract_.valid)return Status("settings-input-outside-range",E_INVALIDARG);
        create_width_=settings_contract_.createWidth;create_height_=settings_contract_.createHeight;
        create_quality_=settings_contract_.quality;
        return Status("settings-negotiated");
    }
    static void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) noexcept {
        D3D12_RESOURCE_BARRIER barrier={};
        barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
        list->ResourceBarrier(1,&barrier);
    }
    static D3D12_RESOURCE_DESC Description(unsigned width,unsigned height,DXGI_FORMAT format,
                                           D3D12_RESOURCE_FLAGS flags) noexcept {
        D3D12_RESOURCE_DESC desc={};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=width;desc.Height=height;desc.DepthOrArraySize=1;desc.MipLevels=1;
        desc.Format=format;desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;desc.Flags=flags;
        return desc;
    }
    bool CorrectList(ID3D12GraphicsCommandList* list) const noexcept {
        if(!list||list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
        ID3D12Device* owner=nullptr;
        const HRESULT hr=list->GetDevice(__uuidof(ID3D12Device),reinterpret_cast<void**>(&owner));
        const bool same=SUCCEEDED(hr)&&owner==device12_;Drop(owner);return same;
    }
    bool CorrectSource(ID3D12Resource* resource,DXGI_FORMAT format) const noexcept {
        if(!resource)return false;
        const auto desc=resource->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Width!=UINT64(input_width_)*2||
           desc.Height!=input_height_||desc.Format!=format||desc.DepthOrArraySize!=1||
           desc.MipLevels!=1||desc.SampleDesc.Count!=1)return false;
        ID3D12Device* owner=nullptr;
        const HRESULT hr=resource->GetDevice(__uuidof(ID3D12Device),reinterpret_cast<void**>(&owner));
        const bool same=SUCCEEDED(hr)&&owner==device12_;Drop(owner);return same;
    }
    // These leaves deliberately contain no objects requiring C++ unwinding.
    static int AllocateLeaf(feed_driver::Allocate function,NVSDK_NGX_Parameter** parameters,
                            DWORD* exception) noexcept {
        __try {return function(parameters);}
        __except(EXCEPTION_EXECUTE_HANDLER){*exception=GetExceptionCode();return NVSDK_NGX_Result_Fail;}
    }
    static int CreateLeaf(feed_driver::Create function,ID3D12GraphicsCommandList* list,
                          NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** feature,
                          unsigned iw,unsigned ih,unsigned ow,unsigned oh,int quality,int flags,
                          ets2_post_sr_model::Preset preset,DWORD* exception) noexcept {
        __try {
            p->Set("Width",iw);p->Set("Height",ih);p->Set("OutWidth",ow);p->Set("OutHeight",oh);
            p->Set("PerfQualityValue",quality);
            p->Set("DLSS.Feature.Create.Flags",flags);p->Set("DLSS.Enable.Output.Subrects",0);
            p->Set("CreationNodeMask",1u);p->Set("VisibilityNodeMask",1u);p->Set("RTXValue",0);
            if(!ets2_post_sr_model::ApplyHint(p,preset,quality))return NVSDK_NGX_Result_Fail;
            return function(list,NVSDK_NGX_Feature_SuperSampling,p,feature);
        } __except(EXCEPTION_EXECUTE_HANDLER){*exception=GetExceptionCode();return NVSDK_NGX_Result_Fail;}
    }
    static int EvaluateLeaf(feed_driver::Evaluate function,ID3D12GraphicsCommandList* list,
                            Eye* eye,unsigned width,unsigned height,bool reset,float jitterX,float jitterY,
                            DWORD* exception,unsigned* evaluationCalls) noexcept {
        __try {
            NVSDK_NGX_Parameter* p=eye->parameters;
            p->Set("Color",eye->input[0]);p->Set("Output",eye->output12);
            p->Set("Depth",eye->input[1]);p->Set("MotionVectors",eye->input[2]);
            p->Set("DLSS.Input.Bias.Current.Color.Mask",static_cast<ID3D12Resource*>(nullptr));
            p->Set("ExposureTexture",static_cast<ID3D12Resource*>(nullptr));
            p->Set("Jitter.Offset.X",jitterX);p->Set("Jitter.Offset.Y",jitterY);
            p->Set("MV.Scale.X",1.0f);p->Set("MV.Scale.Y",1.0f);
            p->Set("Sharpness",0.0f);p->Set("Reset",reset?1:0);
            p->Set("DLSS.Pre.Exposure",1.0f);p->Set("DLSS.Exposure.Scale",1.0f);
            p->Set("DLSS.Render.Subrect.Dimensions.Width",width);
            p->Set("DLSS.Render.Subrect.Dimensions.Height",height);
            const char* bases[]={"DLSS.Input.Color.Subrect.Base.X","DLSS.Input.Color.Subrect.Base.Y",
                "DLSS.Input.Depth.Subrect.Base.X","DLSS.Input.Depth.Subrect.Base.Y",
                "DLSS.Input.MV.Subrect.Base.X","DLSS.Input.MV.Subrect.Base.Y",
                "DLSS.Input.Bias.Current.Color.Subrect.Base.X","DLSS.Input.Bias.Current.Color.Subrect.Base.Y",
                "DLSS.Output.Subrect.Base.X","DLSS.Output.Subrect.Base.Y"};
            for(const char* key:bases)p->Set(key,0u);
            ++*evaluationCalls; // Parameter-map failure above is not an Evaluate call.
            return function(list,eye->feature,p,nullptr);
        } __except(EXCEPTION_EXECUTE_HANDLER){*exception=GetExceptionCode();return NVSDK_NGX_Result_Fail;}
    }
    static int ReleaseLeaf(feed_driver::Release function,NVSDK_NGX_Handle* feature,DWORD* exception) noexcept {
        __try {return function(feature);}
        __except(EXCEPTION_EXECUTE_HANDLER){*exception=GetExceptionCode();return NVSDK_NGX_Result_Fail;}
    }
    static int DestroyLeaf(feed_driver::Destroy function,NVSDK_NGX_Parameter* parameters,DWORD* exception) noexcept {
        __try {return function(parameters);}
        __except(EXCEPTION_EXECUTE_HANDLER){*exception=GetExceptionCode();return NVSDK_NGX_Result_Fail;}
    }
    bool CorrectOutput(const Eye& eye) const noexcept {
        if(!eye.output12||!eye.output11||!eye.shared)return false;
        const auto desc12=eye.output12->GetDesc();
        constexpr auto flags12=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|
                               D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if(desc12.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc12.Width!=output_width_||
           desc12.Height!=output_height_||desc12.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT||
           desc12.DepthOrArraySize!=1||desc12.MipLevels!=1||desc12.SampleDesc.Count!=1||
           desc12.SampleDesc.Quality!=0||(desc12.Flags&flags12)!=flags12||
           (desc12.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return false;
        D3D11_TEXTURE2D_DESC desc11={};eye.output11->GetDesc(&desc11);
        constexpr UINT flags11=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        return desc11.Width==output_width_&&desc11.Height==output_height_&&
            desc11.Format==DXGI_FORMAT_R16G16B16A16_FLOAT&&desc11.MipLevels==1&&desc11.ArraySize==1&&
            desc11.SampleDesc.Count==1&&desc11.SampleDesc.Quality==0&&
            desc11.Usage==D3D11_USAGE_DEFAULT&&desc11.CPUAccessFlags==0&&
            (desc11.BindFlags&flags11)==flags11;
    }
    Result MakeEye(unsigned index,ID3D11Device1* dev1) noexcept {
        auto& eye=eyes_[index];
        D3D12_HEAP_PROPERTIES heap={};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        constexpr DXGI_FORMAT formats[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT};
        for(unsigned slot=0;slot<3;++slot){
            const auto desc=Description(input_width_,input_height_,formats[slot],D3D12_RESOURCE_FLAG_NONE);
            const HRESULT hr=device12_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,__uuidof(ID3D12Resource),
                reinterpret_cast<void**>(&eye.input[slot]));
            if(FAILED(hr))return Status("input-resource",hr,NVSDK_NGX_Result_Success,0,index);
        }
        const auto desc=Description(output_width_,output_height_,DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
        HRESULT hr=device12_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,
            D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&eye.output12));
        if(SUCCEEDED(hr))hr=device12_->CreateSharedHandle(eye.output12,nullptr,GENERIC_ALL,nullptr,&eye.shared);
        if(SUCCEEDED(hr))hr=dev1->OpenSharedResource1(eye.shared,__uuidof(ID3D11Texture2D),
                                                   reinterpret_cast<void**>(&eye.output11));
        if(SUCCEEDED(hr)&&!CorrectOutput(eye))hr=E_INVALIDARG;
        if(SUCCEEDED(hr))eye.shared_direction="D3D12->D3D11";
        else {
            // MakeEye runs before every feature create/evaluate and records no GPU commands.
            // These failed first-route objects have never been used on either API.
            Drop(eye.output11);Drop(eye.output12);
            if(eye.shared){CloseHandle(eye.shared);eye.shared=nullptr;}
            eye.shared_direction="D3D11->D3D12";
            // Match the established MakeSharedPair fallback exactly. No keyed
            // mutex: the parent owns the shared fences and COMMON handoffs.
            D3D11_TEXTURE2D_DESC desc11={};
            desc11.Width=output_width_;desc11.Height=output_height_;
            desc11.MipLevels=1;desc11.ArraySize=1;desc11.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc11.SampleDesc.Count=1;desc11.Usage=D3D11_USAGE_DEFAULT;
            desc11.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            desc11.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
            hr=dev1->CreateTexture2D(&desc11,nullptr,&eye.output11);
            if(FAILED(hr))return Status("shared-output-11to12-resource",hr,NVSDK_NGX_Result_Success,0,index);
            IDXGIResource1* dxgi=nullptr;
            hr=eye.output11->QueryInterface(__uuidof(IDXGIResource1),reinterpret_cast<void**>(&dxgi));
            if(FAILED(hr))return Status("shared-output-11to12-interface",hr,NVSDK_NGX_Result_Success,0,index);
            hr=dxgi->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,
                                       nullptr,&eye.shared);
            Drop(dxgi);
            if(FAILED(hr))return Status("shared-output-11to12-handle",hr,NVSDK_NGX_Result_Success,0,index);
            hr=device12_->OpenSharedHandle(eye.shared,__uuidof(ID3D12Resource),
                                          reinterpret_cast<void**>(&eye.output12));
            if(FAILED(hr))return Status("shared-output-11to12-open12",hr,NVSDK_NGX_Result_Success,0,index);
            if(!CorrectOutput(eye))return Status("shared-output-11to12-description",E_INVALIDARG,
                                                NVSDK_NGX_Result_Success,0,index);
            // The newly opened shared resource has had no D3D11 GPU use. As in
            // MakeSharedPair, its D3D12 handoff state is COMMON; Evaluate makes
            // the explicit COMMON -> UAV -> COMMON transitions on our queue.
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srv={};srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
        hr=device11_->CreateShaderResourceView(eye.output11,&srv,&eye.output_srv);
        return Status("shared-output-srv",hr,NVSDK_NGX_Result_Success,0,index);
    }
public:
    Owner()=default;
    // Intentionally no COM/NGX cleanup here: C++ scope does not prove retirement.
    ~Owner()=default;
    Owner(const Owner&)=delete;Owner& operator=(const Owner&)=delete;
    Owner(Owner&&)=delete;Owner& operator=(Owner&&)=delete;
    bool HasResources() const noexcept {return device12_!=nullptr;}
    bool Ready() const noexcept {return ready_&&!quarantined_;}
    bool Quarantined() const noexcept {return quarantined_;}
    bool ZeroMotion() const noexcept {return zero_motion_;}
    bool MotionJittered() const noexcept {return (flags_&NVSDK_NGX_DLSS_Feature_Flags_MVJittered)!=0;}
    int CreateFlags() const noexcept {return flags_;}
    ID3D12Resource* ZeroMotionBuffer() const noexcept {return zero_motion_upload_.Resource();}
    UINT64 ZeroMotionBytes() const noexcept {return zero_motion_upload_.Bytes();}
    int RequestedQuality() const noexcept {return requested_quality_;}
    const ets2_post_sr_model::Request& ModelRequest() const noexcept {return model_request_;}
    // No effective-model getter is documented by the public direct-core API.
    int EffectivePreset() const noexcept {return ets2_post_sr_model::EffectivePresetUnknown;}
    const ets2_post_sr_settings::Receipt& SettingsReceipt() const noexcept {return settings_receipt_;}
    const ets2_post_sr_settings::Contract& SettingsContract() const noexcept {return settings_contract_;}
    ets2_post_sr_settings::Call SettingsAcquireResult() const noexcept {return settings_acquire_;}
    ets2_post_sr_settings::Call SettingsDestroyResult() const noexcept {return settings_destroy_;}
    const NVSDK_NGX_Handle* Handle(unsigned eye) const noexcept {return eye<2?eyes_[eye].feature:nullptr;}
    const NVSDK_NGX_Parameter* Parameters(unsigned eye) const noexcept {return eye<2?eyes_[eye].parameters:nullptr;}
    ID3D12Resource* OutputResource(unsigned eye) const noexcept {return eye<2?eyes_[eye].output12:nullptr;}
    ID3D12Resource* InputResource(unsigned eye,unsigned slot) const noexcept {
        return eye<2&&slot<3?eyes_[eye].input[slot]:nullptr;
    }
    const char* SharedDirection(unsigned eye) const noexcept {return eye<2?eyes_[eye].shared_direction:"none";}
    ID3D11ShaderResourceView* OutputSrv(unsigned eye) const noexcept {
        return eye<2&&Ready()&&output_valid_?eyes_[eye].output_srv:nullptr;
    }
    Result Initialize(ID3D12Device* dev12,ID3D11Device* dev11,ID3D12GraphicsCommandList* list,
                      unsigned inputEyeW,unsigned inputH,unsigned outputEyeW,unsigned outputH,
                      bool depthInverted,int requestedQuality=-1,bool zeroMotion=false,bool motionJittered=false,
                      ets2_post_sr_model::Request model={}) noexcept {
        if(HasResources()||quarantined_)return Status("initialize-existing-owner",E_UNEXPECTED);
        // Zeroed motion cannot also describe raw image flow containing jitter.
        // Refuse before driver lookup, AddRef, resource creation or latch changes.
        if(zeroMotion&&motionJittered)return Status("initialize-motion-flags",E_INVALIDARG);
        if(requestedQuality<-1||requestedQuality>5)return Status("initialize-quality",E_INVALIDARG);
        if(!ets2_post_sr_model::ValidRequest(model,requestedQuality))return Status("initialize-model-request",E_INVALIDARG);
        if(!dev12||!dev11||!list||!inputEyeW||!inputH||!outputEyeW||!outputH||
           inputEyeW>8192||inputH>16384||outputEyeW>8192||outputH>16384||
           outputEyeW<inputEyeW||outputH<inputH)return Status("initialize-geometry",E_INVALIDARG);
        try {api_=&feed_driver::Get();}
        catch(...){return Status("driver-api",E_FAIL);}
        if(!api_->ready||!api_->allocate||!api_->create||!api_->evaluate||!api_->release||!api_->destroy)
            return Status("driver-api",E_NOINTERFACE,FeedDriverUnavailable);
        device12_=dev12;device12_->AddRef();device11_=dev11;device11_->AddRef();
        input_width_=inputEyeW;input_height_=inputH;output_width_=outputEyeW;output_height_=outputH;
        requested_quality_=requestedQuality;create_quality_=NVSDK_NGX_PerfQuality_Value_MaxPerf;
        model_request_=model;
        create_width_=input_width_;create_height_=input_height_;
        zero_motion_=zeroMotion;
        flags_=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_AutoExposure|
               (depthInverted?NVSDK_NGX_DLSS_Feature_Flags_DepthInverted:0)|
               (motionJittered?NVSDK_NGX_DLSS_Feature_Flags_MVJittered:0);
        if(!CorrectList(list))return Status("initialize-command-list",E_INVALIDARG);
        // -1 retains the existing fixed MaxPerf/input-size creation path exactly.
        // Explicit modes require genuine queried settings; never silently fall back.
        if(requested_quality_>=0){const Result settings=NegotiateSettings();if(!settings.success)return settings;}
        ID3D11Device1* dev1=nullptr;
        const HRESULT query=dev11->QueryInterface(__uuidof(ID3D11Device1),reinterpret_cast<void**>(&dev1));
        if(FAILED(query))return Status("device11-interface",query);
        for(unsigned eye=0;eye<2;++eye){
            const Result made=MakeEye(eye,dev1);
            if(!made.success){Drop(dev1);return made;}
        }
        Drop(dev1);
        if(zero_motion_){const HRESULT made=zero_motion_upload_.Create(device12_,input_width_,input_height_);
            if(FAILED(made))return Status("zero-motion-upload",made);}
        for(unsigned eye=0;eye<2;++eye){
            DWORD exception=0;
            int result=AllocateLeaf(api_->allocate,&eyes_[eye].parameters,&exception);
            if(exception)quarantined_=true;
            if(result!=NVSDK_NGX_Result_Success||exception||!eyes_[eye].parameters)
                return Status("allocate-parameters",eyes_[eye].parameters?S_OK:E_POINTER,result,exception,eye);
            if(eye&&eyes_[eye].parameters==eyes_[0].parameters){
                quarantined_=true;return Status("parameter-alias",E_UNEXPECTED,result,0,eye);
            }
            // Even a failed create may have recorded commands or returned a handle.
            commands_recorded_=true;
            result=CreateLeaf(api_->create,list,eyes_[eye].parameters,&eyes_[eye].feature,
                              create_width_,create_height_,output_width_,output_height_,create_quality_,flags_,model_request_.preset,&exception);
            if(exception)quarantined_=true;
            if(result!=NVSDK_NGX_Result_Success||exception||!eyes_[eye].feature)
                return Status("create-feature1",eyes_[eye].feature?S_OK:E_POINTER,result,exception,eye);
            if(eye&&eyes_[eye].feature==eyes_[0].feature){
                quarantined_=true;return Status("feature-alias",E_UNEXPECTED,result,0,eye);
            }
        }
        ready_=true;
        return Status("created-awaiting-parent-fence");
    }
    Result Evaluate(ID3D12GraphicsCommandList* list,ID3D12Resource* workOutput,
                    ID3D12Resource* depth,ID3D12Resource* motion,bool reset,float jitterX=0,float jitterY=0,
                    D3D12_RESOURCE_STATES colorState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS) noexcept {
        output_valid_=false;
        if(!Ready())return Status("evaluate-not-ready",E_UNEXPECTED);
        if(colorState!=D3D12_RESOURCE_STATE_UNORDERED_ACCESS&&colorState!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            return Status("evaluate-color-state",E_INVALIDARG);
        if(!std::isfinite(jitterX)||!std::isfinite(jitterY)||std::fabs(jitterX)>.5f||std::fabs(jitterY)>.5f)
            return Status("evaluate-jitter",E_INVALIDARG);
        if(!CorrectList(list)||!CorrectSource(workOutput,DXGI_FORMAT_R16G16B16A16_FLOAT)||
           !CorrectSource(depth,DXGI_FORMAT_R32_FLOAT)||!CorrectSource(motion,DXGI_FORMAT_R16G16_FLOAT)||
           workOutput==depth||workOutput==motion||depth==motion){
            ready_=false;return Status("evaluate-sources",E_INVALIDARG);
        }
        ID3D12Resource* source[]={workOutput,depth,motion};
        const D3D12_RESOURCE_STATES states[]={colorState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        const unsigned borrowedSlots=zero_motion_?2u:3u;
        for(unsigned slot=0;slot<borrowedSlots;++slot)Transition(list,source[slot],states[slot],D3D12_RESOURCE_STATE_COPY_SOURCE);
        for(unsigned eye=0;eye<2;++eye){
            const D3D12_BOX box={eye*input_width_,0,0,(eye+1)*input_width_,input_height_,1};
            for(unsigned slot=0;slot<3;++slot){
                auto* input=eyes_[eye].input[slot];
                if(zero_motion_&&slot==2){
                    const HRESULT copied=zero_motion_upload_.CopyTo(list,input);
                    if(FAILED(copied)){
                        for(unsigned restore=0;restore<borrowedSlots;++restore)Transition(list,source[restore],D3D12_RESOURCE_STATE_COPY_SOURCE,states[restore]);
                        ready_=false;return Status("evaluate-zero-motion-copy",copied,NVSDK_NGX_Result_Success,0,eye);
                    }
                    continue;
                }
                Transition(list,input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION from={},to={};from.pResource=source[slot];to.pResource=input;
                from.Type=to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&to,0,0,0,&from,&box);
                Transition(list,input,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        }
        // All borrowed resources are restored before either NGX call can fail.
        for(unsigned slot=0;slot<borrowedSlots;++slot)Transition(list,source[slot],D3D12_RESOURCE_STATE_COPY_SOURCE,states[slot]);
        for(auto& eye:eyes_)Transition(list,eye.output12,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Result result=Status("evaluated-awaiting-parent-fence");
        const bool appliedReset=reset||!ever_evaluated_;
        unsigned evaluationCalls=0;
        for(unsigned eye=0;eye<2;++eye){
            DWORD exception=0;
            const int ngx=EvaluateLeaf(api_->evaluate,list,&eyes_[eye],input_width_,input_height_,
                                       appliedReset,jitterX,jitterY,&exception,&evaluationCalls);
            if(ngx!=NVSDK_NGX_Result_Success||exception){
                ready_=false;quarantined_=exception!=0;
                result=Status("evaluate-feature1",S_OK,ngx,exception,eye);break;
            }
        }
        for(auto& eye:eyes_)Transition(list,eye.output12,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        result.evaluation_calls=evaluationCalls;
        result.applied_reset=appliedReset;
        output_valid_=result.success;
        if(result.success)ever_evaluated_=true;
        return result;
    }
    // Only the parent may call this, after proving creation/evaluation and D3D11
    // output consumption have retired. Failed/exceptional release is quarantined;
    // retrying an opaque handle with uncertain ownership would risk double release.
    Result ReleaseAfterRetired() noexcept {
        ready_=false;output_valid_=false;
        if(quarantined_)return Status("release-quarantined",E_UNEXPECTED);
        for(unsigned eye=0;eye<2;++eye){
            auto& owned=eyes_[eye];DWORD exception=0;
            if(owned.feature){
                const int result=ReleaseLeaf(api_->release,owned.feature,&exception);
                if(result!=NVSDK_NGX_Result_Success||exception){
                    quarantined_=true;return Status("release-feature1",S_OK,result,exception,eye);
                }
                owned.feature=nullptr;
            }
            if(owned.parameters){
                const int result=DestroyLeaf(api_->destroy,owned.parameters,&exception);
                if(result!=NVSDK_NGX_Result_Success||exception){
                    quarantined_=true;return Status("destroy-parameters",S_OK,result,exception,eye);
                }
                owned.parameters=nullptr;
            }
        }
        for(auto& eye:eyes_){
            Drop(eye.output_srv);Drop(eye.output11);Drop(eye.output12);
            for(auto*& input:eye.input)Drop(input);
            if(eye.shared){CloseHandle(eye.shared);eye.shared=nullptr;}
            eye.shared_direction="none";
        }
        zero_motion_upload_.ReleaseAfterRetired();zero_motion_=false;
        Drop(device11_);Drop(device12_);api_=nullptr;
        input_width_=input_height_=output_width_=output_height_=0;
        create_width_=create_height_=0;requested_quality_=-1;create_quality_=NVSDK_NGX_PerfQuality_Value_MaxPerf;
        model_request_={};
        settings_receipt_={};settings_contract_={};settings_acquire_={};settings_destroy_={};
        commands_recorded_=false;ever_evaluated_=false;flags_=0;
        return Status("released-after-parent-retirement");
    }
};
}
