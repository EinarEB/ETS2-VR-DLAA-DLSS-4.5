# Installation

**The version 1.0 download is coming soon.** These instructions describe the drop-in add-on. Its controls are in ReShade's menu.

Required NVIDIA library: **`nvngx_dlss.dll` 310.9.1.0**. Default preset: **M**. Mode: **native-resolution DLAA**.

## 1. Prepare ETS2 for VR

In Steam, open **Euro Truck Simulator 2 → Properties → Betas** and select the VR branch labelled **oculus**. Branch descriptions include the game version and may change. Add `-openxr` to the game's launch options and use its **DirectX 11** renderer. For a Quest headset with Virtual Desktop, select **VDXR** as the OpenXR runtime in Virtual Desktop Streamer.

Make sure the game works in VR before adding the mod. Use **SDR**; HDR is not supported. The validated game executable is **1.60.1.1007**; other versions may require an updated add-on.

## 2. Install ReShade for OpenXR

1. Run [ReShade 6.8.0 with full add-on support](https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe).
2. Select `Euro Truck Simulator 2\bin\win_x64\eurotrucks2.exe`.
3. Select **DirectX 10/11/12** and tick **OpenXR**, then finish setup. Keep the `ReShade.ini` it creates.

A desktop-only ReShade installation does not enable the VR effect. The **full add-on support** download is required; the standard download does not load this add-on. No optional shader pack is needed for DLAA itself.

## 3. Add DLAA

Close the game. Once version 1.0 is available, extract these two files beside `eurotrucks2.exe` in `bin\win_x64`:

- `ets2-dlaa.addon64` — this project's add-on.
- `nvngx_dlss.dll` — download **310.9.1.0** from [NVIDIA's official repository](https://raw.githubusercontent.com/NVIDIA/DLSS/374959484e79a640feaba44c93ac8cfb0a03f5b5/lib/Windows_x86_64/rel/nvngx_dlss.dll). Check **Properties → Details → File version** after saving it. This link is pinned to the required release, rather than whichever version is newest.

Do not use `nvngx_dlssd.dll` or a frame-generation DLL in its place. Keep the filename `ets2-dlaa.addon64` unchanged. If either destination file already exists, keep a backup before replacing it.

The add-on contains its required shaders and installs them automatically into `%LOCALAPPDATA%\ETS2-DLAA\assets`. You do not need to copy individual shaders or download a motion-vector effect. No Snowymoon files are needed.

## 4. Set the game's anti-aliasing and scaling

Set the game's **anti-aliasing to off** and **scaling to 100%**. DLAA should receive the original image rather than another anti-aliasing filter's output.

For a manually edited ETS2 `config.cfg`, the corresponding baseline is:

```text
uset r_aa "0"
uset r_scale_x "1"
uset r_scale_y "1"
uset r_manual_stereo_buffer_scale "1.0"
```

Close the game before editing its configuration. Leave unrelated settings alone. Your headset/OpenXR render-resolution setting still determines the VR image size; the add-on processes that image at matching input and output resolution. It does not choose an upscaling ratio.

## 5. Enable DLAA

Start ETS2 through Steam and enter the driving scene. Press **Home** while the **desktop game window** has focus to open ReShade. Complete ReShade's first-run tutorial if shown.

1. Enable the checkbox beside **ETS2 DLAA** in the effects list.
2. Select the effect to display **Model preset** and start with **M**.
3. Close the ReShade overlay and allow a few seconds for the temporal image to settle.

With VDXR, use the desktop game window for these controls; they apply to the headset scene. Unchecking the effect returns to the original image. ReShade remembers the checkbox and preset in its active preset file; an existing saved-off setting stays off.

**M**, **L** and **K** are model requests, not resolution or performance modes. M and L use DLSS 4.5 models; K uses the earlier DLSS 4 model. If M/L are too expensive on an RTX 20- or 30-series card, try K: [NVIDIA notes their higher cost on those GPUs](https://www.nvidia.com/en-us/geforce/news/dlss-4-5-super-resolution-available-now/).

A change resets the DLAA history. The NVIDIA driver may override a request, and this add-on cannot independently report the effective preset. Leave driver DLSS overrides at their defaults for predictable comparisons. Turn off ReShade's **Performance Mode** while editing the preset selector if the controls are hidden.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| No ReShade overlay | Focus the desktop game window and press Home. Confirm ReShade was installed for the correct `eurotrucks2.exe` with OpenXR selected. |
| ReShade opens, but ETS2 DLAA is missing | Use ReShade **6.8.0 with full add-on support**. Put the add-on beside the 64-bit game executable, keep its filename unchanged, and restart the game. Check ReShade's **Add-ons** tab for a load error. If you customized ReShade's **Add-on search path**, put the add-on in that folder instead; NVIDIA's DLL still goes beside the game EXE. |
| The checkbox is on but the VR scene is unchanged | Enter the driving scene and wait a few seconds. Confirm the game is using OpenXR/DX11 and a supported game build. Check `ets2-dlaa.log` for the specific error. After fixing a processing error, **restart ETS2**; toggling the checkbox alone does not clear it. |
| Missing or wrong DLSS library | Put `nvngx_dlss.dll` **310.9.1.0** beside `eurotrucks2.exe`, then restart. A DLL left inside a ZIP, in the parent game directory or with another version will not work. |
| Blur, trails or unexpected edges | Turn off the game's own AA and any other injected AA/upscaler; use native scaling and default NVIDIA overrides. Compare M and K after allowing the image to settle. If it persists, report whether it occurs while stationary or moving and in one or both eyes. |
| Lower frame rate | DLAA adds GPU work at the full VR resolution. Compare presets and reduce the headset's render resolution if needed. This package does not offer a reduced-resolution DLSS upscaling mode. |
| Effect temporarily disappears after a menu, loading screen or restart | The add-on waits for fresh matching inputs from both eyes and rebuilds its history. If it does not return during driving, collect the logs. |

For an issue, include **`ets2-dlaa.log`** and the relevant **ReShade log** from `bin\win_x64`. `depth-match.log` is useful if the add-on cannot find the current scene inputs. Provide the game version, GPU/driver, headset and OpenXR runtime, selected preset, and any other graphics injectors. Check logs for personal file paths before posting them publicly.

## Updating

Close ETS2 and replace `ets2-dlaa.addon64` with the newer release. Keep your ReShade preset. Change the NVIDIA DLL only when the release specifies a different required version. The add-on manages its versioned shader cache automatically.

## Removing

1. Close ETS2 and remove `ets2-dlaa.addon64` from `bin\win_x64`.
2. Delete `%LOCALAPPDATA%\ETS2-DLAA` to remove the add-on's cached shaders. This prevents ReShade from continuing to load those effects after the add-on is gone.
3. Remove only the `ETS2-DLAA\assets\...` entries from ReShade's effect/texture search paths. Leave other paths and shader folders intact.
4. Remove `nvngx_dlss.dll` only if no other mod uses it.

ReShade can remain installed for other effects. Use its setup program if you also want to uninstall ReShade.
