// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstdint>

// CPU-only handoff. No runtime, device, lock or callback access from the writer.
// Low bit is permission; every change advances the generation, including an
// off/on cycle which the producer has not yet observed. Capture starts only
// after the producer arms that generation at a VR frame boundary.
namespace ets2_dlaa_producer {
inline std::atomic<std::uint64_t> request{0};
inline void Request(bool enabled) noexcept {
    auto previous=request.load(std::memory_order_relaxed);
    while(bool(previous&1)!=enabled){
        const auto next=((previous+2)&~std::uint64_t(1))|std::uint64_t(enabled);
        if(request.compare_exchange_weak(previous,next,std::memory_order_release,std::memory_order_relaxed))return;
    }
}
inline std::uint64_t Read() noexcept {return request.load(std::memory_order_acquire);}
inline bool Enabled(std::uint64_t value) noexcept {return (value&1)!=0;}
}
