#include "pch.h"

#include "MainWindow.h"

#include "core/convert.h"

#include <shellapi.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace {

// Set by --ui-snapshot and --ui-run, empty for normal interactive runs.
std::wstring g_snapshotDirectory;
// --ui-run also starts the conversion, so the whole pipeline can be verified.
bool g_autoConvert = false;
std::string ToUtf8(std::wstring const& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

void WriteText(std::wstring const& path, std::wstring const& text) {
    const std::string utf8 = ToUtf8(text);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

// This is a GUI subsystem executable, so no console is attached: every result
// is reported through files next to the artifacts.
void LogStatus(std::wstring const& directory, std::wstring const& message) {
    static const ULONGLONG start = GetTickCount64();
    const std::wstring line = L"[" + std::to_wstring(GetTickCount64() - start) + L"ms] " + message + L"\n";
    const std::string utf8 = ToUtf8(line);
    std::ofstream file(directory + L"\\ui-status.txt", std::ios::binary | std::ios::app);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

//   NcmToFlac.exe --convert <input.ncm> [output.flac]
// Drives the conversion core without the UI and reports the outcome as key=value
// lines in "<output>.log". Exit code: 0 on success, 1 on failure, 2 on misuse.
int RunHeadless(int argc, wchar_t** argv) {
    if (argc < 3) return 2;

    const std::wstring input = argv[2];
    const std::wstring output = argc >= 4 ? std::wstring(argv[3]) : ncmlib::DefaultOutputPath(input);

    const ncmlib::ConvertOutcome outcome = ncmlib::ConvertNcmToFlac(input, output, true, {});

    std::wstring report;
    report += L"ok=" + std::wstring(outcome.ok ? L"true" : L"false") + L"\n";
    report += L"message=" + outcome.message + L"\n";
    report += L"output=" + outcome.outputPath + L"\n";
    report += L"bytes=" + std::to_wstring(outcome.bytes) + L"\n";
    report += L"title=" + outcome.title + L"\n";
    report += L"artist=" + outcome.artist + L"\n";
    report += L"summary=" + outcome.summary + L"\n";
    WriteText(output + L".log", report);

    return outcome.ok ? 0 : 1;
}

} // namespace

winrt::fire_and_forget CaptureWindowAndExit(MainWindow* window, std::wstring directory);

struct App : ApplicationT<App> {
    // IApplicationOverrides::OnLaunched is a const, non-virtual shim target in
    // the C++/WinRT projection, so it must not carry an override specifier.
    void OnLaunched(LaunchActivatedEventArgs const&) const {
        window = std::make_unique<MainWindow>();
        window->Activate();
        if (!PendingPaths().empty()) window->AddPaths(PendingPaths());

        if (g_snapshotDirectory.empty()) return; // interactive run

        // Development hook: paint the drop hint so the snapshot captures it.
        wchar_t dropHint[32] = {};
        if (GetEnvironmentVariableW(L"NCM_SNAPSHOT_DROP", dropHint, 32) > 0) {
            const int count = static_cast<int>(std::wcstol(dropHint, nullptr, 10));
            window->PreviewDropHint(count > 0 ? count : 2);
        }

        LogStatus(g_snapshotDirectory, L"window activated");
        snapshotTimer = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
        snapshotTimer.Interval(std::chrono::milliseconds(250));
        snapshotTimer.Tick([this](auto const&, auto const&) {
            ++beats;
            if (g_autoConvert && beats == 4) window->RunConversionNow();
            // Give layout and the theme brushes two seconds to settle (eight
            // seconds when a conversion has to finish first), then render,
            // write the report and close.
            const int target = g_autoConvert ? 36 : 8;
            if (beats < target) return;
            snapshotTimer.Stop();
            CaptureWindowAndExit(window.get(), g_snapshotDirectory);
        });
        snapshotTimer.Start();
    }

    // Files handed to the process before Application::Start ran. A static is
    // used because make<App>() yields the projected Application type, which
    // cannot expose extra instance methods.
    static std::vector<std::wstring>& PendingPaths() {
        static std::vector<std::wstring> value;
        return value;
    }

    mutable std::unique_ptr<MainWindow> window;
    mutable int beats = 0;
    mutable winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer snapshotTimer{ nullptr };
};

winrt::fire_and_forget CaptureWindowAndExit(MainWindow* window, std::wstring directory) {
    try {
        co_await window->SaveSnapshot(directory + L"\\ui.png", directory + L"\\ui-tree.txt");
        LogStatus(directory, L"snapshot written");
    } catch (winrt::hresult_error const& error) {
        LogStatus(directory, L"snapshot failed: 0x" +
                                  std::to_wstring(static_cast<uint32_t>(error.code())) + L" " +
                                  std::wstring(error.message()));
    } catch (...) {
        LogStatus(directory, L"snapshot failed: unknown error");
    }
    Application::Current().Exit();
}

int RunGui() {
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // RegisterDragDrop needs OLE to be running on this thread, and that is a
    // separate step from the COM apartment: without OleInitialize it fails
    // with E_OUTOFMEMORY and drops are refused with the "no entry" cursor.
    const HRESULT ole = OleInitialize(nullptr);

    Application::Start([](auto&&) { winrt::make<App>(); });

    if (SUCCEEDED(ole)) OleUninitialize();
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv != nullptr && argc >= 2 && wcscmp(argv[1], L"--convert") == 0) {
        const int result = RunHeadless(argc, argv);
        LocalFree(argv);
        return result;
    }

    // Development hooks, both write <dir>\ui.png plus a visual tree dump before
    // exiting: --ui-snapshot only renders the window, --ui-run also converts the
    // files given after <dir>. Extra arguments are treated as files to load.
    if (argv != nullptr && argc >= 3 &&
        (wcscmp(argv[1], L"--ui-snapshot") == 0 || wcscmp(argv[1], L"--ui-run") == 0)) {
        g_autoConvert = wcscmp(argv[1], L"--ui-run") == 0;
        g_snapshotDirectory = argv[2];
        CreateDirectoryW(g_snapshotDirectory.c_str(), nullptr);
        for (int index = 3; index < argc; ++index) App::PendingPaths().emplace_back(argv[index]);
        LocalFree(argv);
        return RunGui();
    }

    // Interactive: any extra arguments are files to queue up (drag onto the exe,
    // "open with", or a shell shortcut).
    std::vector<std::wstring> initialPaths;
    for (int index = 1; argv != nullptr && index < argc; ++index) {
        initialPaths.emplace_back(argv[index]);
    }
    if (argv != nullptr) LocalFree(argv);
    App::PendingPaths() = std::move(initialPaths);

    return RunGui();
}
