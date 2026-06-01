# Custom Logo Patching

Scripts to inject the Eurobeat custom logo into Forza Horizon 6's radio station assets.

## Files

- **Horizon_Opus2.dds** - Custom logo, standard resolution (247×115)
- **Horizon_Opus3.dds** - Custom logo, HiRes resolution (494×230)
- **apply_logos.py** - Patch standard Anthem.zip with custom logo
- **apply_logos_hires.py** - Patch HiRes/Anthem.zip with custom logo
- **texconv.exe** - Microsoft DirectXTex tool for DDS manipulation

## Usage

### Patch Standard Resolution

```bash
python3 apply_logos.py
```

Updates: `E:/Steam/steamapps/common/ForzaHorizon6/media/UI/Textures/Anthem.zip`

### Patch HiRes Resolution

```bash
python3 apply_logos_hires.py
```

Updates: `E:/Steam/steamapps/common/ForzaHorizon6/media/UI/Textures/HiRes/Anthem.zip`

## How It Works

1. **Extract BC7 payload** from the DDS file
2. **Calculate mip0 size** for the texture dimensions
3. **Patch the swatchbin** with new BC7 data and updated headers
4. **Rewrite the ZIP** using Deflate compression (preserving original format)
5. **Backup** the original ZIP before modifying

## Backup & Restore

Original ZIPs are backed up as `Anthem.zip.py_backup` and `HiRes/Anthem.zip.py_backup`.

To restore:
```bash
cp E:/Steam/steamapps/common/ForzaHorizon6/media/UI/Textures/Anthem.zip.py_backup E:/Steam/steamapps/common/ForzaHorizon6/media/UI/Textures/Anthem.zip
cp E:/Steam/steamapps/common/ForzaHorizon6/media/UI/Textures/HiRes/Anthem.zip.py_backup E:/Steam/steamapps/common/ForzaHorizon6/media/UI/Textures/HiRes/Anthem.zip
```

Or verify game files through Steam to restore originals.

## Requirements

- Python 3.6+
- FH6 game directory with original Anthem.zip files

## Technical Details

Uses BC7 UNORM compression (same as game assets) with Deflate ZIP compression. Based on techniques from [FH6-Custom-RaDX](https://github.com/DrQuarters/FH6-Custom-RaDX).
