#include "pch.h"

#include "core/id3.h"

#include <algorithm>

namespace ncmlib {
namespace {

constexpr uint32_t kBitrateMpeg1Layer3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
constexpr uint32_t kBitrateMpeg2Layer3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
constexpr uint32_t kSampleRateMpeg1[4] = {44100, 48000, 32000, 0};
constexpr uint32_t kSampleRateMpeg2[4] = {22050, 24000, 16000, 0};
constexpr uint32_t kSampleRateMpeg25[4] = {11025, 12000, 8000, 0};

uint32_t ReadSynchsafe(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0] & 0x7F) << 21) | (static_cast<uint32_t>(p[1] & 0x7F) << 14) |
           (static_cast<uint32_t>(p[2] & 0x7F) << 7) | static_cast<uint32_t>(p[3] & 0x7F);
}

void WriteBe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void WriteSynchsafe(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 21) & 0x7F));
    out.push_back(static_cast<uint8_t>((value >> 14) & 0x7F));
    out.push_back(static_cast<uint8_t>((value >> 7) & 0x7F));
    out.push_back(static_cast<uint8_t>(value & 0x7F));
}

std::vector<uint8_t> Utf16Le(const std::string& utf8) {
    std::vector<uint8_t> bytes;
    // The BOM is part of the encoding, so even an empty description carries one;
    // tools reject an empty UTF-16 field that has nothing in front of the terminator.
    bytes.push_back(0xFF);
    bytes.push_back(0xFE);
    if (utf8.empty()) return bytes;

    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wide <= 0) return bytes;
    std::wstring text(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), text.data(), wide);

    bytes.reserve(text.size() * 2 + 2);
    for (wchar_t character : text) {
        bytes.push_back(static_cast<uint8_t>(character & 0xFF));
        bytes.push_back(static_cast<uint8_t>((character >> 8) & 0xFF));
    }
    return bytes;
}

// A UTF-16 string that ends with the two zero bytes a terminated string needs.
std::vector<uint8_t> Utf16LeTerminated(const std::string& utf8) {
    std::vector<uint8_t> bytes = Utf16Le(utf8);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    return bytes;
}

void AppendFrame(std::vector<uint8_t>& tag, const char* id, const std::vector<uint8_t>& payload) {
    if (payload.empty() || payload.size() > 0xFFFFFF) return;
    tag.insert(tag.end(), id, id + 4);
    WriteBe32(tag, static_cast<uint32_t>(payload.size()));
    tag.push_back(0x00); // flags
    tag.push_back(0x00);
    tag.insert(tag.end(), payload.begin(), payload.end());
}

void AppendTextFrame(std::vector<uint8_t>& tag, const char* id, const std::string& value) {
    if (value.empty()) return;
    std::vector<uint8_t> payload;
    payload.push_back(0x01); // UTF-16 with BOM
    const std::vector<uint8_t> text = Utf16Le(value);
    payload.insert(payload.end(), text.begin(), text.end());
    AppendFrame(tag, id, payload);
}

void AppendTxxxFrame(std::vector<uint8_t>& tag, const std::string& description, const std::string& value) {
    if (value.empty()) return;
    std::vector<uint8_t> payload;
    payload.push_back(0x01);
    const std::vector<uint8_t> name = Utf16LeTerminated(description);
    payload.insert(payload.end(), name.begin(), name.end());
    const std::vector<uint8_t> text = Utf16Le(value);
    payload.insert(payload.end(), text.begin(), text.end());
    AppendFrame(tag, "TXXX", payload);
}

void AppendCommentFrame(std::vector<uint8_t>& tag, const std::string& text) {
    if (text.empty()) return;
    std::vector<uint8_t> payload;
    payload.push_back(0x01);
    payload.push_back('e');
    payload.push_back('n');
    payload.push_back('g');
    const std::vector<uint8_t> empty = Utf16LeTerminated(std::string());
    payload.insert(payload.end(), empty.begin(), empty.end());
    const std::vector<uint8_t> body = Utf16Le(text);
    payload.insert(payload.end(), body.begin(), body.end());
    AppendFrame(tag, "COMM", payload);
}

void AppendLyricsFrame(std::vector<uint8_t>& tag, const std::string& text) {
    if (text.empty()) return;
    std::vector<uint8_t> payload;
    payload.push_back(0x01);
    payload.push_back('e');
    payload.push_back('n');
    payload.push_back('g');
    const std::vector<uint8_t> empty = Utf16LeTerminated(std::string());
    payload.insert(payload.end(), empty.begin(), empty.end());
    const std::vector<uint8_t> body = Utf16Le(text);
    payload.insert(payload.end(), body.begin(), body.end());
    AppendFrame(tag, "USLT", payload);
}

void AppendPictureFrame(std::vector<uint8_t>& tag, const std::string& mime, const std::vector<uint8_t>& data) {
    if (data.empty()) return;
    std::vector<uint8_t> payload;
    payload.push_back(0x00); // ISO-8859-1 for the description
    payload.insert(payload.end(), mime.begin(), mime.end());
    payload.push_back(0x00);
    payload.push_back(0x03); // picture type: front cover
    payload.push_back(0x00); // empty description
    payload.insert(payload.end(), data.begin(), data.end());
    AppendFrame(tag, "APIC", payload);
}

} // namespace

bool ProbeMp3(const std::vector<uint8_t>& stream, Mp3StreamInfo& info) {
    size_t begin = 0;
    if (stream.size() >= 10 && memcmp(stream.data(), "ID3", 3) == 0) {
        const size_t size = ReadSynchsafe(stream.data() + 6);
        begin = 10 + size + ((stream[5] & 0x10) != 0 ? 10 : 0);
        if (begin >= stream.size()) return false;
    }

    const size_t limit = (std::min)(stream.size(), begin + 65536);
    for (size_t i = begin; i + 4 <= limit; ++i) {
        if (stream[i] != 0xFF || (stream[i + 1] & 0xE0) != 0xE0) continue;

        const uint8_t versionBits = static_cast<uint8_t>((stream[i + 1] >> 3) & 0x03);
        const uint8_t layerBits = static_cast<uint8_t>((stream[i + 1] >> 1) & 0x03);
        const uint8_t bitrateIndex = static_cast<uint8_t>((stream[i + 2] >> 4) & 0x0F);
        const uint8_t sampleIndex = static_cast<uint8_t>((stream[i + 2] >> 2) & 0x03);
        if (versionBits == 1 || layerBits == 0 || bitrateIndex == 0 || bitrateIndex == 0x0F || sampleIndex == 3) {
            continue;
        }

        uint32_t bitrate = 0;
        uint32_t sampleRate = 0;
        if (versionBits == 3) {
            bitrate = kBitrateMpeg1Layer3[bitrateIndex];
            sampleRate = kSampleRateMpeg1[sampleIndex];
        } else if (versionBits == 2) {
            bitrate = kBitrateMpeg2Layer3[bitrateIndex];
            sampleRate = kSampleRateMpeg2[sampleIndex];
        } else {
            bitrate = kBitrateMpeg2Layer3[bitrateIndex];
            sampleRate = kSampleRateMpeg25[sampleIndex];
        }
        if (bitrate == 0 || sampleRate == 0) continue;

        info.bitrateKbps = bitrate;
        info.sampleRate = sampleRate;
        info.channels = ((stream[i + 3] >> 6) == 3) ? 1u : 2u;
        const double bytes = static_cast<double>(stream.size() - i);
        info.seconds = bytes * 8.0 / (static_cast<double>(bitrate) * 1000.0);
        return true;
    }
    return false;
}

bool BuildTaggedMp3(const std::vector<uint8_t>& source, const Mp3Tags& tags, std::vector<uint8_t>& out,
                    std::wstring& error) {
    out.clear();

    size_t audioStart = 0;
    if (source.size() >= 10 && memcmp(source.data(), "ID3", 3) == 0) {
        const size_t size = ReadSynchsafe(source.data() + 6);
        size_t tagSize = 10 + size + ((source[5] & 0x10) != 0 ? 10 : 0);
        if (tagSize < source.size()) audioStart = tagSize;
    }

    size_t audioEnd = source.size();
    if (audioEnd >= audioStart + 128 && memcmp(source.data() + audioEnd - 128, "TAG", 3) == 0) {
        audioEnd -= 128;
    }
    if (audioStart >= audioEnd) {
        error = L"MP3 码流为空或已损坏。";
        return false;
    }

    std::vector<uint8_t> frames;
    AppendTextFrame(frames, "TIT2", tags.title);
    AppendTextFrame(frames, "TPE1", tags.artist);
    AppendTextFrame(frames, "TALB", tags.album);
    AppendTextFrame(frames, "TPE2", tags.albumArtist);
    AppendTextFrame(frames, "TIT3", tags.translatedTitle);
    AppendTextFrame(frames, "TENC", tags.encoder);
    AppendTxxxFrame(frames, "MUSIC_ID", tags.musicId);
    AppendCommentFrame(frames, tags.comment);
    AppendLyricsFrame(frames, tags.lyrics);
    if (tags.cover != nullptr) AppendPictureFrame(frames, tags.coverMime, *tags.cover);

    if (frames.size() > 0x0FFFFFFF) {
        error = L"标签数据过大，无法写入 ID3 头。";
        return false;
    }

    out.reserve(10 + frames.size() + (audioEnd - audioStart));
    out.push_back('I');
    out.push_back('D');
    out.push_back('3');
    out.push_back(0x03); // version 2.3
    out.push_back(0x00); // revision
    out.push_back(0x00); // flags
    WriteSynchsafe(out, static_cast<uint32_t>(frames.size()));
    out.insert(out.end(), frames.begin(), frames.end());
    out.insert(out.end(), source.begin() + audioStart, source.begin() + audioEnd);
    return true;
}

} // namespace ncmlib
