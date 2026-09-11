// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
namespace reshade::api { struct effect_runtime; }

namespace ets2_dlaa_assets {
struct Result {
    bool success = false;
    bool pathsChanged = false;
    bool reloadQueued = false;
    std::uint32_t filesCreated = 0;
    DWORD win32Error = ERROR_SUCCESS;
    const char *stage = "not-started";
    std::string directoryUtf8;
};
// Call once for each initialized runtime, on its callback thread, OUTSIDE DllMain.
// Extracts only this module's five resources. Existing files must match exactly;
// modified files are never overwritten. Adds its owned directory to the runtime
// search paths through the official API, retaining unrelated paths/settings.
// Success means files and search paths are ready, not that effects have compiled
// or been enabled. The caller owns technique activation and reload readiness.
Result EnsureAssets(HMODULE module, reshade::api::effect_runtime *runtime) noexcept;
}
