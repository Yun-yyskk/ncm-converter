<#
    Build script for NcmConverter (WinUI 3 / C++).

    Usage:
        powershell -ExecutionPolicy Bypass -File build.ps1 [-Configuration Release] [-Clean]
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$vsRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools'
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'

if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }
if (-not (Test-Path $msbuild)) { throw "MSBuild.exe not found: $msbuild" }

$project = Join-Path $root 'NcmConverter.vcxproj'
$targets = if ($Clean) { 'Clean;Build' } else { 'Build' }

$cmd = '"' + $vcvars + '" >nul && "' + $msbuild + '" "' + $project + '" /t:' + $targets + ' /p:Configuration=' + $Configuration + ' /p:Platform=x64 /m /nologo /v:minimal'
Write-Host ">> $cmd"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

$exe = Join-Path $root ("bin\x64\" + $Configuration + "\NcmConverter.exe")
if (Test-Path $exe) { Write-Host "OK -> $exe" } else { throw "Build did not produce $exe" }
