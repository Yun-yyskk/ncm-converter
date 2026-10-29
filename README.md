# NCM 转换

<img src="docs/icon.png" width="88" alt="应用图标">

一个 Windows 上的小工具：把 `.ncm` 文件还原成无损 **FLAC** 或 **MP3**，封面、标签和歌词
一并写入，全程不重新编码。

![界面预览](docs/preview.png)

## 用法

1. 从 [Releases](../../releases/latest) 下载 `NcmConverter-win-x64.zip`，解压后双击
   `NcmConverter.exe`（绿色免安装）
2. 把 `.ncm` 拖进窗口（支持多选），选好输出位置，点「开始转换」

- 输出格式跟着文件里的音频流走：无损流出 `.flac`，有损流出 `.mp3`
- 输出位置默认是每个文件自己的源目录，也可以统一指定；目标文件已存在时直接覆盖
- 浅色 / 深色跟随系统，窗口大小 640 × 360（按系统 DPI 缩放）

## 系统要求

Windows 10 1809（17763）及以上的 64 位系统。不需要安装 .NET、VC++ 运行库或
Windows App SDK —— 便携包里已经带全。

> 如果双击时提示「Windows 已保护你的电脑」，那是 SmartScreen 对带"来自 Internet"
> 标记的文件给出的提示：右键属性勾选「解除锁定」，或双击包里的 `解除锁定.cmd` 即可。

## 自己编译

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch-packages.ps1
powershell -ExecutionPolicy Bypass -File build.ps1 -Configuration Release
```

产物 `bin\x64\Release\NcmConverter.exe`，同目录带上运行库，整个文件夹可以拷到
别的机器上直接运行。

打包便携 zip：

```powershell
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Configuration Release
```

需要 Visual Studio 的 C++ 生成工具（x64）与 Windows SDK 10.0.26100.0；本仓库在
VS 2026 BuildTools / MSVC 14.51 / 工具集 v145 上验证。

## 许可

仅供个人使用，请自行确认对所使用的文件拥有合法权利。
