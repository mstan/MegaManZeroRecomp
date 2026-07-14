<#
Build the MegaManZeroRecomp Windows x64 release archive.

The archive contains the stripped executable, its MinGW/SDL runtime DLLs, the
toolchain-less overlay compiler used for uncovered paths, and a short player
README. It never contains a ROM, GBA BIOS, save data, generated source, or
developer configuration.

Usage:
  powershell -File tools\make_release.ps1 -Version 0.0.1
#>
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDir = 'build-release',
    [string]$GbarecompRoot = (Join-Path $PSScriptRoot '..\..\gbarecomp'),
    [ValidateRange(1, 32)][int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'

if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') {
    throw "Version must look like 0.0.1 (received '$Version')."
}

$mingwBin = 'C:\msys64\mingw64\bin'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$engine = (Resolve-Path $GbarecompRoot).Path
$build = Join-Path $root $BuildDir
$out = Join-Path $root 'release-stage'
$target = 'MegaManZeroRecomp'
$stageName = "$target-windows-x64-v$Version"
$stage = Join-Path $out $stageName
$zip = Join-Path $out "$stageName.zip"
$expectedSha1 = '193b14120119162518a73c70876f0b8bffdbd96e'
$dlls = @('SDL2.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')

foreach ($required in @(
    (Join-Path $mingwBin 'cc.exe'),
    (Join-Path $mingwBin 'c++.exe'),
    (Join-Path $mingwBin 'ninja.exe'),
    (Join-Path $mingwBin 'strip.exe'),
    (Join-Path $engine 'CMakeLists.txt'),
    (Join-Path $engine 'tools\fetch_tcc.ps1'),
    (Join-Path $engine 'src\runtime\generated_bios\bios_recompiled.cpp'),
    (Join-Path $engine 'src\runtime\generated_bios\bios_recompiled.h')
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required release dependency is missing: $required"
    }
}

$generated = @(Get-ChildItem -LiteralPath (Join-Path $root 'generated') `
    -Filter 'recompiled_*.cpp' -File -ErrorAction SilentlyContinue)
if ($generated.Count -eq 0) {
    throw 'Generated guest shards are missing. Supply the verified ROM and run tools\regen.ps1 first.'
}

$env:PATH = "$mingwBin;$env:PATH"
New-Item -ItemType Directory -Force -Path $out | Out-Null

& cmake -S $root -B $build -G Ninja `
    -DCMAKE_C_COMPILER="$mingwBin/cc.exe" `
    -DCMAKE_CXX_COMPILER="$mingwBin/c++.exe" `
    -DCMAKE_MAKE_PROGRAM="$mingwBin/ninja.exe" `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG" `
    -DGBARECOMP_ROOT="$engine" `
    -DGBARECOMP_BUILD_ORACLE=OFF `
    -DGBARECOMP_MINGW_PREFIX_UNIX='/c/msys64/mingw64' `
    -DSDL2_INCLUDE_DIR='C:/msys64/mingw64/include/SDL2' `
    -DSDL2_LIBRARY='C:/msys64/mingw64/lib/libSDL2.dll.a'
if ($LASTEXITCODE -ne 0) { throw "Release configure failed ($LASTEXITCODE)." }

& cmake --build $build --target $target --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "Release build failed ($LASTEXITCODE)." }

$exe = Join-Path $build "$target.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "Expected executable is missing: $exe" }
& (Join-Path $mingwBin 'strip.exe') $exe
if ($LASTEXITCODE -ne 0) { throw "strip failed ($LASTEXITCODE)." }

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath $exe -Destination $stage
foreach ($dll in $dlls) {
    $source = Join-Path $mingwBin $dll
    if (-not (Test-Path -LiteralPath $source)) { throw "Runtime DLL is missing: $source" }
    Copy-Item -LiteralPath $source -Destination $stage
}

& (Join-Path $engine 'tools\fetch_tcc.ps1') `
    -Toolchain (Join-Path $stage 'overlay_toolchain') -EngineRoot $engine
if ($LASTEXITCODE -ne 0) { throw "Overlay toolchain staging failed ($LASTEXITCODE)." }

@"
# Mega Man Zero - GBA static recompilation (Windows x64)

This is a playable, static-first v$Version build. It executes the real GBA BIOS
through the LLE path by default. If play reaches a target outside the reviewed
static corpus, the runtime bridges that target through its instruction
interpreter, compiles it to native code, and caches it for later launches.

## How to run

1. Keep this entire extracted folder together.
2. Run MegaManZeroRecomp.exe.
3. Select your legally obtained Mega Man Zero (USA) ROM when prompted.
   Expected SHA-1: $expectedSha1
4. Select your legally obtained gba_bios.bin when prompted.

The selected paths are cached next to the executable. SRAM saves, save states,
and the native coverage cache are also stored locally. The ROM and BIOS are not
included in this archive.

Keyboard: arrows = D-pad, Z = A, X = B, A = L, S = R, Enter = Start,
Right Shift = Select, Tab = fast-forward. Shift+F1-F9 saves a state; F1-F9
loads one.

Project: https://github.com/mstan/MegaManZeroRecomp
Engine: https://github.com/mstan/gbarecomp
"@ | Out-File -LiteralPath (Join-Path $stage 'README.md') -Encoding utf8

$forbidden = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
    $_.Extension -in @('.gba', '.sav', '.srm') -or
    $_.Name -ieq 'gba_bios.bin' -or
    $_.Name -in @('game.toml', 'rom.cfg', 'bios.cfg')
}
if ($forbidden) {
    throw "Forbidden release content detected: $($forbidden.FullName -join ', ')"
}

if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

Write-Host "--- $stageName ---"
Get-ChildItem -LiteralPath $stage | Select-Object Name, Length | Out-Host
Get-Item -LiteralPath $zip | Select-Object Name, Length | Out-Host
Get-FileHash -LiteralPath $zip -Algorithm SHA256 | Select-Object Algorithm, Hash, Path | Out-Host
