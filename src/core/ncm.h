#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncmlib {

enum class AudioCodec {
    Unknown = 0,
    Flac,
    Mp3,
};

struct NcmContent {
    std::string metaJson;             // UTF-8 JSON blob describing the track
    std::string coverMime;            // "image/jpeg" etc., empty when no cover
    std::vector<uint8_t> cover;       // embedded album art bytes
    std::vector<uint8_t> audio;       // decrypted stream (fLaC or MPEG audio)
    uint8_t version = 0;              // container version byte
    AudioCodec codec = AudioCodec::Unknown; // what the decrypted stream carries
};

// Opens an .ncm file, unwraps the container and returns the decrypted audio
// stream plus metadata. On failure returns false and fills error with a
// user-facing message.
bool DecodeNcmFile(const std::wstring& path, NcmContent& out, std::wstring& error);

} // namespace ncmlib
