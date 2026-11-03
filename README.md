# RE4DLSS 1.1.0

ReShade add-on for **Resident Evil 4** (the 2023 remake, RE Engine, DirectX 12) that puts
**NVIDIA DLSS** or **AMD FSR 3.1 / FSR 4** in place of the game's FSR 2 upscaler. The game ships
AMD FSR 2.1; with this add-on, FSR 2's quality setting picks the render size and the chosen
upscaler does the upscaling.

DLSS: NVIDIA RTX graphics card. FSR 3.1: any DirectX 12 graphics card; FSR 4: Radeon cards that
support it (AMD's runtime picks). ReShade 6.8 or later with add-on support.

## What it does

- **DLSS or FSR 3/4 in place of FSR 2** (F9 to switch on and off): DLSS Super Resolution
  310.9.16, or AMD FSR (SDK 2.4.0: FSR 4 where the GPU supports it, FSR 3.1 elsewhere), takes over
  FSR 2's upscaling pass, with the inputs the game prepares for FSR 2 (colour, depth, motion
  vectors, jitter; FSR also gets the game's reactive mask and camera). History resets follow
  FSR 2's.
- **Upscaler**: Auto (default: DLSS on RTX cards, FSR otherwise), DLSS, or FSR 3.1 / FSR 4.
- **Render size**: the game's FSR 2 quality setting: Quality (67%), Balanced (59%), Performance
  (50%) and Ultra Performance (33%).
- **Sharpening**: FSR 2's sharpening pass still runs on the upscaled image, so the game's
  Sharpness setting applies as before.
- **DLSS model**: K (transformer, default), J, or NVIDIA's default.

Settings are in ReShade's Add-ons tab (Home key) under RE4DLSS, saved in `ReShade.ini` under
`[RE4DLSS]`.

## Install

**With Fluffy Mod Manager**: put `RE4DLSS-1.1.0.zip` in Fluffy Mod Manager's `Games\RE4R\Mods`
folder (or drag it onto the manager) and enable it. It installs into the game folder:

- `RE4DLSS.addon64` (the add-on) and `nvngx_dlss.dll` (NVIDIA's DLSS runtime, 310.9.16)
- `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` (AMD's FSR runtime,
  SDK 2.4.0)
- `RE4DLSS-README.md`, `RE4DLSS-THIRD_PARTY_NOTICES.txt`, `RE4DLSS-licenses\`

**By hand**: copy the contents of the zip's `RE4DLSS` folder into the game folder (where
`re4.exe` is), leaving out `modinfo.ini`.

Then:

1. ReShade 6.8 or later with full add-on support must be installed for `re4.exe` (DirectX 12).
2. In the game's graphics settings, turn **FidelityFX Super Resolution 2** on and pick a quality.
   With FSR 2 off (TAA), the add-on does nothing.
3. Upscaling is on by default (DLSS on RTX cards, FSR otherwise); ReShade (Home), Add-ons tab,
   RE4DLSS picks the upscaler and shows what it's doing.

If the NVIDIA App has a DLSS model override for the game, it takes precedence over the model here.

## Notes

- Tested at 3840x2160 on an NVIDIA RTX card at every FSR 2 quality.
- FSR 3.1/4 through this add-on is untested on Radeon cards so far.
- `RE4DLSS.log` next to `re4.exe` records what the add-on did and why it fell back to FSR 2, if
  it did.

## Building

`build.ps1` builds `bin\RE4DLSS.addon64` with Visual Studio 2022's C++ tools (MSVC): `-Install`
copies it, `nvngx_dlss.dll` and AMD's FSR runtime into the game folder; `-Package` makes the Fluffy Mod Manager zip
in `bin\release`. `nvngx_dlss.dll` isn't in the repository: put NVIDIA's signed 310.9.16 file (from
the Streamline SDK 2.14.16 release, `bin\x64`) in `third_party\bin`, and AMD's signed
`amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` (FSR SDK 2.4.0,
`Kits\FidelityFX\signedbin`) in `third_party\fidelityfx\bin`. `docs\FINDINGS.md` describes
the game's FSR 2 passes and what the add-on changes.

## Licence

MIT (see `LICENSE`). Third-party code and NVIDIA's and AMD's runtimes: `THIRD_PARTY_NOTICES.txt`.
