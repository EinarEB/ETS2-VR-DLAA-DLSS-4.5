#pragma once
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <wrl/client.h>

namespace ets2_d3d11 {
using Microsoft::WRL::ComPtr;
// One private block reused by sequential feeder stages. Scope retains the
// entire caller state, including all aliases, predicates and CB subranges.
class PrivateState {
    ComPtr<ID3D11Device> owner;
    ComPtr<ID3DDeviceContextState> state;
public:
    PrivateState()=default;
    PrivateState(const PrivateState&)=delete;
    PrivateState& operator=(const PrivateState&)=delete;
    void Release(){state.Reset();owner.Reset();}
    HRESULT Ensure(ID3D11Device* device){
        if(owner.Get()==device&&state)return S_OK;
        Release();if(!device)return E_POINTER;
        ComPtr<ID3D11Device1> device1;HRESULT hr=device->QueryInterface(IID_PPV_ARGS(&device1));if(FAILED(hr))return hr;
        const auto level=device->GetFeatureLevel();D3D_FEATURE_LEVEL selected{};
        const UINT flags=(device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)?D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED:0;
        hr=device1->CreateDeviceContextState(flags,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device1),&selected,&state);
        if(FAILED(hr)||selected!=level){Release();return FAILED(hr)?hr:E_FAIL;}
        owner=device;return S_OK;
    }
    ID3DDeviceContextState* Get()const{return state.Get();}
};
class Scope {
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3DDeviceContextState> previous;
    ComPtr<ID3D11Multithread> multithread;
    bool entered=false,swapped=false;
    HRESULT status=E_FAIL;
public:
    Scope(PrivateState& state,ID3D11DeviceContext* c){
        if(!c){status=E_POINTER;return;}
        if(c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){status=E_INVALIDARG;return;}
        status=c->QueryInterface(IID_PPV_ARGS(&multithread));if(FAILED(status))return;
        // Session attachment owns enabling/restoring protection. This scope
        // never changes that global flag, and cannot assume an unprotected
        // Enter() excludes another immediate-context user.
        if(!multithread->GetMultithreadProtected()){status=E_ACCESSDENIED;return;}
        multithread->Enter();entered=true;
        ComPtr<ID3D11Device> device;c->GetDevice(&device);status=state.Ensure(device.Get());if(FAILED(status))return;
        status=c->QueryInterface(IID_PPV_ARGS(&context));if(FAILED(status))return;
        context->SwapDeviceContextState(state.Get(),&previous);
        swapped=true;
        context->ClearState();context->SetPredication(nullptr,FALSE);
    }
    ~Scope(){
        if(swapped){context->ClearState();context->SwapDeviceContextState(previous.Get(),nullptr);}
        if(entered)multithread->Leave();
    }
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
    explicit operator bool()const{return swapped;}
    HRESULT Status()const{return status;}
};
}
