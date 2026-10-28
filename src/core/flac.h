#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncmlib {

struct FlacStreamInfo {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bitsPerSample = 0;
    uint64_t totalSamples = 0;
};

struct FlacTag {
    std::string key;
    std::string value; // UTF-8
};

struct FlacPicture {
    std::string mime;
    std::string description;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 24;
    uint32_t colors = 0;
    std::vector<uint8_t> data;
};

// Reads the STREAMINFO of an in-memory FLAC stream.
bool ReadFlacStreamInfo(const std::vector<uint8_t>& flac, FlacStreamInfo& info);

// Rewrites a FLAC stream with a fresh Vorbis comment block (and optionally a
// front-cover picture block). The audio frames are copied verbatim, so the
// STREAMINFO MD5 stays valid.
bool BuildTaggedFlac(const std::vector<uint8_t>& source, const std::vector<FlacTag>& tags, const FlacPicture* picture,
                     std::vector<uint8_t>& out, FlacStreamInfo& info, std::wstring& error);

// Detects the image format and its pixel dimensions.
bool ProbeImage(const std::vector<uint8_t>& bytes, std::string& mime, uint32_t& width, uint32_t& height,
                uint32_t& depth, uint32_t& colors);

} // namespace ncmlib
