# Builds bin\RE4DLSS.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# Usage: .\build.ps1 [-Install] [-Package] [-GameDir <folder of re4.exe>]
#   -Install  copies the add-on, nvngx_dlss.dll, AMD's FSR runtime and NVIDIA's Streamline runtime
#             (in a streamline folder) next to re4.exe (game closed).
#   -Package  makes bin\release\RE4DLSS-<version>.zip for Fluffy Mod Manager: one folder,
#             RE4DLSS, with modinfo.ini and the files laid out as they go in the game folder.
# AMD's FSR runtime (amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll,
# amd_fidelityfx_framegeneration_dx12.dll) comes from Kits\FidelityFX\signedbin of AMD's FSR SDK 2.4.0
# (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases), copied to third_party\fidelityfx\bin.
# NVIDIA's Streamline 2.14.16 runtime (bin\x64 of the SDK release) goes in third_party\streamline\bin\x64.
param(
    [switch]$Install,
    [switch]$Package,
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\RESIDENT EVIL 4  BIOHAZARD RE4"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars64.bat runs vswhere by name
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$addon = "$root\bin\RE4DLSS.addon64"

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0.
$cmd = @(
    "call `"$vcvars`" >nul",
    "fxc /nologo /T cs_5_1 /E main /O3 /Vn kUiAlphaCs /Fh `"$obj\ui_alpha_cs.h`" `"$root\src\ui_alpha.hlsl`" >nul",
    "fxc /nologo /T cs_5_1 /E main /O3 /Vn kHudComposeCs /Fh `"$obj\hud_compose_cs.h`" `"$root\src\hud_compose.hlsl`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$obj`" /I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" /I `"$root\third_party\dlss\include`" " +
     "/I `"$root\third_party\streamline\include`" " +
     "/I `"$root\third_party\fidelityfx\api\include`" /I `"$root\third_party\fidelityfx\upscalers\include`" /I `"$root\third_party\fidelityfx\framegeneration\include`" " +
     "/Fo`"$obj\\`" `"$root\src\main.cpp`" `"$root\src\upscaler.cpp`" `"$root\src\dlss.cpp`" `"$root\src\fsr.cpp`" " +
     "`"$root\src\framegen.cpp`" `"$root\src\fsrfg.cpp`" `"$root\third_party\reshade\utils\descriptor_tracking.cpp`" " +
     "user32.lib advapi32.lib ole32.lib d3d12.lib dxgi.lib `"$root\third_party\dlss\lib\nvsdk_ngx_s.lib`" " +
     "/link /DLL /OUT:`"$addon`" /PDB:`"$obj\RE4DLSS.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "Built $addon"

# NVIDIA's signed DLSS runtime (310.9.16, from the Streamline 2.14.16 SDK's bin\x64; not in git).
$ngx = "$root\third_party\bin\nvngx_dlss.dll"
# NVIDIA's Streamline runtime (frame generation), signed, in a streamline folder next to the exe.
$sl = "$root\third_party\streamline\bin\x64"
$slFiles = "sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlssg.dll"
# AMD's FSR runtime, signed, next to the exe.
$ffx = "$root\third_party\fidelityfx\bin"
$ffxFiles = "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll", "amd_fidelityfx_framegeneration_dx12.dll"
if ($Install -or $Package) {
    if (-not (Test-Path $ngx)) { throw "nvngx_dlss.dll not found in third_party\bin" }
    foreach ($f in $slFiles) { if (-not (Test-Path "$sl\$f")) { throw "$f not found in $sl (unzip NVIDIA's Streamline 2.14.16 release there)" } }
    foreach ($f in $ffxFiles) { if (-not (Test-Path "$ffx\$f")) { throw "$f not found in $ffx (copy Kits\FidelityFX\signedbin from AMD's FSR SDK 2.4.0)" } }
}

# The add-on and the NVIDIA and AMD runtimes, as they go in the game folder.
function Copy-Runtime([string]$dest) {
    Copy-Item $addon, $ngx $dest -Force
    foreach ($f in $ffxFiles) { Copy-Item "$ffx\$f" $dest -Force }
    New-Item -ItemType Directory -Force "$dest\streamline" | Out-Null
    foreach ($f in $slFiles) { Copy-Item "$sl\$f" "$dest\streamline" -Force }
}

if ($Install) {
    if (-not (Test-Path "$GameDir\re4.exe")) { throw "re4.exe not found in $GameDir (pass -GameDir)" }
    if (Get-Process re4 -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Runtime $GameDir
    Write-Host "Installed to $GameDir"
}

if ($Package) {
    $version = [regex]::Match((Get-Content "$root\src\version.h" -Raw), 'RD_VERSION "([^"]+)"').Groups[1].Value
    $stage = "$root\bin\release\RE4DLSS-$version"
    $mod = "$stage\RE4DLSS"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force "$mod\RE4DLSS-licenses" | Out-Null

    # Files installed into the game folder (where re4.exe is).
    Copy-Runtime $mod
    Copy-Item "$root\README.md" "$mod\RE4DLSS-README.md"
    Copy-Item "$root\THIRD_PARTY_NOTICES.txt" "$mod\RE4DLSS-THIRD_PARTY_NOTICES.txt"
    Copy-Item "$root\LICENSE" "$mod\RE4DLSS-licenses\RE4DLSS-LICENSE.txt"
    Copy-Item "$root\third_party\dlss\LICENSE.txt" "$mod\RE4DLSS-licenses\NVIDIA-DLSS-LICENSE.txt"
    Copy-Item "$root\third_party\streamline\license.txt" "$mod\RE4DLSS-licenses\Streamline-LICENSE.txt"
    Copy-Item "$root\third_party\streamline\licenses\reflex.license.txt" "$mod\RE4DLSS-licenses\NVIDIA-Reflex-LICENSE.txt"
    Copy-Item "$root\third_party\reshade\LICENSE.md" "$mod\RE4DLSS-licenses\ReShade-LICENSE.md"
    Copy-Item "$root\third_party\fidelityfx\license.md" "$mod\RE4DLSS-licenses\AMD-FSR-LICENSE.txt"

    # Fluffy Mod Manager's mod information (key=value, one per line).
    @(
        "name=RE4DLSS - DLSS, FSR 3/4 and frame generation for Resident Evil 4",
        "version=v$version",
        "description=Puts NVIDIA DLSS or AMD FSR 3.1 / FSR 4 in place of the game's FSR 2 upscaler, and adds DLSS or FSR frame generation: turn FSR 2 on in the game and its quality setting picks the render size, the chosen upscaler does the upscaling, and the game's sharpness setting still applies. DLSS needs an NVIDIA RTX card (frame generation RTX 40/50); FSR runs on any DirectX 12 GPU (FSR 4 on Radeons that support it). Needs ReShade 6.8+ with add-on support. Settings: ReShade overlay (Home), Add-ons tab, RE4DLSS; F9 switches upscaling on and off.",
        "author=Scott Dinwiddie"
    ) | Set-Content "$mod\modinfo.ini" -Encoding ascii

    # Zip with forward-slash entry names (Compress-Archive in PowerShell 5.1 writes backslashes,
    # which some tools mishandle).
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zip = "$root\bin\release\RE4DLSS-$version.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        Get-ChildItem $stage -Recurse -File | ForEach-Object {
            $entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
    Write-Host "Packaged $zip"
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        "{0,-60} {1,12:N0}" -f $_.FullName.Substring($stage.Length + 1), $_.Length
    }
}
