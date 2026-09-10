# Installation

**The version 1.0 download is coming soon.**

## 1. Prepare ETS2 for VR

In Steam, open **Euro Truck Simulator 2 → Properties → Betas** and select the **oculus** branch. Connect your headset and keep Steam open. If you use Virtual Desktop, select **VDXR**.

## 2. Get the required files

| Download | File to keep |
| --- | --- |
| [ReShade 6.8.0 with full add-on support](https://reshade.me/) | `ReShade_Setup_6.8.0_Addon.exe` — leave it intact |
| DLSS 310.8.0.0, from the [RenoDX download channel](https://discord.com/invite/renodx) | Extract `nvngx_dlss.dll` from `DLSS310.8.0-Streamline2.13.zip` |

You only need those two files. The installer checks that they are the correct versions.

## 3. Install

1. Download and extract the **ETS2 VR DLAA 1.0** Windows package from the Releases page.
2. Put both required files in its **Required files** folder.
3. Run **Setup ETS2 DLSS 4.5.exe** and select **Check requirements**.
4. Choose a new installation folder on the same NTFS drive as ETS2, then select **Install DLSS 4.5**.

Setup creates a separate game home. Your usual installation, settings and saves stay in place. Keep the new folder where you installed it.

## 4. Play

Open **ETS2 DLSS 4.5.exe** and select **Start VR**. On your first launch, create a new local profile with **Steam Cloud unchecked**, configure your controls, and enter the truck.

The launcher shows **DLSS 310.8.0.0 · preset M · native DLAA**.

- **Scroll Lock:** switch DLAA on or off for comparison while ETS2 has focus.
- **Home:** open the ReShade menu.

If setup rejects a file, use the version shown above; renaming another download will not make it compatible.
