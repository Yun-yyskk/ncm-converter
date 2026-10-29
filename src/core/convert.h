#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ncmlib {

struct ConvertOutcome {
    bool ok = false;
    std::wstring message;      // user-facing result or failure description
    std::wstring outputPath;
    std::wstring format;       // "FLAC" or "MP3", the stream the container held
    uint64_t bytes = 0;
    std::wstring summary;      // "44.1 kHz · 2 ch · 16 bit · 03:25"
    std::wstring title;        // track title when known
    std::wstring artist;
};

// Converts one .ncm file. The container decides the output format: a lossless
// payload becomes a tagged FLAC, an MPEG payload becomes a tagged MP3 (the
// audio frames are copied, never re-encoded). Progress is reported as 0..100.
// An empty outputPath puts the result next to the source; the extension is
// always corrected to match the stream.
ConvertOutcome ConvertNcm(const std::wstring& inputPath, const std::wstring& outputPath, bool overwrite,
                          const std::function<void(int)>& onProgress = {});

// Replaces the extension with ".flac" and keeps the source folder. The
// converter swaps in ".mp3" when the container turns out to hold MPEG audio.
std::wstring DefaultOutputPath(const std::wstring& inputPath);

} // namespace ncmlib
