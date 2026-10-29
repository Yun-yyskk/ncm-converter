#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncmlib {

struct Mp3StreamInfo {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bitrateKbps = 0;
    double seconds = 0.0; // estimated from the payload size and bitrate
};

// Reads the first MPEG audio frame header (skipping any ID3v2 tag). VBR files
// report their first frame's bitrate, so the duration stays an estimate.
bool ProbeMp3(const std::vector<uint8_t>& stream, Mp3StreamInfo& info);

struct Mp3Tags {
    std::string title;
    std::string artist;          // "A/B/C", stored as a single TPE1 frame
    std::string album;
    std::string albumArtist;
    std::string translatedTitle;
    std::string musicId;
    std::string comment;
    std::string lyrics;          // LRC text
    std::string encoder;
    std::string coverMime;       // e.g. "image/jpeg"
    const std::vector<uint8_t>* cover = nullptr;
};

// Writes an ID3v2.3 tag in front of the MPEG audio frames. A tag shipped inside
// the container and an ID3v1 trailer are dropped; the audio frames themselves
// are copied verbatim, so nothing is re-encoded.
bool BuildTaggedMp3(const std::vector<uint8_t>& source, const Mp3Tags& tags, std::vector<uint8_t>& out,
                    std::wstring& error);

} // namespace ncmlib
