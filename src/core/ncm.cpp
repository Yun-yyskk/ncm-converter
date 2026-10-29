#include "pch.h"

#include "core/ncm.h"

#include "core/aes.h"
#include "core/base64.h"

#include <fstream>

namespace ncmlib {
namespace {

constexpr uint8_t kKeyCipher[16] = {0x68, 0x7A, 0x48, 0x52, 0x41, 0x6D, 0x73, 0x6F,
                                    0x35, 0x6B, 0x49, 0x6E, 0x62, 0x61, 0x78, 0x57};
constexpr uint8_t kMetaCipher[16] = {0x23, 0x31, 0x34, 0x6C, 0x6A, 0x6B, 0x5F, 0x21,
                                     0x5C, 0x5D, 0x26, 0x30, 0x55, 0x3C, 0x27, 0x28};

constexpr char kNcmMagic[8] = {'C', 'T', 'E', 'N', 'F', 'D', 'A', 'M'};
constexpr char kKeyPrefix[] = "neteasecloudmusic";
constexpr char kMetaPrefix[] = "163 key(Don't modify):";
constexpr char kJsonPrefix[] = "music:";

uint32_t ReadU32Le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Removes a PKCS#7 pad. A malformed pad leaves the buffer untouched; the caller
// validates the following prefix anyway.
void StripPkcs7(std::vector<uint8_t>& buffer) {
    if (buffer.empty()) return;
    const uint8_t pad = buffer.back();
    if (pad == 0 || pad > 16 || static_cast<size_t>(pad) > buffer.size()) return;
    for (size_t i = buffer.size() - pad; i < buffer.size(); ++i) {
        if (buffer[i] != pad) return;
    }
    buffer.resize(buffer.size() - static_cast<size_t>(pad));
}

// Builds the 256-byte key box used by the stream cipher. The construction is
// RC4's key scheduling with the running sum folded into the seed index.
void BuildKeyBox(const std::vector<uint8_t>& key, uint8_t box[256]) {
    for (int i = 0; i < 256; ++i) box[i] = static_cast<uint8_t>(i);
    if (key.empty()) return;

    uint8_t lastByte = 0;
    size_t keyOffset = 0;
    for (int i = 0; i < 256; ++i) {
        const uint8_t swap = box[i];
        const uint8_t c = static_cast<uint8_t>((swap + lastByte + key[keyOffset % key.size()]) & 0xFF);
        ++keyOffset;
        box[i] = box[c];
        box[c] = swap;
        lastByte = c;
    }
}

void DecryptAudio(std::vector<uint8_t>& data, const std::vector<uint8_t>& key) {
    if (data.empty() || key.empty()) return;

    uint8_t box[256];
    BuildKeyBox(key, box);

    // NOTE: this is the NCM variant of the cipher, not plain RC4. The key box is
    // fixed after key scheduling - no per-byte swap - the stream byte for
    // position i only depends on i and the box.
    for (size_t i = 0; i < data.size(); ++i) {
        const uint8_t j = static_cast<uint8_t>((i + 1) & 0xFF);
        const uint8_t swap1 = box[j];
        const uint8_t index = static_cast<uint8_t>((j + swap1) & 0xFF);
        data[i] ^= box[static_cast<uint8_t>((swap1 + box[index]) & 0xFF)];
    }
}

const char* DetectImageMime(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 12 > data.size()) return nullptr;
    const uint8_t* p = data.data() + offset;
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "image/jpeg";
    if (p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4E && p[3] == 0x47) return "image/png";
    if (memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0) return "image/webp";
    if (memcmp(p, "GIF8", 4) == 0) return "image/gif";
    if (p[0] == 'B' && p[1] == 'M') return "image/bmp";
    return nullptr;
}

// A bare MPEG stream starts with an 11 bit frame sync; an ID3v2 tag marks one
// just as reliably. Anything else is a format this tool does not handle.
AudioCodec DetectCodec(const std::vector<uint8_t>& stream) {
    if (stream.size() >= 4 && memcmp(stream.data(), "fLaC", 4) == 0) return AudioCodec::Flac;
    if (stream.size() >= 3 && memcmp(stream.data(), "ID3", 3) == 0) return AudioCodec::Mp3;

    const size_t limit = (std::min)(stream.size(), static_cast<size_t>(4096));
    for (size_t i = 0; i + 1 < limit; ++i) {
        if (stream[i] != 0xFF || (stream[i + 1] & 0xE0) != 0xE0) continue;
        if ((stream[i + 1] & 0x06) == 0) continue; // layer bits 00 are reserved
        return AudioCodec::Mp3;
    }
    return AudioCodec::Unknown;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& data, std::wstring& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = L"无法打开文件，可能被占用或没有读取权限。";
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        error = L"文件为空或无法读取。";
        return false;
    }
    file.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        error = L"读取文件失败。";
        return false;
    }
    return true;
}

} // namespace

bool DecodeNcmFile(const std::wstring& path, NcmContent& out, std::wstring& error) {
    out = NcmContent{};

    std::vector<uint8_t> raw;
    if (!ReadWholeFile(path, raw, error)) return false;
    if (raw.size() < 16) {
        error = L"文件太小，不是有效的 NCM 文件。";
        return false;
    }
    if (memcmp(raw.data(), kNcmMagic, sizeof(kNcmMagic)) != 0) {
        error = L"文件头不是 CTENFDAM，不是 NCM 文件。";
        return false;
    }

    size_t pos = sizeof(kNcmMagic) + 2; // magic, then two fixed bytes
    if (pos + 4 > raw.size()) {
        error = L"文件结构损坏（缺少密钥长度）。";
        return false;
    }
    const uint32_t keyLength = ReadU32Le(&raw[pos]);
    pos += 4;
    if (keyLength == 0 || pos + keyLength > raw.size()) {
        error = L"文件结构损坏（密钥区块长度异常）。";
        return false;
    }

    std::vector<uint8_t> keyBlock(raw.begin() + pos, raw.begin() + pos + keyLength);
    pos += keyLength;
    for (uint8_t& byte : keyBlock) byte ^= 0x64;

    std::vector<uint8_t> keyPlain;
    if (!Aes128EcbDecrypt(kKeyCipher, keyBlock.data(), keyBlock.size(), keyPlain)) {
        error = L"无法解密密钥区块（AES 失败）。";
        return false;
    }
    StripPkcs7(keyPlain);

    const size_t keyPrefixLength = sizeof(kKeyPrefix) - 1;
    if (keyPlain.size() <= keyPrefixLength || memcmp(keyPlain.data(), kKeyPrefix, keyPrefixLength) != 0) {
        error = L"密钥区块校验失败（前缀不匹配）。";
        return false;
    }
    std::vector<uint8_t> rc4Key(keyPlain.begin() + keyPrefixLength, keyPlain.end());

    // ---- metadata block -----------------------------------------------------
    if (pos + 4 > raw.size()) {
        error = L"文件结构损坏（缺少元数据长度）。";
        return false;
    }
    const uint32_t metaLength = ReadU32Le(&raw[pos]);
    pos += 4;
    if (metaLength == 0 || pos + metaLength > raw.size()) {
        error = L"文件结构损坏（元数据长度异常）。";
        return false;
    }

    std::vector<uint8_t> metaBlock(raw.begin() + pos, raw.begin() + pos + metaLength);
    pos += metaLength;
    for (uint8_t& byte : metaBlock) byte ^= 0x63;

    std::string metaBase64(reinterpret_cast<const char*>(metaBlock.data()), metaBlock.size());
    const std::string metaPrefix = kMetaPrefix;
    if (metaBase64.compare(0, metaPrefix.size(), metaPrefix) == 0) metaBase64.erase(0, metaPrefix.size());

    const std::vector<uint8_t> metaCipher = Base64Decode(metaBase64);
    if (metaCipher.empty() || (metaCipher.size() % 16) != 0) {
        error = L"元数据解码失败（base64 或密文长度无效）。";
        return false;
    }

    std::vector<uint8_t> metaPlain;
    if (!Aes128EcbDecrypt(kMetaCipher, metaCipher.data(), metaCipher.size(), metaPlain)) {
        error = L"无法解密元数据（AES 失败）。";
        return false;
    }
    StripPkcs7(metaPlain);

    std::string metaText(reinterpret_cast<const char*>(metaPlain.data()), metaPlain.size());
    const std::string jsonPrefix = kJsonPrefix;
    if (metaText.compare(0, jsonPrefix.size(), jsonPrefix) == 0) metaText.erase(0, jsonPrefix.size());
    out.metaJson = metaText;

    // ---- crc, version, cover ------------------------------------------------
    if (pos + 5 > raw.size()) {
        error = L"文件结构损坏（缺少封面信息）。";
        return false;
    }
    pos += 4; // crc32 of the metadata, not validated by any known player
    out.version = raw[pos];
    pos += 1;

    if (pos + 4 > raw.size()) {
        error = L"文件结构损坏（缺少封面长度）。";
        return false;
    }
    const uint32_t imageLength = ReadU32Le(&raw[pos]);
    pos += 4;

    // Some container revisions repeat the length, others leave a zero gap right
    // after it. Locate the picture by its signature instead of trusting either.
    size_t imageStart = 0;
    const char* mime = nullptr;
    size_t searchFrom = pos;
    size_t searchTo = pos + 16;
    if (searchTo > raw.size()) searchTo = raw.size();
    for (size_t candidate = searchFrom + (imageLength == 0 ? 8 : 0); candidate < searchTo; ++candidate) {
        mime = DetectImageMime(raw, candidate);
        if (mime != nullptr) {
            imageStart = candidate;
            break;
        }
    }

    size_t audioStart = 0;
    if (mime != nullptr && imageLength > 0 && imageStart + imageLength <= raw.size()) {
        out.cover.assign(raw.begin() + imageStart, raw.begin() + imageStart + imageLength);
        out.coverMime = mime;
        audioStart = imageStart + imageLength;
    } else {
        // No cover: fall back to the documented layout (length, duplicate, gap).
        audioStart = pos + 8;
        if (audioStart > raw.size()) audioStart = raw.size();
    }
    if (audioStart > raw.size()) audioStart = raw.size();

    out.audio.assign(raw.begin() + audioStart, raw.end());
    DecryptAudio(out.audio, rc4Key);

    out.codec = DetectCodec(out.audio);
    if (out.codec == AudioCodec::Unknown) {
        error = L"解密后的码流既不是 FLAC 也不是 MP3，暂不支持这种封装。";
        return false;
    }

    return true;
}

} // namespace ncmlib
