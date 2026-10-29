# NCM 转换

用 C++ / WinUI 3 写的小工具，把网易云音乐的 `.ncm` 加密容器还原成里面的原始码流 —— **不重新编码**：
容器里是 FLAC 就输出逐字节无损的 FLAC，是 MP3 就原样输出 MP3（补上 ID3 标签），
封面、标签和歌词一并写入。

![界面预览](docs/preview.png)

## 功能

- **两种输出**：FLAC 码流 → `.flac`（VORBIS_COMMENT + PICTURE）；MP3 码流 → `.mp3`（ID3v2.3 + APIC），
  扩展名由码流本身决定，不需要手动选
- **批量转换**：把 .ncm 拖进窗口任意位置，或用「添加文件」多选；支持全选 / 逐个移除
- **拖放提示**：文件拖到窗口上方时整窗铺一层浅蓝，中间浮出一张小卡片写着「松开即可添加」
  与本次拖进来的 .ncm 数量，鼠标指针同时变成"复制"样式
- **输出目录**：默认写回**每个文件自己的源目录**（文件来自不同文件夹时各归各位），
  也可统一指定一个目录
- **直接覆盖**：目标文件已存在时直接重写，不再多问一句
- **跟随系统主题**：浅色 / 深色跟随 Windows，强调色固定为 Windows 11 的默认蓝（浅色 #005FB8 / 深色 #60CDFF）；系统切换主题时
  窗口会实时重绘（包括标题栏）
- **一键打开目录**：转换完成后直接跳到输出位置（输出分散在多个目录时跳到它们的公共上层）
- **完整标签**：TITLE / ARTIST / ALBUM / ALBUMARTIST / TITLE_TRANSLATED / MUSIC_ID /
  COMMENT / LYRICS / ENCODER
- **封面嵌入**：把 NCM 内嵌的专辑图作为 FLAC 的 PICTURE 块（front cover）或 MP3 的 APIC 帧
- **歌词**：优先用 NCM 内的歌词（逐行 JSON、YRC 卡拉OK 两种格式都会转成标准 `[mm:ss.xx]` LRC），
  没有时回退到同名 `.lrc` 文件
- **命令行模式**：`NcmConverter.exe --convert in.ncm [out]`，结果写 `<输出>.log`，便于批处理与回归测试

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

产物：`bin\x64\Release\NcmConverter.exe`，同目录带上 Windows App SDK 运行库 DLL，可整体拷走使用。

> 没有走 `msbuild -t:Restore`：Build Tools 里不带 NuGet.targets，所以由
> `tools\fetch-packages.ps1` 按 nuspec 递归抓包、解压到标准 NuGet 目录，
> vcxproj 里再用显式 `Import` 引用。换机器时改 vcxproj 顶部的 `PlatformToolset`、
> `WindowsTargetPlatformVersion` 与 `PackageVersions` 组即可。

## 使用

1. 运行 `NcmConverter.exe`
2. 把 `.ncm` 文件拖进窗口，或点「添加文件」多选
3. 选输出位置（默认写回各文件自己的源目录，可改为统一目录）
4. 点「开始转换」，状态胶囊从「待转换」变成「完成」

窗口默认尺寸 640 × 360，会按系统 DPI 缩放。

## NCM 容器的解包流程

`.ncm` 是网易云音乐加过密的容器，内部本来就存着完整的 FLAC / MP3 码流，解密即可，不需要转码：

1. 文件头 `CTENFDAM` + 2 字节，随后是 `uint32` 的密钥长度
2. **音频密钥**：密文逐字节 XOR `0x64` → AES-128-ECB 解密 → **去掉 PKCS#7 填充** →
   去掉 `neteasecloudmusic` 前缀，剩下 98 字节作为音频解密种子。
   最常见的坑就是忘了去填充：种子多出十几个字节，解出来的音频全是噪声
3. **元数据**：`uint32` 长度 → 逐字节 XOR `0x63` → 去前缀 `163 key(Don't modify):` →
   Base64 → AES-128-ECB 解密（另一把固定密钥）→ 去填充 → 去 `music:` 前缀 → JSON
4. 跳过 CRC32 / 版本号 / 封面长度字段，读封面图（JPEG / PNG / WebP / GIF），音频从封面之后开始
5. **音频**：用种子做 KSA 生成 256 字节置换盒，再按 RC4 变体逐字节异或。
   注意这里**解密时不能交换置换盒**（与标准 RC4 不同），否则前 4 个字节还能对上，从第 5 个字节起全错
6. 解密结果以 `fLaC` 开头就是 FLAC，以 `ID3` 或 `0xFFEx` 帧同步开头就是 MP3；两者都不像就报不支持

写 FLAC 时保留原始 `STREAMINFO`（含 PCM MD5），丢掉源文件自带的其他标签块，重新生成
`VORBIS_COMMENT` 与 `PICTURE`，再接上原始音频帧 —— 帧 CRC 与 PCM MD5 前后一致，
可以证明全程没有重编码。

MP3 则剥掉容器里原有的 ID3v2 与结尾的 ID3v1，写一份新的 ID3v2.3（文本帧一律 UTF-16 + BOM），
音频帧原样拷贝 —— 解码 PCM 与源 MP3 逐字节相同。

## 校验

样例：26,636,421 字节的 `.ncm`，44.1 kHz / 立体声 / 16 bit / 03:25。

| 检查项 | 结果 |
| --- | --- |
| 输出 FLAC 大小 | 26,630,479 字节（`ENCODER` 字符串长度变化时会浮动几个字节，音频字节不受影响） |
| 解码 PCM MD5 | `9ce663edd3b08a683d0c2cd2602028ac`，与 `STREAMINFO` 记录一致 |
| 帧 CRC | 逐帧复核通过 |
| 完整解码 | 全程无错误 |

MP3 分支用一个"外壳按格式自造、内部塞真实 MP3"的合成容器验证：输出 `.mp3` 的
解码 PCM MD5 与源 MP3 相同，`ffmpeg` 能读出 title / artist / album / TIT3(译名) /
comment / USLT(歌词) / APIC(封面)。

## 代码结构

| 路径 | 说明 |
| --- | --- |
| `src/core/aes.*` | BCrypt 的 AES-128-ECB（只解密，PKCS#7 由调用方剥离） |
| `src/core/base64.*` | Base64 解码（容忍换行与 URL-safe 变体） |
| `src/core/json.*` | 最小 JSON 解析器 |
| `src/core/ncm.*` | 容器解包：密钥、元数据、封面、音频解密、码流类型识别 |
| `src/core/flac.*` | 解析 `STREAMINFO`、重写元数据块、读取封面尺寸 |
| `src/core/id3.*` | MP3 帧头探测 + ID3v2.3 标签写出 |
| `src/core/lyrics.*` | LRC / YRC / JSON 歌词统一成标准 LRC |
| `src/core/convert.*` | 单文件转换编排（码流决定输出格式、进度回调、覆盖策略、错误文案） |
| `src/MainWindow.*` | WinUI 3 界面（纯 C++ 构建，无 XAML、无代码生成） |
| `src/App.cpp` | 启动、命令行模式与两个开发钩子 |
| `tools/fetch-packages.ps1` | 递归下载 Windows App SDK 依赖包 |
| `tools/screenshot.ps1` | 走 `--ui-snapshot` 渲染窗口到 `docs/preview.png` |

## 实现说明：界面为什么是"手搓控件"

WinUI 3 的控件默认样式不在 DLL 里，而在框架资源包（`Microsoft.UI.Xaml.Controls.pri`）中，
以 XBF 形式挂在下面这个 URI 上；自包含部署（`WindowsAppSDKSelfContained=true`）时这份资源
没有被合并进应用的 `resources.pri`，于是第一次用到依赖它的控件就会失败：

```text
Cannot load DefaultStyleResourceUri 'ms-appx:///Microsoft.UI.Xaml/Themes/generic.xaml'
for TargetType 'Microsoft.UI.Xaml.Controls.ProgressBar' in assembly 'Unknown'.
```

这个异常在 XAML 内部被升级成 fail-fast（`0xC000027B`，`Microsoft.UI.Xaml.dll` 内 `E_FAIL`），
进程直接死，`try/catch` 和 `UnhandledException` 都接不到——表面上就是"程序一闪而退"。
受影响的只有样式走 `DefaultStyleResourceUri` 的控件，即 `ProgressBar`、`ProgressRing`、
`TextBox`、`InfoBar`；而 `Border`、`Button`、`CheckBox`、`RadioButton`、`ToggleSwitch`、
`Slider`、`ComboBox`、`ScrollViewer`、`ItemsControl`、`HyperlinkButton` 的样式由框架内置，
全部正常。

修复方式有两条：改成 framework-dependent 部署（依赖机器上已装的 Windows App Runtime），
或者干脆不用那四个控件。这里选后者——**进度条、目录输入框、提示横幅都改成 `Border` + `Grid`
自绘**（见 `MainWindow.cpp` 里的 `LiteBar`），换来零运行时依赖，配色与主题也完全可控。

同一处限制还带来两个自绘控件：原生 `CheckBox` / `RadioButton` 的勾选图形固定 20 × 20，
想改小只能改模板，而模板正好走上面那条 fail-fast 路径。所以列表里的复选框和两个输出模式
单选按钮是 16 px 的 `LiteCheck` / `LiteRadio`（`Border` + `FontIcon`），尺寸、圆角、
强调色都跟着调色板走。
界面本身同样是代码式构建（`ApplicationT<App>` + `Window()` + 直接装配控件），
完全绕开 XAML 编译器与 `XamlTypeInfo` 生成。

拖放没有用 `DragAcceptFiles` + `WM_DROPFILES`——那条路径只在松手之后才有消息，拖动过程中
无从给出反馈，指针也会停在"禁止"上。改成实现 OLE 的 `IDropTarget`，注册到顶层窗口以及它的
每个 WinUI 子窗口（`RegisterDragDrop` 命中测试看的是光标下那个 HWND）。这里有个坑：
`winrt::init_apartment` 只做 `CoInitializeEx`，而 `RegisterDragDrop` 要求先
`OleInitialize`，否则返回 `0x8007000E`（E_OUTOFMEMORY），拖放静默失效。

ID3v2.3 的文本帧要求 UTF-16 带 BOM，COMM / USLT 里的"空描述"也必须带 BOM —— 少了这两个字节，
`ffmpeg` 会报 `Incorrect BOM value` 并整帧跳过。

主题跟随走系统事件：启动时由 `UISettings` 判断浅色还是深色并生成调色板，之后
`ActualThemeChanged` 触发重绘（逐控件重映射画刷，不重建界面）；自绘控件另有
`Repaint()` 一起换色。强调色不取系统强调色——它经常是棕色或橙色，和这里的图标、
进度条不搭——固定用 Windows 11 的默认蓝（浅色 `#005FB8` / 深色 `#60CDFF`），
前景文字再按 WCAG 对比度自动选黑或白。

## 开发与调试钩子

| 命令 | 用途 |
| --- | --- |
| `NcmConverter.exe --convert in.ncm [out]` | 无界面转换，结果写 `<输出>.log`（`format=` 行说明识别成 FLAC 还是 MP3） |
| `NcmConverter.exe --ui-snapshot <目录> [文件...]` | 渲染窗口到 `ui.png` + 视觉树 `ui-tree.txt` 后退出 |
| `NcmConverter.exe --ui-run <目录> [文件...]` | 同上，但先跑一次完整转换，验证界面链路 |
| 环境变量 `NCM_SNAPSHOT_DROP=2` | 截图时强制显示拖放提示（否则那张浮层只在真实拖拽时出现） |

## 许可

仅供个人学习，以及处理自己合法获得的音乐文件。
