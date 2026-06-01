param(
    [Parameter(Mandatory)][string]$InputFile,
    [string]$GamePath    = "E:\Steam\steamapps\common\ForzaHorizon6",
    [string]$StationLogo = "Horizon_Opus"
)
$ErrorActionPreference = "Stop"

function ConvertTo-DDS([string]$Src, [int]$W, [int]$H) {
    $tc = Get-Command texconv -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    if (-not $tc) { $tc = Join-Path $PSScriptRoot "texconv.exe" }
    if (-not (Test-Path $tc)) { Write-Error "texconv.exe no encontrado." }
    $tmp  = [IO.Path]::GetTempPath()
    $stem = [IO.Path]::GetFileNameWithoutExtension($Src)
    Write-Host "  texconv -> ${W}x${H} BC3..."
    $out = & $tc -nologo -y -f BC3_UNORM -m 1 -w $W -h $H -o $tmp $Src 2>&1
    if ($LASTEXITCODE -ne 0) { $out | ForEach-Object { Write-Host "  $_" }; Write-Error "texconv fallo" }
    return Join-Path $tmp "$stem.dds"
}

function Build-Swatchbin([string]$DdsPath) {
    $dds = [IO.File]::ReadAllBytes($DdsPath)
    $dH  = [BitConverter]::ToUInt32($dds, 12)
    $dW  = [BitConverter]::ToUInt32($dds, 16)
    $fcc = [Text.Encoding]::ASCII.GetString($dds[84..87])
    $off = if ($fcc -eq "DX10") { 148 } else { 128 }
    $wB  = [Math]::Ceiling($dW / 4)
    $hB  = [Math]::Ceiling($dH / 4)
    $ds  = [uint32]($wB * $hB * 16)
    $tex = $dds[$off..([int]($off + $ds - 1))]
    Write-Host "  ${dW}x${dH} BC3  data=$ds  swatchbin=$($ds+140)"
    [byte[]]$hdr = @(
        0x62,0x75,0x72,0x47,0x01,0x01,0x00,0x00,0x8C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x01,0x00,0x00,0x00,0x42,0x43,0x58,0x54,0x00,0x00,0x01,0x00,0x2C,0x00,0x00,0x00,
        0x8C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x48,0x43,0x58,0x54,
        0x80,0x05,0x08,0x00,0x38,0x00,0x00,0x00,0x50,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x01,0x06,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,
        0x44,0x00,0x00,0x00,0x09,0x00,0x00,0x00,0x4C,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF
    )
    $p = [BitConverter]::GetBytes([uint32](0x8C+$ds)); [Array]::Copy($p,0,$hdr,0x0C,4)
    $p = [BitConverter]::GetBytes($ds);                [Array]::Copy($p,0,$hdr,0x24,4)
    $p = [BitConverter]::GetBytes($ds);                [Array]::Copy($p,0,$hdr,0x28,4)
    $p = [BitConverter]::GetBytes($dW);                [Array]::Copy($p,0,$hdr,0x4C,4)
    $p = [BitConverter]::GetBytes($dH);                [Array]::Copy($p,0,$hdr,0x50,4)
    $p = [BitConverter]::GetBytes($ds);                [Array]::Copy($p,0,$hdr,0x80,4)
    return [byte[]]($hdr + $tex)
}

$src       = Resolve-Path $InputFile | Select-Object -ExpandProperty Path
$isDDS     = [IO.Path]::GetExtension($src).ToLower() -eq ".dds"
$entryName = "HUD/RadioLogos/$StationLogo.swatchbin"
$zipPath   = "$GamePath\media\UI\Textures\HiRes\Anthem.zip"

Write-Host "[HiRes only - 494x230] $zipPath"
$dds       = if ($isDDS) { $src } else { ConvertTo-DDS $src 494 230 }
$swatchbin = Build-Swatchbin $dds

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, 2)
try {
    $old = $zip.Entries | Where-Object { $_.FullName -eq $entryName }
    if ($old) { $old.Delete() }
    $entry  = $zip.CreateEntry($entryName, 0)
    $stream = $entry.Open()
    $stream.Write($swatchbin, 0, $swatchbin.Length)
    $stream.Dispose()
} finally { $zip.Dispose() }

# Verify
$z2 = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
$e2 = $z2.Entries | Where-Object { $_.FullName -eq $entryName }
Write-Host "  Verificado: size=$($e2.Length) bytes"
$z2.Dispose()

Write-Host "`nListo. Cierra FH6 completamente y reinicia."
Write-Host "Si funciona sin crash -> el problema era modificar Anthem.zip estandar."