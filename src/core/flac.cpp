#include "pch.h"

#include "core/flac.h"

namespace ncmlib {
namespace {

constexpr uint8_t kBlockStreamInfo = 0;
constexpr uint8_t kBlockVorbisComment = 4;
constexpr uint8_t kBlockPicture = 6;

uint32_t ReadBe24(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

void AppendU32Le(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void AppendU32Be(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void AppendBlockHeader(std::vector<uint8_t>& out, uint8_t type, bool last, size_t length) {
    out.push_back(static_cast<uint8_t>(type | (last ? 0x80 : 0x00)));
    out.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(length & 0xFF));
}

bool ParseStreamInfo(const uint8_t* payload, size_t length, FlacStreamInfo& info) {
    if (length < 34) return false;
    const uint8_t* p = payload;
    info.sampleRate = (static_cast<uint32_t>(p[10]) << 12) | (static_cast<uint32_t>(p[11]) << 4) |
                      (static_cast<uint32_t>(p[12]) >> 4);
    info.channels = ((static_cast<uint32_t>(p[12]) >> 1) & 0x07) + 1;
    info.bitsPerSample = (((static_cast<uint32_t>(p[12]) & 0x01) << 4) | (static_cast<uint32_t>(p[13]) >> 4)) + 1;
    info.totalSamples = (static_cast<uint64_t>(p[13] & 0x0F) << 32) | (static_cast<uint64_t>(p[14]) << 24) |
                        (static_cast<uint64_t>(p[15]) << 16) | (static_cast<uint64_t>(p[16]) << 8) |
                        static_cast<uint64_t>(p[17]);
    return info.sampleRate > 0;
}

bool ProbeJpeg(const std::vector<uint8_t>& bytes, uint32_t& width, uint32_t& height, uint32_t& depth) {
    size_t pos = 2;
    while (pos + 4 <= bytes.size()) {
        if (bytes[pos] != 0xFF) {
            ++pos;
            continue;
        }
        const uint8_t marker = bytes[pos + 1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        if (pos + 4 > bytes.size()) return false;
        const uint32_t segmentLength =
            (static_cast<uint32_t>(bytes[pos + 2]) << 8) | static_cast<uint32_t>(bytes[pos + 3]);
        const bool isStartOfFrame = (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
                                    (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
        if (isStartOfFrame && pos + 10 <= bytes.size()) {
            const uint8_t precision = bytes[pos + 4];
            height = (static_cast<uint32_t>(bytes[pos + 5]) << 8) | bytes[pos + 6];
            width = (static_cast<uint32_t>(bytes[pos + 7]) << 8) | bytes[pos + 8];
            const uint8_t components = bytes[pos + 9];
            depth = static_cast<uint32_t>(precision) * (components > 0 ? components : 3);
            return width > 0 && height > 0;
        }
        if (segmentLength < 2) return false;
        pos += 2 + segmentLength;
    }
    return false;
}

bool ProbePng(const std::vector<uint8_t>& bytes, uint32_t& width, uint32_t& height, uint32_t& depth) {
    if (bytes.size() < 33) return false;
    if (memcmp(bytes.data() + 12, "IHDR", 4) != 0) return false;
    width = (static_cast<uint32_t>(bytes[16]) << 24) | (static_cast<uint32_t>(bytes[17]) << 16) |
            (static_cast<uint32_t>(bytes[18]) << 8) | bytes[19];
    height = (static_cast<uint32_t>(bytes[20]) << 24) | (static_cast<uint32_t>(bytes[21]) << 16) |
             (static_cast<uint32_t>(bytes[22]) << 8) | bytes[23];
    const uint8_t bitDepth = bytes[24];
    const uint8_t colorType = bytes[25];
    uint32_t channels = 3;
    switch (colorType) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: channels = 3; break;
    }
    depth = static_cast<uint32_t>(bitDepth) * channels;
    return width > 0 && height > 0;
}

} // namespace

bool ProbeImage(const std::vector<uint8_t>& bytes, std::string& mime, uint32_t& width, uint32_t& height,
                uint32_t& depth, uint32_t& colors) {
    width = height = 0;
    depth = 24;
    colors = 0;
    if (bytes.size() < 16) return false;

    const uint8_t* p = bytes.data();
    if (p[0] == 0xFF && p[1] == 0xD8) {
        mime = "image/jpeg";
        return ProbeJpeg(bytes, width, height, depth);
    }
    if (p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4E && p[3] == 0x47) {
        mime = "image/png";
        return ProbePng(bytes, width, height, depth);
    }
    if (memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0) {
        mime = "image/webp";
        width = height = 0;
        return false;
    }
    mime = "application/octet-stream";
    return false;
}

bool ReadFlacStreamInfo(const std::vector<uint8_t>& flac, FlacStreamInfo& info) {
    info = FlacStreamInfo{};
    if (flac.size() < 8 || memcmp(flac.data(), "fLaC", 4) != 0) return false;

    size_t pos = 4;
    while (pos + 4 <= flac.size()) {
        const uint8_t header = flac[pos];
        const uint8_t type = header & 0x7F;
        const size_t length = ReadBe24(&flac[pos + 1]);
        const size_t payload = pos + 4;
        if (payload + length > flac.size()) return false;
        if (type == kBlockStreamInfo) return ParseStreamInfo(&flac[payload], length, info);
        if ((header & 0x80) != 0) return false;
        pos = payload + length;
    }
    return false;
}

bool BuildTaggedFlac(const std::vector<uint8_t>& source, const std::vector<FlacTag>& tags, const FlacPicture* picture,
                     std::vector<uint8_t>& out, FlacStreamInfo& info, std::wstring& error) {
    out.clear();
    info = FlacStreamInfo{};
    if (source.size() < 8 || memcmp(source.data(), "fLaC", 4) != 0) {
        error = L"解密后的码流不是 FLAC。";
        return false;
    }

    size_t pos = 4;
    const uint8_t* streamInfoPayload = nullptr;
    size_t streamInfoLength = 0;
    size_t audioStart = 0;
    bool foundStreamInfo = false;
    bool foundLast = false;

    while (pos + 4 <= source.size()) {
        const uint8_t header = source[pos];
        const bool isLast = (header & 0x80) != 0;
        const uint8_t type = header & 0x7F;
        const size_t length = ReadBe24(&source[pos + 1]);
        const size_t payload = pos + 4;
        if (payload + length > source.size()) {
            error = L"FLAC 元数据块损坏。";
            return false;
        }
        if (type == kBlockStreamInfo && !foundStreamInfo) {
            streamInfoPayload = &source[payload];
            streamInfoLength = length;
            foundStreamInfo = true;
            ParseStreamInfo(streamInfoPayload, streamInfoLength, info);
        }
        pos = payload + length;
        if (isLast) {
            audioStart = pos;
            foundLast = true;
            break;
        }
    }

    if (!foundStreamInfo) {
        error = L"FLAC 缺少 STREAMINFO。";
        return false;
    }
    if (!foundLast) audioStart = pos;

    out.reserve(source.size() + 4096);
    out.push_back('f');
    out.push_back('L');
    out.push_back('a');
    out.push_back('C');

    // STREAMINFO keeps its original payload (including the audio MD5).
    AppendBlockHeader(out, kBlockStreamInfo, false, streamInfoLength);
    out.insert(out.end(), streamInfoPayload, streamInfoPayload + streamInfoLength);

    // Vorbis comment.
    std::vector<FlacTag> effectiveTags;
    effectiveTags.reserve(tags.size());
    for (const FlacTag& tag : tags) {
        if (!tag.key.empty() && !tag.value.empty()) effectiveTags.push_back(tag);
    }

    std::vector<uint8_t> comment;
    const std::string vendor = "NcmToFlac";
    AppendU32Le(comment, static_cast<uint32_t>(vendor.size()));
    comment.insert(comment.end(), vendor.begin(), vendor.end());
    AppendU32Le(comment, static_cast<uint32_t>(effectiveTags.size()));
    for (const FlacTag& tag : effectiveTags) {
        const std::string entry = tag.key + "=" + tag.value;
        AppendU32Le(comment, static_cast<uint32_t>(entry.size()));
        comment.insert(comment.end(), entry.begin(), entry.end());
    }

    const bool hasPicture = picture != nullptr && !picture->data.empty();

    AppendBlockHeader(out, kBlockVorbisComment, !hasPicture, comment.size());
    out.insert(out.end(), comment.begin(), comment.end());

    if (hasPicture) {
        std::vector<uint8_t> pictureBlock;
        AppendU32Be(pictureBlock, 3); // front cover
        AppendU32Be(pictureBlock, static_cast<uint32_t>(picture->mime.size()));
        pictureBlock.insert(pictureBlock.end(), picture->mime.begin(), picture->mime.end());
        AppendU32Be(pictureBlock, static_cast<uint32_t>(picture->description.size()));
        pictureBlock.insert(pictureBlock.end(), picture->description.begin(), picture->description.end());
        AppendU32Be(pictureBlock, picture->width);
        AppendU32Be(pictureBlock, picture->height);
        AppendU32Be(pictureBlock, picture->depth);
        AppendU32Be(pictureBlock, picture->colors);
        AppendU32Be(pictureBlock, static_cast<uint32_t>(picture->data.size()));
        pictureBlock.insert(pictureBlock.end(), picture->data.begin(), picture->data.end());

        AppendBlockHeader(out, kBlockPicture, true, pictureBlock.size());
        out.insert(out.end(), pictureBlock.begin(), pictureBlock.end());
    }

    out.insert(out.end(), source.begin() + audioStart, source.end());
    return true;
}

} // namespace ncmlib