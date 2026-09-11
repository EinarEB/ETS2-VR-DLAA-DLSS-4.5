#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>
#include <vector>

namespace ets2_d3d11 {
// The Feeder is the sole owner of changing this process-shared context flag.
// Attach before depth proof; keep it through NGX/session rebuilds. The depth
// companion only checks the flag and never changes or independently restores it.
class ProtectionRegistry {
    struct Entry {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        bool wasOn=false;
    };
    std::vector<Entry> entries;
public:
    bool Acquire(ID3D11DeviceContext* context) noexcept try {
        if(!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
        Microsoft::WRL::ComPtr<ID3D11Device> device;context->GetDevice(&device);
        if(!device||(device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED))return false;
        for(const auto& e:entries)if(e.device.Get()==device.Get())return e.multithread->GetMultithreadProtected()!=FALSE;
        Entry e;e.device=device;
        if(FAILED(context->QueryInterface(IID_PPV_ARGS(&e.multithread))))return false;
        // Reserve before changing the flag, so allocation failure has no side effect.
        entries.reserve(entries.size()+1);
        e.wasOn=e.multithread->SetMultithreadProtected(TRUE)!=FALSE;
        if(!e.multithread->GetMultithreadProtected())return false;
        entries.push_back(std::move(e));return true;
    }catch(...){return false;}
    void ReleaseDevice(ID3D11Device* device){
        for(auto i=entries.begin();i!=entries.end();){
            if(!device||i->device.Get()==device){
                // Device destruction or quiescent addon detach only, never a
                // feature resize, preset reload, or runtime presentation.
                if(!i->wasOn)i->multithread->SetMultithreadProtected(FALSE);
                i=entries.erase(i);
            }else ++i;
        }
    }
};
}
