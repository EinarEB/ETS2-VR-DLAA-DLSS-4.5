// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace ets2_scene_r_depth {
struct Size { unsigned width=0,height=0; };
inline bool operator==(Size a,Size b){return a.width==b.width&&a.height==b.height;}
inline bool operator!=(Size a,Size b){return !(a==b);}
inline bool Valid(Size s){return s.width>=128&&s.height>=128&&s.width<=16384&&s.height<=16384;}
class Extents {
    Size final_{},scene_{};
    bool sceneSelected_=false;
public:
    void Final(Size value){final_=value;scene_=value;sceneSelected_=false;}
    Size Final()const{return final_;}
    Size Scene()const{return scene_;}
    // Equal dimensions alone never enable early scene publication. A valid
    // explicit Scene request is required, including at native resolution.
    bool SceneSelected()const{return sceneSelected_;}
    unsigned SnapshotLimit()const{return sceneSelected_?16u:4u;}
    bool Scene(Size value){
        if(!Valid(final_)||!Valid(value)||value.width>final_.width||value.height>final_.height||
           uint64_t(value.width)*2<final_.width||uint64_t(value.height)*2<final_.height)return false;
        scene_=value;sceneSelected_=true;return true;
    }
    bool Accept(Size value)const{return Valid(value)&&(value==final_||value==scene_);}
    bool Replay(Size source,Size destination,bool pinnedCopy)const{
        if(!Accept(source)||!Accept(destination))return false;
        return source==destination||(pinnedCopy&&source==scene_&&destination==final_);
    }
    bool Resolves(Size source,Size destination,bool pinnedCopy)const{
        // The pinned final-copy shader and its validated tone/depth link define
        // this boundary; a size change is not necessary in explicit native mode.
        return sceneSelected_&&pinnedCopy&&source==scene_&&destination==final_&&
            Replay(source,destination,pinnedCopy);
    }
};
// Metadata for an immutable pre-resolve depth copy. A shared source pointer is
// not an eye ID: the two draws must have distinct generations/destinations.
struct SourceStamp {
    uint64_t sourceColor=0,targetColor=0,sequence=0;
    Size extent{};
    bool Valid()const{return sourceColor&&targetColor&&sourceColor!=targetColor&&sequence&&ets2_scene_r_depth::Valid(extent);}
};
inline bool Pair(const SourceStamp& a,const SourceStamp& b){
    return a.Valid()&&b.Valid()&&a.targetColor!=b.targetColor&&a.sequence!=b.sequence&&a.extent==b.extent;
}
// Exact existing write policy: the known final UI overlays geometry; any other
// nonzero write invalidates the destination association before replay succeeds.
inline bool PreserveOnWrite(unsigned stage,bool knownUi,unsigned writeMask){
    return writeMask==0||(stage==2&&knownUi);
}
struct Request {bool valid=true,enabled=false;Size size{};bool native=false;};
inline bool Dimension(const char* text,unsigned& value){
    if(!text||!*text)return false;
    unsigned next=0;
    for(const char* p=text;*p;++p){
        if(*p<'0'||*p>'9')return false;
        next=next*10+unsigned(*p-'0');if(next>16384)return false;
    }
    if(next<128)return false;value=next;return true;
}
inline Request ParseRequest(const char* width,const char* height,const char* native=nullptr){
    Request result;
    if(native){
        result.enabled=true;result.native=true;
        result.valid=native[0]=='1'&&native[1]==0&&!width&&!height;
        return result;
    }
    if(!width&&!height)return result;
    result.enabled=true;
    result.valid=Dimension(width,result.size.width)&&Dimension(height,result.size.height);
    return result;
}
// Native mode derives its grid from the current, validated VR runtime only.
// This selects an expected source grid; actual shader/resource lineage and
// synchronous eye matching still have to prove the published pair.
inline Request ResolveRequest(Request request,Size runtimeFinal){
    if(!request.valid||!request.enabled)return request;
    if(request.native)request.size=runtimeFinal;
    Extents extents;extents.Final(runtimeFinal);
    request.valid=extents.Scene(request.size);
    return request;
}
}
