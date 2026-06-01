# Eurobeat Radio - Installation Script
# Simply run this script and follow the prompts

Write-Host "======================================" -ForegroundColor Cyan
Write-Host "  Eurobeat Radio for Forza Horizon 6" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan
Write-Host ""

# Find FH6 installation
Write-Host "Looking for Forza Horizon 6..." -ForegroundColor Yellow

$possiblePaths = @(
    "E:\Steam\steamapps\common\ForzaHorizon6",
    "C:\Program Files (x86)\Steam\steamapps\common\ForzaHorizon6",
    "D:\SteamLibrary\steamapps\common\ForzaHorizon6"
)

$gameDir = $null
foreach ($path in $possiblePaths) {
    if (Test-Path "$path\forzahorizon6.exe") {
        $gameDir = $path
        break
    }
}

if (-not $gameDir) {
    Write-Host "Could not automatically find FH6." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Choose how to locate the game:" -ForegroundColor Cyan
    Write-Host "1. Manual path entry" -ForegroundColor White
    Write-Host "2. Browse folder" -ForegroundColor White
    $choice = Read-Host "Enter your choice (1 or 2)"

    if ($choice -eq "2") {
        # Open folder browser dialog
        Add-Type -AssemblyName System.Windows.Forms
        $folderBrowser = New-Object System.Windows.Forms.FolderBrowserDialog
        $folderBrowser.Description = "Select your Forza Horizon 6 installation folder"
        $folderBrowser.RootFolder = [System.Environment+SpecialFolder]::MyComputer

        if ($folderBrowser.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            $gameDir = $folderBrowser.SelectedPath
        } else {
            Write-Host "Cancelled." -ForegroundColor Red
            Read-Host "Press Enter to exit"
            exit 1
        }
    } else {
        Write-Host "Enter the path to your FH6 installation:" -ForegroundColor Yellow
        Write-Host "Example: E:\Steam\steamapps\common\ForzaHorizon6" -ForegroundColor Gray
        $gameDir = Read-Host "FH6 Installation Path"
    }

    if (-not (Test-Path "$gameDir\forzahorizon6.exe")) {
        Write-Host "ERROR: forzahorizon6.exe not found at $gameDir" -ForegroundColor Red
        Read-Host "Press Enter to exit"
        exit 1
    }
}

Write-Host "Found FH6 at: $gameDir" -ForegroundColor Green
Write-Host ""

# Close FH6 if running
$fh6Process = Get-Process "forzahorizon6" -ErrorAction SilentlyContinue
if ($fh6Process) {
    Write-Host "Closing Forza Horizon 6..." -ForegroundColor Yellow
    Stop-Process -InputObject $fh6Process -Force
    Start-Sleep -Seconds 2
}

# Install files
Write-Host "Installing Eurobeat Radio..." -ForegroundColor Yellow

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceDir = Join-Path $scriptDir "dist"

if (-not (Test-Path $sourceDir)) {
    Write-Host "ERROR: 'dist' folder not found. Ensure this script is run from the release folder." -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

# Copy version.dll
Copy-Item "$sourceDir\version.dll" "$gameDir\version.dll" -Force
Write-Host "✓ Copied version.dll" -ForegroundColor Green

# Copy media folder if it exists
if (Test-Path "$sourceDir\media") {
    Copy-Item "$sourceDir\media\*" "$gameDir\media\" -Recurse -Force
    Write-Host "✓ Copied media files" -ForegroundColor Green
}

# Create fh6-radio directory if needed
$radioDir = "$gameDir\fh6-radio"
if (-not (Test-Path $radioDir)) {
    New-Item -ItemType Directory -Path $radioDir -Force | Out-Null
    Write-Host "✓ Created fh6-radio directory" -ForegroundColor Green
}

# Copy UI files if provided
$uiSource = Join-Path $scriptDir "ui"
if (Test-Path $uiSource) {
    Copy-Item "$uiSource\*" "$radioDir\ui\" -Recurse -Force
    Write-Host "✓ Copied dashboard UI" -ForegroundColor Green
}

# Apply custom Eurobeat logo automatically
Write-Host ""
Write-Host "Applying Eurobeat logo..." -ForegroundColor Yellow

$logoDir = Join-Path $scriptDir "LogoRadio"
if (Test-Path $logoDir) {
    # Check if Python is available
    $python = $null
    try {
        $python = (Get-Command python -ErrorAction Stop).Source
    } catch {
        try {
            $python = (Get-Command python3 -ErrorAction Stop).Source
        } catch {
            Write-Host "⚠ Python not found. Logo will not be applied." -ForegroundColor Yellow
            Write-Host "   Install Python or run LogoRadio scripts manually:" -ForegroundColor Yellow
            Write-Host "   cd LogoRadio && python3 apply_logos.py" -ForegroundColor Gray
        }
    }

    if ($python) {
        # Run logo patcher for standard resolution
        Write-Host "Applying standard resolution logo..." -ForegroundColor Cyan
        & $python "$logoDir\apply_logos.py" 2>$null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "✓ Standard logo applied" -ForegroundColor Green
        } else {
            Write-Host "⚠ Standard logo application failed" -ForegroundColor Yellow
        }

        # Run logo patcher for HiRes resolution
        Write-Host "Applying HiRes resolution logo..." -ForegroundColor Cyan
        & $python "$logoDir\apply_logos_hires.py" 2>$null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "✓ HiRes logo applied" -ForegroundColor Green
        } else {
            Write-Host "⚠ HiRes logo application failed" -ForegroundColor Yellow
        }
    }
} else {
    Write-Host "⚠ LogoRadio folder not found. Logo scripts not available." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "======================================" -ForegroundColor Green
Write-Host "  Installation Complete!" -ForegroundColor Green
Write-Host "======================================" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "1. Launch Forza Horizon 6" -ForegroundColor White
Write-Host "2. Tune to the Eurobeat station (R9)" -ForegroundColor White
Write-Host "3. Open http://localhost:8420 in your browser" -ForegroundColor White
Write-Host ""
Write-Host "Dashboard will open when the bridge connects." -ForegroundColor Yellow
Write-Host "The Eurobeat logo should appear in the radio station display." -ForegroundColor Yellow
Write-Host ""

Read-Host "Press Enter to exit"
