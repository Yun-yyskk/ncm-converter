#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ncmlib {

struct ConvertOutcome {
    bool ok = false;
    std::wstring message;      // user-facing result or failure description
    std::wstring outputPath;
    uint64_t bytes = 0;
    std::wstring summary;      // "44.1 kHz · 2 ch · 16 bit · 03:25"
    std::wstring title;        // track title when known
    std::wstring artist;
};

// Converts one .ncm file into a tagged FLAC. Progress is reported as 0..100.
ConvertOutcome ConvertNcmToFlac(const std::wstring& inputPath, const std::wstring& outputPath, bool overwrite,
                                const std::function<void(int)>& onProgress = {});

// Replaces the extension with ".flac" and keeps the source folder.
std::wstring DefaultOutputPath(const std::wstring& inputPath);

} // namespace ncmlib
