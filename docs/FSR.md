# FSR 3.1 / FSR 4 in place of FSR 2 (1.1.0, 2026-11-03)

Requested by a user: FSR 3/4 for Radeon (and other non-RTX) players. The game's own FSR 2.1
already prepares everything a newer upscaler needs, so FSR joins DLSS as a second backend at the
same hook: the accumulate pass (cs226d3330), writing its u0; RCAS still sharpens afterwards.

Backend: `src/fsr.cpp` from DoubleExposureDLSS / ReunionDLSS (AMD FSR SDK 2.4.0 through
`amd_fidelityfx_loader_dx12.dll` + `amd_fidelityfx_upscaler_dx12.dll` next to re4.exe). The runtime
picks FSR 4 (ML) on Radeons that support it and FSR 3.1 elsewhere. Changes for RE Engine:
`metersPerUnit` 1 (metres, UE was centimetres), finite or infinite far plane from the game,
pre-exposure passed through.

| FSR 3/4 input | From |
|---|---|
| Colour | luminance pyramid csaa8d773e t0 (r11g11b10f, render size) |
| Depth | CS_UpscalingDepthTranslation csd2efdab0 u0 (r32f, reversed Z) |
| Motion | CS_UpscalingVelocityTranslate cs5469503e u0 (rg16f UV offsets, current -> previous), scale = render size |
| Reactive | FSR 2 reactive mask cs1e379886 u0 (r8), setting "FSR: use the game's reactive mask" (default on) |
| Jitter | cbFSR2 fJitter (byte 40), render pixels, same convention as FSR 2 |
| Near / far | cbFSR2 fDeviceToViewDepth (byte 64): reversed + infinite = (~0, near); reversed finite = (-q, q*far), q = near/(far-near) |
| FOV | 2 atan(cbFSR2 fTanHalfFOV, byte 116) |
| Pre-exposure | cbFSR2 byte 112 |
| Reset | uFrameIndex (byte 28) drops to 0 |
| Sharpening | off (the game's RCAS runs after) |

Settings: `Upscaler` 0 Auto (DLSS if NGX starts, else FSR) / 1 DLSS / 2 FSR, `FsrReactiveMask`.
The inactive backend's history is released on a switch. The log line "FSR running" shows the
near/far/FOV read from the game and whether the reactive mask was found.

Status: built and installed, untested. To check on an RTX card: Upscaler = FSR, every FSR 2
quality, the log's near/far/FOV look sane (near ~0.01, FOV ~ the game's), no ghosting on
particles/fire, F9 and switching back to DLSS. FSR 4 needs a Radeon tester.
