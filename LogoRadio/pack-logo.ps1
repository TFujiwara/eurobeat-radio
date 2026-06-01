<#
.SYNOPSIS  Construye fh6-radio\custom\Anthem.zip con el logo de Horizon Opus reemplazado.
           El ZIP preserva el orden y las posiciones de todas las entradas para que
           el juego no crashee. Requiere texconv.exe en la misma carpeta o en PATH.
.PARAMETER InputFile   PNG/BMP/TGA o DDS ya en BC3. Solo se usa a 247x115 (standard).
.PARAMETER GamePath    Raiz de FH6. Default: E:\Steam\steamapps\common\ForzaHorizon6
.PARAMETER StationLogo Entrada a reemplazar. Default: Horizon_Opus
.EXAMPLE   .\pack-logo.ps1 -InputFile eurobeat.png
#>
param(
    [Parameter(Mandatory)][string]$InputFile,
    [string]$GamePath    = "E:\Steam\steamapps\common\ForzaHorizon6",
    [string]$StationLogo = "Horizon_Opus"
)
$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
public static class Crc32 {
    static readonly uint[] T;
    static Crc32() {
        T = new uint[256];
        for (uint i = 0; i < 256; i++) {
            uint c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            T[i] = c;
        }
    }
    public static uint Compute(byte[] data) {
        uint crc = 0xFFFFFFFFu;
        foreach (byte b in data) crc = T[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }
}
"@

# ---- Build swatchbin (247x115, BC3) ----------------------------------------
function ConvertTo-Swatchbin([string]$Src) {
    $tc = Get-Command texconv -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    if (-not $tc) { $tc = Join-Path $PSScriptRoot "texconv.exe" }
    if (-not (Test-Path $tc)) { Write-Error "texconv.exe no encontrado." }

    $ext = [IO.Path]::GetExtension($Src).ToLower()
    $ddsPath = $Src
    if ($ext -ne ".dds") {
        $tmp  = [IO.Path]::GetTempPath()
        $stem = [IO.Path]::GetFileNameWithoutExtension($Src)
        Write-Host "  texconv -> 247x115 BC3..."
        $out = & $tc -nologo -y -f BC3_UNORM -m 1 -w 247 -h 115 -o $tmp $Src 2>&1
        if ($LASTEXITCODE -ne 0) { $out|%{Write-Host "  $_"}; Write-Error "texconv fallo" }
        $ddsPath = Join-Path $tmp "$stem.dds"
    }

    $dds = [IO.File]::ReadAllBytes($ddsPath)
    $fcc = [Text.Encoding]::ASCII.GetString($dds[84..87])
    $off = if ($fcc -eq "DX10") { 148 } else { 128 }
    $ds  = [uint32]([Math]::Ceiling(247/4)*[Math]::Ceiling(115/4)*16)
    $tex = $dds[$off..([int]($off+$ds-1))]

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
    $p = [BitConverter]::GetBytes([uint32]247);         [Array]::Copy($p,0,$hdr,0x4C,4)
    $p = [BitConverter]::GetBytes([uint32]115);         [Array]::Copy($p,0,$hdr,0x50,4)
    $p = [BitConverter]::GetBytes($ds);                [Array]::Copy($p,0,$hdr,0x80,4)
    [byte[]]$sb = $hdr + $tex
    Write-Host "  swatchbin: $($sb.Length) bytes  (${247}x${115} BC3)"
    return $sb
}

# ---- Compress with Deflate -------------------------------------------------
function Compress-Deflate([byte[]]$data) {
    $ms  = New-Object IO.MemoryStream
    $def = New-Object IO.Compression.DeflateStream($ms, [IO.Compression.CompressionLevel]::Optimal)
    $def.Write($data, 0, $data.Length)
    $def.Dispose()
    return $ms.ToArray()
}

# ---- Build order-preserving modified ZIP -----------------------------------
# Copies all local-header+data blocks verbatim except for the target entry,
# then rebuilds the central directory with updated offsets and CRC.
function Build-ModifiedZip([string]$SrcZip, [string]$EntryName, [byte[]]$NewUncompressed, [string]$DstZip) {
    $raw     = [IO.File]::ReadAllBytes($SrcZip)
    $nameB   = [Text.Encoding]::UTF8.GetBytes($EntryName)
    $newComp = Compress-Deflate $NewUncompressed
    $newCRC  = [Crc32]::Compute($NewUncompressed)
    $newCompSz = [uint32]$newComp.Length
    Write-Host "  Compressed: $($newComp.Length) bytes  (original: 6773)"

    # -- Parse EOCD ----------------------------------------------------------
    $eocdOff = -1
    for ($i = $raw.Length-22; $i -ge [Math]::Max(0,$raw.Length-65557); $i--) {
        if ($raw[$i] -eq 0x50 -and $raw[$i+1] -eq 0x4B -and
            $raw[$i+2] -eq 0x05 -and $raw[$i+3] -eq 0x06) { $eocdOff=$i; break }
    }
    if ($eocdOff -lt 0) { Write-Error "EOCD not found" }

    $cdOff   = [int][BitConverter]::ToUInt32($raw, $eocdOff+16)
    $cdCount = [int][BitConverter]::ToUInt16($raw, $eocdOff+10)

    # -- Find target entry in central directory ------------------------------
    $targetLocalOff = -1; $targetCdOff = -1; $targetOrigCompSz = 0
    $pos = $cdOff
    for ($n = 0; $n -lt $cdCount; $n++) {
        $fnLen = [int][BitConverter]::ToUInt16($raw, $pos+28)
        $exLen = [int][BitConverter]::ToUInt16($raw, $pos+30)
        $cmLen = [int][BitConverter]::ToUInt16($raw, $pos+32)
        if ($fnLen -eq $nameB.Length) {
            $fn = [Text.Encoding]::UTF8.GetString($raw, $pos+46, $fnLen)
            if ($fn -eq $EntryName) {
                $targetLocalOff   = [int][BitConverter]::ToUInt32($raw, $pos+42)
                $targetCdOff      = $pos
                $targetOrigCompSz = [int][BitConverter]::ToUInt32($raw, $pos+20)
            }
        }
        $pos += 46+$fnLen+$exLen+$cmLen
    }
    if ($targetLocalOff -lt 0) { Write-Error "Entry '$EntryName' not found" }

    # Local header details
    $tFnLen  = [int][BitConverter]::ToUInt16($raw, $targetLocalOff+26)
    $tExLen  = [int][BitConverter]::ToUInt16($raw, $targetLocalOff+28)
    $dataStart = $targetLocalOff + 30 + $tFnLen + $tExLen
    $dataEnd   = $dataStart + $targetOrigCompSz
    $sizeDiff  = $newComp.Length - $targetOrigCompSz

    # -- Build new ZIP -------------------------------------------------------
    $out = New-Object IO.MemoryStream

    # 1. Everything before our entry: verbatim
    $out.Write($raw, 0, $targetLocalOff)

    # 2. Our new local header (copy original header, patch compSz + CRC)
    $lhLen     = 30 + $tFnLen + $tExLen
    $newLH     = $raw[$targetLocalOff..($targetLocalOff+$lhLen-1)].Clone()
    $p = [BitConverter]::GetBytes($newCRC);    [Array]::Copy($p,0,$newLH,14,4)
    $p = [BitConverter]::GetBytes($newCompSz); [Array]::Copy($p,0,$newLH,18,4)
    $out.Write($newLH, 0, $newLH.Length)
    $out.Write($newComp, 0, $newComp.Length)

    # 3. Entries after ours up to CD: verbatim (they stay in same sequential order)
    $afterLen = $cdOff - $dataEnd
    if ($afterLen -gt 0) { $out.Write($raw, $dataEnd, $afterLen) }

    # 4. Rebuild central directory with updated offsets
    $newCdOff = [uint32]($out.Position)  # CD now starts here
    $pos = $cdOff
    for ($n = 0; $n -lt $cdCount; $n++) {
        $fnLen = [int][BitConverter]::ToUInt16($raw, $pos+28)
        $exLen = [int][BitConverter]::ToUInt16($raw, $pos+30)
        $cmLen = [int][BitConverter]::ToUInt16($raw, $pos+32)
        $cdEntryLen = 46+$fnLen+$exLen+$cmLen
        $cd = $raw[$pos..($pos+$cdEntryLen-1)].Clone()

        $localOff = [int][BitConverter]::ToUInt32($cd, 42)
        if ($pos -eq $targetCdOff) {
            # Update our entry's CRC and compSz
            $p = [BitConverter]::GetBytes($newCRC);    [Array]::Copy($p,0,$cd,16,4)
            $p = [BitConverter]::GetBytes($newCompSz); [Array]::Copy($p,0,$cd,20,4)
            # Our local offset is unchanged
        } elseif ($localOff -gt $targetLocalOff) {
            # Shift offset for all entries that come AFTER ours in the file
            $p = [BitConverter]::GetBytes([uint32]($localOff+$sizeDiff))
            [Array]::Copy($p, 0, $cd, 42, 4)
        }
        $out.Write($cd, 0, $cdEntryLen)
        $pos += $cdEntryLen
    }

    # 5. New EOCD (only CD offset changes, count and size stay the same)
    $eocd = $raw[$eocdOff..($raw.Length-1)].Clone()
    $p = [BitConverter]::GetBytes($newCdOff); [Array]::Copy($p,0,$eocd,16,4)
    $out.Write($eocd, 0, $eocd.Length)

    $result = $out.ToArray(); $out.Dispose()
    [IO.File]::WriteAllBytes($DstZip, $result)
    Write-Host "  Escrito: $DstZip ($($result.Length) bytes,  delta=$sizeDiff bytes)"
}

# ---- Main ------------------------------------------------------------------
$src        = Resolve-Path $InputFile | Select-Object -ExpandProperty Path
$srcZip     = "$GamePath\media\UI\Textures\Anthem.zip"
$entryName  = "HUD/RadioLogos/$StationLogo.swatchbin"
$customDir  = "$GamePath\fh6-radio\custom"
$dstZip     = "$customDir\Anthem.zip"

if (-not (Test-Path $srcZip)) { Write-Error "No encontrado: $srcZip" }
New-Item -ItemType Directory -Force $customDir | Out-Null

Write-Host "`n[Standard 247x115] $srcZip"
$swatchbin = ConvertTo-Swatchbin $src
Build-ModifiedZip $srcZip $entryName $swatchbin $dstZip

Write-Host "`nListo. El DLL cargara fh6-radio\custom\Anthem.zip automaticamente."
Write-Host "Rebuild la DLL si aun no lo has hecho y copia dist\version.dll al juego."
Write-Host "Despues reinicia FH6 completamente."