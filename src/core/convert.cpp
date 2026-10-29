#include "pch.h"

#include "core/convert.h"

#include "core/flac.h"
#include "core/id3.h"
#include "core/json.h"
#include "core/lyrics.h"
#include "core/ncm.h"

#include <filesystem>
#include <fstream>

namespace ncmlib {
namespace {

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

void Report(const std::function<void(int)>& onProgress, int value) {
    if (onProgress) onProgress(value);
}

std::string JoinNames(const Json* value, const char* separator) {
    if (value == nullptr) return {};
    std::string joined;
    if (value->IsArray()) {
        for (const Json& item : value->Items()) {
            std::string name;
            if (item.IsString()) {
                name = item.AsString();
            } else if (item.IsArray() && !item.Items().empty()) {
                name = item.Items().front().AsString();
            }
            if (name.empty()) continue;
            if (!joined.empty()) joined += separator;
            joined += name;
        }
    } else if (value->IsString()) {
        joined = value->AsString();
    }
    return joined;
}

std::wstring FormatDuration(uint64_t totalSamples, uint32_t sampleRate) {
    if (sampleRate == 0) return L"--:--";
    const uint64_t totalSeconds = totalSamples / sampleRate;
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%02llu:%02llu", static_cast<unsigned long long>(totalSeconds / 60),
               static_cast<unsigned long long>(totalSeconds % 60));
    return buffer;
}

std::wstring FormatSeconds(double seconds) {
    if (seconds <= 0.0) return L"--:--";
    const uint64_t totalSeconds = static_cast<uint64_t>(seconds + 0.5);
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%02llu:%02llu", static_cast<unsigned long long>(totalSeconds / 60),
               static_cast<unsigned long long>(totalSeconds % 60));
    return buffer;
}

// Vorbis comments keep one ARTIST value per performer, ID3 keeps them in a
// single slash-separated TPE1.
std::vector<std::string> SplitArtists(const std::string& artists) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= artists.size()) {
        const size_t slash = artists.find('/', start);
        const std::string single = slash == std::string::npos ? artists.substr(start) : artists.substr(start, slash - start);
        if (!single.empty()) result.push_back(single);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return result;
}

std::vector<LyricLine> LoadLyrics(const std::string& metaJson, const std::wstring& inputPath) {
    std::vector<LyricLine> lines = ExtractLyrics(metaJson);
    if (!lines.empty()) return lines;

    // Fall back to a sidecar file next to the source, if one exists.
    const std::filesystem::path source(inputPath);
    std::filesystem::path sidecar = source;
    sidecar.replace_extension(L".lrc");
    std::error_code ec;
    if (!std::filesystem::exists(sidecar, ec)) return lines;

    std::ifstream file(sidecar, std::ios::binary);
    if (!file) return lines;
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return ParseLrcSidecar(text);
}

bool EnsureParentDirectory(const std::wstring& filePath) {
    const std::filesystem::path parent = std::filesystem::path(filePath).parent_path();
    if (parent.empty()) return true;
    std::error_code ec;
    if (std::filesystem::exists(parent, ec)) return true;
    std::filesystem::create_directories(parent, ec);
    return std::filesystem::exists(parent, ec);
}

bool WriteAll(const std::wstring& path, const std::vector<uint8_t>& bytes, std::wstring& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"无法写入输出文件，可能没有写入权限。";
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
        error = L"写入输出文件时出错（磁盘空间不足？）。";
        return false;
    }
    return true;
}

} // namespace

std::wstring DefaultOutputPath(const std::wstring& inputPath) {
    std::filesystem::path path(inputPath);
    path.replace_extension(L".flac");
    return path.wstring();
}

ConvertOutcome ConvertNcm(const std::wstring& inputPath, const std::wstring& outputPath, bool overwrite,
                          const std::function<void(int)>& onProgress) {
    ConvertOutcome outcome;

    Report(onProgress, 4);

    NcmContent content;
    std::wstring error;
    if (!DecodeNcmFile(inputPath, content, error)) {
        outcome.message = error.empty() ? L"解析 NCM 失败。" : error;
        return outcome;
    }

    const bool lossless = content.codec == AudioCodec::Flac;
    outcome.format = lossless ? L"FLAC" : L"MP3";
    Report(onProgress, 26);

    // The stream decides the container, so the name follows it rather than the
    // other way round: a "song.flac" request for MPEG audio becomes "song.mp3".
    std::filesystem::path target = outputPath.empty() ? std::filesystem::path(DefaultOutputPath(inputPath))
                                                      : std::filesystem::path(outputPath);
    target.replace_extension(lossless ? L".flac" : L".mp3");
    outcome.outputPath = target.wstring();

    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        if (!overwrite) {
            outcome.message = L"目标文件已存在（未开启覆盖）。";
            return outcome;
        }
    } else if (!EnsureParentDirectory(outcome.outputPath)) {
        outcome.message = L"无法创建输出目录。";
        return outcome;
    }

    bool metaOk = false;
    const Json meta = Json::Parse(content.metaJson, &metaOk);

    std::string title;
    std::string album;
    std::string albumArtist;
    std::string artists;
    std::string translatedTitle;
    std::string musicId;
    std::string comment;

    if (metaOk && meta.IsObject()) {
        if (const Json* value = meta.Find("musicName")) title = value->AsString();
        if (title.empty()) {
            if (const Json* value = meta.Find("name")) title = value->AsString();
        }
        if (const Json* value = meta.Find("album")) album = value->AsString();
        if (const Json* value = meta.Find("albumArtist")) albumArtist = value->AsString();
        if (const Json* value = meta.Find("artist")) artists = JoinNames(value, "/");
        if (const Json* value = meta.Find("transNames")) translatedTitle = JoinNames(value, "/");
        if (translatedTitle.empty()) {
            if (const Json* value = meta.Find("transName")) translatedTitle = value->AsString();
        }
        if (const Json* value = meta.Find("musicId")) {
            if (value->IsNumber()) musicId = std::to_string(value->AsInteger());
            else musicId = value->AsString();
        }
        if (const Json* value = meta.Find("comment")) comment = value->AsString();
        if (comment.empty()) {
            if (const Json* value = meta.Find("description")) comment = value->AsString();
        }
    }
    if (albumArtist.empty()) albumArtist = artists;
    if (title.empty()) title = WideToUtf8(std::filesystem::path(inputPath).stem().wstring());
    Report(onProgress, 40);

    const std::vector<LyricLine> lyricLines = LoadLyrics(content.metaJson, inputPath);
    const std::string lrc = BuildLrcText(lyricLines);

    FlacPicture picture;
    bool hasPicture = false;
    if (!content.cover.empty()) {
        std::string mime;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 24;
        uint32_t colors = 0;
        if (ProbeImage(content.cover, mime, width, height, depth, colors)) {
            picture.mime = content.coverMime.empty() ? mime : content.coverMime;
            picture.description = "Cover";
            picture.width = width;
            picture.height = height;
            picture.depth = depth;
            picture.colors = colors;
            picture.data = content.cover;
            hasPicture = true;
        }
    }
    Report(onProgress, 65);

    const std::string encoder = "NcmConverter 1.0 (WinUI 3)";
    std::vector<uint8_t> output;

    if (lossless) {
        FlacStreamInfo info;
        if (!ReadFlacStreamInfo(content.audio, info)) {
            outcome.message = L"FLAC 码流缺少有效的 STREAMINFO。";
            return outcome;
        }

        std::vector<FlacTag> tags;
        tags.push_back({ "TITLE", title });
        for (auto const& single : SplitArtists(artists)) tags.push_back({ "ARTIST", single });
        if (!album.empty()) tags.push_back({ "ALBUM", album });
        if (!albumArtist.empty()) tags.push_back({ "ALBUMARTIST", albumArtist });
        if (!translatedTitle.empty()) tags.push_back({ "TITLE_TRANSLATED", translatedTitle });
        if (!musicId.empty()) tags.push_back({ "MUSIC_ID", musicId });
        if (!comment.empty()) tags.push_back({ "COMMENT", comment });
        if (!lrc.empty()) tags.push_back({ "LYRICS", lrc });
        tags.push_back({ "ENCODER", encoder });
        Report(onProgress, 55);

        std::wstring buildError;
        if (!BuildTaggedFlac(content.audio, tags, hasPicture ? &picture : nullptr, output, info, buildError)) {
            outcome.message = buildError.empty() ? L"写入 FLAC 标签失败。" : buildError;
            return outcome;
        }

        wchar_t summary[160] = {};
        swprintf_s(summary, L"%.1f kHz · %u 声道 · %u bit · %s", info.sampleRate / 1000.0, info.channels,
                   info.bitsPerSample, FormatDuration(info.totalSamples, info.sampleRate).c_str());
        outcome.summary = summary;
    } else {
        Mp3StreamInfo info;
        const bool probed = ProbeMp3(content.audio, info);

        Mp3Tags tags;
        tags.title = title;
        tags.artist = artists;
        tags.album = album;
        tags.albumArtist = albumArtist;
        tags.translatedTitle = translatedTitle;
        tags.musicId = musicId;
        tags.comment = comment;
        tags.lyrics = lrc;
        tags.encoder = encoder;
        tags.cover = hasPicture ? &picture.data : nullptr;
        tags.coverMime = hasPicture ? picture.mime : std::string();
        Report(onProgress, 55);

        std::wstring buildError;
        if (!BuildTaggedMp3(content.audio, tags, output, buildError)) {
            outcome.message = buildError.empty() ? L"写入 MP3 标签失败。" : buildError;
            return outcome;
        }

        if (probed) {
            wchar_t summary[160] = {};
            swprintf_s(summary, L"%.1f kHz · %u 声道 · %u kbps · %s", info.sampleRate / 1000.0, info.channels,
                       info.bitrateKbps, FormatSeconds(info.seconds).c_str());
            outcome.summary = summary;
        } else {
            outcome.summary = L"MP3 码流";
        }
    }
    Report(onProgress, 82);

    if (!WriteAll(outcome.outputPath, output, error)) {
        outcome.message = error;
        return outcome;
    }

    outcome.ok = true;
    outcome.bytes = output.size();
    outcome.title = Utf8ToWide(title);
    outcome.artist = Utf8ToWide(artists);
    outcome.message = lossless ? L"转换完成" : L"转换完成（MP3 码流，已按 MP3 保存）";
    Report(onProgress, 100);
    return outcome;
}

} // namespace ncmlib
