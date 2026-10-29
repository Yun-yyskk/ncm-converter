<#
    Renders the window to docs\preview.png without an interactive desktop session.

    Uses the app's --ui-snapshot test hook, which lays out the window, renders it
    through RenderTargetBitmap, dumps the visual tree and exits.

    Usage:
        powershell -ExecutionPolicy Bypass -File tools\screenshot.ps1 [-Configuration Release] [-File <sample.ncm>]
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$File
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root ('bin\x64\' + $Configuration + '\NcmConverter.exe')
if (-not (Test-Path $exe)) { throw "NcmConverter.exe not found: $exe (build it first)" }

$outDir = Join-Path $root 'docs\snapshot'
if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$arguments = '--ui-snapshot "' + $outDir + '"'
if ($File) { $arguments += ' "' + (Resolve-Path $File).Path + '"' }

$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $exe
$start.Arguments = $arguments
$start.UseShellExecute = $false
# A GUI child left on the console pipeline would keep this script waiting forever.
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [System.Diagnostics.Process]::Start($start)
if (-not $process.WaitForExit(30000)) {
    $process.Kill()
    throw 'Snapshot timed out after 30s'
}

if ($process.ExitCode -ne 0) {
    $status = Get-Content (Join-Path $outDir 'ui-status.txt') -Raw -ErrorAction SilentlyContinue
    throw ('Snapshot failed (exit ' + $process.ExitCode + ') ' + $status)
}

$png = Join-Path $outDir 'ui.png'
if (-not (Test-Path $png)) { throw "No snapshot produced: $png" }
Copy-Item $png (Join-Path $root 'docs\preview.png') -Force
Write-Host ('OK -> docs\preview.png (' + (Get-Item $png).Length + ' bytes, tree: docs\snapshot\ui-tree.txt)')
