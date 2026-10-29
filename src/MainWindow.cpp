#include "pch.h"

#include "MainWindow.h"

#include "core/convert.h"

#include <shellapi.h>
#include <shobjidl_core.h>

#include <commctrl.h>
#include <dwmapi.h>

#include <winrt/Windows.UI.ViewManagement.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

#include <algorithm>
#include <chrono>
#include <cmath>
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
// Palette. Colours follow the Windows light/dark preference and the accent
// colour, so the window looks native on every machine.
// ---------------------------------------------------------------------------
winrt::Windows::UI::Color Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF) {
    return winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b);
}

struct Palette {
    bool dark = false;
    winrt::Windows::UI::Color surface{};
    winrt::Windows::UI::Color card{};
    winrt::Windows::UI::Color stroke{};
    winrt::Windows::UI::Color textPrimary{};
    winrt::Windows::UI::Color textSecondary{};
    winrt::Windows::UI::Color textTertiary{};
    winrt::Windows::UI::Color accent{};
    winrt::Windows::UI::Color accentAlt{};
    winrt::Windows::UI::Color success{};
    winrt::Windows::UI::Color danger{};
    winrt::Windows::UI::Color warning{};
};

Palette g_palette;

uint8_t Blend(uint8_t value, double amount) {
    const double mixed = static_cast<double>(value) + (255.0 - static_cast<double>(value)) * amount;
    if (mixed < 0.0) return 0;
    if (mixed > 255.0) return 255;
    return static_cast<uint8_t>(mixed);
}

winrt::Windows::UI::Color Lighten(winrt::Windows::UI::Color color, double amount) {
    return Rgba(Blend(color.R, amount), Blend(color.G, amount), Blend(color.B, amount), color.A);
}

winrt::Windows::UI::Color Tint(winrt::Windows::UI::Color color, uint8_t alpha) {
    return Rgba(color.R, color.G, color.B, alpha);
}

// Fixed brand blue - the same blue Windows ships as its default accent, so the
// tool always looks like a Windows app instead of borrowing the user's colour.
winrt::Windows::UI::Color WindowsBlue(bool dark) {
    return dark ? Rgba(0x60, 0xCD, 0xFF) : Rgba(0x00, 0x5F, 0xB8);
}

bool SystemPrefersDark() {
    try {
        winrt::Windows::UI::ViewManagement::UISettings settings;
        const auto background = settings.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Background);
        const double luminance = 0.299 * background.R + 0.587 * background.G + 0.114 * background.B;
        return luminance < 128.0;
    } catch (...) {
        return false;
    }
}

Palette MakePalette(bool dark) {
    Palette palette;
    palette.dark = dark;
    palette.accent = WindowsBlue(dark);
    // Just a breath of a gradient: the second stop is a touch lighter, so
    // buttons and badges read as surfaces rather than flat rectangles.
    palette.accentAlt = Lighten(palette.accent, dark ? 0.34 : 0.16);
    if (dark) {
        palette.surface = Rgba(32, 32, 32);
        palette.card = Rgba(43, 43, 43);
        palette.stroke = Rgba(255, 255, 255, 18);
        palette.textPrimary = Rgba(255, 255, 255);
        palette.textSecondary = Rgba(200, 200, 200);
        palette.textTertiary = Rgba(148, 148, 148);
        palette.success = Rgba(108, 203, 95);
        palette.danger = Rgba(255, 153, 164);
        palette.warning = Rgba(252, 225, 0);
    } else {
        palette.surface = Rgba(243, 243, 243);
        palette.card = Rgba(252, 252, 252);
        palette.stroke = Rgba(0, 0, 0, 16);
        palette.textPrimary = Rgba(26, 26, 26);
        palette.textSecondary = Rgba(96, 96, 96);
        palette.textTertiary = Rgba(128, 128, 128);
        palette.success = Rgba(15, 123, 15);
        palette.danger = Rgba(196, 43, 28);
        palette.warning = Rgba(157, 93, 0);
    }
    return palette;
}

double LinearChannel(uint8_t channel) {
    const double value = channel / 255.0;
    return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

// Ink for labels drawn on top of the accent fill. Uses the WCAG luminance
// cut-off, so pale accents (yellow, light blue) get dark text instead of
// unreadable white.
winrt::Windows::UI::Color OnAccent(winrt::Windows::UI::Color accent) {
    const double luminance =
        0.2126 * LinearChannel(accent.R) + 0.7152 * LinearChannel(accent.G) + 0.0722 * LinearChannel(accent.B);
    return luminance > 0.179 ? Rgba(20, 20, 20) : Rgba(255, 255, 255);
}

bool SameColor(winrt::Windows::UI::Color a, winrt::Windows::UI::Color b) {
    return a.A == b.A && a.R == b.R && a.G == b.G && a.B == b.B;
}

SolidColorBrush Solid(winrt::Windows::UI::Color const& color) { return SolidColorBrush(color); }
SolidColorBrush Transparent() { return SolidColorBrush(Rgba(0, 0, 0, 0)); }

// Faint surface used for cards, rows and dividers. It is derived from the
// foreground ink, so one alpha value reads correctly in both themes.
winrt::Windows::UI::Color SurfaceInk(uint8_t alpha) {
    if (g_palette.dark) return Rgba(255, 255, 255, alpha);
    return Rgba(0, 0, 0, static_cast<uint8_t>(std::min<int>(255, alpha + 10)));
}

SolidColorBrush WhiteFill(uint8_t alpha) { return SolidColorBrush(SurfaceInk(alpha)); }

LinearGradientBrush AccentGradient() {
    LinearGradientBrush brush;
    brush.StartPoint(Point{ 0.0f, 0.0f });
    brush.EndPoint(Point{ 1.0f, 1.0f });
    GradientStop first;
    first.Color(g_palette.accent);
    first.Offset(0.0);
    GradientStop second;
    second.Color(g_palette.accentAlt);
    second.Offset(1.0);
    brush.GradientStops().Append(first);
    brush.GradientStops().Append(second);
    return brush;
}

// Recolours brushes that were painted with the previous palette. Colours are
// matched exactly against the palette entries; translucent white or black
// fills flip their ink when the theme changes.
winrt::Windows::UI::Color RemapColor(winrt::Windows::UI::Color color, Palette const& from, Palette const& to) {
    if (SameColor(color, from.surface)) return to.surface;
    if (SameColor(color, from.card)) return to.card;
    if (SameColor(color, from.stroke)) return to.stroke;
    if (SameColor(color, from.textPrimary)) return to.textPrimary;
    if (SameColor(color, from.textSecondary)) return to.textSecondary;
    if (SameColor(color, from.textTertiary)) return to.textTertiary;
    if (SameColor(color, from.accent)) return to.accent;
    if (SameColor(color, from.accentAlt)) return to.accentAlt;
    if (SameColor(color, from.success)) return to.success;
    if (SameColor(color, from.danger)) return to.danger;
    if (SameColor(color, from.warning)) return to.warning;
    if (from.dark == to.dark || color.A == 0xFF) return color;

    const bool paleInk = color.R > 200 && color.G > 200 && color.B > 200;
    const bool darkInk = color.R < 40 && color.G < 40 && color.B < 40;
    if (paleInk || darkInk) {
        const uint8_t ink = to.dark ? 255 : 0;
        return Rgba(ink, ink, ink, color.A);
    }
    return color;
}

void RecolorBrush(Brush const& brush, Palette const& from, Palette const& to) {
    if (!brush) return;
    if (auto solid = brush.try_as<SolidColorBrush>()) {
        const auto current = solid.Color();
        const auto remapped = RemapColor(current, from, to);
        if (!SameColor(current, remapped)) solid.Color(remapped);
        return;
    }
    if (auto gradient = brush.try_as<LinearGradientBrush>()) {
        const auto stops = gradient.GradientStops();
        if (stops.Size() == 2) {
            stops.GetAt(0).Color(to.accent);
            stops.GetAt(1).Color(to.accentAlt);
        }
    }
}

void RecolorTree(DependencyObject const& node, Palette const& from, Palette const& to) {
    if (!node) return;
    if (auto text = node.try_as<TextBlock>()) {
        RecolorBrush(text.Foreground(), from, to);
    } else if (auto icon = node.try_as<FontIcon>()) {
        RecolorBrush(icon.Foreground(), from, to);
    } else if (auto border = node.try_as<Border>()) {
        RecolorBrush(border.Background(), from, to);
        RecolorBrush(border.BorderBrush(), from, to);
    } else if (auto grid = node.try_as<Grid>()) {
        RecolorBrush(grid.Background(), from, to);
    }

    const int count = Media::VisualTreeHelper::GetChildrenCount(node);
    for (int index = 0; index < count; ++index) {
        RecolorTree(Media::VisualTreeHelper::GetChild(node, index), from, to);
    }
}

// Explorer still turns a drop over a window that accepted files through
// DragAcceptFiles into WM_DROPFILES, so the shell drop path is handled at the
// HWND level instead of through the XAML drag/drop API.
std::function<void(std::vector<std::wstring> const&)> g_filesDropped;
std::vector<HWND> g_dropWindows;

LRESULT CALLBACK DropSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message != WM_DROPFILES) return DefSubclassProc(hwnd, message, wParam, lParam);

    auto drop = reinterpret_cast<HDROP>(wParam);
    std::vector<std::wstring> paths;

    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) continue;
        std::wstring path(length, L'\0');
        if (DragQueryFileW(drop, index, path.data(), length + 1) != 0) {
            path.resize(length);
            paths.push_back(std::move(path));
        }
    }
    DragFinish(drop);

    if (!paths.empty() && g_filesDropped) g_filesDropped(paths);
    return 0;
}

void RegisterDropWindow(HWND hwnd) {
    if (!hwnd) return;
    for (HWND registered : g_dropWindows) {
        if (registered == hwnd) return;
    }
    DragAcceptFiles(hwnd, TRUE);
    SetWindowSubclass(hwnd, DropSubclassProc, 1, 0);
    g_dropWindows.push_back(hwnd);
}

// The drop target has to be on the HWND that actually sits under the cursor,
// and WinUI creates its content child windows asynchronously, so the scan runs
// again a moment after the window is up.
void RegisterDropTree(HWND hwnd) {
    RegisterDropWindow(hwnd);
    EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM) -> BOOL {
            RegisterDropWindow(child);
            return TRUE;
        },
        0);
}

// Deepest folder that still contains every converted file, used by the
// "open folder" button when the outputs ended up in different directories.
std::wstring CommonDirectory(std::vector<std::wstring> const& directories) {
    if (directories.empty()) return {};
    std::wstring common = directories.front();
    for (size_t index = 1; index < directories.size(); ++index) {
        const std::wstring& other = directories[index];
        const size_t limit = (std::min)(common.size(), other.size());
        size_t shared = 0;
        while (shared < limit && towlower(common[shared]) == towlower(other[shared])) ++shared;
        common.resize(shared);
        const size_t separator = common.find_last_of(L"\\/");
        common.resize(separator == std::wstring::npos ? 0 : separator);
        if (common.empty()) return {};
    }
    return common;
}

// WinUI draws CheckBox, RadioButton and ToggleSwitch with the system accent
// brushes, which on this machine is a muddy brown. Seeding the application
// resources with our own accent keeps those stock controls on brand.
void SeedAccentResources() {
    auto insert = [](wchar_t const* key, winrt::Windows::Foundation::IInspectable const& value) {
        try {
            Application::Current().Resources().Insert(box_value(hstring(key)), value);
        } catch (...) {
            // A locked-down resource dictionary only costs the accent colour.
        }
    };
    auto color = [&](wchar_t const* key, winrt::Windows::UI::Color value) { insert(key, box_value(value)); };
    auto brush = [&](wchar_t const* key, winrt::Windows::UI::Color value) { insert(key, Solid(value)); };

    color(L"SystemAccentColor", g_palette.accent);
    color(L"SystemAccentColorLight1", Lighten(g_palette.accent, 0.12));
    color(L"SystemAccentColorLight2", Lighten(g_palette.accent, 0.24));
    color(L"SystemAccentColorLight3", Lighten(g_palette.accent, 0.36));
    color(L"SystemAccentColorDark1", Lighten(g_palette.accent, -0.12));
    color(L"SystemAccentColorDark2", Lighten(g_palette.accent, -0.24));
    color(L"SystemAccentColorDark3", Lighten(g_palette.accent, -0.36));

    brush(L"AccentFillColorDefaultBrush", g_palette.accent);
    const auto accent = g_palette.accent;
    brush(L"AccentFillColorSecondaryBrush", Rgba(accent.R, accent.G, accent.B, 0xE6));
    brush(L"AccentFillColorTertiaryBrush", Rgba(accent.R, accent.G, accent.B, 0xCC));
    brush(L"SystemControlHighlightAccentBrush", g_palette.accent);
    brush(L"SystemControlForegroundAccentBrush", g_palette.accent);
    brush(L"SystemControlBackgroundAccentBrush", g_palette.accent);
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
    icon.HorizontalAlignment(HorizontalAlignment::Center);
    icon.VerticalAlignment(VerticalAlignment::Center);
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
    card.Background(Solid(g_palette.card));
    card.BorderBrush(Solid(g_palette.stroke));
    card.BorderThickness(Thickness{ 1, 1, 1, 1 });
    card.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
    card.Padding(Thickness{ 16, 14, 16, 14 });
    return card;
}

Button MakeButton(Symbol symbol, std::wstring const& text) {
    StackPanel content;
    content.Orientation(Orientation::Horizontal);
    content.Spacing(5);
    content.VerticalAlignment(VerticalAlignment::Center);

    FontIcon icon = MakeIcon(symbol, 14, g_palette.textPrimary);
    icon.VerticalAlignment(VerticalAlignment::Center);

    TextBlock label = MakeText(text, 14, g_palette.textPrimary);
    label.VerticalAlignment(VerticalAlignment::Center);

    content.Children().Append(icon);
    content.Children().Append(label);

    Button button;
    button.Content(content);
    button.Background(Solid(g_palette.card));
    button.BorderBrush(Solid(g_palette.stroke));
    button.BorderThickness(Thickness{ 1, 1, 1, 1 });
    button.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
    button.Padding(Thickness{ 12, 5, 14, 5 });
    button.MinHeight(32);
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

// CheckBox and RadioButton own a 20px glyph that cannot be resized without
// retemplating, and retemplating is the code path that fail-fasts on this
// machine. These two paint the same affordance at 16px from primitives.
class LiteCheck {
public:
    explicit LiteCheck(double size = 16) {
        m_root = Border();
        m_root.Width(size);
        m_root.Height(size);
        m_root.CornerRadius(CornerRadius{ 3, 3, 3, 3 });
        m_root.BorderThickness(Thickness{ 1, 1, 1, 1 });
        m_root.HorizontalAlignment(HorizontalAlignment::Center);
        m_root.VerticalAlignment(VerticalAlignment::Center);

        m_tick = FontIcon();
        m_tick.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));
        m_tick.Glyph(L"\uE73E");
        m_tick.FontSize(10);
        m_tick.HorizontalAlignment(HorizontalAlignment::Center);
        m_tick.VerticalAlignment(VerticalAlignment::Center);
        m_root.Child(m_tick);

        // Mark the tap handled so a row that forwards taps to this box does not
        // toggle it a second time on the way up the tree.
        m_root.Tapped([this](Inspectable const&, TappedRoutedEventArgs const& args) {
            Toggle();
            args.Handled(true);
        });
        Paint();
    }

    Border Root() const { return m_root; }
    bool IsChecked() const { return m_checked; }

    void SetChecked(bool value) {
        if (value == m_checked) return;
        m_checked = value;
        Paint();
        if (m_onChange) m_onChange(m_checked);
    }

    void SetCheckedSilently(bool value) {
        if (value == m_checked) {
            Paint();
            return;
        }
        m_checked = value;
        Paint();
    }

    void SetEnabled(bool value) {
        m_enabled = value;
        m_root.Opacity(value ? 1.0 : 0.45);
    }

    void OnChange(std::function<void(bool)> handler) { m_onChange = std::move(handler); }
    void Toggle() {
        if (!m_enabled) return;
        SetChecked(!m_checked);
    }
    void Repaint() { Paint(); }

private:
    void Paint() {
        const auto outline = g_palette.dark ? Rgba(255, 255, 255, 120) : Rgba(0, 0, 0, 120);
        m_root.Background(m_checked ? Solid(g_palette.accent) : Transparent());
        m_root.BorderBrush(m_checked ? Solid(g_palette.accent) : Solid(outline));
        m_tick.Foreground(Solid(OnAccent(g_palette.accent)));
        m_tick.Visibility(m_checked ? Visibility::Visible : Visibility::Collapsed);
    }

    Border m_root{ nullptr };
    FontIcon m_tick{ nullptr };
    bool m_checked = false;
    bool m_enabled = true;
    std::function<void(bool)> m_onChange;
};

class LiteRadio {
public:
    explicit LiteRadio(double size = 16) : m_size(size) {
        m_root = Border();
        m_root.Width(size);
        m_root.Height(size);
        m_root.CornerRadius(CornerRadius{ size / 2, size / 2, size / 2, size / 2 });
        m_root.BorderThickness(Thickness{ 1.5, 1.5, 1.5, 1.5 });
        m_root.Background(Transparent());
        m_root.HorizontalAlignment(HorizontalAlignment::Center);
        m_root.VerticalAlignment(VerticalAlignment::Center);

        m_dot = Border();
        m_dot.Width(size * 0.44);
        m_dot.Height(size * 0.44);
        m_dot.CornerRadius(CornerRadius{ size, size, size, size });
        m_dot.HorizontalAlignment(HorizontalAlignment::Center);
        m_dot.VerticalAlignment(VerticalAlignment::Center);
        m_dot.Background(Solid(g_palette.accent));
        m_root.Child(m_dot);

        m_root.Tapped([this](Inspectable const&, TappedRoutedEventArgs const& args) {
            SetChecked(true);
            args.Handled(true);
        });
        Paint();
    }

    Border Root() const { return m_root; }
    bool IsChecked() const { return m_checked; }

    void SetChecked(bool value) {
        if (value == m_checked) return;
        m_checked = value;
        Paint();
        if (m_onChange) m_onChange(m_checked);
    }

    void SetCheckedSilently(bool value) {
        if (value == m_checked) {
            Paint();
            return;
        }
        m_checked = value;
        Paint();
    }

    void OnChange(std::function<void(bool)> handler) { m_onChange = std::move(handler); }
    void Repaint() { Paint(); }

private:
    void Paint() {
        const auto outline = g_palette.dark ? Rgba(255, 255, 255, 120) : Rgba(0, 0, 0, 120);
        m_root.BorderBrush(Solid(m_checked ? g_palette.accent : outline));
        m_dot.Visibility(m_checked ? Visibility::Visible : Visibility::Collapsed);
    }

    Border m_root{ nullptr };
    Border m_dot{ nullptr };
    double m_size = 16;
    bool m_checked = false;
    std::function<void(bool)> m_onChange;
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
    std::shared_ptr<LiteCheck> check;
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
    std::shared_ptr<LiteCheck> selectAll;
    Button addButton{ nullptr };
    Button clearButton{ nullptr };
    std::shared_ptr<LiteRadio> sameDirRadio;
    std::shared_ptr<LiteRadio> customDirRadio;
    Border dirBox{ nullptr };
    TextBlock dirText{ nullptr };
    Button browseButton{ nullptr };
    Border banner{ nullptr };
    FontIcon bannerIcon{ nullptr };
    TextBlock bannerTitle{ nullptr };
    TextBlock bannerText{ nullptr };
    std::shared_ptr<LiteBar> overallBar;
    TextBlock statusText{ nullptr };

    // The footer only speaks while something is happening; an idle window shows
    // no status line and no progress track at all.
    void SetStatus(std::wstring const& text) {
        if (!statusText) return;
        statusText.Text(hstring(text));
        statusText.Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
    }
    Border convertButton{ nullptr };
    TextBlock convertLabel{ nullptr };
    bool convertEnabled = true;
    Button openFolderButton{ nullptr };

    std::vector<std::shared_ptr<Item>> items;
    std::atomic<bool> running{ false };
    std::atomic<bool> closed{ false };
    bool syncingSelectAll = false;
    std::wstring lastOutputDirectory;
    std::vector<std::wstring> outputRoots;
    FontIcon brandIcon{ nullptr };
    FontIcon convertIcon{ nullptr };
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
    void FinishAll(int done, int total, int succeeded, std::vector<std::wstring> const& outputDirectories);

    void SetChip(Item& item, std::wstring const& text, winrt::Windows::UI::Color foreground,
                 winrt::Windows::UI::Color background);

    void Post(std::function<void()> action);

    void InstallDropSupport();
    void ApplyPalette(bool dark);
    void SyncTheme();
    void ApplyNativeTitlebarTheme();
    void RecolorItemChips();
};

void MainWindow::Impl::Build() {
    window = Window();
    window.Title(L"NCM 转 FLAC");
    queue = DispatcherQueue::GetForCurrentThread();

    g_palette = MakePalette(SystemPrefersDark());
    SeedAccentResources();

    root = Grid();
    root.RequestedTheme(ElementTheme::Default);
    // Opaque window surface: keeps the palette readable over any backdrop.
    root.Background(Solid(g_palette.surface));
    root.RowDefinitions().Append(RowAuto());   // custom title bar
    root.RowDefinitions().Append(RowStar());   // content

    // ----- title bar --------------------------------------------------------
    // The caption is drawn by the system (drag strip only), which is what a
    // native Windows 11 window looks like.
    Grid titleBar;
    titleBar.Height(32);
    titleBar.Padding(Thickness{ 16, 0, 16, 0 });
    titleBar.VerticalAlignment(VerticalAlignment::Top);

    Grid::SetRow(titleBar, 0);
    root.Children().Append(titleBar);

    // ----- content ----------------------------------------------------------
    Grid content;
    content.Padding(Thickness{ 20, 4, 20, 16 });
    content.RowDefinitions().Append(RowAuto());  // hero
    content.RowDefinitions().Append(RowAuto());  // list
    content.RowDefinitions().Append(RowStar());  // slack
    content.RowDefinitions().Append(RowAuto());  // settings
    content.RowDefinitions().Append(RowAuto());  // info bar
    content.RowDefinitions().Append(RowAuto());  // footer
    Grid::SetRow(content, 1);
    root.Children().Append(content);

    // page header, Win11 settings style
    Grid hero;
    hero.Margin(Thickness{ 0, 0, 0, 10 });

    TextBlock heroTitle = MakeText(L"NCM 转 FLAC", 18, g_palette.textPrimary, true);
    hero.Children().Append(heroTitle);

    Grid::SetRow(hero, 0);
    content.Children().Append(hero);

    // file list card
    Border listCard = MakeCard();
    listCard.MinHeight(96);
    listCard.MaxHeight(300);
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
    listHeader.Margin(Thickness{ 0, 0, 0, 6 });

    StackPanel listTitleStack;
    listTitleStack.Orientation(Orientation::Horizontal);
    listTitleStack.Spacing(8);
    listTitleStack.VerticalAlignment(VerticalAlignment::Center);
    listTitleStack.Children().Append(MakeText(L"文件列表", 13, g_palette.textPrimary, true));

    countText = MakeText(L"暂无文件", 11.5, g_palette.textTertiary);
    countText.VerticalAlignment(VerticalAlignment::Center);
    listTitleStack.Children().Append(countText);
    listHeader.Children().Append(listTitleStack);

    selectAll = std::make_shared<LiteCheck>(16);
    selectAll->OnChange([this](bool value) {
        if (!syncingSelectAll) SetAllChecked(value);
    });

    StackPanel selectAllRow;
    selectAllRow.Orientation(Orientation::Horizontal);
    selectAllRow.Spacing(6);
    selectAllRow.VerticalAlignment(VerticalAlignment::Center);
    selectAllRow.Margin(Thickness{ 0, 0, 10, 0 });
    selectAllRow.Children().Append(selectAll->Root());

    TextBlock selectAllLabel = MakeText(L"全选", 12, g_palette.textSecondary);
    selectAllLabel.VerticalAlignment(VerticalAlignment::Center);
    selectAllRow.Children().Append(selectAllLabel);
    selectAllRow.Tapped([this](Inspectable const&, TappedRoutedEventArgs const&) { selectAll->Toggle(); });
    Grid::SetColumn(selectAllRow, 1);

    addButton = MakeButton(Symbol::Add, L"添加文件");
    addButton.Margin(Thickness{ 0, 0, 8, 0 });
    Grid::SetColumn(addButton, 2);

    clearButton = MakeButton(Symbol::Delete, L"清空");
    Grid::SetColumn(clearButton, 3);

    listHeader.Children().Append(selectAllRow);
    listHeader.Children().Append(addButton);
    listHeader.Children().Append(clearButton);

    Grid::SetRow(listHeader, 0);
    listGrid.Children().Append(listHeader);

    Grid listHost;
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

    FontIcon emptyIcon = MakeIcon(Symbol::MusicInfo, 26, SurfaceInk(70));
    emptyIcon.HorizontalAlignment(HorizontalAlignment::Center);
    emptyState.Children().Append(emptyIcon);

    TextBlock emptyTitle = MakeText(L"还没有文件", 13, g_palette.textSecondary, true);
    emptyTitle.HorizontalAlignment(HorizontalAlignment::Center);
    emptyState.Children().Append(emptyTitle);

    listHost.Children().Append(emptyState);
    listGrid.Children().Append(listHost);

    // output settings card
    Border settingsCard = MakeCard();
    settingsCard.Margin(Thickness{ 0, 10, 0, 0 });
    Grid::SetRow(settingsCard, 2);
    content.Children().Append(settingsCard);

    StackPanel settings;
    settings.Spacing(8);

    StackPanel outputHeader;
    outputHeader.Orientation(Orientation::Horizontal);
    outputHeader.Spacing(4);
    outputHeader.VerticalAlignment(VerticalAlignment::Center);
    outputHeader.Children().Append(MakeText(L"输出位置", 13, g_palette.textPrimary, true));

    sameDirRadio = std::make_shared<LiteRadio>(16);
    sameDirRadio->SetCheckedSilently(true);
    customDirRadio = std::make_shared<LiteRadio>(16);
    sameDirRadio->OnChange([this](bool value) {
        if (!value) return;
        customDirRadio->SetCheckedSilently(false);
        UpdateOutputMode();
    });
    customDirRadio->OnChange([this](bool value) {
        if (!value) return;
        sameDirRadio->SetCheckedSilently(false);
        UpdateOutputMode();
    });

    auto makeRadioLabel = [this](std::shared_ptr<LiteRadio> const& radio, std::wstring const& text) {
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(6);
        row.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(radio->Root());
        TextBlock label = MakeText(text, 12.5, g_palette.textPrimary);
        label.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(label);
        row.Tapped([radio](Inspectable const&, TappedRoutedEventArgs const&) { radio->SetChecked(true); });
        return row;
    };

    StackPanel radioRow;
    radioRow.Orientation(Orientation::Horizontal);
    radioRow.Spacing(18);
    radioRow.Margin(Thickness{ 14, 0, 0, 0 });
    radioRow.VerticalAlignment(VerticalAlignment::Center);
    radioRow.Children().Append(makeRadioLabel(sameDirRadio, L"源文件所在目录"));
    radioRow.Children().Append(makeRadioLabel(customDirRadio, L"指定目录"));
    outputHeader.Children().Append(radioRow);
    settings.Children().Append(outputHeader);

    Grid dirRow;
    dirRow.ColumnDefinitions().Append(ColStar());
    dirRow.ColumnDefinitions().Append(ColAuto());

    dirBox = Border();
    dirBox.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
    dirBox.Background(Solid(g_palette.card));
    dirBox.BorderBrush(Solid(g_palette.stroke));
    dirBox.BorderThickness(Thickness{ 1, 1, 1, 1 });
    dirBox.Padding(Thickness{ 10, 5, 10, 7 });
    dirBox.MinHeight(32);
    dirBox.Margin(Thickness{ 0, 0, 8, 0 });
    dirBox.VerticalAlignment(VerticalAlignment::Center);
    dirBox.Opacity(0.55);

    Grid dirInner;
    dirInner.ColumnDefinitions().Append(ColAuto());
    dirInner.ColumnDefinitions().Append(ColStar());

    FontIcon dirIcon = MakeIcon(Symbol::Folder, 12, g_palette.textTertiary);
    dirIcon.Margin(Thickness{ 0, 0, 6, 0 });
    dirIcon.VerticalAlignment(VerticalAlignment::Center);

    dirText = MakeText(L"未选择目录", 12.5, g_palette.textTertiary);
    dirText.TextTrimming(TextTrimming::CharacterEllipsis);
    dirText.TextWrapping(TextWrapping::NoWrap);
    dirText.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(dirText, 1);

    dirInner.Children().Append(dirIcon);
    dirInner.Children().Append(dirText);
    dirBox.Child(dirInner);

    browseButton = MakeButton(Symbol::Folder, L"浏览");
    browseButton.MinHeight(32);
    browseButton.VerticalAlignment(VerticalAlignment::Center);
    browseButton.IsEnabled(false);
    Grid::SetColumn(browseButton, 1);

    dirRow.Children().Append(dirBox);
    dirRow.Children().Append(browseButton);
    settings.Children().Append(dirRow);

    settingsCard.Child(settings);

    // inline notice banner (InfoBar fail-fasts on this machine, so it is drawn
    // from a Border plus text blocks instead)
    banner = Border();
    banner.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
    banner.BorderThickness(Thickness{ 1, 1, 1, 1 });
    banner.Padding(Thickness{ 10, 7, 10, 7 });
    banner.Margin(Thickness{ 0, 10, 0, 0 });
    banner.Visibility(Visibility::Collapsed);

    Grid bannerInner;
    bannerInner.ColumnDefinitions().Append(ColAuto());
    bannerInner.ColumnDefinitions().Append(ColStar());

    bannerIcon = FontIcon();
    bannerIcon.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));
    bannerIcon.FontSize(14);
    bannerIcon.Margin(Thickness{ 0, 1, 8, 0 });
    bannerIcon.VerticalAlignment(VerticalAlignment::Top);

    StackPanel bannerStack;
    bannerStack.Spacing(2);

    bannerTitle = MakeText(L"", 12, g_palette.textPrimary, true);
    bannerTitle.Visibility(Visibility::Collapsed);

    bannerText = MakeText(L"", 12, g_palette.textSecondary);
    bannerText.TextWrapping(TextWrapping::Wrap);

    bannerStack.Children().Append(bannerTitle);
    bannerStack.Children().Append(bannerText);
    Grid::SetColumn(bannerStack, 1);

    bannerInner.Children().Append(bannerIcon);
    bannerInner.Children().Append(bannerStack);
    banner.Child(bannerInner);

    Grid::SetRow(banner, 4);
    content.Children().Append(banner);

    // footer
    Grid footer;
    footer.Margin(Thickness{ 0, 10, 0, 0 });
    footer.ColumnDefinitions().Append(ColStar());
    footer.ColumnDefinitions().Append(ColAuto());
    footer.ColumnDefinitions().Append(ColAuto());
    Grid::SetRow(footer, 5);
    content.Children().Append(footer);

    StackPanel statusStack;
    statusStack.Spacing(6);
    statusStack.VerticalAlignment(VerticalAlignment::Center);

    statusText = MakeText(L"", 11.5, g_palette.textSecondary);
    statusText.Visibility(Visibility::Collapsed);
    statusStack.Children().Append(statusText);

    overallBar = std::make_shared<LiteBar>(3, g_palette.accentAlt, 26);
    overallBar->Root().HorizontalAlignment(HorizontalAlignment::Stretch);
    overallBar->Root().Margin(Thickness{ 0, 0, 20, 0 });
    overallBar->SetVisibility(Visibility::Collapsed);
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
    convertButton.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
    convertButton.Padding(Thickness{ 18, 0, 20, 0 });
    convertButton.MinWidth(120);
    convertButton.Height(32);
    convertButton.Background(Solid(g_palette.accent));
    convertButton.VerticalAlignment(VerticalAlignment::Center);

    StackPanel convertStack;
    convertStack.Orientation(Orientation::Horizontal);
    convertStack.Spacing(7);
    convertStack.VerticalAlignment(VerticalAlignment::Center);
    convertIcon = MakeIcon(Symbol::Play, 13, OnAccent(g_palette.accent));
    convertStack.Children().Append(convertIcon);

    convertLabel = MakeText(L"开始转换", 14, OnAccent(g_palette.accent), true);
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
    window.AppWindow().Resize({ static_cast<int32_t>(640 * scale), static_cast<int32_t>(360 * scale) });

    root.ActualThemeChanged([this](FrameworkElement const&, Inspectable const&) { SyncTheme(); });

    addButton.Click([this](Inspectable const&, RoutedEventArgs const&) { AddFilesFromDialog(); });
    clearButton.Click([this](Inspectable const&, RoutedEventArgs const&) {
        items.clear();
        listPanel.Children().Clear();
        ClearInfo();
        overallBar->SetPercent(0);
        SetStatus(L"");
        RefreshCounters();
    });

    auto chooseDirectory = [this]() {
        auto picked = PickPaths(true, L"选择输出目录");
        if (picked.empty()) return;
        customDirectory = picked.front();
        dirText.Text(hstring(customDirectory));
        dirText.Foreground(Solid(g_palette.textPrimary));
        customDirRadio->SetCheckedSilently(true);
        sameDirRadio->SetCheckedSilently(false);
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
        std::wstring target = CommonDirectory(outputRoots);
        if (target.empty()) target = lastOutputDirectory;
        if (target.empty()) return;
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });

    window.Closed([this](Inspectable const&, WindowEventArgs const&) {
        closed = true;
        for (HWND hwnd : g_dropWindows) {
            DragAcceptFiles(hwnd, FALSE);
            RemoveWindowSubclass(hwnd, DropSubclassProc, 1);
        }
        g_dropWindows.clear();
        g_filesDropped = nullptr;
    });

    RefreshCounters();
}

void MainWindow::Impl::InstallDropSupport() {
    g_filesDropped = [this](std::vector<std::wstring> const& paths) {
        if (closed.load()) return;

        int added = 0;
        for (auto const& path : paths) {
            if (AddFile(path)) ++added;
        }

        if (added > 0) {
            ClearInfo();
            SetStatus(L"");
            RefreshCounters();
        } else {
            SetInfo(L"没有可用的文件", L"拖进来的内容里没有 .ncm 文件，或者已经在列表中了。", NoticeKind::Info);
        }
    };

    HWND hwnd = nullptr;
    try {
        hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(window.AppWindow().Id());
    } catch (...) {
        return;
    }
    if (!hwnd) return;

    RegisterDropTree(hwnd);

    try {
        auto rescan = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
        rescan.Interval(std::chrono::milliseconds(700));
        std::weak_ptr<Impl> weak = weak_from_this();
        rescan.Tick([weak, hwnd, rescan](auto const&, auto const&) mutable {
            rescan.Stop();
            if (weak.lock()) RegisterDropTree(hwnd);
        });
        rescan.Start();
    } catch (...) {
    }

}

void MainWindow::Impl::ApplyNativeTitlebarTheme() {
    HWND hwnd = nullptr;
    try {
        hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(window.AppWindow().Id());
    } catch (...) {
        return;
    }
    if (!hwnd) return;

    const BOOL dark = g_palette.dark ? TRUE : FALSE;
    constexpr DWORD kImmersiveDarkMode = 20;
    DwmSetWindowAttribute(hwnd, kImmersiveDarkMode, &dark, sizeof(dark));

    const COLORREF border = g_palette.dark ? RGB(48, 48, 48) : RGB(222, 222, 222);
    constexpr DWORD kBorderColor = 34;
    DwmSetWindowAttribute(hwnd, kBorderColor, &border, sizeof(border));

    // Windows 11 rounds window corners at the DWM level; ask for it explicitly
    // so the app matches the shell even when it is not the foreground window.
    constexpr DWORD kCornerPreference = 33;
    constexpr int kRound = 2;
    DwmSetWindowAttribute(hwnd, kCornerPreference, &kRound, sizeof(kRound));
}

void MainWindow::Impl::ApplyPalette(bool dark) {
    const Palette previous = g_palette;
    g_palette = MakePalette(dark);
    SeedAccentResources();

    root.Background(Solid(g_palette.surface));
    RecolorTree(root, previous, g_palette);

    const auto ink = OnAccent(g_palette.accent);
    if (convertLabel) convertLabel.Foreground(Solid(ink));
    if (convertIcon) convertIcon.Foreground(Solid(ink));
    if (brandIcon) brandIcon.Foreground(Solid(Rgba(255, 255, 255)));

    // Native buttons carry their face on the Control, not on a Border, so the
    // visual-tree walk below never reaches it; repaint the four we own.
    for (Button const& button : { addButton, clearButton, browseButton, openFolderButton }) {
        if (!button) continue;
        button.Background(Solid(g_palette.card));
        button.BorderBrush(Solid(g_palette.stroke));
    }

    if (selectAll) selectAll->Repaint();
    if (sameDirRadio) sameDirRadio->Repaint();
    if (customDirRadio) customDirRadio->Repaint();
    for (auto const& item : items) {
        if (item->check) item->check->Repaint();
    }

    RecolorItemChips();
    ClearInfo();
    ApplyNativeTitlebarTheme();
}

void MainWindow::Impl::SyncTheme() {
    try {
        const bool dark = root.ActualTheme() == ElementTheme::Dark;
        if (dark == g_palette.dark) return;
        ApplyPalette(dark);
    } catch (...) {
        // A colour refresh must never take the window down; the original
        // palette simply stays in place until the next change.
    }
}

void MainWindow::Impl::RecolorItemChips() {
    for (auto const& item : items) {
        switch (item->state) {
        case Item::State::Pending:
            SetChip(*item, L"待转换", g_palette.textSecondary, SurfaceInk(14));
            break;
        case Item::State::Running:
            SetChip(*item, L"转换中", g_palette.warning, Tint(g_palette.warning, 40));
            break;
        case Item::State::Done:
            SetChip(*item, L"完成", g_palette.success, Tint(g_palette.success, 40));
            break;
        case Item::State::Failed:
            SetChip(*item, L"失败", g_palette.danger, Tint(g_palette.danger, 40));
            break;
        }
    }
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
        SetStatus(L"");
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
    row.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
    row.Background(Transparent());
    row.BorderThickness(Thickness{ 0, 0, 0, 0 });
    row.Padding(Thickness{ 0, 4, 0, 4 });
    row.Margin(Thickness{ 0, 0, 0, 1 });

    Grid layout;
    layout.ColumnDefinitions().Append(ColAuto());
    layout.ColumnDefinitions().Append(ColAuto());
    layout.ColumnDefinitions().Append(ColStar());
    layout.ColumnDefinitions().Append(ColAuto());
    layout.ColumnDefinitions().Append(ColAuto());
    layout.ColumnSpacing(8);

    auto check = std::make_shared<LiteCheck>(16);
    check->SetCheckedSilently(true);
    item->check = check;
    layout.Children().Append(check->Root());

    Border iconBorder;
    iconBorder.Width(26);
    iconBorder.Height(26);
    iconBorder.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
    iconBorder.Background(WhiteFill(12));
    iconBorder.Child(MakeIcon(Symbol::MusicInfo, 12, g_palette.textSecondary));
    iconBorder.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(iconBorder, 1);
    layout.Children().Append(iconBorder);

    StackPanel textStack;
    textStack.Spacing(2);
    textStack.VerticalAlignment(VerticalAlignment::Center);

    item->titleText = MakeText(item->name, 13, g_palette.textPrimary);
    item->titleText.TextWrapping(TextWrapping::NoWrap);
    item->titleText.TextTrimming(TextTrimming::CharacterEllipsis);

    item->subtitleText = MakeText(sizeText + L" · NCM", 11.5, g_palette.textTertiary);
    item->subtitleText.TextWrapping(TextWrapping::NoWrap);
    item->subtitleText.TextTrimming(TextTrimming::CharacterEllipsis);

    item->bar = std::make_shared<LiteBar>(3, g_palette.accent, 24);
    item->bar->Root().Margin(Thickness{ 0, 6, 0, 0 });
    item->bar->SetVisibility(Visibility::Collapsed);

    textStack.Children().Append(item->titleText);
    textStack.Children().Append(item->subtitleText);
    textStack.Children().Append(item->bar->Root());
    Grid::SetColumn(textStack, 2);
    layout.Children().Append(textStack);

    Border chip;
    chip.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
    chip.Padding(Thickness{ 7, 1, 7, 1 });
    chip.VerticalAlignment(VerticalAlignment::Center);
    chip.Background(WhiteFill(12));
    item->chipText = MakeText(L"待转换", 11, g_palette.textSecondary);
    chip.Child(item->chipText);
    item->chip = chip;
    Grid::SetColumn(chip, 3);
    layout.Children().Append(chip);

    Button remove = MakeButton(Symbol::Cancel, L"");
    remove.MinWidth(24);
    remove.Padding(Thickness{ 3, 3, 3, 3 });
    remove.Background(Transparent());
    remove.BorderThickness(Thickness{ 0, 0, 0, 0 });
    remove.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(remove, 4);
    layout.Children().Append(remove);

    row.Child(layout);
    item->row = row;

    auto hoverOn = [row](Inspectable const&, PointerRoutedEventArgs const&) {
        row.Background(WhiteFill(static_cast<uint8_t>(g_palette.dark ? 10 : 12)));
    };
    auto hoverOff = [row](Inspectable const&, PointerRoutedEventArgs const&) { row.Background(Transparent()); };
    row.PointerEntered(hoverOn);
    row.PointerExited(hoverOff);

    auto weak = weak_from_this();
    remove.Click([weak, item](Inspectable const&, RoutedEventArgs const&) {
        if (auto self = weak.lock()) self->RemoveItem(item);
    });
    check->OnChange([weak](bool) {
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
        item->check->SetCheckedSilently(checked);
        syncingSelectAll = false;
    }
    RefreshCounters();
}

void MainWindow::Impl::RefreshCounters() {
    int selected = 0;
    for (auto const& item : items) {
        if (item->check->IsChecked()) ++selected;
    }

    if (items.empty()) {
        countText.Text(hstring(L"暂无文件"));
    } else {
        countText.Text(hstring(std::to_wstring(items.size()) + L" 个文件"));
    }

    syncingSelectAll = true;
    selectAll->SetCheckedSilently(!items.empty() && selected == static_cast<int>(items.size()));
    syncingSelectAll = false;

    emptyState.Visibility(items.empty() ? Visibility::Visible : Visibility::Collapsed);
    SetConvertEnabled(!items.empty() && !running.load());
    selectAll->SetEnabled(!items.empty());
}

void MainWindow::Impl::SetConvertEnabled(bool enabled) {
    convertEnabled = enabled;
    convertButton.Opacity(enabled ? 1.0 : 0.45);
}

void MainWindow::Impl::UpdateOutputMode() {
    const bool enabled = customDirRadio->IsChecked();
    dirBox.Opacity(enabled ? 1.0 : 0.55);
    browseButton.IsEnabled(enabled);
}

void MainWindow::Impl::SetInfo(std::wstring const& title, std::wstring const& message, NoticeKind kind) {
    winrt::Windows::UI::Color color = g_palette.accentAlt;
    hstring glyph = L"\uE946";
    if (kind == NoticeKind::Success) {
        color = g_palette.success;
        glyph = L"\uE930";
    } else if (kind == NoticeKind::Warning) {
        color = g_palette.warning;
        glyph = L"\uE7BA";
    } else if (kind == NoticeKind::Error) {
        color = g_palette.danger;
        glyph = L"\uE783";
    }

    bannerIcon.Glyph(glyph);
    bannerIcon.Foreground(Solid(color));
    bannerTitle.Text(hstring(title));
    bannerTitle.Foreground(Solid(color));
    bannerTitle.Visibility(title.empty() ? Visibility::Collapsed : Visibility::Visible);
    bannerText.Text(hstring(message));
    bannerText.Foreground(Solid(g_palette.textSecondary));
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
        if (item->check->IsChecked()) targets.push_back(item);
    }
    if (targets.empty()) {
        SetInfo(L"没有选中文件", L"请至少勾选一个文件再开始转换。", NoticeKind::Warning);
        return;
    }

    const bool sameDirectory = !customDirRadio->IsChecked();

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

    const bool overwrite = true;

    ClearInfo();
    running = true;
    SetConvertEnabled(false);
    openFolderButton.Visibility(Visibility::Collapsed);
    overallBar->SetPercent(0);
    overallBar->SetVisibility(Visibility::Visible);
    SetStatus(L"准备转换…");

    auto self = shared_from_this();
    const int total = static_cast<int>(targets.size());

    std::thread([self, targets, sameDirectory, outputDirectory, overwrite, total]() {
        int done = 0;
        int succeeded = 0;
        std::vector<std::wstring> outputDirectories;

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
                const std::filesystem::path written(outcome.outputPath);
                const std::wstring directory = written.parent_path().wstring();
                bool known = false;
                for (auto const& existing : outputDirectories) {
                    if (_wcsicmp(existing.c_str(), directory.c_str()) == 0) {
                        known = true;
                        break;
                    }
                }
                if (!known) outputDirectories.push_back(directory);
            }
            self->Post([self, item, outcome, done, total]() { self->FinishItem(item, outcome, done, total); });
        }

        self->Post([self, done, total, succeeded, outputDirectories]() {
            self->FinishAll(done, total, succeeded, outputDirectories);
        });
    }).detach();
}

void MainWindow::Impl::SetItemRunning(std::shared_ptr<Item> const& item) {
    item->state = Item::State::Running;
    SetChip(*item, L"转换中", g_palette.warning, Tint(g_palette.warning, 40));
    item->bar->SetVisibility(Visibility::Visible);
    item->bar->SetPercent(0);
    SetStatus(L"正在转换：" + item->name);
}

void MainWindow::Impl::SetItemProgress(std::shared_ptr<Item> const& item, int percent) {
    if (item->state != Item::State::Running) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    item->bar->SetPercent(percent);
    SetChip(*item, L"转换中 " + std::to_wstring(percent) + L"%", g_palette.warning, Rgba(255, 203, 112, 40));
}

void MainWindow::Impl::FinishItem(std::shared_ptr<Item> const& item, ncmlib::ConvertOutcome const& outcome, int done,
                                  int total) {
    if (total > 0) overallBar->SetPercent(100.0 * done / total);

    if (outcome.ok) {
        item->state = Item::State::Done;
        SetChip(*item, L"完成", g_palette.success, Tint(g_palette.success, 40));
        item->bar->SetVisibility(Visibility::Collapsed);

        std::wstring detail = outcome.summary;
        if (!detail.empty()) detail += L" · ";
        detail += FormatBytes(outcome.bytes);
        item->subtitleText.Text(hstring(detail));
        if (!outcome.title.empty()) item->titleText.Text(hstring(outcome.title));
    } else {
        item->state = Item::State::Failed;
        SetChip(*item, L"失败", g_palette.danger, Tint(g_palette.danger, 40));
        item->bar->SetVisibility(Visibility::Collapsed);
        item->subtitleText.Text(hstring(outcome.message.empty() ? L"转换失败" : outcome.message));
    }

    SetStatus(L"进度 " + std::to_wstring(done) + L" / " + std::to_wstring(total));
}

void MainWindow::Impl::FinishAll(int done, int total, int succeeded, std::vector<std::wstring> const& outputDirectories) {
    running = false;
    SetConvertEnabled(!items.empty());
    overallBar->SetPercent(total > 0 ? 100.0 : 0.0);

    outputRoots = outputDirectories;
    if (succeeded > 0 && !outputDirectories.empty()) {
        lastOutputDirectory = outputDirectories.front();
        openFolderButton.Visibility(Visibility::Visible);
    }

    std::wstring summary;
    if (done < total) {
        summary = L"已停止 · 完成 " + std::to_wstring(succeeded) + L" / " + std::to_wstring(total);
    } else if (succeeded == total) {
        summary = L"全部完成 · 共 " + std::to_wstring(succeeded) + L" 个文件";
    } else {
        summary = L"完成 " + std::to_wstring(succeeded) + L" / " + std::to_wstring(total) + L"，" +
                  std::to_wstring(total - succeeded) + L" 个失败";
        SetInfo(L"部分文件转换失败", L"失败原因显示在对应文件的下方。", NoticeKind::Warning);
    }

    if (outputDirectories.size() > 1) summary += L" · 已写入各自的源目录";
    SetStatus(summary);
    overallBar->SetPercent(100);
    overallBar->SetVisibility(Visibility::Collapsed);
}

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------
MainWindow::MainWindow() : m_impl(std::make_shared<Impl>()) {
    m_impl->Build();
}

MainWindow::~MainWindow() = default;

void MainWindow::RunConversionNow() {
    if (m_impl) m_impl->StartConversion();
}

void MainWindow::Activate() {
    m_impl->window.Activate();
    m_impl->ApplyNativeTitlebarTheme();
    m_impl->InstallDropSupport();
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
        m_impl->SetStatus(L"");
    }
}