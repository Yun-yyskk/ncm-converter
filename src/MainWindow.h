#pragma once

#include <winrt/Windows.Foundation.h>

#include <memory>
#include <string>
#include <vector>

// Code-built WinUI 3 window: file list, output options, conversion runner.
class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(MainWindow const&) = delete;
    MainWindow& operator=(MainWindow const&) = delete;

    void Activate();

    // Queues files handed to the process on the command line (drag onto the exe,
    // "open with", shell "send to"). Must be called on the UI thread.
    void AddPaths(std::vector<std::wstring> const& paths);

    // Test hook: renders the window content to a PNG and dumps the live visual
    // tree (element sizes and text) next to it. Used by tools/snapshot.ps1.
    winrt::Windows::Foundation::IAsyncAction SaveSnapshot(std::wstring pngPath, std::wstring treePath);

    // Test hook: writes the live visual tree (element sizes and text) only.
    void DumpTreeTo(std::wstring const& treePath);

    winrt::Microsoft::UI::Xaml::FrameworkElement Root() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};