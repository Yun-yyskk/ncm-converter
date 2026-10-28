#include "pch.h"

#include "MainWindow.h"

#include "core/convert.h"

#include <shellapi.h>
#include <shobjidl_core.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Dispatching;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Text;

// Alias keeps the projected IInspectable unambiguous next to the raw COM one.
using Inspectable = winrt::Windows::Foundation::IInspectable;

namespace {

// ---------------------------------------------------------------------------
// Palette. The window runs on a dark Mica backdrop, so these are tuned for it.
// ---------------------------------------------------------------------------
winrt::Windows::UI::Color Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF) {
    return winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b);
}

const winrt::Windows::UI::Color kTextPrimary = Rgba(243, 244, 248);
const winrt::Windows::UI::Color kTextSecondary = Rgba(160, 165, 180);
const winrt::Windows::UI::Color kTextTertiary = Rgba(118, 123, 140);
const winrt::Windows::UI::Color kAccentA = Rgba(255, 92, 106);
const winrt::Windows::UI::Color kAccentB = Rgba(255, 150, 74);
const winrt::Windows::UI::Color kSuccess = Rgba(122, 226, 148);
const winrt::Windows::UI::Color kDanger = Rgba(255, 138, 138);
const winrt::Windows::UI::Color kWarning = Rgba(255, 203, 112);
const winrt::Windows::UI::Color kIdle = Rgba(150, 156, 172);

SolidColorBrush Solid(winrt::Windows::UI::Color const& color) { return SolidColorBrush(color); }
SolidColorBrush WhiteFill(uint8_t alpha) { return SolidColorBrush(Rgba(255, 255, 255, alpha)); }
SolidColorBrush Transparent() { return SolidColorBrush(Rgba(0, 0, 0, 0)); }

LinearGradientBrush AccentGradient() {
    LinearGradientBrush brush;
    brush.StartPoint(Point{ 0.0f, 0.0f });
    brush.EndPoint(Point{ 1.0f, 1.0f });
    GradientStop first;
    first.Color(kAccentA);
    first.Offset(0.0);
    GradientStop second;
    second.Color(kAccentB);
    second.Offset(1.0);
    brush.GradientStops().Append(first);
    brush.GradientStops().Append(second);
    return brush;
}

TextBlock MakeText(std::wstring const& value, double size, winrt::Windows::UI::Color color, bool semibold = false) {
    TextBlock block;
    block.Text(hstring(value));
    block.FontSize(size);
    block.Foreground(Solid(color));
    block.TextWrapping(TextWrapping::Wrap);
    if (semibold) block.FontWeight(FontWeights::SemiBold());
    return block;
}

// Segoe Fluent Icons codepoints for the symbols used by this window. FontIcon is
// used instead of SymbolIcon because only FontIcon exposes FontSize.
wchar_t const* GlyphFor(Symbol symbol) {
    switch (symbol) {
    case Symbol::MusicInfo: return L"\uE8D6";
    case Symbol::Add: return L"\uE710";
    case Symbol::Delete: return L"\uE74D";
    case Symbol::Cancel: return L"\uE711";
    case Symbol::Folder: return L"\uE8B7";
    case Symbol::OpenFile: return L"\uE8E5";
    case Symbol::Play: return L"\uE768";
    case Symbol::Accept: return L"\uE8FB";
    case Symbol::Setting: return L"\uE713";
    case Symbol::Sync: return L"\uE895";
    default: return L"\uE946";
    }
}

FontIcon MakeIcon(Symbol symbol, double size, winrt::Windows::UI::Color color) {
    FontIcon icon;
    icon.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));
    icon.Glyph(GlyphFor(symbol));
    icon.FontSize(size);
    icon.Foreground(Solid(color));
    return icon;
}

RowDefinition RowAuto() {
    RowDefinition row;
    row.Height(GridLength{ 0.0, GridUnitType::Auto });
    return row;
}

RowDefinition RowStar(double weight = 1.0) {
    RowDefinition row;
    row.Height(GridLength{ weight, GridUnitType::Star });
    return row;
}

ColumnDefinition ColAuto() {
    ColumnDefinition column;
    column.Width(GridLength{ 0.0, GridUnitType::Auto });
    return column;
}

ColumnDefinition ColStar(double weight = 1.0) {
    ColumnDefinition column;
    column.Width(GridLength{ weight, GridUnitType::Star });
    return column;
}

Border MakeCard() {
    Border card;
    card.Background(WhiteFill(9));
    card.BorderBrush(WhiteFill(24));
    card.BorderThickness(Thickness{ 1, 1, 1, 1 });
    card.CornerRadius(CornerRadius{ 14, 14, 14, 14 });
    card.Padding(Thickness{ 18, 14, 18, 14 });
    return card;
}

Button MakeButton(Symbol symbol, std::wstring const& text) {
    StackPanel content;
    content.Orientation(Orientation::Horizontal);
    content.Spacing(7);
    content.VerticalAlignment(VerticalAlignment::Center);

    FontIcon icon = MakeIcon(symbol, 14, kTextPrimary);
    icon.VerticalAlignment(VerticalAlignment::Center);

    TextBlock label = MakeText(text, 13, kTextPrimary);
    label.VerticalAlignment(VerticalAlignment::Center);

    content.Children().Append(icon);
    content.Children().Append(label);

    Button button;
    button.Content(content);
    button.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
    button.Padding(Thickness{ 12, 7, 14, 7 });
    return button;
}

Style FindStyle(std::wstring_view key) {
    try {
        auto found = Application::Current().Resources().Lookup(box_value(hstring(key)));
        return found.try_as<Style>();
    } catch (...) {
        return nullptr;
    }
}

std::wstring FormatBytes(uint64_t bytes) {
    wchar_t buffer[64] = {};
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        swprintf_s(buffer, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        swprintf_s(buffer, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        swprintf_s(buffer, L"%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

std::wstring Trim(std::wstring value) {
    constexpr wchar_t kSpace[] = L" \t\r\n\"";
    const size_t begin = value.find_first_not_of(kSpace);
    if (begin == std::wstring::npos) return {};
    const size_t end = value.find_last_not_of(kSpace);
    return value.substr(begin, end - begin + 1);
}

std::wstring MakeUniquePath(std::filesystem::path const& target) {
    std::error_code ec;
    if (!std::filesystem::exists(target, ec)) return target.wstring();

    const std::filesystem::path parent = target.parent_path();
    const std::wstring stem = target.stem().wstring();
    const std::wstring extension = target.extension().wstring();
    for (int index = 2; index < 999; ++index) {
        const std::filesystem::path candidate = parent / (stem + L" (" + std::to_wstring(index) + L")" + extension);
        if (!std::filesystem::exists(candidate, ec)) return candidate.wstring();
    }
    return target.wstring();
}

std::wstring ResolveOutputPath(std::wstring const& input, bool sameDirectory, std::wstring const& outputDirectory,
                               bool overwrite) {
    std::filesystem::path target;
    if (sameDirectory) {
        target = ncmlib::DefaultOutputPath(input);
    } else {
        const std::filesystem::path source(input);
        target = std::filesystem::path(outputDirectory) / source.stem();
        target.replace_extension(L".flac");
    }
    return overwrite ? target.wstring() : MakeUniquePath(target);
}

std::vector<std::wstring> PickPaths(bool pickFolder, std::wstring const& title) {
    std::vector<std::wstring> result;

    com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
        return result;
    }

    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (pickFolder) {
        options |= FOS_PICKFOLDERS;
    } else {
        options |= FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST;
    }
    dialog->SetOptions(options);
    dialog->SetTitle(title.c_str());

    if (!pickFolder) {
        COMDLG_FILTERSPEC filters[] = {
            { L"网易云音乐文件 (*.ncm)", L"*.ncm" },
            { L"所有文件", L"*.*" },
        };
        dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        dialog->SetFileTypeIndex(1);
    }

    if (FAILED(dialog->Show(GetActiveWindow()))) return result;

    if (pickFolder) {
        com_ptr<IShellItem> folder;
        if (SUCCEEDED(dialog->GetResult(folder.put()))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(folder->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result.push_back(path);
                CoTaskMemFree(path);
            }
        }
        return result;
    }

    com_ptr<IShellItemArray> selection;
    if (FAILED(dialog->GetResults(selection.put()))) return result;

    DWORD count = 0;
    selection->GetCount(&count);
    for (DWORD index = 0; index < count; ++index) {
        com_ptr<IShellItem> entry;
        if (FAILED(selection->GetItemAt(index, entry.put()))) continue;
        PWSTR path = nullptr;
        if (SUCCEEDED(entry->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            result.push_back(path);
            CoTaskMemFree(path);
        }
    }
    return result;
}

// Small rounded progress track drawn with plain layout primitives. WinUI's
// ProgressBar raises E_FAIL inside Microsoft.UI.Xaml on some machines (no GPU,
// WARP fallback), which fail-fasts the process, so the app draws its own.
class LiteBar {
public:
    LiteBar(double height, winrt::Windows::UI::Color fill, uint8_t trackAlpha) {
        const double radius = height / 2.0;

        m_root = Grid();
        m_root.Height(height);

        Border track;
        track.CornerRadius(CornerRadius{ radius, radius, radius, radius });
        track.Background(WhiteFill(trackAlpha));
        Grid::SetColumnSpan(track, 2);

        m_fill = Border();
        m_fill.CornerRadius(CornerRadius{ radius, radius, radius, radius });
        m_fill.Background(Solid(fill));
        Grid::SetColumn(m_fill, 0);

        m_fillColumn = ColumnDefinition();
        m_fillColumn.Width(GridLength(0, GridUnitType::Star));
        m_restColumn = ColumnDefinition();
        m_restColumn.Width(GridLength(1, GridUnitType::Star));

        m_root.ColumnDefinitions().Append(m_fillColumn);
        m_root.ColumnDefinitions().Append(m_restColumn);
        m_root.Children().Append(track);
        m_root.Children().Append(m_fill);
    }

    Grid Root() const { return m_root; }

    void SetPercent(double percent) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        const double fraction = percent / 100.0;
        m_fillColumn.Width(GridLength(fraction, GridUnitType::Star));
        m_restColumn.Width(GridLength(1.0 - fraction, GridUnitType::Star));
    }

    void SetVisibility(Visibility visibility) { m_root.Visibility(visibility); }

private:
    Grid m_root{ nullptr };
    ColumnDefinition m_fillColumn{ nullptr };
    ColumnDefinition m_restColumn{ nullptr };
    Border m_fill{ nullptr };
};

// Severity of the inline notice banner.
enum class NoticeKind { Info, Success, Warning, Error };

// One row in the file list.
struct Item {
    enum class State { Pending, Running, Done, Failed };

    std::wstring path;
    std::wstring name;
    State state = State::Pending;

    Border row{ nullptr };
    CheckBox check{ nullptr };
    TextBlock titleText{ nullptr };
    TextBlock subtitleText{ nullptr };
    Border chip{ nullptr };
    TextBlock chipText{ nullptr };
    std::shared_ptr<LiteBar> bar;
};

} // namespace

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------
struct MainWindow::Impl : std::enable_shared_from_this<MainWindow::Impl> {
    Window window{ nullptr };
    DispatcherQueue queue{ nullptr };
    Grid root{ nullptr };

    StackPanel listPanel{ nullptr };
    StackPanel emptyState{ nullptr };
    TextBlock countText{ nullptr };
    CheckBox selectAll{ nullptr };
    Button addButton{ nullptr };
    Button clearButton{ nullptr };
    RadioButton sameDirRadio{ nullptr };
    RadioButton customDirRadio{ nullptr };
    Border dirBox{ nullptr };
    TextBlock dirText{ nullptr };
    Button browseButton{ nullptr };
    ToggleSwitch overwriteSwitch{ nullptr };
    Border banner{ nullptr };
    FontIcon bannerIcon{ nullptr };
    TextBlock bannerTitle{ nullptr };
    TextBlock bannerText{ nullptr };
    std::shared_ptr<LiteBar> overallBar;
    TextBlock statusText{ nullptr };
    Border convertButton{ nullptr };
    TextBlock convertLabel{ nullptr };
    bool convertEnabled = true;
    Button openFolderButton{ nullptr };

    std::vector<std::shared_ptr<Item>> items;
    std::atomic<bool> running{ false };
    std::atomic<bool> closed{ false };
    bool syncingSelectAll = false;
    bool syncingOutputMode = false;
    std::wstring lastOutputDirectory;
    std::wstring customDirectory;

    void Build();
    void AddFilesFromDialog();
    bool AddFile(std::wstring const& path);
    void RemoveItem(std::shared_ptr<Item> const& item);
    void SetAllChecked(bool checked);
    void RefreshCounters();
    void UpdateOutputMode();
    void SetConvertEnabled(bool enabled);

    void SetInfo(std::wstring const& title, std::wstring const& message, NoticeKind kind);
    void ClearInfo();

    void StartConversion();
    void SetItemRunning(std::shared_ptr<Item> const& item);
    void SetItemProgress(std::shared_ptr<Item> const& item, int percent);
    void FinishItem(std::shared_ptr<Item> const& item, ncmlib::ConvertOutcome const& outcome, int done, int total);
    void FinishAll(int done, int total, int succeeded, std::wstring const& lastDirectory);

    void SetChip(Item& item, std::wstring const& text, winrt::Windows::UI::Color foreground,
                 winrt::Windows::UI::Color background);

    void Post(std::function<void()> action);

    winrt::fire_and_forget OnDrop(Inspectable const& sender, DragEventArgs const& args);
};

void MainWindow::Impl::Build() {
    window = Window();
    window.Title(L"NCM 转 FLAC");
    queue = DispatcherQueue::GetForCurrentThread();

    root = Grid();
    root.RequestedTheme(ElementTheme::Dark);
    // Opaque window surface: keeps the dark palette readable even when the
    // desktop backdrop would be light (or when nothing is composited at all).
    root.Background(Solid(Rgba(16, 17, 23)));
    root.RowDefinitions().Append(RowAuto());   // custom title bar
    root.RowDefinitions().Append(RowStar());   // content
    root.AllowDrop(true);

    // ----- title bar --------------------------------------------------------
    Grid titleBar;
    titleBar.Height(48);
    titleBar.Padding(Thickness{ 18, 0, 18, 0 });
    titleBar.ColumnDefinitions().Append(ColAuto());
    titleBar.ColumnDefinitions().Append(ColStar());
    titleBar.VerticalAlignment(VerticalAlignment::Top);

    StackPanel brand;
    brand.Orientation(Orientation::Horizontal);
    brand.Spacing(10);
    brand.VerticalAlignment(VerticalAlignment::Center);

    Border brandBadge;
    brandBadge.Width(24);
    brandBadge.Height(24);
    brandBadge.CornerRadius(CornerRadius{ 7, 7, 7, 7 });
    brandBadge.Background(AccentGradient());
    brandBadge.Child(MakeIcon(Symbol::MusicInfo, 11, Rgba(255, 255, 255)));
    brand.Children().Append(brandBadge);

    TextBlock brandText = MakeText(L"NCM 转 FLAC", 12, kTextSecondary);
    brandText.VerticalAlignment(VerticalAlignment::Center);
    brand.Children().Append(brandText);

    titleBar.Children().Append(brand);
    Grid::SetRow(titleBar, 0);
    root.Children().Append(titleBar);

    // ----- content ----------------------------------------------------------
    Grid content;
    content.Padding(Thickness{ 28, 4, 28, 22 });
    content.RowDefinitions().Append(RowAuto());  // hero
    content.RowDefinitions().Append(RowStar());  // list
    content.RowDefinitions().Append(RowAuto());  // settings
    content.RowDefinitions().Append(RowAuto());  // info bar
    content.RowDefinitions().Append(RowAuto());  // footer
    Grid::SetRow(content, 1);
    root.Children().Append(content);

    // hero
    Grid hero;
    hero.Margin(Thickness{ 0, 6, 0, 18 });
    hero.ColumnDefinitions().Append(ColStar());
    hero.ColumnDefinitions().Append(ColAuto());

    StackPanel heroText;
    heroText.Spacing(6);
    heroText.VerticalAlignment(VerticalAlignment::Center);
    TextBlock heroTitle = MakeText(L"", 25, kTextPrimary, true);
    {
        auto inlines = heroTitle.Inlines();
        winrt::Microsoft::UI::Xaml::Documents::Run prefix;
        prefix.Text(L"把 ");
        inlines.Append(prefix);

        winrt::Microsoft::UI::Xaml::Documents::Run source;
        source.Text(L".ncm");
        source.Foreground(AccentGradient());
        inlines.Append(source);

        winrt::Microsoft::UI::Xaml::Documents::Run middle;
        middle.Text(L" 还原成无损 ");
        inlines.Append(middle);

        winrt::Microsoft::UI::Xaml::Documents::Run target;
        target.Text(L"FLAC");
        target.Foreground(AccentGradient());
        inlines.Append(target);
    }
    heroText.Children().Append(heroTitle);

    TextBlock heroHint = MakeText(L"读取网易云音乐的加密容器，输出标准 FLAC —— 封面、歌词与标签一并写入。", 13,
                                  kTextSecondary);
    heroHint.MaxWidth(640);
    heroText.Children().Append(heroHint);
    hero.Children().Append(heroText);

    Border heroBadge;
    heroBadge.Width(62);
    heroBadge.Height(62);
    heroBadge.CornerRadius(CornerRadius{ 18, 18, 18, 18 });
    heroBadge.Background(AccentGradient());
    heroBadge.VerticalAlignment(VerticalAlignment::Center);
    heroBadge.Child(MakeIcon(Symbol::MusicInfo, 26, Rgba(255, 255, 255)));
    Grid::SetColumn(heroBadge, 1);
    hero.Children().Append(heroBadge);

    Grid::SetRow(hero, 0);
    content.Children().Append(hero);

    // file list card
    Border listCard = MakeCard();
    listCard.Padding(Thickness{ 16, 12, 16, 12 });
    Grid::SetRow(listCard, 1);
    content.Children().Append(listCard);

    Grid listGrid;
    listGrid.RowDefinitions().Append(RowAuto());
    listGrid.RowDefinitions().Append(RowStar());
    listCard.Child(listGrid);

    Grid listHeader;
    listHeader.ColumnDefinitions().Append(ColStar());
    listHeader.ColumnDefinitions().Append(ColAuto());
    listHeader.ColumnDefinitions().Append(ColAuto());
    listHeader.ColumnDefinitions().Append(ColAuto());
    listHeader.Margin(Thickness{ 2, 0, 2, 6 });

    StackPanel listTitleStack;
    listTitleStack.Orientation(Orientation::Horizontal);
    listTitleStack.Spacing(10);
    listTitleStack.VerticalAlignment(VerticalAlignment::Center);
    listTitleStack.Children().Append(MakeText(L"文件列表", 14, kTextPrimary, true));

    countText = MakeText(L"暂无文件", 12, kTextTertiary);
    countText.VerticalAlignment(VerticalAlignment::Center);
    listTitleStack.Children().Append(countText);
    listHeader.Children().Append(listTitleStack);

    selectAll = CheckBox();
    selectAll.Content(box_value(hstring(L"全选")));
    selectAll.MinWidth(0);
    selectAll.Margin(Thickness{ 0, 0, 8, 0 });
    selectAll.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(selectAll, 1);

    addButton = MakeButton(Symbol::Add, L"添加文件");
    addButton.Margin(Thickness{ 0, 0, 8, 0 });
    Grid::SetColumn(addButton, 2);

    clearButton = MakeButton(Symbol::Delete, L"清空");
    Grid::SetColumn(clearButton, 3);

    listHeader.Children().Append(selectAll);
    listHeader.Children().Append(addButton);
    listHeader.Children().Append(clearButton);

    Grid::SetRow(listHeader, 0);
    listGrid.Children().Append(listHeader);

    Grid listHost;
    listHost.MinHeight(150);
    Grid::SetRow(listHost, 1);

    ScrollViewer scroller;
    scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    scroller.Padding(Thickness{ 0, 6, 0, 0 });
    scroller.Content(listPanel);

    listPanel = StackPanel();
    listPanel.Spacing(0);
    scroller.Content(listPanel);
    listHost.Children().Append(scroller);

    emptyState = StackPanel();
    emptyState.Spacing(8);
    emptyState.HorizontalAlignment(HorizontalAlignment::Center);
    emptyState.VerticalAlignment(VerticalAlignment::Center);
    emptyState.IsHitTestVisible(false);

    FontIcon emptyIcon = MakeIcon(Symbol::MusicInfo, 30, Rgba(255, 255, 255, 70));
    emptyIcon.HorizontalAlignment(HorizontalAlignment::Center);
    emptyState.Children().Append(emptyIcon);

    TextBlock emptyTitle = MakeText(L"还没有文件", 14, kTextSecondary, true);
    emptyTitle.HorizontalAlignment(HorizontalAlignment::Center);
    emptyState.Children().Append(emptyTitle);

    TextBlock emptyHint = MakeText(L"点击「添加文件」，或把 .ncm 直接拖进窗口。", 12, kTextTertiary);
    emptyHint.HorizontalAlignment(HorizontalAlignment::Center);
    emptyState.Children().Append(emptyHint);

    listHost.Children().Append(emptyState);
    listGrid.Children().Append(listHost);

    // output settings card
    Border settingsCard = MakeCard();
    settingsCard.Margin(Thickness{ 0, 14, 0, 0 });
    Grid::SetRow(settingsCard, 2);
    content.Children().Append(settingsCard);

    StackPanel settings;
    settings.Spacing(10);

    StackPanel outputHeader;
    outputHeader.Orientation(Orientation::Horizontal);
    outputHeader.Spacing(4);
    outputHeader.VerticalAlignment(VerticalAlignment::Center);
    outputHeader.Children().Append(MakeText(L"输出位置", 13, kTextPrimary, true));

    sameDirRadio = RadioButton();
    sameDirRadio.GroupName(L"output-mode");
    sameDirRadio.Content(box_value(hstring(L"与源文件同目录")));
    sameDirRadio.IsChecked(true);
    sameDirRadio.Margin(Thickness{ 0, 0, 16, 0 });

    customDirRadio = RadioButton();
    customDirRadio.GroupName(L"output-mode");
    customDirRadio.Content(box_value(hstring(L"指定目录")));

    StackPanel radioRow;
    radioRow.Orientation(Orientation::Horizontal);
    radioRow.Spacing(2);
    radioRow.Children().Append(sameDirRadio);
    radioRow.Children().Append(customDirRadio);
    outputHeader.Children().Append(radioRow);
    settings.Children().Append(outputHeader);

    Grid dirRow;
    dirRow.ColumnDefinitions().Append(ColStar());
    dirRow.ColumnDefinitions().Append(ColAuto());

    dirBox = Border();
    dirBox.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
    dirBox.Background(WhiteFill(10));
    dirBox.BorderBrush(WhiteFill(20));
    dirBox.BorderThickness(Thickness{ 1, 1, 1, 1 });
    dirBox.Padding(Thickness{ 12, 9, 12, 9 });
    dirBox.Margin(Thickness{ 0, 0, 8, 0 });
    dirBox.Opacity(0.55);

    Grid dirInner;
    dirInner.ColumnDefinitions().Append(ColAuto());
    dirInner.ColumnDefinitions().Append(ColStar());

    FontIcon dirIcon = MakeIcon(Symbol::Folder, 13, kTextTertiary);
    dirIcon.Margin(Thickness{ 0, 1, 8, 0 });
    dirIcon.VerticalAlignment(VerticalAlignment::Center);

    dirText = MakeText(L"点击选择输出目录", 12, kTextTertiary);
    dirText.TextTrimming(TextTrimming::CharacterEllipsis);
    dirText.TextWrapping(TextWrapping::NoWrap);
    dirText.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(dirText, 1);

    dirInner.Children().Append(dirIcon);
    dirInner.Children().Append(dirText);
    dirBox.Child(dirInner);

    browseButton = MakeButton(Symbol::Folder, L"浏览");
    browseButton.IsEnabled(false);
    Grid::SetColumn(browseButton, 1);

    dirRow.Children().Append(dirBox);
    dirRow.Children().Append(browseButton);
    settings.Children().Append(dirRow);

    overwriteSwitch = ToggleSwitch();
    overwriteSwitch.Header(box_value(hstring(L"覆盖已存在的 FLAC")));
    overwriteSwitch.OnContent(box_value(hstring(L"是")));
    overwriteSwitch.OffContent(box_value(hstring(L"否")));
    overwriteSwitch.IsOn(false);
    settings.Children().Append(overwriteSwitch);

    settingsCard.Child(settings);

    // inline notice banner (InfoBar fail-fasts on this machine, so it is drawn
    // from a Border plus text blocks instead)
    banner = Border();
    banner.CornerRadius(CornerRadius{ 10, 10, 10, 10 });
    banner.BorderThickness(Thickness{ 1, 1, 1, 1 });
    banner.Padding(Thickness{ 14, 10, 14, 10 });
    banner.Margin(Thickness{ 0, 14, 0, 0 });
    banner.Visibility(Visibility::Collapsed);

    Grid bannerInner;
    bannerInner.ColumnDefinitions().Append(ColAuto());
    bannerInner.ColumnDefinitions().Append(ColStar());

    bannerIcon = FontIcon();
    bannerIcon.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));
    bannerIcon.FontSize(14);
    bannerIcon.Margin(Thickness{ 0, 1, 10, 0 });
    bannerIcon.VerticalAlignment(VerticalAlignment::Top);

    StackPanel bannerStack;
    bannerStack.Spacing(2);

    bannerTitle = MakeText(L"", 12, kTextPrimary, true);
    bannerTitle.Visibility(Visibility::Collapsed);

    bannerText = MakeText(L"", 12, kTextSecondary);
    bannerText.TextWrapping(TextWrapping::Wrap);

    bannerStack.Children().Append(bannerTitle);
    bannerStack.Children().Append(bannerText);
    Grid::SetColumn(bannerStack, 1);

    bannerInner.Children().Append(bannerIcon);
    bannerInner.Children().Append(bannerStack);
    banner.Child(bannerInner);

    Grid::SetRow(banner, 3);
    content.Children().Append(banner);

    // footer
    Grid footer;
    footer.Margin(Thickness{ 0, 16, 0, 0 });
    footer.ColumnDefinitions().Append(ColStar());
    footer.ColumnDefinitions().Append(ColAuto());
    footer.ColumnDefinitions().Append(ColAuto());
    Grid::SetRow(footer, 4);
    content.Children().Append(footer);

    StackPanel statusStack;
    statusStack.Spacing(8);
    statusStack.VerticalAlignment(VerticalAlignment::Center);

    statusText = MakeText(L"就绪 · 选择文件后开始转换", 12, kTextSecondary);
    statusStack.Children().Append(statusText);

    overallBar = std::make_shared<LiteBar>(4, kAccentB, 26);
    overallBar->Root().Width(440);
    overallBar->Root().MaxWidth(440);
    overallBar->Root().HorizontalAlignment(HorizontalAlignment::Left);
    statusStack.Children().Append(overallBar->Root());

    footer.Children().Append(statusStack);

    openFolderButton = MakeButton(Symbol::OpenFile, L"打开目录");
    openFolderButton.Margin(Thickness{ 0, 0, 10, 0 });
    openFolderButton.VerticalAlignment(VerticalAlignment::Center);
    openFolderButton.Visibility(Visibility::Collapsed);
    Grid::SetColumn(openFolderButton, 1);
    footer.Children().Append(openFolderButton);

    // Primary action, drawn as a border so the gradient backdrop is fully under
    // our control (the stock AccentButtonStyle uses the system accent colour).
    convertButton = Border();
    convertButton.CornerRadius(CornerRadius{ 11, 11, 11, 11 });
    convertButton.Padding(Thickness{ 22, 0, 24, 0 });
    convertButton.MinWidth(160);
    convertButton.Height(40);
    convertButton.Background(AccentGradient());
    convertButton.VerticalAlignment(VerticalAlignment::Center);

    StackPanel convertStack;
    convertStack.Orientation(Orientation::Horizontal);
    convertStack.Spacing(9);
    convertStack.VerticalAlignment(VerticalAlignment::Center);
    convertStack.Children().Append(MakeIcon(Symbol::Play, 13, Rgba(255, 255, 255)));

    convertLabel = MakeText(L"开始转换", 14, Rgba(255, 255, 255), true);
    convertLabel.VerticalAlignment(VerticalAlignment::Center);
    convertStack.Children().Append(convertLabel);
    convertButton.Child(convertStack);

    Grid::SetColumn(convertButton, 2);
    footer.Children().Append(convertButton);

    // ----- behaviour --------------------------------------------------------
    window.Content(root);
    try {
        window.ExtendsContentIntoTitleBar(true);
        window.SetTitleBar(titleBar);
    } catch (...) {
    }

    const double scale = GetDpiForSystem() / 96.0;
    window.AppWindow().Resize({ static_cast<int32_t>(1040 * scale), static_cast<int32_t>(776 * scale) });

    addButton.Click([this](Inspectable const&, RoutedEventArgs const&) { AddFilesFromDialog(); });
    clearButton.Click([this](Inspectable const&, RoutedEventArgs const&) {
        items.clear();
        listPanel.Children().Clear();
        ClearInfo();
        overallBar->SetPercent(0);
        statusText.Text(hstring(L"就绪 · 选择文件后开始转换"));
        RefreshCounters();
    });

    selectAll.Checked([this](Inspectable const&, RoutedEventArgs const&) {
        if (!syncingSelectAll) SetAllChecked(true);
    });
    selectAll.Unchecked([this](Inspectable const&, RoutedEventArgs const&) {
        if (!syncingSelectAll) SetAllChecked(false);
    });

    sameDirRadio.Checked([this](Inspectable const&, RoutedEventArgs const&) { UpdateOutputMode(); });
    sameDirRadio.Unchecked([this](Inspectable const&, RoutedEventArgs const&) { UpdateOutputMode(); });
    customDirRadio.Checked([this](Inspectable const&, RoutedEventArgs const&) { UpdateOutputMode(); });

    auto chooseDirectory = [this]() {
        auto picked = PickPaths(true, L"选择输出目录");
        if (picked.empty()) return;
        customDirectory = picked.front();
        dirText.Text(hstring(customDirectory));
        dirText.Foreground(Solid(kTextPrimary));
        if (auto checked = customDirRadio.IsChecked(); !checked || !checked.Value()) {
            syncingOutputMode = true;
            customDirRadio.IsChecked(true);
            syncingOutputMode = false;
        }
        UpdateOutputMode();
    };
    browseButton.Click([chooseDirectory](Inspectable const&, RoutedEventArgs const&) { chooseDirectory(); });
    dirBox.Tapped([chooseDirectory](Inspectable const&, TappedRoutedEventArgs const&) { chooseDirectory(); });

    convertButton.Tapped([this](Inspectable const&, TappedRoutedEventArgs const&) {
        if (convertEnabled) StartConversion();
    });
    convertButton.PointerEntered([this](Inspectable const&, PointerRoutedEventArgs const&) {
        convertButton.Opacity(convertEnabled ? 0.9 : 0.45);
    });
    convertButton.PointerExited([this](Inspectable const&, PointerRoutedEventArgs const&) {
        convertButton.Opacity(convertEnabled ? 1.0 : 0.45);
    });

    openFolderButton.Click([this](Inspectable const&, RoutedEventArgs const&) {
        if (lastOutputDirectory.empty()) return;
        ShellExecuteW(nullptr, L"open", lastOutputDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });

    window.Closed([this](Inspectable const&, WindowEventArgs const&) { closed = true; });

    root.DragOver([](Inspectable const&, DragEventArgs const& args) {
        args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
        args.DragUIOverride().IsCaptionVisible(true);
        args.DragUIOverride().Caption(L"添加到列表");
    });
    root.Drop({ this, &Impl::OnDrop });

    RefreshCounters();
}

winrt::fire_and_forget MainWindow::Impl::OnDrop(Inspectable const& sender, DragEventArgs const& args) {
    auto self = shared_from_this();
    (void)sender;

    auto deferral = args.GetDeferral();
    try {
        auto view = args.DataView();
        if (view.Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems())) {
            auto storageItems = co_await view.GetStorageItemsAsync();
            int added = 0;
            int ignored = 0;
            for (auto const& entry : storageItems) {
                if (auto file = entry.try_as<winrt::Windows::Storage::StorageFile>()) {
                    if (AddFile(std::wstring(file.Path().c_str()))) {
                        ++added;
                    } else {
                        ++ignored;
                    }
                } else {
                    ++ignored;
                }
            }
            if (added > 0) {
                ClearInfo();
            } else if (ignored > 0) {
                SetInfo(L"没有可用的文件", L"拖进来的内容里没有 .ncm 文件，或者已经在列表中了。",
                        NoticeKind::Info);
            }
        }
    } catch (...) {
    }
    deferral.Complete();
}

void MainWindow::Impl::AddFilesFromDialog() {
    auto files = PickPaths(false, L"选择 NCM 文件");
    if (files.empty()) return;

    int added = 0;
    for (auto const& file : files) {
        if (AddFile(file)) ++added;
    }
    if (added > 0) {
        ClearInfo();
        statusText.Text(hstring(L"已添加 " + std::to_wstring(added) + L" 个文件"));
    } else {
        SetInfo(L"没有可用的文件", L"选中的内容里没有 .ncm 文件，或者已经在列表中了。", NoticeKind::Info);
    }
}

bool MainWindow::Impl::AddFile(std::wstring const& path) {
    if (path.empty()) return false;

    const std::filesystem::path source(path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) return false;

    std::wstring extension = source.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (extension != L".ncm") return false;

    for (auto const& existing : items) {
        if (_wcsicmp(existing->path.c_str(), path.c_str()) == 0) return false;
    }

    auto item = std::make_shared<Item>();
    item->path = path;
    item->name = source.stem().wstring();
    if (item->name.empty()) item->name = source.filename().wstring();

    const uint64_t size = std::filesystem::file_size(source, ec);
    const std::wstring sizeText = ec ? std::wstring(L"未知大小") : FormatBytes(size);

    Border row;
    row.CornerRadius(CornerRadius{ 12, 12, 12, 12 });
    row.Background(WhiteFill(10));
    row.BorderBrush(WhiteFill(18));
    row.BorderThickness(Thickness{ 1, 1, 1, 1 });
    row.Padding(Thickness{ 12, 10, 10, 10 });
    row.Margin(Thickness{ 0, 0, 0, 8 });

    Grid layout;
    layout.ColumnDefinitions().Append(ColAuto());
    layout.ColumnDefinitions().Append(ColAuto());
    layout.ColumnDefinitions().Append(ColStar());
    layout.ColumnDefinitions().Append(ColAuto());
    layout.ColumnDefinitions().Append(ColAuto());
    layout.ColumnSpacing(10);

    CheckBox check;
    check.MinWidth(24);
    check.VerticalAlignment(VerticalAlignment::Center);
    check.IsChecked(true);
    item->check = check;
    layout.Children().Append(check);

    Border iconBorder;
    iconBorder.Width(34);
    iconBorder.Height(34);
    iconBorder.CornerRadius(CornerRadius{ 9, 9, 9, 9 });
    iconBorder.Background(WhiteFill(16));
    iconBorder.Child(MakeIcon(Symbol::MusicInfo, 15, Rgba(255, 255, 255, 190)));
    iconBorder.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(iconBorder, 1);
    layout.Children().Append(iconBorder);

    StackPanel textStack;
    textStack.Spacing(2);
    textStack.VerticalAlignment(VerticalAlignment::Center);

    item->titleText = MakeText(item->name, 14, kTextPrimary, true);
    item->titleText.TextWrapping(TextWrapping::NoWrap);
    item->titleText.TextTrimming(TextTrimming::CharacterEllipsis);

    item->subtitleText = MakeText(sizeText + L" · NCM", 12, kTextTertiary);
    item->subtitleText.TextWrapping(TextWrapping::NoWrap);
    item->subtitleText.TextTrimming(TextTrimming::CharacterEllipsis);

    item->bar = std::make_shared<LiteBar>(3, kAccentB, 24);
    item->bar->Root().Margin(Thickness{ 0, 6, 0, 0 });
    item->bar->SetVisibility(Visibility::Collapsed);

    textStack.Children().Append(item->titleText);
    textStack.Children().Append(item->subtitleText);
    textStack.Children().Append(item->bar->Root());
    Grid::SetColumn(textStack, 2);
    layout.Children().Append(textStack);

    Border chip;
    chip.CornerRadius(CornerRadius{ 10, 10, 10, 10 });
    chip.Padding(Thickness{ 9, 3, 9, 3 });
    chip.VerticalAlignment(VerticalAlignment::Center);
    chip.Background(WhiteFill(14));
    item->chipText = MakeText(L"待转换", 11, kIdle);
    chip.Child(item->chipText);
    item->chip = chip;
    Grid::SetColumn(chip, 3);
    layout.Children().Append(chip);

    Button remove = MakeButton(Symbol::Cancel, L"");
    remove.MinWidth(28);
    remove.Padding(Thickness{ 4, 4, 4, 4 });
    remove.Background(Transparent());
    remove.BorderThickness(Thickness{ 0, 0, 0, 0 });
    remove.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(remove, 4);
    layout.Children().Append(remove);

    row.Child(layout);
    item->row = row;

    auto weak = weak_from_this();
    remove.Click([weak, item](Inspectable const&, RoutedEventArgs const&) {
        if (auto self = weak.lock()) self->RemoveItem(item);
    });
    check.Checked([weak](Inspectable const&, RoutedEventArgs const&) {
        if (auto self = weak.lock()) self->RefreshCounters();
    });
    check.Unchecked([weak](Inspectable const&, RoutedEventArgs const&) {
        if (auto self = weak.lock()) self->RefreshCounters();
    });

    listPanel.Children().Append(row);
    items.push_back(item);
    RefreshCounters();
    return true;
}

void MainWindow::Impl::RemoveItem(std::shared_ptr<Item> const& item) {
    auto found = std::find(items.begin(), items.end(), item);
    if (found == items.end()) return;
    items.erase(found);

    uint32_t index = 0;
    if (listPanel.Children().IndexOf(item->row, index)) listPanel.Children().RemoveAt(index);
    RefreshCounters();
}

void MainWindow::Impl::SetAllChecked(bool checked) {
    for (auto const& item : items) {
        syncingSelectAll = true;
        item->check.IsChecked(checked);
        syncingSelectAll = false;
    }
    RefreshCounters();
}

void MainWindow::Impl::RefreshCounters() {
    int selected = 0;
    for (auto const& item : items) {
        auto state = item->check.IsChecked();
        if (state && state.Value()) ++selected;
    }

    if (items.empty()) {
        countText.Text(hstring(L"暂无文件"));
    } else {
        countText.Text(hstring(std::to_wstring(items.size()) + L" 个文件 · 已选 " + std::to_wstring(selected) + L" 个"));
    }

    syncingSelectAll = true;
    selectAll.IsChecked(items.empty() ? false : (selected == static_cast<int>(items.size())));
    syncingSelectAll = false;

    emptyState.Visibility(items.empty() ? Visibility::Visible : Visibility::Collapsed);
    SetConvertEnabled(!items.empty() && !running.load());
    selectAll.IsEnabled(!items.empty());
}

void MainWindow::Impl::SetConvertEnabled(bool enabled) {
    convertEnabled = enabled;
    convertButton.Opacity(enabled ? 1.0 : 0.45);
}

void MainWindow::Impl::UpdateOutputMode() {
    if (syncingOutputMode) return;
    auto custom = customDirRadio.IsChecked();
    const bool enabled = custom && custom.Value();
    dirBox.Opacity(enabled ? 1.0 : 0.55);
    browseButton.IsEnabled(enabled);
}

void MainWindow::Impl::SetInfo(std::wstring const& title, std::wstring const& message, NoticeKind kind) {
    winrt::Windows::UI::Color color = kAccentB;
    hstring glyph = L"\uE946";
    if (kind == NoticeKind::Success) {
        color = kSuccess;
        glyph = L"\uE930";
    } else if (kind == NoticeKind::Warning) {
        color = kWarning;
        glyph = L"\uE7BA";
    } else if (kind == NoticeKind::Error) {
        color = kDanger;
        glyph = L"\uE783";
    }

    bannerIcon.Glyph(glyph);
    bannerIcon.Foreground(Solid(color));
    bannerTitle.Text(hstring(title));
    bannerTitle.Foreground(Solid(color));
    bannerTitle.Visibility(title.empty() ? Visibility::Collapsed : Visibility::Visible);
    bannerText.Text(hstring(message));
    bannerText.Foreground(Solid(kTextSecondary));
    banner.Background(Solid(Rgba(color.R, color.G, color.B, 22)));
    banner.BorderBrush(Solid(Rgba(color.R, color.G, color.B, 96)));
    banner.Visibility(Visibility::Visible);
}

void MainWindow::Impl::ClearInfo() {
    banner.Visibility(Visibility::Collapsed);
}

void MainWindow::Impl::SetChip(Item& item, std::wstring const& text, winrt::Windows::UI::Color foreground,
                               winrt::Windows::UI::Color background) {
    item.chipText.Text(hstring(text));
    item.chipText.Foreground(Solid(foreground));
    item.chip.Background(Solid(background));
}

void MainWindow::Impl::Post(std::function<void()> action) {
    auto self = shared_from_this();
    queue.TryEnqueue([self, action]() {
        if (self->closed) return;
        if (action) action();
    });
}

void MainWindow::Impl::StartConversion() {
    if (running.load()) return;

    std::vector<std::shared_ptr<Item>> targets;
    for (auto const& item : items) {
        auto state = item->check.IsChecked();
        if (state && state.Value()) targets.push_back(item);
    }
    if (targets.empty()) {
        SetInfo(L"没有选中文件", L"请至少勾选一个文件再开始转换。", NoticeKind::Warning);
        return;
    }

    bool sameDirectory = true;
    if (auto custom = customDirRadio.IsChecked(); custom && custom.Value()) sameDirectory = false;

    std::wstring outputDirectory;
    if (!sameDirectory) {
        outputDirectory = customDirectory;
        if (outputDirectory.empty()) {
            SetInfo(L"请选择输出目录", L"「指定目录」模式下需要先选择一个文件夹。", NoticeKind::Warning);
            return;
        }
        std::error_code ec;
        if (!std::filesystem::exists(outputDirectory, ec)) {
            std::filesystem::create_directories(outputDirectory, ec);
            if (ec || !std::filesystem::exists(outputDirectory, ec)) {
                SetInfo(L"无法创建输出目录", L"请换一个可以写入的文件夹。", NoticeKind::Error);
                return;
            }
        }
    }

    const bool overwrite = overwriteSwitch.IsOn();

    ClearInfo();
    running = true;
    SetConvertEnabled(false);
    openFolderButton.Visibility(Visibility::Collapsed);
    overallBar->SetPercent(0);
    statusText.Text(hstring(L"准备转换…"));

    auto self = shared_from_this();
    const int total = static_cast<int>(targets.size());

    std::thread([self, targets, sameDirectory, outputDirectory, overwrite, total]() {
        int done = 0;
        int succeeded = 0;
        std::wstring lastDirectory;

        for (auto const& item : targets) {
            if (self->closed.load()) break;

            self->Post([self, item]() { self->SetItemRunning(item); });

            const std::wstring output = ResolveOutputPath(item->path, sameDirectory, outputDirectory, overwrite);
            auto progress = [self, item](int percent) {
                self->Post([self, item, percent]() { self->SetItemProgress(item, percent); });
            };
            ncmlib::ConvertOutcome outcome = ncmlib::ConvertNcmToFlac(item->path, output, overwrite, progress);

            ++done;
            if (outcome.ok) {
                ++succeeded;
                std::filesystem::path written(outcome.outputPath);
                lastDirectory = written.parent_path().wstring();
            }
            self->Post([self, item, outcome, done, total]() { self->FinishItem(item, outcome, done, total); });
        }

        self->Post([self, done, total, succeeded, lastDirectory]() {
            self->FinishAll(done, total, succeeded, lastDirectory);
        });
    }).detach();
}

void MainWindow::Impl::SetItemRunning(std::shared_ptr<Item> const& item) {
    item->state = Item::State::Running;
    SetChip(*item, L"转换中", kWarning, Rgba(255, 203, 112, 40));
    item->bar->SetVisibility(Visibility::Visible);
    item->bar->SetPercent(0);
    statusText.Text(hstring(L"正在转换：" + item->name));
}

void MainWindow::Impl::SetItemProgress(std::shared_ptr<Item> const& item, int percent) {
    if (item->state != Item::State::Running) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    item->bar->SetPercent(percent);
    SetChip(*item, L"转换中 " + std::to_wstring(percent) + L"%", kWarning, Rgba(255, 203, 112, 40));
}

void MainWindow::Impl::FinishItem(std::shared_ptr<Item> const& item, ncmlib::ConvertOutcome const& outcome, int done,
                                  int total) {
    if (total > 0) overallBar->SetPercent(100.0 * done / total);

    if (outcome.ok) {
        item->state = Item::State::Done;
        SetChip(*item, L"完成", kSuccess, Rgba(122, 226, 148, 40));
        item->bar->SetVisibility(Visibility::Collapsed);

        std::wstring detail = outcome.summary;
        if (!detail.empty()) detail += L" · ";
        detail += FormatBytes(outcome.bytes);
        item->subtitleText.Text(hstring(detail));
        if (!outcome.title.empty()) item->titleText.Text(hstring(outcome.title));
    } else {
        item->state = Item::State::Failed;
        SetChip(*item, L"失败", kDanger, Rgba(255, 138, 138, 40));
        item->bar->SetVisibility(Visibility::Collapsed);
        item->subtitleText.Text(hstring(outcome.message.empty() ? L"转换失败" : outcome.message));
    }

    statusText.Text(hstring(L"进度 " + std::to_wstring(done) + L" / " + std::to_wstring(total)));
}

void MainWindow::Impl::FinishAll(int done, int total, int succeeded, std::wstring const& lastDirectory) {
    running = false;
    SetConvertEnabled(!items.empty());
    overallBar->SetPercent(total > 0 ? 100.0 : 0.0);

    if (succeeded > 0 && !lastDirectory.empty()) {
        lastOutputDirectory = lastDirectory;
        openFolderButton.Visibility(Visibility::Visible);
    }

    if (done < total) {
        statusText.Text(hstring(L"已停止 · 完成 " + std::to_wstring(succeeded) + L" / " + std::to_wstring(total)));
    } else if (succeeded == total) {
        statusText.Text(hstring(L"全部完成 · 共 " + std::to_wstring(succeeded) + L" 个文件"));
    } else {
        statusText.Text(hstring(L"完成 " + std::to_wstring(succeeded) + L" / " + std::to_wstring(total) + L"，" +
                                std::to_wstring(total - succeeded) + L" 个失败"));
        SetInfo(L"部分文件转换失败", L"失败原因显示在对应文件的下方。", NoticeKind::Warning);
    }
}

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------
MainWindow::MainWindow() : m_impl(std::make_shared<Impl>()) {
    m_impl->Build();
}

MainWindow::~MainWindow() = default;

void MainWindow::Activate() {
    m_impl->window.Activate();
}

winrt::Microsoft::UI::Xaml::FrameworkElement MainWindow::Root() const {
    return m_impl->root;
}

namespace {

void DumpNode(winrt::Microsoft::UI::Xaml::DependencyObject const& node, int depth, std::wstring& out) {
    using namespace winrt::Microsoft::UI::Xaml;

    std::wstring line(static_cast<size_t>(depth) * 2, L' ');
    line += std::wstring(winrt::get_class_name(node));

    if (auto element = node.try_as<FrameworkElement>()) {
        wchar_t size[64] = {};
        swprintf_s(size, L" %.0fx%.0f", element.ActualWidth(), element.ActualHeight());
        line += size;
        if (element.Visibility() != Visibility::Visible) line += L" (hidden)";
    }
    if (auto block = node.try_as<TextBlock>()) {
        std::wstring value(block.Text());
        if (!value.empty()) line += std::wstring(L" text=\"") + value + L"\"";
    }
    out += line + L"\n";

    const int children = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(node);
    for (int index = 0; index < children; ++index) {
        DumpNode(winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(node, index), depth + 1, out);
    }
}

void WriteBinary(std::wstring const& path, std::vector<uint8_t> const& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void WriteText(std::wstring const& path, std::wstring const& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

} // namespace

void MainWindow::DumpTreeTo(std::wstring const& treePath) {
    m_impl->root.UpdateLayout();
    std::wstring tree;
    DumpNode(m_impl->root, 0, tree);
    WriteText(treePath, tree);
}

winrt::Windows::Foundation::IAsyncAction MainWindow::SaveSnapshot(std::wstring pngPath, std::wstring treePath) {
    using namespace winrt::Microsoft::UI::Xaml;

    const auto root = m_impl->root;
    root.UpdateLayout();

    Media::Imaging::RenderTargetBitmap bitmap;
    co_await bitmap.RenderAsync(root);

    const int width = static_cast<int>(bitmap.PixelWidth());
    const int height = static_cast<int>(bitmap.PixelHeight());

    const auto buffer = co_await bitmap.GetPixelsAsync();
    std::vector<uint8_t> pixels(buffer.Length());
    memcpy(pixels.data(), buffer.data(), pixels.size());

    auto stream = winrt::Windows::Storage::Streams::InMemoryRandomAccessStream();
    auto encoder = co_await winrt::Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(
        winrt::Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(), stream);
    encoder.SetPixelData(winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
                         winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied,
                         static_cast<uint32_t>(width), static_cast<uint32_t>(height), 96.0, 96.0, pixels);
    co_await encoder.FlushAsync();

    stream.Seek(0);
    std::vector<uint8_t> png(stream.Size());
    if (!png.empty()) {
        winrt::Windows::Storage::Streams::DataReader reader(stream);
        co_await reader.LoadAsync(static_cast<uint32_t>(png.size()));
        reader.ReadBytes(png);
    }
    WriteBinary(pngPath, png);

    std::wstring tree;
    DumpNode(root, 0, tree);
    WriteText(treePath, tree);
}

void MainWindow::AddPaths(std::vector<std::wstring> const& paths) {
    int added = 0;
    for (auto const& path : paths) {
        if (m_impl->AddFile(path)) ++added;
    }
    if (added > 0) {
        m_impl->ClearInfo();
        m_impl->statusText.Text(hstring(L"已添加 " + std::to_wstring(added) + L" 个文件"));
    }
}