# ETS2 VR DLAA — DLSS 4.5

NVIDIA DLAA anti-aliasing for **Euro Truck Simulator 2 in VR**. It processes each eye at its original resolution to smooth jagged edges and reduce shimmering in the scene.

The mod is one file, `ets2-dlaa.addon64`, controlled from ReShade: an **on/off checkbox** and a **model preset** selector.

**Version 1.0 download: coming soon.**

| Setting | Version 1.0 |
| --- | --- |
| NVIDIA DLSS library | **310.9.1.0** (`nvngx_dlss.dll`) |
| Default model preset | **M — DLSS 4.5** |
| Other selectable presets | L (DLSS 4.5), K (DLSS 4) |
| Rendering mode | **DLAA: native input and output resolution** |
| Controls | ReShade effect checkbox and model preset selector |

> [!WARNING]
> **DLAA has a heavy GPU performance cost. Spacewarp or another headset frame-generation mode is recommended for VR.** DLAA uses the DLSS reconstruction model for anti-aliasing at native resolution; this package does not lower the game's render resolution or generate additional frames itself.

## Requirements

- **ETS2 VR, DirectX 11, OpenXR, SDR.** HDR is not supported. The current game build used for validation is **1.60.1.1007**. Rendering changes in later game updates may require an add-on update.
- **Windows 11** and an **NVIDIA GeForce RTX GPU** with a current driver. Hardware performance varies; other Windows versions are not verified.
- **[ReShade 6.8.0 with full add-on support](https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe)**, installed with **both DirectX 10/11/12 and OpenXR selected**.
- NVIDIA's **310.9.1.0** DLSS library, obtained separately from the official download linked in the guide.

For Quest headsets through Virtual Desktop, use **VDXR**. Other OpenXR runtimes have not been verified for this release.

## Install and use

> [!IMPORTANT]
> In the ReShade installer, select **DirectX 10/11/12** and tick the **OpenXR checkbox at the bottom of that same rendering API selection page**. **Both must be selected correctly or the mod will not work at all.**

1. Select ETS2's **oculus VR beta branch** in Steam. Install ReShade for its executable with **DirectX 10/11/12** and **OpenXR** selected.
2. Copy `ets2-dlaa.addon64` and the matching `nvngx_dlss.dll` into `Euro Truck Simulator 2\bin\win_x64`.
3. Launch the VR game with `-openxr`. Open ReShade with **Home** in the desktop game window and enable **ETS2 DLAA**. Start with preset **M**.

Use **100% game scaling** and turn the game's own anti-aliasing off. The [installation guide](docs/INSTALLATION.md) covers the exact setup, controls, troubleshooting, updates and removal.

## Compatibility

This add-on targets the VR driving scene. It does not apply DLAA to the desktop game mode or the separate VR menu overlay. It requires the game's original color and depth for both eyes; if those inputs are unavailable, the original image is kept.

Avoid combining it with another injected anti-aliasing or upscaling replacement. Ordinary ReShade effects can follow DLAA; the add-on places its motion preparation and DLAA stages first. Model changes reset the temporal history, so allow a few seconds before comparing them.

The selector requests an NVIDIA model preset. Driver overrides can affect the model used; the integration cannot report an independently confirmed effective preset. Leave DLSS overrides at their defaults when comparing presets.

## Support and credits

For a problem, [open an issue](https://github.com/EinarEB/ETS2-VR-DLAA-DLSS-4.5/issues) with your game version, GPU and driver, headset/OpenXR runtime, selected preset, and whether the problem happens in one or both eyes. See the guide for the relevant logs.

See [credits and licenses](THIRD-PARTY-NOTICES.md) for the included components. The bundled motion estimator has a **noncommercial license**; the entire package is not solely MIT-licensed.
