# Credits and licenses

ETS2 VR DLAA is an independent mod. NVIDIA, SCS Software and the projects credited below do not endorse it.

| Included component | Authors and license |
| --- | --- |
| DLAA integration, stereo capture and transport | ETS2 VR preview contributors — MIT. |
| Portions of guide preparation and transport | Jean-Laurent ROUZIES, [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder/tree/0c0afc4305feecd5b00a3c943f22e793cffb6280) — MIT. Adapted for native stereo DLAA. |
| Driver interface declarations and bridge portions | NIGos, [dlss5-bridge](https://github.com/NIGos/dlss5-bridge/tree/5e4bccfc88d60676641ca5af7c10197dd420d144) — MIT. |
| Motion estimation | Jakob Wapenhensch (Jak0bW), Pascal Gilcher / Marty McFly and Vortigern, [Vort shaders](https://github.com/vortigern11/vort_Shaders/tree/b410b9f0c0fbb83c8cb42164aaf1655fab386f4a) — **CC BY-NC 4.0** for the eye estimator; MIT for the wrapper and blue-noise asset. Modified for separate eye histories, matched depth and coordinate conversion. |
| ReShade API headers | Patrick Mours and contributors, [ReShade 6.8.0](https://github.com/crosire/reshade/tree/v6.8.0) — BSD-3-Clause or MIT, with the original header notices retained. |
| Dear ImGui headers for add-on settings | Omar Cornut and contributors — MIT. The matching 1.92.5 API is supplied by ReShade at runtime; its license is included in `licenses/LICENSE-ImGui-MIT.txt`. |
| ReShade shader helper header | Patrick Mours, [ReShade.fxh](https://github.com/crosire/reshade-shaders/blob/6db142b4b1a05c764222e5b0bd9a644b7ccfe1dc/Shaders/ReShade.fxh) — CC0-1.0. |

The bundled motion estimator is licensed for **noncommercial use**. The package as a whole is therefore not solely MIT-licensed. Keep the component notices and attribution when redistributing it; full terms are in `licenses/` and the source headers.

ReShade's runtime and NVIDIA's DLSS library are obtained separately and retain their own terms. Game files and NVIDIA model binaries are not included in this project's download.
