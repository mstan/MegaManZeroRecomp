param(
    [string]$Rom = (Join-Path $PSScriptRoot '..\roms\megaman_zero_usa.gba'),
    [string]$GbarecompRoot = (Join-Path $PSScriptRoot '..\..\gbarecomp-wt-mmz-static'),
    [int]$MaxFunctions = 65536
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$romPath = (Resolve-Path $Rom).Path
$engine = (Resolve-Path $GbarecompRoot).Path
$tool = Join-Path $engine 'build\gba_recompile.exe'
if (-not (Test-Path -LiteralPath $tool)) {
    throw "Missing $tool. Configure and build the isolated gbarecomp worktree first."
}

$actual = (Get-FileHash -LiteralPath $romPath -Algorithm SHA1).Hash.ToLowerInvariant()
$expected = '193b14120119162518a73c70876f0b8bffdbd96e'
if ($actual -ne $expected) {
    throw "Mega Man Zero ROM SHA-1 mismatch: got $actual expected $expected"
}

& $tool --rom $romPath --config (Join-Path $root 'game.toml') `
    --out (Join-Path $root 'generated') --max-functions $MaxFunctions
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
