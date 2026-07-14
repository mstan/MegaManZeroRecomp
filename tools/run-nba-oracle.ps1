param(
    [string]$OracleRoot = "F:\Projects\gbarecomp\_nba_oracle",
    [string]$Bios = "F:\Projects\gbarecomp\gbarecomp-wt-mmz-static\bios\gba_bios.bin",
    [string]$Rom = "$PSScriptRoot\..\roms\megaman_zero_usa.gba",
    [int]$Frames = 10000,
    [ValidateSet("menu", "walk", "campaign")]
    [string]$InputProfile = "menu",
    [int]$Port = 19866
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
public static class OracleHex {
    private static int Nibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw new FormatException("Invalid hex digit");
    }
    public static byte[] Decode(string hex) {
        if ((hex.Length & 1) != 0) throw new FormatException("Odd hex length");
        var result = new byte[hex.Length / 2];
        for (int i = 0; i < result.Length; ++i)
            result[i] = (byte)((Nibble(hex[i * 2]) << 4) | Nibble(hex[i * 2 + 1]));
        return result;
    }
}
"@

$tag = Get-Date -Format "yyyyMMdd-HHmmss"
$runDir = Join-Path $OracleRoot "build\oracle_runs\$tag"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$oracleRom = Join-Path $runDir "megaman_zero_blank.gba"
Copy-Item -LiteralPath (Resolve-Path $Rom) -Destination $oracleRom

$stdout = Join-Path $runDir "oracle.stdout.log"
$stderr = Join-Path $runDir "oracle.stderr.log"
$oracleExe = Join-Path $OracleRoot "build\nba_oracle.exe"
$process = Start-Process -FilePath $oracleExe `
    -ArgumentList @($Bios, $oracleRom, "--port", $Port) `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr `
    -WindowStyle Hidden -PassThru

$client = $null
$reader = $null
$writer = $null
try {
    for ($attempt = 0; $attempt -lt 100 -and $null -eq $client; ++$attempt) {
        try {
            $candidate = New-Object System.Net.Sockets.TcpClient
            $candidate.Connect("127.0.0.1", $Port)
            $client = $candidate
        } catch {
            if ($null -ne $candidate) { $candidate.Dispose() }
            Start-Sleep -Milliseconds 100
        }
    }
    if ($null -eq $client) { throw "Oracle did not accept a TCP connection" }

    $stream = $client.GetStream()
    $reader = New-Object System.IO.StreamReader($stream, [Text.Encoding]::UTF8, $false, 262144, $true)
    $writer = New-Object System.IO.StreamWriter($stream, (New-Object Text.UTF8Encoding($false)), 262144, $true)
    $writer.AutoFlush = $true

    function Invoke-Oracle([string]$json) {
        $writer.WriteLine($json)
        $line = $reader.ReadLine()
        if ($null -eq $line) { throw "Oracle closed after request: $json" }
        return $line
    }

    function Get-DemoKey([long]$frame) {
        [int[]]$buttons = @(8, 1, 1, 128, 1, 16, 2, 1, 32, 1, 64, 2)
        if (([math]::Floor($frame / 6) -band 1) -ne 0) { return 1023 }
        $index = [int]([math]::Floor($frame / 12) % $buttons.Count)
        return (1023 -band (-bnot $buttons[$index]))
    }

    function Get-WalkKey([long]$frame) {
        [int[]]$directions = @(16, 128, 32, 64)
        $button = $directions[[int]([math]::Floor($frame / 180) % 4)]
        if (($frame % 60) -lt 2) { $button = $button -bor 1 }
        return (1023 -band (-bnot $button))
    }

    function Get-KeyInput([long]$frame) {
        if ($InputProfile -eq "walk") { return Get-WalkKey $frame }
        if ($InputProfile -eq "campaign" -and $frame -ge 9000) {
            return Get-WalkKey ($frame - 9000)
        }
        return Get-DemoKey $frame
    }

    function Run-To([long]$target, [ref]$current) {
        while ($current.Value -lt $target) {
            $frame = [long]$current.Value
            $keyinput = Get-KeyInput $frame
            [long]$count = 1
            while (($frame + $count) -lt $target -and
                   (Get-KeyInput ($frame + $count)) -eq $keyinput) {
                ++$count
            }
            $response = Invoke-Oracle "{`"cmd`":`"run_frames`",`"n`":$count,`"keyinput`":$keyinput}"
            if (-not $response.Contains('"ok":true')) { throw $response }
            $current.Value += $count
        }
    }

    function Save-Screenshot([long]$frame) {
        $shot = (Invoke-Oracle '{"cmd":"screenshot"}') | ConvertFrom-Json
        if (-not $shot.ok) { throw "Oracle screenshot failed" }
        $rgb = [OracleHex]::Decode([string]$shot.data)
        $header = [Text.Encoding]::ASCII.GetBytes("P6`n240 160`n255`n")
        $ppm = New-Object byte[] ($header.Length + $rgb.Length)
        [Array]::Copy($header, 0, $ppm, 0, $header.Length)
        [Array]::Copy($rgb, 0, $ppm, $header.Length, $rgb.Length)
        $ppmPath = Join-Path $runDir "oracle_f$frame.ppm"
        $pngPath = Join-Path $runDir "oracle_f$frame.png"
        [IO.File]::WriteAllBytes($ppmPath, $ppm)
        & "C:\Program Files\ImageMagick-7.1.1-Q16-HDRI\magick.exe" $ppmPath $pngPath
        if ($LASTEXITCODE -ne 0) { throw "ImageMagick failed with $LASTEXITCODE" }
        return $pngPath
    }

    # Match runtime.cpp exactly: demo_last_frame starts invalid, but the first
    # generated dispatch runs until the first VBlank yield before the runtime
    # observes frame_count=0 and applies profile(0). Therefore boot-to-first-
    # VBlank has released keys; profile(N) drives the following frame interval.
    $preroll = Invoke-Oracle '{"cmd":"run_frames","n":1,"keyinput":1023}'
    if (-not $preroll.Contains('"ok":true')) { throw $preroll }
    [long]$current = 0
    Run-To ([math]::Max(0, $Frames - 1)) ([ref]$current)
    $before = Save-Screenshot $current
    Run-To $Frames ([ref]$current)
    $at = Save-Screenshot $current
    Run-To ($Frames + 1) ([ref]$current)
    $after = Save-Screenshot $current
    $registers = Invoke-Oracle '{"cmd":"registers"}'
    [void](Invoke-Oracle '{"cmd":"quit"}')
    [void]$process.WaitForExit(5000)

    Write-Output "RUN_DIR=$runDir"
    Write-Output "PNG_BEFORE=$before"
    Write-Output "PNG_AT=$at"
    Write-Output "PNG_AFTER=$after"
    Write-Output "REGISTERS=$registers"
} finally {
    if ($null -ne $writer) { $writer.Dispose() }
    if ($null -ne $reader) { $reader.Dispose() }
    if ($null -ne $client) { $client.Dispose() }
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    $process.WaitForExit()
    Write-Output "ORACLE_EXIT=$($process.ExitCode)"
    if (Test-Path $stderr) { Get-Content $stderr }
}
