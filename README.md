# NCM 转 FLAC

用 C++ / WinUI 3 写的小工具，把网易云音乐的 `.ncm` 加密容器还原成标准 FLAC —— **不重新编码**，
解出来的就是原始 FLAC 码流，做到逐字节无损，同时把封面、标签和歌词一起写回文件。

![界面预览](docs/preview.png)

## 功能

- **批量转换**：拖放文件到窗口任意位置，或用「添加文件」多选；支持全选 / 逐个移除
- **输出目录**：默认与源文件同目录，也可指定目录
- **覆盖开关**：目标文件已存在时默认跳过，打开开关后覆盖
- **一键打开目录**：转换完成后直接跳到输出位置
- **完整标签**：TITLE / ARTIST / ALBUM / ALBUMARTIST / TITLE_TRANSLATED / MUSIC_ID /
  COMMENT / LYRICS / ENCODER 顺序写入 VORBIS_COMMENT
- **封面嵌入**：把 NCM 内嵌的专辑图作为 PICTURE 块（front cover）写进 FLAC
- **歌词**：优先用 NCM 内的歌词（逐行 JSON、YRC 卡拉OK 两种格式都会转成标准 `[mm:ss.xx]` LRC），
  没有时回退到同名 `.lrc` 文件
- **命令行模式**：`NcmToFlac.exe --convert in.ncm [out.flac]`，结果写 `<out>.log`，便于批处理与回归测试

## 环境要求

| 项目 | 要求 |
| --- | --- |
| 系统 | Windows 10 1809 (10.0.17763) 及以上 |
| 编译器 | Visual Studio 的 C++ 生成工具（x64）；本仓库在 VS 2026 BuildTools / MSVC 14.51 / 工具集 v145 上验证 |
| SDK | Windows SDK 10.0.26100.0 |
| 依赖 | Windows App SDK 1.8（C++/WinRT）与 WebView2 的 WinRT 元数据包；不打包、自包含部署 |

## 构建

```powershell
# 1. 下载 Windows App SDK / C++/WinRT 等 NuGet 包（解压到 %USERPROFILE%\.nuget\packages）
powershell -ExecutionPolicy Bypass -File tools\fetch-packages.ps1

# 2. 编译
powershell -ExecutionPolicy Bypass -File build.ps1 -Configuration Release
```

产物：`bin\x64\Release\NcmToFlac.exe`，同目录带上 Windows App SDK 运行库 DLL，可整体拷走使用。

> 没有走 `msbuild -t:Restore`：Build Tools 里不带 NuGet.targets，所以由
> `tools\fetch-packages.ps1` 按 nuspec 递归抓包、解压到标准 NuGet 目录，
> vcxproj 里再用显式 `Import` 引用。换机器时改 vcxproj 顶部的 `PlatformToolset`、
> `WindowsTargetPlatformVersion` 与 `PackageVersions` 组即可。

## 使用

1. 运行 `NcmToFlac.exe`
2. 把 `.ncm` 文件拖进窗口，或点「添加文件」多选
3. 选输出位置（默认与源文件同目录）
4. 点「开始转换」，状态胶囊从「待转换」变成「完成」

## NCM 容器的解包流程

`.ncm` 是网易云音乐加过密的容器，内部本来就存着完整的 FLAC / MP3 码流，解密即可，不需要转码：

1. 文件头 `CTENFDAM` + 2 字节，随后是 `uint32` 的密钥长度与 `uint32` 的图片占位字段
2. **音频密钥**：密文逐字节 XOR `0x64` → AES-128-ECB 解密 → **去掉 PKCS#7 填充** →
   去掉 `neteasecloudmusic` 前缀，剩下 98 字节作为音频解密种子。
   最常见的坑就是忘了去填充：种子多出十几个字节，解出来的音频全是噪声
3. **元数据**：`uint32` 长度 → 逐字节 XOR `0x63` → 去前缀 `163 key(Don't modify):` →
   Base64 → AES-128-ECB 解密（另一把固定密钥）→ 去填充 → 去 `music:` 前缀 → JSON
4. 跳过 CRC32 / 版本号 / 封面长度字段，读封面图（JPEG / PNG / WebP / GIF），音频从封面之后开始
5. **音频**：用种子做 KSA 生成 256 字节置换盒，再按 RC4 变体逐字节异或。
   注意这里**解密时不能交换置换盒**（与标准 RC4 不同），否则前 4 个字节还能对上，从第 5 个字节起全错
6. 得到的 `fLaC` 码流即可直接使用

写文件时保留原始 `STREAMINFO`（含 PCM MD5），丢掉源文件自带的其他标签块，重新生成
`VORBIS_COMMENT` 与 `PICTURE`，再接上原始音频帧 —— 帧 CRC 与 PCM MD5 前后一致，
可以证明全程没有重编码。

## 校验

样例：26,636,421 字节的 `.ncm`，44.1 kHz / 立体声 / 16 bit / 03:25。

| 检查项 | 结果 |
| --- | --- |
| 输出 FLAC 大小 | 26,630,476 字节 |
| 解码 PCM MD5 | `9ce663edd3b08a683d0c2cd2602028ac`，与 `STREAMINFO` 记录一致 |
| 帧 CRC | 逐帧复核通过 |
| 完整解码 | 全程无错误 |

## 代码结构

| 路径 | 说明 |
| --- | --- |
| `src/core/aes.*` | BCrypt 的 AES-128-ECB（只解密，PKCS#7 由调用方剥离） |
| `src/core/base64.*` | Base64 解码（容忍换行与 URL-safe 变体） |
| `src/core/json.*` | 最小 JSON 解析器 |
| `src/core/ncm.*` | 容器解包：密钥、元数据、封面、音频解密 |
| `src/core/flac.*` | 解析 `STREAMINFO`、重写元数据块、读取封面尺寸 |
| `src/core/lyrics.*` | LRC / YRC / JSON 歌词统一成标准 LRC |
| `src/core/convert.*` | 单文件转换编排（进度回调、覆盖策略、错误文案） |
| `src/MainWindow.*` | WinUI 3 界面（纯 C++ 构建，无 XAML、无代码生成） |
| `src/App.cpp` | 启动、命令行模式、截图测试钩子 |
| `tools/fetch-packages.ps1` | 递归下载 Windows App SDK 依赖包 |
| `tools/screenshot.ps1` | 走 `--ui-snapshot` 渲染窗口到 `docs/preview.png` |

## 实现说明：界面为什么是"手搓控件"

目标机器上 WinUI 3（1.8）的 `ProgressBar`、`TextBox`、`InfoBar`、`ProgressRing`
渲染时 fail-fast 崩溃（`0xC000027B`，在 `Microsoft.UI.Xaml.dll` 内 `E_FAIL`），
而 `Border`、`Button`、`CheckBox`、`ScrollViewer`、`ComboBox`、`ToggleSwitch`、
`AcrylicBrush` 全都正常。

于是进度条、目录输入框、提示横幅都改成 `Border` + `Grid` 自绘（见 `MainWindow.cpp` 里的
`LiteBar`），颜色可控也顺便避免了系统主题渗色。界面本身同样是代码式构建
（`ApplicationT<App>` + `Window()` + 直接装配控件），完全绕开 XAML 编译器与 `XamlTypeInfo` 生成。

## 许可

仅供个人学习，以及处理自己合法获得的音乐文件。
