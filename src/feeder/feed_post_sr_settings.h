// Explicit experimental direct-core settings negotiation. SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <array>
#include <cmath>
#include "feed_driver_api.h"
#include "feed_post_sr_model.h"

namespace ets2_post_sr_settings {
// Exact public ABI: NVIDIA nvsdk_ngx_defs.h:550 and nvsdk_ngx_helpers.h:65-112.
// Retained source/query evidence: Research/V02-20260909/implementation/post-sr-settings-check.
using OptimalCallback=int(__cdecl*)(NVSDK_NGX_Parameter*);
inline constexpr const char* Keys[6]={"OutWidth","OutHeight",
 "DLSS.Get.Dynamic.Min.Render.Width","DLSS.Get.Dynamic.Min.Render.Height",
 "DLSS.Get.Dynamic.Max.Render.Width","DLSS.Get.Dynamic.Max.Render.Height"};
inline constexpr const char* ModeNames[6]={"MaxPerf","Balanced","MaxQuality","UltraPerformance","UltraQuality","DLAA"};
struct Call {int code=FeedDriverUnavailable;DWORD exception=0;bool attempted=false;};
inline bool Success(const Call& c) noexcept{return c.attempted&&c.code==1&&!c.exception;}
struct UIntGetter {Call call{};unsigned value=0;};
struct FloatGetter {Call call{};float value=0;};
struct Receipt {
 unsigned outputWidth=0,outputHeight=0;int quality=-1;
 ets2_post_sr_model::Preset requestedPreset=ets2_post_sr_model::Preset::Legacy;
 NVSDK_NGX_Parameter* parameters=nullptr;void* callbackPointer=nullptr;
 Call availabilityGet{},callbackGet{},setRequest{},callback{};int available=0;
 std::array<UIntGetter,6> fields{};FloatGetter sharpness{};
 bool valid=false;const char* stage="not-queried";
};
inline bool ValidMode(int mode) noexcept{return mode>=0&&mode<6;}
inline bool ValidBounds(const Receipt& r) noexcept {
 if(!ValidMode(r.quality)||!r.outputWidth||!r.outputHeight||!Success(r.callback))return false;
 for(const auto& v:r.fields)if(!Success(v.call)||!v.value)return false;
 const auto& v=r.fields;
 return v[2].value<=v[0].value&&v[0].value<=v[4].value&&v[4].value<=r.outputWidth&&
        v[3].value<=v[1].value&&v[1].value<=v[5].value&&v[5].value<=r.outputHeight;
}
struct Contract {
 bool valid=false;int quality=-1;
 ets2_post_sr_model::Preset requestedPreset=ets2_post_sr_model::Preset::Legacy;
 ets2_post_sr_model::CreationPolicy creationPolicy=ets2_post_sr_model::CreationPolicy::Legacy;
 unsigned createWidth=0,createHeight=0,outputWidth=0,outputHeight=0;
 unsigned inputAllocationWidth=0,inputAllocationHeight=0,evaluateWidth=0,evaluateHeight=0;
};
inline Contract MakeContract(const Receipt& r,unsigned actualWidth,unsigned actualHeight,
                             ets2_post_sr_model::CreationPolicy policy=ets2_post_sr_model::CreationPolicy::Legacy) noexcept {
 Contract c{};c.requestedPreset=r.requestedPreset;c.creationPolicy=policy;
 if(!ets2_post_sr_model::ValidRequest({r.requestedPreset,policy},r.quality)||
    !r.valid||!ValidBounds(r)||!actualWidth||!actualHeight||
    actualWidth<r.fields[2].value||actualHeight<r.fields[3].value||
    actualWidth>r.fields[4].value||actualHeight>r.fields[5].value)return c;
 c.valid=true;c.quality=r.quality;c.createWidth=r.fields[0].value;c.createHeight=r.fields[1].value;
 if(ets2_post_sr_model::UsesFixedInputMaximum(policy)){c.createWidth=actualWidth;c.createHeight=actualHeight;}
 c.outputWidth=r.outputWidth;c.outputHeight=r.outputHeight;
 c.inputAllocationWidth=c.evaluateWidth=actualWidth;c.inputAllocationHeight=c.evaluateHeight=actualHeight;
 return c;
}
namespace detail {
inline Call Capabilities(feed_driver::Allocate f,NVSDK_NGX_Parameter** out) noexcept {
 Call r;if(!f||!out)return r;r.attempted=true;
 __try{r.code=f(out);}__except(EXCEPTION_EXECUTE_HANDLER){r.exception=GetExceptionCode();}return r;
}
inline Call Destroy(feed_driver::Destroy f,NVSDK_NGX_Parameter* p) noexcept {
 Call r;if(!f||!p)return r;r.attempted=true;
 __try{r.code=f(p);}__except(EXCEPTION_EXECUTE_HANDLER){r.exception=GetExceptionCode();}return r;
}
inline Call GetPointer(NVSDK_NGX_Parameter* p,const char* key,void** value) noexcept {
 Call r;if(!p)return r;r.attempted=true;
 __try{r.code=p->Get(key,value);}__except(EXCEPTION_EXECUTE_HANDLER){r.exception=GetExceptionCode();}return r;
}
inline Call GetInt(NVSDK_NGX_Parameter* p,const char* key,int* value) noexcept {
 Call r;if(!p)return r;r.attempted=true;
 __try{r.code=p->Get(key,value);}__except(EXCEPTION_EXECUTE_HANDLER){r.exception=GetExceptionCode();}return r;
}
inline Call GetUInt(NVSDK_NGX_Parameter* p,const char* key,unsigned* value) noexcept {
 Call r;if(!p)return r;r.attempted=true;
 __try{r.code=p->Get(key,value);}__except(EXCEPTION_EXECUTE_HANDLER){r.exception=GetExceptionCode();}return r;
}
inline Call GetFloat(NVSDK_NGX_Parameter* p,const char* key,float* value) noexcept {
 Call r;if(!p)return r;r.attempted=true;
 __try{r.code=p->Get(key,value);}__except(EXCEPTION_EXECUTE_HANDLER){r.exception=GetExceptionCode();}return r;
}
inline Call SetRequest(NVSDK_NGX_Parameter* p,unsigned w,unsigned h,int quality,
                       ets2_post_sr_model::Preset preset=ets2_post_sr_model::Preset::Legacy) noexcept {
 Call r;if(!p)return r;r.attempted=true;
 __try{p->Set("Width",w);p->Set("Height",h);p->Set("PerfQualityValue",quality);p->Set("RTXValue",0);
  if(ets2_post_sr_model::ApplyHint(p,preset,quality))r.code=1;}
 __except(EXCEPTION_EXECUTE_HANDLER){r.exception=GetExceptionCode();}return r;
}
inline Call Invoke(OptimalCallback f,NVSDK_NGX_Parameter* p) noexcept {
 Call r;if(!f||!p)return r;r.attempted=true;
 __try{r.code=f(p);}__except(EXCEPTION_EXECUTE_HANDLER){r.exception=GetExceptionCode();}return r;
}
}
// Dedicated capability-map owner. No implicit destructor calls into NGX. Query
// one mode per acquired map, then explicitly Destroy before the next Acquire.
// An exception or failed Destroy retains the map and poisons this owner. The
// caller must retain the enclosing initialized device/core until process exit.
class QueryOwner final {
 NVSDK_NGX_Parameter* map_=nullptr;bool poisoned_=false,queried_=false;
 bool Observe(const Call& c) noexcept {if(c.exception)poisoned_=true;return !c.exception;}
public:
 QueryOwner()=default;QueryOwner(const QueryOwner&)=delete;QueryOwner& operator=(const QueryOwner&)=delete;
 bool HasParameters()const noexcept{return map_!=nullptr;}
 bool Quarantined()const noexcept{return poisoned_;}
 NVSDK_NGX_Parameter* Parameters()const noexcept{return map_;}
 Call Acquire(feed_driver::Api& api) noexcept {
  if(map_||poisoned_)return {};queried_=false;
  const Call r=detail::Capabilities(api.capabilities,&map_);Observe(r);return r;
 }
  Receipt Read(unsigned width,unsigned height,int quality,
               ets2_post_sr_model::Preset preset=ets2_post_sr_model::Preset::Legacy) noexcept {
  Receipt r;r.outputWidth=width;r.outputHeight=height;r.quality=quality;r.parameters=map_;
  r.requestedPreset=preset;
  if(!map_||poisoned_||queried_||!width||!height||!ValidMode(quality)||
     !ets2_post_sr_model::ValidPresetForMode(preset,quality)){r.stage="invalid-query-state";return r;}
  queried_=true;r.availabilityGet=detail::GetInt(map_,"SuperSampling.Available",&r.available);
  if(!Observe(r.availabilityGet)){r.stage="availability-exception";return r;}
  r.callbackGet=detail::GetPointer(map_,"DLSSOptimalSettingsCallback",&r.callbackPointer);
  if(!Observe(r.callbackGet)||!Success(r.callbackGet)||!r.callbackPointer){r.stage="callback-unavailable";return r;}
  r.setRequest=detail::SetRequest(map_,width,height,quality,preset);
  if(!Observe(r.setRequest)||!Success(r.setRequest)){r.stage="set-request";return r;}
  r.callback=detail::Invoke(reinterpret_cast<OptimalCallback>(r.callbackPointer),map_);
  if(!Observe(r.callback)||!Success(r.callback)){r.stage="callback-failed";return r;}
  for(unsigned i=0;i<6;++i){r.fields[i].call=detail::GetUInt(map_,Keys[i],&r.fields[i].value);
   if(!Observe(r.fields[i].call)){r.stage="bounds-get-exception";return r;}}
  r.sharpness.call=detail::GetFloat(map_,"Sharpness",&r.sharpness.value);
  if(!Observe(r.sharpness.call)){r.stage="sharpness-get-exception";return r;}
  r.valid=Success(r.availabilityGet)&&r.available!=0&&ValidBounds(r);
  r.stage=r.valid?"valid-settings":"invalid-settings";return r;
 }
 Call Destroy(feed_driver::Api& api) noexcept {
  if(!map_||poisoned_)return {};
  const Call r=detail::Destroy(api.destroy,map_);
  if(Success(r)){map_=nullptr;queried_=false;}else poisoned_=true;
  return r;
 }
};
}
