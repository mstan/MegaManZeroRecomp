param(
    [int]$Frames = 10000,
    [ValidateRange(240, 320)]
    [int]$ViewWidth = 240,
    [string]$InputProfile = 'menu',
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
    [string]$InputTrace = '',
    [string]$InitialSave = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = (Resolve-Path $BuildDir).Path
$exe = Join-Path $build 'MegaManZeroRecomp.exe'
$config = Join-Path $root 'game.toml'
$profileTag = ($InputProfile -replace '[^A-Za-z0-9_.-]', '_')
$png = Join-Path $build "mmz_strict_${profileTag}_${ViewWidth}x160_f$Frames.png"
$log = Join-Path $build "mmz_strict_${profileTag}_${ViewWidth}x160_f$Frames.log"
$save = Join-Path $build "mmz_strict_${profileTag}_blank.sav"
$stdoutLog = Join-Path $build '.strict-stdout.tmp'
$stderrLog = Join-Path $build '.strict-stderr.tmp'

if (-not (Test-Path -LiteralPath $exe)) {
    throw "Missing $exe. Regenerate and build MegaManZeroRecomp first."
}

$env:GBARECOMP_BIOS_HLE = '0'
$env:GBARECOMP_STRICT_STATIC = '1'
$env:GBARECOMP_SELFHEAL_RECOMPILE = '0'
$env:GBARECOMP_HANG_WATCHDOG = '0'
$env:GBARECOMP_VIEW_WIDTH = $ViewWidth.ToString(
    [System.Globalization.CultureInfo]::InvariantCulture)
Remove-Item Env:GBARECOMP_WIDESCREEN -ErrorAction SilentlyContinue
Remove-Item Env:GBARECOMP_WS_WIP -ErrorAction SilentlyContinue
Remove-Item Env:GBARECOMP_MMZ_WS_TRACE -ErrorAction SilentlyContinue
Remove-Item Env:GBARECOMP_FORCE_INTERP -ErrorAction SilentlyContinue
Remove-Item Env:GBARECOMP_BOSS_STRATEGY -ErrorAction SilentlyContinue
Remove-Item Env:GBARECOMP_INPUT_RECORD -ErrorAction SilentlyContinue
if ($InputTrace) {
    $tracePath = (Resolve-Path $InputTrace).Path
    $env:GBARECOMP_INPUT_REPLAY = $tracePath
    Remove-Item Env:GBARECOMP_DEMO_INPUT -ErrorAction SilentlyContinue
} else {
    Remove-Item Env:GBARECOMP_INPUT_REPLAY -ErrorAction SilentlyContinue
    $env:GBARECOMP_DEMO_INPUT = $InputProfile
}
if ($InitialSave) {
    Copy-Item -LiteralPath (Resolve-Path $InitialSave).Path -Destination $save -Force
} else {
    Remove-Item -LiteralPath $save -ErrorAction SilentlyContinue
}

Push-Location $root
try {
    $process = Start-Process -FilePath $exe -ArgumentList @(
        '--frames', $Frames, '--view-width', $ViewWidth,
        '--dump-png', $png, '--save', $save, $config
    ) -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
    $exitCode = $process.ExitCode
    $output = @(
        Get-Content -LiteralPath $stdoutLog
        Get-Content -LiteralPath $stderrLog
    )
} finally {
    Pop-Location
}

$output | ForEach-Object { $_.ToString() } | Set-Content -LiteralPath $log -Encoding utf8
$output | Write-Output
Remove-Item -LiteralPath $stdoutLog, $stderrLog -ErrorAction SilentlyContinue
if ($exitCode -ne 0) {
    throw "Strict run exited with code $exitCode; see $log"
}

$text = ($output | ForEach-Object { $_.ToString() }) -join "`n"
$required = @(
    'bios_backend=LLE (recompiled BIOS)',
    'force_interp=DISABLED',
    'strict_static=ENABLED self_heal_recompile=DISABLED cache_load=DISABLED interpreter_bridge=ABORT',
    'self_heal_coverage=FULLY_STATIC dispatch_misses=0 interpreted_insns=0 healed_native=0',
    'unmapped=0 io_unhandled=0'
)
if ($InputTrace) {
    $required += 'input_replay=ENABLED'
} else {
    $required += 'input_replay=DISABLED'
}
foreach ($marker in $required) {
    if (-not $text.Contains($marker)) {
        throw "Strict proof marker missing: $marker; see $log"
    }
}

$extendedMarker = "extended view ON: requested=${ViewWidth}x160 "
if ($ViewWidth -gt 240 -and -not $text.Contains($extendedMarker)) {
    throw "Requested extended-view width was not activated: $extendedMarker; see $log"
}
if ($ViewWidth -eq 240 -and $text.Contains('extended view ON:')) {
    throw "Faithful 240x160 verification unexpectedly activated extended view; see $log"
}

$inputLabel = if ($InputTrace) { "trace=$tracePath" } else { "input=$InputProfile" }
Write-Output "strict_verification=PASS frames=$Frames view=${ViewWidth}x160 $inputLabel png=$png log=$log"
