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
if (-not (Test-Path $exe)) { throw "Build did not produce $exe" }

# The icon is compiled into the exe, but the window itself is pointed at the
# file, so it has to travel next to the executable as well.
$icon = Join-Path $root 'res\app.ico'
if (Test-Path $icon) { Copy-Item $icon (Join-Path (Split-Path -Parent $exe) 'app.ico') -Force }

# The project links against the dynamic CRT, so a plain copy of bin\x64\<cfg>
# fails to start on a machine without the Visual C++ redistributable. Ship the
# runtime next to the executable instead of asking the user to install it.
$crt = Get-ChildItem (Join-Path $vsRoot 'VC\Redist\MSVC') -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter 'x64' -ErrorAction SilentlyContinue } |
    ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if ($crt) {
    $binDir = Split-Path -Parent $exe
    Get-ChildItem $crt.FullName -File -Filter '*.dll' | Copy-Item -Destination $binDir -Force
    Write-Host ("CRT -> " + $crt.FullName)
} else {
    Write-Warning 'VC++ redistributable DLLs not found; the output folder will not be portable.'
}

Write-Host "OK -> $exe"
