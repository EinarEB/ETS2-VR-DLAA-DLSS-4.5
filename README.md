# ETS2 VR DLAA — DLSS 4.5

Native-resolution **DLAA antialiasing for Euro Truck Simulator 2 in VR**.

| Configuration | Version 1.0 target |
| --- | --- |
| NVIDIA DLSS library | **310.8.0.0** |
| Model preset | **M**, explicitly requested |
| Mode | **DLAA** |
| Resolution | **100% input → 100% output, per eye** |

**Release status:** version 1.0 is in development. The native-resolution prototype has received positive headset feedback. The minimal standalone package is being prepared and tested; no downloadable release is available yet.

## How it works

The integration supplies DLSS with matched color, depth and estimated motion for each eye, maintains independent eye histories, and presents the antialiased image at the original resolution. Preset M is explicitly requested; the driver interface does not independently report the effective internal model.

[ReShade](https://reshade.me/) provides the graphics hooks and stereo shader processing. The release will require the ReShade build with full add-on support and the matching NVIDIA `nvngx_dlss.dll`. The installer is being prepared around those two downloads.

## Installation

See the [installation guide](docs/INSTALLATION.md) for the release layout, required files and intended setup steps. Follow the version 1.0 release instructions when the download becomes available.

## Validation

The current visual reference was tested with a Quest 3 through Virtual Desktop/VDXR, at **2504 × 2600 per eye**. That is an observed test configuration, not a fixed resolution requirement or a claim of compatibility with every headset, scene or GPU. The final package requires its own validation before release.

This project is an independent community integration and is not affiliated with SCS Software or NVIDIA.
