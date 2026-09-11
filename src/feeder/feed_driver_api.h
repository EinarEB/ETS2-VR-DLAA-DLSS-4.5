// SPDX-License-Identifier: MIT
// Direct driver ABI for the ETS2 preview. No NVIDIA SDK implementation is linked.
// Parameter interface and driver entry-point signatures adapted from NIGos
// dlss5-bridge, commit 5e4bccfc88d60676641ca5af7c10197dd420d144 (MIT).
// Copyright (c) 2026 NIGos; additions Copyright (c) 2026 ETS2 VR preview contributors.
// Integer/key names are the interoperation protocol. Requires Windows x64/MSVC.
#pragma once
#include <mutex>
#include <string>

using NVSDK_NGX_Result = int;
using NVSDK_NGX_Feature = int;
using NVSDK_NGX_PerfQuality_Value = int;
constexpr int NVSDK_NGX_Version_API = 0x15;
constexpr int NVSDK_NGX_ENGINE_TYPE_CUSTOM = 0;
constexpr int NVSDK_NGX_Feature_SuperSampling = 1;
constexpr int NVSDK_NGX_Result_Success = 1;
constexpr int NVSDK_NGX_Result_Fail = static_cast<int>(0xBAD00000u);
constexpr int FeedDriverUnavailable = static_cast<int>(0xBAD00007u);
inline bool NVSDK_NGX_FAILED(int r) { return r != 1; }
inline bool NVSDK_NGX_SUCCEED(int r) { return r == 1; }
constexpr int NVSDK_NGX_PerfQuality_Value_MaxPerf=0, NVSDK_NGX_PerfQuality_Value_Balanced=1,
    NVSDK_NGX_PerfQuality_Value_MaxQuality=2, NVSDK_NGX_PerfQuality_Value_UltraPerformance=3,
    NVSDK_NGX_PerfQuality_Value_UltraQuality=4, NVSDK_NGX_PerfQuality_Value_DLAA=5;
constexpr int NVSDK_NGX_DLSS_Feature_Flags_IsHDR=1, NVSDK_NGX_DLSS_Feature_Flags_MVLowRes=2,
    NVSDK_NGX_DLSS_Feature_Flags_DepthInverted=8, NVSDK_NGX_DLSS_Feature_Flags_AutoExposure=64;
using PFN_NVSDK_NGX_ProgressCallback = void(__cdecl*)(float,bool&);
struct NVSDK_NGX_Handle { unsigned int Id; };
// Declaration order is ABI-critical with MSVC's overloaded virtual methods.
struct NVSDK_NGX_Parameter {
    virtual void Set(const char*,unsigned long long)=0;
    virtual void Set(const char*,float)=0;
    virtual void Set(const char*,double)=0;
    virtual void Set(const char*,unsigned int)=0;
    virtual void Set(const char*,int)=0;
    virtual void Set(const char*,ID3D11Resource*)=0;
    virtual void Set(const char*,ID3D12Resource*)=0;
    virtual void Set(const char*,void*)=0;
    virtual int Get(const char*,unsigned long long*)const=0;
    virtual int Get(const char*,float*)const=0;
    virtual int Get(const char*,double*)const=0;
    virtual int Get(const char*,unsigned int*)const=0;
    virtual int Get(const char*,int*)const=0;
    virtual int Get(const char*,ID3D11Resource**)const=0;
    virtual int Get(const char*,ID3D12Resource**)const=0;
    virtual int Get(const char*,void**)const=0;
    virtual void Reset()=0;
};
namespace feed_driver {
using Init=int(__cdecl*)(unsigned long long,const wchar_t*,ID3D12Device*,int,const void*);
using InitProject=int(__cdecl*)(const char*,int,const char*,const wchar_t*,ID3D12Device*,int,const void*);
using Allocate=int(__cdecl*)(NVSDK_NGX_Parameter**);
using Destroy=int(__cdecl*)(NVSDK_NGX_Parameter*);
using Create=int(__cdecl*)(ID3D12GraphicsCommandList*,int,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using Evaluate=int(__cdecl*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback);
using Release=int(__cdecl*)(NVSDK_NGX_Handle*);
struct Api {
    HMODULE module=nullptr; std::wstring path; DWORD error=0; bool ready=false;
    Init init=nullptr; InitProject project=nullptr; Allocate allocate=nullptr,capabilities=nullptr;
    Destroy destroy=nullptr; Create create=nullptr;
    Evaluate evaluate=nullptr; Release release=nullptr;
};
template<class T> inline bool Resolve(HMODULE m,const char* name,T& p) {
    p=reinterpret_cast<T>(GetProcAddress(m,name)); return p!=nullptr;
}
inline Api& Get() {
    static Api* a=new Api;
    static std::once_flag once;
    std::call_once(once,[] {
        wchar_t directory[32768]={}; DWORD bytes=sizeof(directory);
        LSTATUS s=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
            L"FullPath",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,directory,&bytes);
        if(s!=ERROR_SUCCESS){a->error=s;return;}
        // A driver registration must be an absolute local path; no search-path fallback.
        if(wcslen(directory)<3 || directory[1]!=L':' || directory[2]!=L'\\'){
            a->error=ERROR_BAD_PATHNAME;return;
        }
        a->path=directory;
        if(a->path.back()!=L'\\')a->path+=L'\\';
        a->path+=L"_nvngx.dll";
        a->module=LoadLibraryExW(a->path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!a->module){a->error=GetLastError();return;}
        bool ok=true;
        ok &= Resolve(a->module,"NVSDK_NGX_D3D12_Init_Ext",a->init);
        ok &= Resolve(a->module,"NVSDK_NGX_D3D12_Init_ProjectID",a->project);
        ok &= Resolve(a->module,"NVSDK_NGX_D3D12_AllocateParameters",a->allocate);
        ok &= Resolve(a->module,"NVSDK_NGX_D3D12_GetCapabilityParameters",a->capabilities);
        ok &= Resolve(a->module,"NVSDK_NGX_D3D12_DestroyParameters",a->destroy);
        ok &= Resolve(a->module,"NVSDK_NGX_D3D12_CreateFeature",a->create);
        ok &= Resolve(a->module,"NVSDK_NGX_D3D12_EvaluateFeature",a->evaluate);
        ok &= Resolve(a->module,"NVSDK_NGX_D3D12_ReleaseFeature",a->release);
        a->ready=ok; a->error=ok?0:ERROR_PROC_NOT_FOUND;
        // Retained for the process lifetime: observers can hold pointers into this DLL.
    });
    return *a;
}
}
