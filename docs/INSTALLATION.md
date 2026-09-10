# Installing ETS2 VR DLAA — DLSS 4.5

**Version 1.0 is being prepared. These instructions describe its intended installer flow; wait for a published release before installing.**

The configuration is **DLSS 310.8.0.0, requested preset M, native-resolution DLAA**. Both eyes render and present at 100% resolution.

## Required files

The installer uses your existing ETS2 installation and two separately supplied downloads:

1. **ReShade 6.8.0 with full add-on support**, from [ReShade's official site](https://reshade.me/). Keep the original add-on installer intact.
2. **`nvngx_dlss.dll`, version 310.8.0.0**. The exact validated file is 58,956,400 bytes with SHA-256:

   ```text
   c85f971ce023c9f3492fc7455f0b01a24ba18ea39636407a846902c4360b0b7e
   ```

The matching model archive used during development is `DLSS310.8.0-Streamline2.13.zip`, available through the [RenoDX community download channel](https://discord.com/invite/renodx). Extract only `nvngx_dlss.dll` for this integration. The model is not redistributed in this repository. [NVIDIA's DLSS repository](https://github.com/NVIDIA/DLSS) provides upstream SDK information; another download is not automatically the same validated binary.

## Setup steps when 1.0 is released

1. Download the Windows package from this repository's Releases page and extract it.
2. Place the ReShade add-on installer and `nvngx_dlss.dll` in **Required files**.
3. Run **Setup ETS2 DLSS 4.5.exe**. Select your ETS2 executable, check requirements, and choose a new destination folder on the same NTFS drive as the game.
4. Connect your headset. With Virtual Desktop, select **VDXR**; keep Steam open.
5. Run **ETS2 DLSS 4.5.exe** from the prepared folder and select **Start VR**.
6. Create a new local profile with Steam Cloud unchecked, configure your controls, and enter the truck.

The installer creates a separate game home and shares the large game archives. Your usual installation and campaigns remain in place. Keep the prepared folder at its installed location. The launcher displays the DLSS library version, requested preset and DLAA mode centrally.

The development reference uses Windows 11 x64, DirectX 11/OpenXR, ETS2 VR executable **1.60.1.1007** on Steam's `oculus` branch, and NVIDIA RTX hardware. The release will list the hardware and game versions actually validated by the final package.

The guide will be updated with the final checks and controls when the tested package is published.
