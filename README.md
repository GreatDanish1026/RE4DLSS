# RE4DLSS 1.2.1

ReShade add-on for **Resident Evil 4** (the 2023 remake, RE Engine, DirectX 12) that puts
**NVIDIA DLSS** or **AMD FSR 3.1 / FSR 4** in place of the game's FSR 2 upscaler, and adds
**DLSS or FSR frame generation**. The game ships AMD FSR 2.1; with this add-on, FSR 2's quality
setting picks the render size and the chosen upscaler does the upscaling.

DLSS: NVIDIA RTX graphics card (frame generation: RTX 40/50). FSR 3.1: any DirectX 12 graphics
card; FSR 4: Radeon cards that support it (AMD's runtime picks). ReShade 6.8 or later with add-on
support.

## What it does

- **DLSS or FSR 3/4 in place of FSR 2** (F9 to switch on and off): DLSS Super Resolution
  310.9.16, or AMD FSR (SDK 2.4.0: FSR 4 where the GPU supports it, FSR 3.1 elsewhere), takes over
  FSR 2's upscaling pass, with the inputs the game prepares for FSR 2 (colour, depth, motion
  vectors, jitter; FSR also gets the game's reactive mask and camera). History resets follow
  FSR 2's.
- **Upscaler**: Auto (default: DLSS on RTX cards, FSR otherwise), DLSS, or FSR 3.1 / FSR 4.
- **Frame generation**: Off (default), DLSS 2x / 3x / 4x / Dynamic (NVIDIA Streamline 2.14.16,
  RTX 40/50; 3x and 4x need an RTX 50; with NVIDIA Reflex), or FSR 2x (any DirectX 12 card). It
  uses the game's depth, motion vectors and camera, and works with any of the upscalers, including
  the game's own FSR 2. Switching it on, or between DLSS and FSR, takes a game restart.
- **Render size**: the game's FSR 2 quality setting: Quality (67%), Balanced (59%), Performance
  (50%) and Ultra Performance (33%).
- **Sharpening**: FSR 2's sharpening pass still runs on the upscaled image, so the game's
  Sharpness setting applies as before.
- **DLSS model**: Automatic (default: K, M at Performance, L at Ultra Performance, as NVIDIA recommends), or K, J, L or M (DLSS 4.5) at every resolution, or NVIDIA's default.

Settings are in ReShade's Add-ons tab (Home key) under RE4DLSS, saved in `ReShade.ini` under
`[RE4DLSS]`.

## Install

**With Fluffy Mod Manager**: put `RE4DLSS-1.2.1.zip` in Fluffy Mod Manager's `Games\RE4R\Mods`
folder (or drag it onto the manager) and enable it. It installs into the game folder:

- `RE4DLSS.addon64` (the add-on) and `nvngx_dlss.dll` (NVIDIA's DLSS runtime, 310.9.16)
- `streamline\` (NVIDIA's frame generation runtime, Streamline 2.14.16, with a copy of
  `nvngx_dlss.dll`)
- `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` and
  `amd_fidelityfx_framegeneration_dx12.dll` (AMD's FSR runtime, SDK 2.4.0)
- `RE4DLSS-README.md`, `RE4DLSS-THIRD_PARTY_NOTICES.txt`, `RE4DLSS-licenses\`

**By hand**: copy the contents of the zip's `RE4DLSS` folder into the game folder (where
`re4.exe` is), leaving out `modinfo.ini`.

Then:

1. ReShade 6.8 or later with full add-on support must be installed for `re4.exe` (DirectX 12).
2. In the game's graphics settings, turn **FidelityFX Super Resolution 2** on and pick a quality.
   With FSR 2 off (TAA), the add-on does nothing.
3. Upscaling is on by default (DLSS on RTX cards, FSR otherwise); ReShade (Home), Add-ons tab,
   RE4DLSS picks the upscaler and frame generation and shows what's running.

If the NVIDIA App has a DLSS model override for the game, it takes precedence over the model here.

## Notes

- Tested at 3840x2160 on an NVIDIA RTX card at every FSR 2 quality, and with DLSS frame
  generation 2x.
- Works with REFramework: REFramework hooks NVIDIA's frame generation DLL in a way that would hang
  the game at launch; the add-on steps around that hook while the game's swapchain is created.
- Frame generation doesn't get a picture without the HUD yet, so the HUD may wobble slightly.
- FSR 3.1/4 through this add-on is untested on Radeon cards so far.
- `RE4DLSS.log` next to `re4.exe` records what the add-on did and why it fell back to FSR 2, if
  it did; `streamline\sl.log` is NVIDIA's frame generation log.

## Building

`build.ps1` builds `bin\RE4DLSS.addon64` with Visual Studio 2022's C++ tools (MSVC): `-Install`
copies it and the NVIDIA and AMD runtimes into the game folder; `-Package` makes the Fluffy Mod
Manager zip in `bin\release`. The runtimes aren't in the repository: put NVIDIA's signed
`nvngx_dlss.dll` 310.9.16 in `third_party\bin`, the Streamline 2.14.16 release's `bin\x64` in
`third_party\streamline\bin\x64`, and AMD's signed FSR SDK 2.4.0 DLLs
(`Kits\FidelityFX\signedbin`) in `third_party\fidelityfx\bin`. `docs\FINDINGS.md` describes the
game's FSR 2 passes, `docs\FSR.md` the FSR upscaler and `docs\FRAMEGEN.md` frame generation.

## Licence

MIT (see `LICENSE`). Third-party code and NVIDIA's and AMD's runtimes: `THIRD_PARTY_NOTICES.txt`.
