$ErrorActionPreference = "Stop"
try {
    $scriptPath = Join-Path $PSScriptRoot "app.ps1"
    if (-not (Test-Path -LiteralPath $scriptPath)) {
        throw "Main script not found: $scriptPath"
    }

    Write-Host "Starting FFmpeg MJPEG GUI v4..." -ForegroundColor Cyan
    Write-Host "PowerShell: $($PSVersionTable.PSVersion)"
    Write-Host "Folder: $PSScriptRoot"

    $ff = Get-Command ffmpeg -ErrorAction SilentlyContinue
    $fp = Get-Command ffprobe -ErrorAction SilentlyContinue

    if ($ff) {
        Write-Host "FFmpeg: $($ff.Source)" -ForegroundColor Green
        if (-not $fp) {
            $candidate = Join-Path (Split-Path -Parent $ff.Source) "ffprobe.exe"
            if (Test-Path -LiteralPath $candidate) {
                $fp = Get-Item -LiteralPath $candidate
                Write-Host "FFprobe: $candidate (same folder)" -ForegroundColor Green
            }
        }
    } else {
        Write-Host "FFmpeg: NOT FOUND" -ForegroundColor Yellow
    }

    if ($fp -and $fp.Source) {
        Write-Host "FFprobe: $($fp.Source)" -ForegroundColor Green
    } elseif ($fp -and $fp.FullName) {
        # already printed same-folder path above
    } else {
        Write-Host "FFprobe: NOT FOUND" -ForegroundColor Yellow
    }

    & $scriptPath
}
catch {
    Write-Host ""
    Write-Host "STARTUP ERROR" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host ""
    Write-Host "DETAILS" -ForegroundColor Yellow
    Write-Host ($_ | Out-String)
    Write-Host ""
    Read-Host "Press Enter to close"
    exit 1
}
