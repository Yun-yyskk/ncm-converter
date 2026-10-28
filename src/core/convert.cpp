#include "pch.h"

#include "core/convert.h"

#include "core/flac.h"
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

} // namespace

std::wstring DefaultOutputPath(const std::wstring& inputPath) {
    std::filesystem::path path(inputPath);
    path.replace_extension(L".flac");
    return path.wstring();
}

ConvertOutcome ConvertNcmToFlac(const std::wstring& inputPath, const std::wstring& outputPath, bool overwrite,
                                const std::function<void(int)>& onProgress) {
    ConvertOutcome outcome;
    outcome.outputPath = outputPath;

    std::error_code ec;
    if (!std::filesystem::exists(outputPath, ec)) {
        if (!EnsureParentDirectory(outputPath)) {
            outcome.message = L"无法创建输出目录。";
            return outcome;
        }
    } else if (!overwrite) {
        outcome.message = L"目标文件已存在（未开启覆盖）。";
        return outcome;
    }

    Report(onProgress, 4);

    NcmContent content;
    std::wstring error;
    if (!DecodeNcmFile(inputPath, content, error)) {
        outcome.message = error.empty() ? L"解析 NCM 失败。" : error;
        return outcome;
    }
    Report(onProgress, 26);

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
    Report(onProgress, 40);

    FlacStreamInfo info;
    if (!ReadFlacStreamInfo(content.audio, info)) {
        outcome.message = L"FLAC 码流缺少有效的 STREAMINFO。";
        return outcome;
    }

    std::vector<FlacTag> tags;
    const std::wstring sourceName = std::filesystem::path(inputPath).stem().wstring();
    if (title.empty()) title = WideToUtf8(sourceName);

    tags.push_back({ "TITLE", title });
    if (!artists.empty()) {
        // One ARTIST entry per performer, matching how players group tracks.
        size_t start = 0;
        while (start <= artists.size()) {
            const size_t slash = artists.find('/', start);
            std::string single = slash == std::string::npos ? artists.substr(start) : artists.substr(start, slash - start);
            if (!single.empty()) tags.push_back({ "ARTIST", single });
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
    }
    if (!album.empty()) tags.push_back({ "ALBUM", album });
    if (!albumArtist.empty()) tags.push_back({ "ALBUMARTIST", albumArtist });
    if (!translatedTitle.empty()) tags.push_back({ "TITLE_TRANSLATED", translatedTitle });
    if (!musicId.empty()) tags.push_back({ "MUSIC_ID", musicId });
    if (!comment.empty()) tags.push_back({ "COMMENT", comment });

    const std::vector<LyricLine> lyricLines = LoadLyrics(content.metaJson, inputPath);
    const std::string lrc = BuildLrcText(lyricLines);
    if (!lrc.empty()) tags.push_back({ "LYRICS", lrc });
    tags.push_back({ "ENCODER", "NcmToFlac 1.0 (WinUI 3)" });
    Report(onProgress, 55);

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
            picture.data = std::move(content.cover);
            hasPicture = true;
        }
    }
    Report(onProgress, 65);

    std::vector<uint8_t> output;
    std::wstring buildError;
    if (!BuildTaggedFlac(content.audio, tags, hasPicture ? &picture : nullptr, output, info, buildError)) {
        outcome.message = buildError.empty() ? L"写入 FLAC 标签失败。" : buildError;
        return outcome;
    }
    Report(onProgress, 82);

    std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        outcome.message = L"无法写入输出文件，可能没有写入权限。";
        return outcome;
    }
    file.write(reinterpret_cast<const char*>(output.data()), static_cast<std::streamsize>(output.size()));
    file.close();
    if (!file) {
        outcome.message = L"写入输出文件时出错（磁盘空间不足？）。";
        return outcome;
    }

    outcome.ok = true;
    outcome.bytes = output.size();
    outcome.title = Utf8ToWide(title);
    outcome.artist = Utf8ToWide(artists);

    wchar_t summary[128] = {};
    swprintf_s(summary, L"%.1f kHz · %u 声道 · %u bit · %s", info.sampleRate / 1000.0, info.channels,
               info.bitsPerSample, FormatDuration(info.totalSamples, info.sampleRate).c_str());
    outcome.summary = summary;
    outcome.message = L"转换完成";
    Report(onProgress, 100);
    return outcome;
}

} // namespace ncmlib