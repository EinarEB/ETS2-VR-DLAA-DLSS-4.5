#pragma once
#include <cstdint>
namespace ets2_scene_r_publish {
struct CaptureStamp {uint64_t epoch=0,generation=0;bool submitted=false;};
inline bool FreshPair(bool accepted,bool synchronous,unsigned proofAge,const int (&eye)[2],
    uint64_t epoch,const CaptureStamp (&captures)[2])noexcept{
    if(!accepted||!synchronous||proofAge||eye[0]<0||eye[0]>1||eye[1]<0||eye[1]>1||eye[0]==eye[1])return false;
    for(const auto& c:captures)if(!c.submitted||!c.generation||c.epoch!=epoch)return false;
    return captures[0].generation!=captures[1].generation;
}
}
