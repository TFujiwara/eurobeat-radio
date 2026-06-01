# Creating a Release

Guide for building and releasing Eurobeat Radio.

## Prerequisites

- Visual Studio 2022+ with C++ Desktop development
- The project must be built successfully

## Build Steps

1. **Build the project:**
   ```powershell
   cd eurobeat-radio
   .\scripts\build.ps1
   ```

2. **Fetch media assets:**
   ```powershell
   .\scripts\fetch-media.ps1 -Source "path/to/radio-mod.zip"
   ```

3. **Stage for release:**
   ```powershell
   .\scripts\install.ps1 -GameDir "C:\temp\fh6-test"  # Test installation
   ```

## Creating the Release ZIP

The release should contain:

```
eurobeat-radio.zip
├── install.ps1              (installation script)
├── dist/
│   ├── version.dll          (compiled mod DLL)
│   └── media/               (game assets)
├── LogoRadio/
│   ├── apply_logos.py       (standard logo patcher)
│   ├── apply_logos_hires.py (HiRes logo patcher)
│   ├── Horizon_Opus2.dds    (standard logo)
│   ├── Horizon_Opus3.dds    (HiRes logo)
│   └── README.md            (logo instructions)
├── README.md                (main readme)
└── UNINSTALL.md            (uninstall instructions)
```

## GitHub Release Steps

1. **Create git tag:**
   ```bash
   git tag -a v1.0.0 -m "Release version 1.0.0: Eurobeat Radio for FH6"
   git push origin v1.0.0
   ```

2. **Create Release on GitHub:**
   - Go to [Releases](https://github.com/TFujiwara/eurobeat-radio/releases) → **Draft a new release**
   - **Tag:** `v1.0.0`
   - **Title:** `Eurobeat Radio v1.0.0`
   - **Description:**
     ```
     ## Features
     - Eurobeat streaming from laut.fm
     - Custom Eurobeat logo (standard + HiRes)
     - Web dashboard at http://localhost:8420
     - Simple one-click PowerShell installer

     ## Installation
     1. Download eurobeat-radio.zip
     2. Right-click install.ps1 → Run with PowerShell
     3. Launch FH6 and tune to R9

     ## What's New
     [List of changes from previous release]
     ```

3. **Upload Assets:**
   - Click **Attach binaries**
   - Select `eurobeat-radio.zip`

4. **Publish:** Click **Publish release**

## Version Numbering

Use semantic versioning:
- **v1.0.0** - Major: Breaking changes or major features
- **v1.1.0** - Minor: New features, backwards compatible
- **v1.0.1** - Patch: Bug fixes

## Testing Before Release

Before creating a release:

1. Test the installer on a clean machine
2. Verify the dashboard works at localhost:8420
3. Test logo patching with both scripts
4. Verify uninstall/reinstall works cleanly

## Rollback

If a release has critical issues:

1. Delete the release (not the tag) on GitHub
2. Fix the issue
3. Create a new tag with a patch version bump
4. Create a new release

## Distribution

### GitHub Releases (Primary)
- Users download and run installer
- Works for all game installations

### Nexus Mods (Optional)
- Upload to [Nexus Mods - FH6](https://www.nexusmods.com/forzahorizon6/)
- One-click installer for convenience
- Reaches larger audience
