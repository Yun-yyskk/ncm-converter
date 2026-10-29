<#
    Builds the portable distribution folder and zip for NcmConverter.

    Usage:
        powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-Configuration Release]
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$binDir = Join-Path $root ("bin\x64\" + $Configuration)
$exe = Join-Path $binDir 'NcmConverter.exe'
if (-not (Test-Path $exe)) { throw "Build first: $exe not found (run build.ps1)." }

$required = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$missing = $required | Where-Object { -not (Test-Path (Join-Path $binDir $_)) }
if ($missing) {
    throw ("The output folder is not portable, missing: " + ($missing -join ', ') + ". Re-run build.ps1 so the CRT DLLs get copied in.")
}

$dist = Join-Path $root 'dist'
$staging = Join-Path $dist 'NcmConverter-win-x64'
$zip = Join-Path $dist 'NcmConverter-win-x64.zip'
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
if (Test-Path $zip) { Remove-Item $zip -Force }
New-Item $staging -ItemType Directory -Force | Out-Null

Get-ChildItem $binDir -Force |
    Where-Object { $_.Extension -ne '.pdb' } |
    Copy-Item -Destination $staging -Recurse -Force

$readmePath = Join-Path $staging '使用说明.txt'
$readme = @"
NCM 转换 —— 便携版
==================

这是一个绿色免安装的小工具，把一个或多个 .ncm 文件还原成原始的
FLAC（无损）或 MP3，封面、歌词和标签会一并写进去。
音频不会被重新编码，所以音质与原文件完全一致。

怎么用
------
1. 双击 NcmConverter.exe 打开。
2. 点「添加文件」选择 .ncm 文件，或者直接把文件拖进窗口（支持多选）。
3. 输出位置默认是「源文件所在目录」，每个文件写回它自己的文件夹；
   也可以切到「指定目录」统一输出。
4. 点「开始转换」。目标文件已存在时会直接覆盖。

目录里别删的东西
----------------
* NcmConverter.exe、*.dll、*.pri、Microsoft.UI.Xaml 文件夹 —— 运行必需。
* 本说明和「解除锁定.cmd」可以删掉，不影响使用。

Windows 提示「已保护你的电脑」
------------------------------
这个提示来自 Microsoft Defender SmartScreen。文件只要带有「来自 Internet」的
标记（从浏览器下载、或用资源管理器解压 zip 都会带上）就会弹出，和程序本身
有没有问题无关。程序没有买数字签名证书，所以没法让它彻底消失，但可以一次性
去掉这个标记：

  * 最简单：右键 NcmConverter.exe -> 属性 -> 勾选「解除锁定」-> 确定。
  * 或者：双击本目录里的「解除锁定.cmd」，它会去掉整个文件夹的标记。
  * 从 U 盘、局域网共享拷贝过来，或者用 7-Zip / WinRAR 解压 zip，通常不会带上
    这个标记，也就不会弹窗。

系统要求
--------
* Windows 10 1809（17763）或更新的 64 位 Windows，Windows 11 也可以。
* 不需要安装 .NET、Visual C++ 运行库或 Windows App SDK，运行库已经随包附带。
"@
[System.IO.File]::WriteAllText($readmePath, $readme, (New-Object System.Text.UTF8Encoding($true)))

$unblockPath = Join-Path $staging '解除锁定.cmd'
$unblock = @"
@echo off
chcp 65001 >nul
echo 正在解除本文件夹里所有文件的「来自 Internet」标记...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath '%~dp0' -Recurse -File | Unblock-File"
echo 完成，现在可以双击 NcmConverter.exe 了。
pause
"@
[System.IO.File]::WriteAllText($unblockPath, $unblock, (New-Object System.Text.UTF8Encoding($false)))

if (-not (Test-Path $readmePath)) { throw "failed to write $readmePath" }

# Compress-Archive on Windows PowerShell writes backslash separators into the
# entry names, which some extractors then treat as part of the file name. Zip
# them by hand so the archive carries the standard forward-slash paths.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zipStream = [System.IO.File]::Open($zip, [System.IO.FileMode]::Create)
try {
    $archive = New-Object System.IO.Compression.ZipArchive($zipStream, [System.IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        Get-ChildItem $staging -Recurse -File | ForEach-Object {
            $relative = $_.FullName.Substring($staging.Length + 1).Replace('\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $_.FullName, $relative, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $archive.Dispose() }
} finally { $zipStream.Dispose() }
$sizeMb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
$count = (Get-ChildItem $staging -Recurse -File).Count
Write-Host ("folder -> " + $staging + " (" + $count + " files)")
Write-Host ("zip    -> " + $zip + " (" + $sizeMb + " MB)")
