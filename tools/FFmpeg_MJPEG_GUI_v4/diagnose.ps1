$ErrorActionPreference = "Continue"
Write-Host "=== FFmpeg MJPEG GUI v4 environment check ===" -ForegroundColor Cyan
Write-Host "PowerShell: $($PSVersionTable.PSVersion)"
Write-Host "OS: $([Environment]::OSVersion.VersionString)"
Write-Host ""

try {
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
    Add-Type -AssemblyName System.Drawing -ErrorAction Stop
    Write-Host "[OK] WinForms" -ForegroundColor Green
} catch {
    Write-Host "[FAIL] WinForms: $($_.Exception.Message)" -ForegroundColor Red
}

$ff = Get-Command ffmpeg -ErrorAction SilentlyContinue
$fp = Get-Command ffprobe -ErrorAction SilentlyContinue

if ($ff) {
    Write-Host "[OK] ffmpeg: $($ff.Source)" -ForegroundColor Green
    if (-not $fp) {
        $candidate = Join-Path (Split-Path -Parent $ff.Source) "ffprobe.exe"
        if (Test-Path -LiteralPath $candidate) {
            Write-Host "[OK] ffprobe beside ffmpeg: $candidate" -ForegroundColor Green
            $fp = $candidate
        }
    }
} else {
    Write-Host "[FAIL] ffmpeg not found" -ForegroundColor Red
}

if ($fp -and $fp.Source) {
    Write-Host "[OK] ffprobe PATH: $($fp.Source)" -ForegroundColor Green
} elseif (-not $fp) {
    Write-Host "[FAIL] ffprobe not found" -ForegroundColor Red
}

Write-Host ""
Read-Host "Press Enter to close"
