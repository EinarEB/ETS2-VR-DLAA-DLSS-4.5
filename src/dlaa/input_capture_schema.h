// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

// CPU-only schema/packing used by the private verification build and its tests.
namespace ets2_input_capture {
inline constexpr unsigned FrameLimit=16,PlaneCount=10;
inline constexpr uint64_t FrameByteLimit=768ull*1024*1024;
inline constexpr uint64_t DiskByteLimit=12ull*1024*1024*1024;
inline constexpr const char* PlaneNames[PlaneCount]={"completed-color","current-color","raw-vort-motion",
    "validated-motion","assembled-depth","eye0-depth","eye1-depth","vort-diagnostic-current","vort-diagnostic-previous","dlaa-diagnostic-tests"};
struct Shape {uint32_t width=0,height=0,bpp=0,format=0;};
inline bool Valid(const Shape& s) noexcept {
    return s.width&&s.height&&s.width<=16384&&s.height<=16384&&(s.bpp==4||s.bpp==8)&&s.format;
}
inline uint64_t Bytes(const Shape& s) noexcept {return Valid(s)?uint64_t(s.width)*s.height*s.bpp:0;}
// NGX adds two eyes of RGBA16F/R32F/RG16F (16 bytes per SBS pixel).
inline uint64_t NativeBytes(const std::array<Shape,PlaneCount>& shapes) noexcept {
    return Valid(shapes[1])?uint64_t(shapes[1].width)*shapes[1].height*16:0;
}
inline bool Budget(const std::array<Shape,PlaneCount>& shapes,uint64_t& bytes) noexcept {
    bytes=0;for(const auto& shape:shapes){const uint64_t size=Bytes(shape);if(!size||size>FrameByteLimit-bytes)return false;bytes+=size;}
    return bytes+NativeBytes(shapes)<=DiskByteLimit/FrameLimit;
}
struct Crop {uint32_t x=0,y=0,width=0,height=0;};
// No row padding or texture headers enter the raw file. Row zero is top-left.
inline bool WritePacked(const std::filesystem::path& path,const void* data,uint64_t mappedBytes,uint64_t pitch,
                        const Shape& shape,Crop crop) {
    if(!Valid(shape)||!data||!crop.width||!crop.height||crop.x>=shape.width||crop.y>=shape.height||
       crop.width>shape.width-crop.x||crop.height>shape.height-crop.y||pitch<uint64_t(shape.width)*shape.bpp||
       pitch>(std::numeric_limits<uint64_t>::max)()/shape.height||
       mappedBytes<pitch*(shape.height-1)+uint64_t(shape.width)*shape.bpp)return false;
    if(std::filesystem::exists(path))return false;
    auto partial=path;partial+=".part";if(std::filesystem::exists(partial))return false;
    std::ofstream out(partial,std::ios::binary|std::ios::trunc);if(!out)return false;
    const auto* bytes=static_cast<const char*>(data);const uint64_t rowBytes=uint64_t(crop.width)*shape.bpp;
    for(uint32_t row=0;row<crop.height;++row){
        const auto offset=pitch*(crop.y+row)+uint64_t(crop.x)*shape.bpp;
        out.write(bytes+offset,static_cast<std::streamsize>(rowBytes));if(!out)return false;
    }
    out.close();if(!out)return false;
    std::error_code error;std::filesystem::rename(partial,path,error);return !error;
}
struct MotionUniforms {
    bool observed=false,mvValidate=false,validateStatic=false,staticHysteresis=false,validateDepth=false,validateMv=false;
    float mvSign[2]{},mvScale=0,staticBias=0,staticMinContrast=0,depthTolerance=0,mvConsistency=0;
    bool Valid()const noexcept {return observed&&std::isfinite(mvSign[0])&&std::isfinite(mvSign[1])&&std::isfinite(mvScale)&&
        std::isfinite(staticBias)&&std::isfinite(staticMinContrast)&&std::isfinite(depthTolerance)&&std::isfinite(mvConsistency);}
};
struct FrameIdentity {
    uint64_t callback=0,epoch=0,pairGeneration=0,runtimeGeneration=0;
    uint64_t runtime=0,context=0,target=0,vortObservation=0,vortExpectedCallback=0;
    uint32_t vortFrame=0,eyeWidth=0,height=0;
    bool requestedReset=false,appliedReset=false;
    uint64_t historyGeneration=0,evaluationOrdinal=0;
    uint64_t featureHandles[2]{},colorGeneration[2]{},depthGeneration[2]{},resolveSequence[2]{};
    uint32_t candidate[2]{};
    MotionUniforms motion{};
};
inline bool CurrentIdentity(const FrameIdentity& f) noexcept {
    return f.motion.Valid()&&f.callback&&f.epoch&&f.pairGeneration&&f.runtimeGeneration&&f.runtime&&f.context&&f.target&&
        f.vortObservation&&f.vortExpectedCallback==f.callback&&f.eyeWidth&&f.height&&
        f.colorGeneration[0]&&f.colorGeneration[1]&&f.depthGeneration[0]&&f.depthGeneration[1]&&
        f.resolveSequence[0]&&f.resolveSequence[1]&&f.candidate[0]<2&&f.candidate[1]<2&&f.candidate[0]!=f.candidate[1];
}
inline bool Consecutive(const FrameIdentity& previous,const FrameIdentity& current) noexcept {
    return CurrentIdentity(previous)&&CurrentIdentity(current)&&previous.callback!=UINT64_MAX&&previous.epoch!=UINT64_MAX&&
        previous.pairGeneration!=UINT64_MAX&&previous.vortObservation!=UINT64_MAX&&
        current.callback==previous.callback+1&&current.epoch==previous.epoch+1&&
        current.pairGeneration==previous.pairGeneration+1&&current.runtimeGeneration==previous.runtimeGeneration&&
        current.runtime==previous.runtime&&current.context==previous.context&&current.target==previous.target&&
        current.eyeWidth==previous.eyeWidth&&current.height==previous.height&&
        current.vortObservation==previous.vortObservation+1;
}
inline std::string FrameName(unsigned index){std::ostringstream out;out<<"frame"<<std::setw(3)<<std::setfill('0')<<index;return out.str();}
inline std::string Json(const FrameIdentity& f,unsigned index,const std::array<Shape,PlaneCount>& shapes,
                        const std::array<uint64_t,PlaneCount>& sources,uint64_t copyQuery,uint64_t copiedTick,uint64_t retiredTick) {
    std::ostringstream out;out<<std::setprecision(9)<<"{\"schema\":1,\"kind\":\"native-input-frame\",\"frame_index\":"<<index
      <<",\"callback\":"<<f.callback<<",\"epoch\":"<<f.epoch<<",\"pair_generation\":"<<f.pairGeneration
      <<",\"runtime_generation\":"<<f.runtimeGeneration<<",\"runtime\":"<<f.runtime<<",\"context\":"<<f.context
      <<",\"target\":"<<f.target<<",\"vort_frame\":"<<f.vortFrame<<",\"vort_observation\":"<<f.vortObservation
      <<",\"vort_expected_callback\":"<<f.vortExpectedCallback<<",\"requested_reset\":"<<(f.requestedReset?"true":"false")
      <<",\"applied_reset\":"<<(f.appliedReset?"true":"false")<<",\"history_generation\":"<<f.historyGeneration
      <<",\"evaluation_ordinal\":"<<f.evaluationOrdinal<<",\"evaluation_calls\":2,\"evaluation_retired\":true"
      <<",\"feature_handles\":["<<f.featureHandles[0]<<','<<f.featureHandles[1]<<"],\"copy_query\":"<<copyQuery
      <<",\"copy_query_completed\":true,\"copied_tick\":"<<copiedTick<<",\"retired_tick\":"<<retiredTick
      <<",\"eye_width\":"<<f.eyeWidth<<",\"height\":"<<f.height
      <<",\"byte_order\":\"little\",\"row_origin\":\"top_left\",\"raw_row_padding\":0,\"eyes\":[";
    for(unsigned eye=0;eye<2;++eye){if(eye)out<<',';out<<"{\"eye\":"<<eye<<",\"candidate\":"<<f.candidate[eye]
      <<",\"color_generation\":"<<f.colorGeneration[eye]<<",\"depth_generation\":"<<f.depthGeneration[eye]
      <<",\"resolve_sequence\":"<<f.resolveSequence[eye]<<'}';}
    const auto& m=f.motion;
    out<<"],\"motion_uniforms\":{\"observed\":"<<(m.observed?"true":"false")<<",\"MV_SIGN\":["<<m.mvSign[0]<<','<<m.mvSign[1]
       <<"],\"MV_SCALE\":"<<m.mvScale<<",\"MV_VALIDATE\":"<<(m.mvValidate?"true":"false")
       <<",\"VALIDATE_STATIC\":"<<(m.validateStatic?"true":"false")<<",\"STATIC_HYSTERESIS\":"<<(m.staticHysteresis?"true":"false")
       <<",\"STATIC_BIAS\":"<<m.staticBias<<",\"STATIC_MIN_CONTRAST\":"<<m.staticMinContrast
       <<",\"VALIDATE_DEPTH\":"<<(m.validateDepth?"true":"false")<<",\"DEPTH_TOLERANCE\":"<<m.depthTolerance
       <<",\"VALIDATE_MV\":"<<(m.validateMv?"true":"false")<<",\"MV_CONSISTENCY\":"<<m.mvConsistency<<"},\"planes\":[";
    for(unsigned i=0;i<PlaneCount;++i){const auto& p=shapes[i];if(i)out<<',';
      out<<"{\"name\":\""<<PlaneNames[i]<<"\",\"file\":\""<<PlaneNames[i]<<".raw\",\"width\":"<<p.width
        <<",\"height\":"<<p.height<<",\"bytes_per_pixel\":"<<p.bpp<<",\"dxgi_format\":"<<p.format
        <<",\"byte_count\":"<<Bytes(p)<<",\"source_resource\":"<<sources[i]
        <<",\"layout\":\""<<((i==5||i==6)?"eye_local":"sbs")<<"\",\"eye\":"<<((i==5||i==6)?int(i-5):-1)<<'}';}
    out<<"],\"ngx_directory\":\"ngx\",\"complete\":true}\n";return out.str();
}
}
