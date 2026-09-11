// SPDX-License-Identifier: MIT
#pragma once
#include "input_capture_schema.h"
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "../feeder/feed_scene_r_client.h"

namespace ets2_input_capture {
using Microsoft::WRL::ComPtr;
class Capture final {
    bool configured_=false,enabled_=false,failed_=false,pending_=false;
    unsigned completed_=0;
    std::filesystem::path root_,frame_;
    std::array<ComPtr<ID3D11Texture2D>,PlaneCount> staging_;
    ComPtr<ID3D11Query> query_;
    std::array<Shape,PlaneCount> shapes_{};
    std::array<uint64_t,PlaneCount> sources_{};
    FrameIdentity current_{},previous_{};
    uint64_t bytes_=0,copiedTick_=0,retiredTick_=0;
    uint64_t startEpoch_=600;
    const char* reason_="disabled";
    static bool Text(const std::filesystem::path& path,const std::string& text){
        auto partial=path;partial+=L".part";std::ofstream out(partial,std::ios::binary|std::ios::trunc);
        out<<text;out.close();return bool(out)&&MoveFileExW(partial.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    }
    void Status() noexcept try {
        if(root_.empty())return;
        std::ostringstream out;out<<"{\"schema\":1,\"kind\":\"native-input-capture\",\"target_frames\":"<<FrameLimit
          <<",\"completed_frames\":"<<completed_<<",\"complete\":"<<(completed_==FrameLimit?"true":"false")
          <<",\"start_epoch\":"<<startEpoch_<<",\"end_epoch\":"<<startEpoch_+FrameLimit-1
          <<",\"failed\":"<<(failed_?"true":"false")<<",\"reason\":"<<std::quoted(reason_)
          <<",\"bytes_per_frame\":"<<bytes_<<",\"frame_byte_limit\":"<<FrameByteLimit<<",\"disk_byte_limit\":"<<DiskByteLimit
          <<",\"native_bytes_per_frame\":"<<NativeBytes(shapes_)<<",\"combined_bytes_per_frame\":"<<bytes_+NativeBytes(shapes_)
          <<",\"last_completed_callback\":"<<previous_.callback<<",\"last_completed_epoch\":"<<previous_.epoch<<"}\n";
        (void)Text(root_/L"capture-status.json",out.str());
    }catch(...){}
    void Fail(const char* reason) noexcept {reason_=reason;failed_=true;pending_=false;Status();}
    static unsigned Bpp(DXGI_FORMAT f) noexcept {
        switch(f){case DXGI_FORMAT_R16G16B16A16_FLOAT:return 8;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_R32_FLOAT:return 4;default:return 0;}
    }
public:
    bool Enabled() noexcept try {
        if(!configured_){configured_=true;wchar_t directory[32768]{};
            const DWORD size=GetEnvironmentVariableW(L"ETS2_INPUT_CAPTURE_DIR",directory,32768);
            if(!size)return false;if(size>=32768){failed_=true;reason_="capture-directory-too-long";return false;}
            wchar_t epoch[32]{};const DWORD epochSize=GetEnvironmentVariableW(L"ETS2_INPUT_CAPTURE_EPOCH",epoch,32);
            if(epochSize){if(epochSize>=32){failed_=true;reason_="capture-epoch-invalid";return false;}uint64_t value=0;
                for(DWORD i=0;i<epochSize;++i){if(epoch[i]<L'0'||epoch[i]>L'9'||value>(UINT64_MAX-unsigned(epoch[i]-L'0'))/10){failed_=true;reason_="capture-epoch-invalid";return false;}value=value*10+unsigned(epoch[i]-L'0');}
                if(!value||value>UINT64_MAX-FrameLimit){failed_=true;reason_="capture-epoch-invalid";return false;}startEpoch_=value;}
            const std::filesystem::path base(directory);
            if(!base.is_absolute()){failed_=true;reason_="capture-directory-must-be-absolute";return false;}
            std::error_code error;std::filesystem::create_directories(base,error);if(error){failed_=true;reason_="capture-directory-create";return false;}
            const auto proposed=base/L"frame-inputs";
            if(!std::filesystem::create_directory(proposed,error)||error){failed_=true;reason_="capture-directory-already-exists-or-failed";return false;}
            root_=proposed;enabled_=true;reason_="waiting-for-current-native-frame";Status();
        }return enabled_&&!failed_&&completed_<FrameLimit;
    }catch(...){Fail("capture-configuration-exception");return false;}
    const char* Reason()const noexcept{return reason_;}
    void Abort(const char* reason) noexcept {if(enabled_&&!failed_&&completed_<FrameLimit)Fail(reason);}
    bool WantsEpoch(uint64_t epoch) noexcept {if(!Enabled()||epoch<startEpoch_)return false;
        if(epoch!=startEpoch_+completed_){Fail("missed-requested-producer-epoch");return false;}return true;}
    bool Pending()const noexcept{return pending_;}
    const std::filesystem::path& FrameDirectory()const noexcept{return frame_;}
    void Interrupted(const char* reason) noexcept {if(enabled_&&!failed_&&completed_&&completed_<FrameLimit)Fail(reason);}
    bool Begin(ID3D11DeviceContext* context,const std::array<ID3D11Texture2D*,PlaneCount>& inputs,const FrameIdentity& identity) noexcept try {
        if(!WantsEpoch(identity.epoch))return false;
        if(pending_||!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!CurrentIdentity(identity)||
           (completed_&&!Consecutive(previous_,identity))){Fail("nonconsecutive-or-invalid-frame-identity");return false;}
        ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<IUnknown> deviceIdentity;
        if(FAILED(ets2_scene_r_client::detail::DeviceIdentity(device.Get(),deviceIdentity))){Fail("context-device-identity");return false;}
        std::array<Shape,PlaneCount> shapes;std::array<D3D11_TEXTURE2D_DESC,PlaneCount> descriptions{};
        for(unsigned i=0;i<PlaneCount;++i){if(!inputs[i]){Fail("missing-plane");return false;}
            auto& d=descriptions[i];inputs[i]->GetDesc(&d);shapes[i]={d.Width,d.Height,Bpp(d.Format),uint32_t(d.Format)};
            const unsigned expectedWidth=(i==5||i==6)?identity.eyeWidth:(i==7||i==8)?(identity.eyeWidth*2/4)*2:identity.eyeWidth*2;
            const unsigned expectedHeight=(i==7||i==8)?identity.height/2:identity.height;
            if(d.Width!=expectedWidth||d.Height!=expectedHeight||d.MipLevels!=1||d.ArraySize!=1||d.SampleDesc.Count!=1||d.SampleDesc.Quality||
               !Valid(shapes[i])){Fail("plane-description");return false;}
            if(FAILED(ets2_scene_r_client::detail::SameDevice(inputs[i],deviceIdentity.Get()))){Fail("plane-device");return false;}
            if(((i==1||i==9)&&d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)||((i==2||i==3||i==7||i==8)&&d.Format!=DXGI_FORMAT_R16G16_FLOAT)||
               (i>=4&&i<=6&&d.Format!=DXGI_FORMAT_R32_FLOAT&&d.Format!=DXGI_FORMAT_R32_TYPELESS)){Fail("plane-format");return false;}
        }
        uint64_t bytes=0;if(!Budget(shapes,bytes)){Fail("capture-byte-budget");return false;}
        for(unsigned i=0;i<PlaneCount;++i){if(staging_[i]){
                const auto& a=shapes_[i];const auto& b=shapes[i];if(a.width!=b.width||a.height!=b.height||a.format!=b.format){Fail("plane-shape-changed");return false;}
            }else{auto d=descriptions[i];d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
                if(FAILED(device->CreateTexture2D(&d,nullptr,&staging_[i]))){Fail("staging-create");return false;}}
        }
        if(!query_){D3D11_QUERY_DESC description{D3D11_QUERY_EVENT,0};if(FAILED(device->CreateQuery(&description,&query_))){Fail("copy-query-create");return false;}}
        frame_=root_/FrameName(completed_);std::error_code error;if(!std::filesystem::create_directory(frame_,error)||error){Fail("frame-directory-create");return false;}
        current_=identity;shapes_=shapes;bytes_=bytes;copiedTick_=GetTickCount64();
        for(unsigned i=0;i<PlaneCount;++i){sources_[i]=reinterpret_cast<uint64_t>(inputs[i]);context->CopyResource(staging_[i].Get(),inputs[i]);}
        context->End(query_.Get());pending_=true;reason_="copies-enqueued-before-native-evaluate";return true;
    }catch(...){Fail("capture-begin-exception");return false;}
    // Call only after successful NativeSr::Evaluate has retired its input fence.
    bool ReadRetired(ID3D11DeviceContext* context,bool requestedReset,bool appliedReset,uint64_t historyGeneration,
                     uint64_t evaluationOrdinal,const uint64_t (&handles)[2]) noexcept try {
        if(!pending_)return false;
        if(!context||!handles[0]||!handles[1]||handles[0]==handles[1]||requestedReset!=current_.requestedReset){Fail("evaluation-retirement-receipt");return false;}
        BOOL done=FALSE;const HRESULT hr=context->GetData(query_.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(hr!=S_OK||!done){Fail("copy-query-not-complete-after-native-retirement");return false;}
        current_.appliedReset=appliedReset;current_.historyGeneration=historyGeneration;current_.evaluationOrdinal=evaluationOrdinal;
        current_.featureHandles[0]=handles[0];current_.featureHandles[1]=handles[1];retiredTick_=GetTickCount64();
        for(unsigned i=0;i<PlaneCount;++i){D3D11_MAPPED_SUBRESOURCE map{};
            if(FAILED(context->Map(staging_[i].Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map))){Fail("staging-map-after-retirement");return false;}
            bool wrote=false;try{wrote=WritePacked(frame_/(std::string(PlaneNames[i])+".raw"),map.pData,uint64_t(map.RowPitch)*shapes_[i].height,
                  map.RowPitch,shapes_[i],{0,0,shapes_[i].width,shapes_[i].height});}
            catch(...){context->Unmap(staging_[i].Get(),0);throw;}
            context->Unmap(staging_[i].Get(),0);if(!wrote){Fail("plane-file-write");return false;}
        }reason_="awaiting-exact-ngx-capture";return true;
    }catch(...){Fail("capture-readback-exception");return false;}
    void Commit(bool ngxCaptureSucceeded) noexcept try {
        if(!pending_)return;if(!ngxCaptureSucceeded){Fail("exact-ngx-capture-failed");return;}
        if(!Text(frame_/L"frame.json",Json(current_,completed_,shapes_,sources_,reinterpret_cast<uint64_t>(query_.Get()),copiedTick_,retiredTick_))){Fail("frame-metadata-write");return;}
        previous_=current_;++completed_;pending_=false;reason_=completed_==FrameLimit?"sixteen-consecutive-native-frames-captured":"capturing";Status();
        if(completed_==FrameLimit){for(auto& stage:staging_)stage.Reset();query_.Reset();}
    }catch(...){Fail("capture-commit-exception");}
    void EvaluationFailed()noexcept{if(pending_)Fail("native-evaluation-failed");}
};
}
