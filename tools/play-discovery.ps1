param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
    [string]$SavePath = (Join-Path $PSScriptRoot '..\saves\megaman_zero_discovery.sav'),
    [switch]$FreshSave,
    [int]$HeadlessFrames = 0,
    [string]$InputProfile = 'campaign-clear'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = (Resolve-Path $BuildDir).Path
$exe = Join-Path $build 'MegaManZeroRecomp.exe'
$config = Join-Path $root 'game.toml'
$rom = Join-Path $root 'roms\megaman_zero_usa.gba'

if (-not (Test-Path -LiteralPath $exe)) {
    throw "Missing $exe. Regenerate and build MegaManZeroRecomp first."
}
if (-not (Test-Path -LiteralPath $rom)) {
    throw "Missing $rom. See baserom.md for the required ROM identity."
}

$save = [IO.Path]::GetFullPath($SavePath)
$saveDir = Split-Path -Parent $save
New-Item -ItemType Directory -Force -Path $saveDir | Out-Null
if ($FreshSave) {
    Remove-Item -LiteralPath $save -ErrorAction SilentlyContinue
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$session = Join-Path $build "discovery_sessions\$stamp"
New-Item -ItemType Directory -Force -Path $session | Out-Null
$coverage = Join-Path $session 'coverage.json'
$missFrag = Join-Path $session 'misses.toml.frag'
$iwram = Join-Path $session 'first_ram_miss_iwram.bin'
$inputTrace = Join-Path $session 'keyinput.csv'
$initialSave = Join-Path $session 'initial.sav'
$finalSave = Join-Path $session 'final.sav'
$log = Join-Path $session 'session.log'
$stdoutLog = Join-Path $session 'runtime.stdout.log'
$stderrLog = Join-Path $session 'runtime.stderr.log'
$gameReview = Join-Path $session 'review_game.toml.frag'
$biosReview = Join-Path $session 'review_bios.toml.frag'

if (Test-Path -LiteralPath $save) {
    Copy-Item -LiteralPath $save -Destination $initialSave
} else {
    # MMZ uses the 32 KiB SRAM hardware path; an absent save reads as erased FF.
    $blank = [byte[]]::new(32 * 1024)
    for ($i = 0; $i -lt $blank.Length; ++$i) {
        $blank[$i] = 0xFF
    }
    [IO.File]::WriteAllBytes($initialSave, $blank)
}

$manifest = [ordered]@{
    mode = if ($HeadlessFrames -gt 0) { 'headless-discovery-smoke' } else { 'interactive-discovery' }
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    executable = $exe
    executable_sha1 = (Get-FileHash -LiteralPath $exe -Algorithm SHA1).Hash.ToLowerInvariant()
    rom_sha1 = (Get-FileHash -LiteralPath $rom -Algorithm SHA1).Hash.ToLowerInvariant()
    config_sha1 = (Get-FileHash -LiteralPath $config -Algorithm SHA1).Hash.ToLowerInvariant()
    bios_sha1 = '300c20df6731a33952ded8c436f7f186d25d3492'
    save = $save
    initial_save = $initialSave
    initial_save_sha1 = (Get-FileHash -LiteralPath $initialSave -Algorithm SHA1).Hash.ToLowerInvariant()
    input_trace = $inputTrace
    real_bios_lle = $true
    strict_static = $false
    interpreter_bridge = 'on-dispatch-miss'
    native_self_heal = $false
    cache_load = $false
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $session 'manifest.json') -Encoding utf8

$envNames = @(
    'GBARECOMP_BIOS_HLE', 'GBARECOMP_STRICT_STATIC',
    'GBARECOMP_SELFHEAL_RECOMPILE', 'GBARECOMP_HANG_WATCHDOG',
    'GBARECOMP_COVERAGE_JSON', 'GBARECOMP_MISS_FRAG',
    'GBARECOMP_MISS_IWRAM_DUMP', 'GBARECOMP_DEMO_INPUT',
    'GBARECOMP_FORCE_INTERP', 'GBARECOMP_HEAL_CACHE',
    'GBARECOMP_INPUT_RECORD', 'GBARECOMP_INPUT_REPLAY'
)
$savedEnv = @{}
foreach ($name in $envNames) {
    $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

$exitCode = 1
Push-Location $root
try {
    $env:GBARECOMP_BIOS_HLE = '0'
    $env:GBARECOMP_STRICT_STATIC = '0'
    # Discovery uses the real instruction interpreter only at a missing static
    # dispatch target. Native overlay compilation/cache loading stays off so
    # the coverage report remains reproducible and reviewable.
    $env:GBARECOMP_SELFHEAL_RECOMPILE = '0'
    $env:GBARECOMP_HANG_WATCHDOG = '0'
    $env:GBARECOMP_COVERAGE_JSON = $coverage
    $env:GBARECOMP_MISS_FRAG = $missFrag
    $env:GBARECOMP_MISS_IWRAM_DUMP = $iwram
    $env:GBARECOMP_INPUT_RECORD = $inputTrace
    Remove-Item Env:GBARECOMP_FORCE_INTERP -ErrorAction SilentlyContinue
    Remove-Item Env:GBARECOMP_HEAL_CACHE -ErrorAction SilentlyContinue
    Remove-Item Env:GBARECOMP_INPUT_REPLAY -ErrorAction SilentlyContinue

    if ($HeadlessFrames -gt 0) {
        $env:GBARECOMP_DEMO_INPUT = $InputProfile
        $arguments = @('--frames', $HeadlessFrames, '--save', $save, $config)
    } else {
        Remove-Item Env:GBARECOMP_DEMO_INPUT -ErrorAction SilentlyContinue
        $arguments = @('--window', '--save', $save, $config)
        Write-Host 'Discovery mode: real BIOS/LLE and native recompilation first.'
        Write-Host 'An uncovered dispatch target falls back to the interpreter, is logged as NOT_STATIC, and is never auto-merged.'
        Write-Host 'Close the game window to finalize this session''s coverage artifacts.'
    }

    # Keep native stderr as ordinary diagnostic text. Direct `2>&1` piping in
    # Windows PowerShell wraps every stderr line in a noisy NativeCommandError.
    # Start-Process still opens the SDL game window normally (no hidden-window
    # flag) and gives us the authoritative native exit code after it closes.
    $process = Start-Process -FilePath $exe -ArgumentList $arguments `
        -Wait -PassThru -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog
    $exitCode = $process.ExitCode
    $output = @(
        Get-Content -LiteralPath $stdoutLog
        Get-Content -LiteralPath $stderrLog
    )
    $output | ForEach-Object { $_.ToString() } | Set-Content -LiteralPath $log -Encoding utf8
    $output | Write-Output
} finally {
    Pop-Location
    foreach ($name in $envNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], 'Process')
    }
}

if (Test-Path -LiteralPath $coverage) {
    $summary = Get-Content -LiteralPath $coverage -Raw | ConvertFrom-Json
    if ($summary.coverage -eq 'NOT_STATIC') {
        Write-Warning "Discovery session used interpreter fallback: misses=$($summary.distinct_misses) interpreted_insns=$($summary.interpreted_insns)"
        Push-Location $root
        try {
            python ./tools/review_misses.py --coverage $coverage `
                --game-out $gameReview --bios-out $biosReview
            if ($LASTEXITCODE -ne 0) {
                Write-Warning 'Automatic classification aid failed; raw coverage and miss fragment are still preserved.'
            }
        } finally {
            Pop-Location
        }
        Write-Host "Review proposals only; do not merge blindly: $gameReview and $biosReview"
    } else {
        Write-Host 'Discovery session stayed fully static for the path actually played.'
    }
} else {
    Write-Warning 'The runtime did not emit coverage.json; inspect the session log.'
}

if (Test-Path -LiteralPath $save) {
    Copy-Item -LiteralPath $save -Destination $finalSave -Force
} else {
    # Preserve the effective erased SRAM even when the guest did not write it.
    Copy-Item -LiteralPath $initialSave -Destination $finalSave -Force
}
$manifest['final_save'] = $finalSave
$manifest['final_save_sha1'] =
    (Get-FileHash -LiteralPath $finalSave -Algorithm SHA1).Hash.ToLowerInvariant()
$manifest['ended_utc'] = (Get-Date).ToUniversalTime().ToString('o')
$manifest['exit_code'] = $exitCode
$manifest['input_trace_sha1'] = if (Test-Path -LiteralPath $inputTrace) {
    (Get-FileHash -LiteralPath $inputTrace -Algorithm SHA1).Hash.ToLowerInvariant()
} else { $null }
if (Test-Path -LiteralPath $coverage) {
    $manifest['coverage'] = $summary.coverage
    $manifest['distinct_misses'] = $summary.distinct_misses
    $manifest['interpreted_insns'] = $summary.interpreted_insns
}
if (Test-Path -LiteralPath $log) {
    $frameMatch = [regex]::Match((Get-Content -LiteralPath $log -Raw), 'ppu_frames=(\d+)')
    if ($frameMatch.Success) {
        $manifest['final_ppu_frame'] = [uint64]$frameMatch.Groups[1].Value
    }
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $session 'manifest.json') -Encoding utf8

Write-Host "Discovery session: $session"
Write-Host "Persistent SRAM: $save"
if ($exitCode -ne 0) {
    throw "Discovery runtime exited with code $exitCode; see $log"
}
