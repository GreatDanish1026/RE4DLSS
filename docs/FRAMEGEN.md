# Frame generation (1.2.0, started 2026-11-03)

DLSS Frame Generation (Streamline 2.14.16, RTX 40/50: 2x/3x/4x/Dynamic) or AMD FSR frame
generation (FSR SDK 2.4.0, any DirectX 12 GPU: 2x). `src/framegen.cpp` and `src/fsrfg.cpp` are
forked from StrayDLSS 1.0.0 (Unreal Engine 4.27); what changed for RE Engine:

| | Unreal add-ons | RE4 |
|---|---|---|
| Hook point | TSR / TAA pass | FSR 2's accumulate (cs226d3330), whoever upscales (DLSS, FSR 3/4 or the game's FSR 2) |
| Depth | UE depth buffer (depth read) | FSR 2's r32f conversion (csd2efdab0 u0), copied; state srv (0xC0) |
| Motion | our motion.hlsl, render pixels | the game's rg16f UV offsets (cs5469503e u0), state srv; DLSS-G mvecScale 1, FSR FG scale = render size |
| Camera | View uniform buffer | RE Engine SceneInfo, found by content (below) |
| Units | cm | m (FSR FG viewSpaceToMetersFactor 1) |
| Pre-HUD picture | UE's HDR back buffer copy | final pass ps9d1f6421 repeated after the game's, with the scene (ps792d66db's #85ef6, copied into the HUD texture #fd900 again), into our back-buffer-format target |
| Streamline engine | eUnreal | eCustom "RE Engine", own project id |
| Tagging | at submission (`kTagAtRecord` false) | same; the first 120 "trace:" log lines show recorded/submitted/present order |

## SceneInfo

RE Engine's per-view constant buffer (layout from RE Village's shaders, 528 bytes, row_major,
row vector * matrix): viewProjMat 0, transposeViewMat 64 (3x4), transposeViewInvMat 112 (3x4),
projElement 160, projInvElements 192, viewProjInvMat 224, prevViewProjMat 288, ZToLinear 352,
screenSize 368, screenInverseSize 376, cameraNearPlane 392, cameraFarPlane 396.

RE4's FSR passes don't bind it, so `upscaler.cpp` looks at every root constant buffer the game
binds (upload memory only, each range once per command list) while frame generation runs, and keeps
the last one whose screenSize / screenInverseSize are the render size (from cbFSR2). Frame
generation builds from it:
- viewInv (4x4) from transposeViewInvMat: right, up, forward axes and the camera position;
- projection = viewInv * viewProj, checked to be perspective (w = +-view z), jitter terms
  ([2][0], [2][1]) zeroed;
- clipToPrevClip = viewProjInv * prevViewProj;
- near / far from SceneInfo (far 0 / huge = infinite).
The log shows the first checks ("frame generation: SceneInfo ok/REJECTED ..."). If RE4's layout
differs from Village's, that's where it shows.

## To test

1. Overlay: Frame generation = DLSS 2x (or FSR 2x), restart the game.
2. Log: "swapchain created through Streamline", "SceneInfo found", "SceneInfo ok", "first frame
   tagged", trace lines with "present with inputs", frame generation state with 2 frames presented.
3. In game: smoothness, HUD wobble, ghosting on moving characters; DLSS-G status bits in the
   Frame generation details.
