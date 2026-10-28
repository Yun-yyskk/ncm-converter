#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncmlib {

struct LyricLine {
    int64_t timeMs = 0;
    std::string text; // UTF-8
};

// Turns whatever the container stores as "lyric"/"tlyric" (plain LRC, a JSON
// wrapper, per-line JSON or karaoke YRC) into plain timed lines. Translated
// lines are merged onto the matching original line when available.
std::vector<LyricLine> ExtractLyrics(const std::string& metaJson);

// Renders lines as a standard "[mm:ss.xx]text" LRC document.
std::string BuildLrcText(const std::vector<LyricLine>& lines);

// Parses a standalone .lrc sidecar (also tolerates a JSON wrapper).
std::vector<LyricLine> ParseLrcSidecar(const std::string& text);

} // namespace ncmlib
