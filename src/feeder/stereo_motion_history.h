// Copyright (c) 2026 ETS2 VR preview contributors. SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
namespace ets2_motion {
// Observe only after the provider technique completed. Clear readiness at every
// presentation so a disabled or incorrectly ordered provider cannot reuse it.
struct HistoryGate {
    uintptr_t owner=0;
    uint64_t technique=0;
    uint32_t frame=0;
    bool previous_valid=false,seen=false,ready=false;
    bool Observe(uintptr_t runtime,uint64_t handle,uint32_t current,bool depth_valid,bool reset) {
        ready=runtime==owner && handle==technique && previous_valid &&
            uint32_t(frame+1)==current && depth_valid && !reset;
        owner=runtime;technique=handle;frame=current;
        previous_valid=depth_valid&&!reset;seen=true;
        return ready;
    }
    void Present(uintptr_t runtime) {
        if(runtime!=owner)return;
        if(!seen)previous_valid=false;
        seen=false;ready=false;
    }
    void Reset(uintptr_t runtime) { if(runtime==owner)*this={}; }
};
}
