# Building Eurobeat Radio

This is a specialized fork of [FH6 Universal Radio](https://github.com/g0ldyy/fh6-universal-radio) configured for Eurobeat streaming.

## Build Requirements

- **Windows 10/11**
- **Visual Studio 2022+** with Desktop C++ development
- **CMake** (bundled with VS 2022)
- **Git**

## Build Steps

1. **Clone the repository:**
   ```powershell
   git clone https://github.com/TFujiwara/eurobeat-radio.git
   cd eurobeat-radio
   ```

2. **Enter the eurobeat-radio directory:**
   ```powershell
   cd eurobeat-radio
   ```

3. **Install dependencies (one-time):**
   ```powershell
   .\scripts\get-deps.ps1
   ```

4. **Build the DLL:**
   ```powershell
   .\scripts\build.ps1
   ```

   The output `version.dll` will be in `dist/`

5. **For a full installation with media assets:**

   First, get the radio station overlay from any working FH6 radio mod (e.g., from Nexus Mods):

   ```powershell
   .\scripts\fetch-media.ps1 -Source "C:\path\to\radio-mod.zip"
   ```

   Then install to your game directory:
   ```powershell
   .\scripts\install.ps1 -GameDir "E:\Steam\steamapps\common\ForzaHorizon6"
   ```

## Project Structure

```
eurobeat-radio/
├── src/                    # C++ source code
│   ├── fmod/               # Audio DSP and FMOD bridge
│   ├── sources/            # Audio sources (internet radio, etc.)
│   ├── http/               # Web dashboard HTTP server
│   └── ...
├── include/                # C++ headers
├── ui/                      # Web dashboard HTML/CSS/JS
├── scripts/                # Build and install scripts
├── CMakeLists.txt          # Build configuration
└── LICENSE
```

## Logo Modification

Custom logo patching is handled via Python scripts in `../LogoRadio/`:

```powershell
cd ../LogoRadio
python3 apply_logos.py           # Update standard resolution logos
python3 apply_logos_hires.py     # Update HiRes resolution logos
```

These scripts inject BC7-compressed textures directly into `Anthem.zip` files.

## Troubleshooting

**Build fails with "CMake not found":**
- Ensure Visual Studio 2022+ is installed with C++ workload
- CMake is bundled; you may need to add it to PATH manually

**Missing header files:**
- Run `get-deps.ps1` again to ensure all dependencies are installed

**DLL won't load in game:**
- Check Windows Defender/antivirus isn't quarantining the DLL
- Verify you're using the correct game architecture (64-bit)
- Check `fh6-radio/bridge.log` for initialization errors

## Testing

After building, place the compiled `version.dll` in your FH6 game directory and launch the game. Dashboard should be accessible at `http://localhost:8420`.

## Contributing

For issues or improvements, see [CONTRIBUTING.md](CONTRIBUTING.md).
