# Resident Evil 4 (remake) frame analysis

Game: Steam, re4.exe, D3D12 (ships D3D12Core.dll), 3840x2160, SDR (rgb10a2 back buffer, colour
space 1) in the first captures. ReShade 6.8.0. Settings in local_config.ini.

## FSR 2 (captures/RE4DLSS-frame-1.txt: FSR 2 Quality, 2560x1440 -> 3840x2160)

Passes only present with FSR 2 on, in order (after the scene):

| Pass | Shader | Reads | Writes |
|---|---|---|---|
| reactive mask | cs1e379886 | t0 opaque colour, t1 colour #26d94 r11g11b10f | r8 |
| velocity convert (game) | cs5469503e | t0 velocity rgba16snorm | rg16f #271c3 |
| depth convert (game) | csd2efdab0 | t0 depth r32g8x24 | r32f #272f5 |
| luminance pyramid | csaa8d773e | colour | r16f mips, 1x1 rg32f exposure #262d2 |
| prepare input | cs26f116ba | colour, exposure | r32uint, rgba16unorm, rgba8 |
| reconstruct and dilate | cs7bfbae60 | motion rg16f, depth r32f, reactive, ... | dilated motion/depth |
| depth clip | cs99b4b3b1 | | r8 |
| lock | cs370d5579 | lock status 3840x2160 | lock status |
| **accumulate** | **cs226d3330** | 1x1 exposure, dilated motion, history, ... | u0 3840x2160 rgba16f #27bec |
| **RCAS** | **cs1cfde08a** | t0 exposure, t1 accumulate output | **u0 3840x2160 rgba16f #85dc4** (read by the post chain) |

ps3724273e (5 draws before) writes the colour #26d94 and an rg8 target (scene/transparency).
With TAA instead (frame-2): cscd215ebc, ps97d06463, psd9321f67.

Plan: skip accumulate and RCAS, run DLSS into RCAS's u0 (already a UAV). Inputs: colour #26d94,
depth (r32f conversion or the original), motion (rg16f conversion) with the scale and jitter from
FSR 2's constants (next capture: DumpShaders for the FSR 2 passes; bytecode for names).

## FSR 2 constants and inputs (captures/r2, shaders/)

The game's FSR is AMD FSR 2.1 (cbFSR2, 144 bytes, root CBV b0): iRenderSize 0, iDisplaySize 8,
uFrameIndex 28 (FSR resets it to 0 on a history reset), fJitter 40 (render pixels, e.g.
-0.469 0.093), fProjSpaceJitter 48, fDeviceToViewDepth 64 (reverse-Z, near 0.01), fPreExposure
112 (1), fTanHalfFOV 116, fJitterSequenceLength 120 (18).

- CS_UpscalingDepthTranslation (csd2efdab0): plain copy of the depth into r32f (u0).
- CS_UpscalingVelocityTranslate (cs5469503e): u0 = (velocity.xy - fProjSpaceJitter) * (0.5, -0.5):
  UV offset from the current to the previous position, y down, jitter removed. DLSS motion scale =
  render size.
- RCAS (cs1cfde08a, cbRCAS) reads accumulate's output (t1) and the exposure; it divides and
  multiplies back, so it only sharpens.

0.1.0: DLSS replaces accumulate (cs226d3330), writing its u0; RCAS runs as before. One DLSS
history (FSR alternates its accumulate output texture). Render size = the game's FSR 2 quality.

Confirmed by the user (2026-11-02): looks good. Log: DLSS ran at every FSR 2 quality, 2560x1440,
2258x1270, 1920x1080 and 1280x720 -> 3840x2160, no fallbacks, default jitter sign.

## Render size and DLAA (REFramework probes, 2026-11-06)

`via.render.UpscalingInterface` (native singleton) has `get_UpscalingQuality` /
`set_UpscalingQuality` (UInt32): FSR 2's quality. At 3840x2160: 0 = native (3840x2160 ->
3840x2160, so DLSS runs as DLAA), 1 = Quality (2560x1440), 2 = Balanced (2258x1270); 3 and 4 are
Performance and Ultra Performance; 5-7 change nothing. Setting it takes effect at once.
`RenderConfig.ImageQualityRate` is ignored while FSR 2 is on. `reframework/RE4DLSS.lua` applies the
value the add-on writes to `reframework/data/RE4DLSS_mode.txt` (-1 = the game's own) and reports
the current one in `RE4DLSS_status.txt`.
