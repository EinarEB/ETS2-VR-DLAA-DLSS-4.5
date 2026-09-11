# Building the add-on

Players can use the compiled download described in [Installation](INSTALLATION.md). Building is only necessary when changing the source.

## Requirements

- Windows x64.
- Visual Studio 2022 Build Tools with **Desktop development with C++**, an x64 MSVC C++20 compiler and a Windows SDK.

ReShade and matching Dear ImGui headers, plus shader sources, are included. ReShade supplies the ImGui implementation; no separate UI library is linked. NVIDIA SDK libraries are not needed at build time; the add-on uses the NGX interface provided by the installed NVIDIA driver at runtime.

## Build

To use the same compile configuration as the version 1.0 binary, run from the repository directory in Command Prompt:

```bat
set CL=/DETS2_GEOMETRY_TAA_PROBE
build-dlaa.cmd
```

The script locates the x64 Visual Studio toolchain if it is not already active. It compiles with `/MT /W4 /WX` and embeds the shader assets in:

```text
build\dlaa\ets2-dlaa.addon64
```

The geometry-probe define retains optional diagnostic support in the release configuration. It does not enable geometry readbacks or file captures; those require an explicit output-directory environment variable. Omitting the define builds without that probe.

Install that file using the normal guide. ReShade and the matching NVIDIA DLL are still runtime prerequisites.

## Source layout

| Directory | Responsibility |
| --- | --- |
| `src/dlaa` | ReShade callbacks, native image composition, desktop-to-VR controls and embedded assets. |
| `src/standalone` | DLAA feature ownership and D3D11/D3D12 resource transfer. |
| `src/depth` | Current stereo scene/depth identification and matching. |
| `src/feeder` | Shared SR, device/context and stereo-history helpers. |
| `shaders` | Motion estimation and guide preparation, embedded by the resource compiler. |
| `external/reshade/include` | Pinned ReShade API headers. |
| `external/imgui` | Matching 1.92.5 headers for ReShade's Add-ons settings panel. |

`source-manifest.json` records the build inputs and their hashes. Consult [third-party notices](../THIRD-PARTY-NOTICES.md) before redistributing modified builds; the bundled motion estimator uses CC BY-NC 4.0.
