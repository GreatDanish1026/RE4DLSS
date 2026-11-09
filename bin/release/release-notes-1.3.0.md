**Experimental DLSS Ray Reconstruction** for Resident Evil 4's ray-traced reflections (as in RE2DLSS, RE3DLSS, RE7DLSS and VillageDLSS 1.2.0).

- New **Ray Reconstruction** option (Settings tab, DLSS, off by default): with the game's ray tracing on, NVIDIA's DLSS Ray Reconstruction replaces DLSS Super Resolution. It upscales and cleans up the remaining noise in the ray-traced reflections, on top of the game's own ray tracing denoiser, using the game's surface data (G-buffer) and camera. Without ray tracing the add-on uses Super Resolution as before.
- **Ray Reconstruction model**: NVIDIA's default, D, E or F. An override in the NVIDIA App takes precedence (set it to "Use app default" there to choose here).
- Advanced tab, for testing: **Game's ray tracing denoiser** can skip the game's temporal filtering (or all of its denoiser) and hand Ray Reconstruction the reflection rays directly; keeping the game's denoiser is the default. **Show guide** displays the surface pictures Ray Reconstruction gets.

**Install**: replace the previous version with `RE4DLSS-1.3.0.zip` in Fluffy Mod Manager (or copy its files over the old ones). New file: `nvngx_dlssd.dll` (NVIDIA's Ray Reconstruction runtime 310.9.16, signed), next to `re4.exe` and in `streamline\`. FSR 2 must be on in the game's settings, as before.

Tested at 3840x2160, Quality and Ultra Performance, with ray tracing on, on an RTX 5080.

**Known issue**: users report visual problems with the biosensor scope while the add-on is on; a fix is planned for 1.3.1.
