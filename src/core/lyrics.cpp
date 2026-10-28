#include "pch.h"

#include "core/lyrics.h"

#include "core/json.h"

#include <algorithm>
#include <cstdio>

namespace ncmlib {
namespace {

std::string Trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r' || value[begin] == '\n')) {
        ++begin;
    }
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool IsDigits(char c) {
    return c >= '0' && c <= '9';
}

// "[mm:ss.xx]" / "[mm:ss:xx]" -> milliseconds.
bool ParseTimestamp(const std::string& token, int64_t& outMs) {
    int minutes = 0;
    int seconds = 0;
    int fraction = 0;
    int fractionDigits = 0;
    size_t i = 0;
    while (i < token.size() && IsDigits(token[i])) minutes = minutes * 10 + (token[i++] - '0');
    if (i >= token.size() || token[i] != ':') return false;
    ++i;
    while (i < token.size() && IsDigits(token[i])) seconds = seconds * 10 + (token[i++] - '0');
    if (i < token.size() && (token[i] == '.' || token[i] == ':')) {
        ++i;
        while (i < token.size() && IsDigits(token[i]) && fractionDigits < 3) {
            fraction = fraction * 10 + (token[i++] - '0');
            ++fractionDigits;
        }
    }
    while (fractionDigits < 3) {
        fraction *= 10;
        ++fractionDigits;
    }
    outMs = static_cast<int64_t>(minutes) * 60000 + static_cast<int64_t>(seconds) * 1000 + fraction;
    return true;
}

// Karaoke flavour: "[start,duration](start,dur,0)word(.,.,.)word..."
bool IsKaraokeLine(const std::string& line) {
    if (line.size() < 6 || line[0] != '[') return false;
    size_t i = 1;
    while (i < line.size() && IsDigits(line[i])) ++i;
    if (i >= line.size() || line[i] != ',') return false;
    ++i;
    while (i < line.size() && IsDigits(line[i])) ++i;
    return i < line.size() && line[i] == ']';
}

std::string StripKaraokeMarkup(const std::string& line) {
    const size_t header = line.find(']');
    if (header == std::string::npos) return line;
    std::string text;
    size_t i = header + 1;
    while (i < line.size()) {
        if (line[i] == '(') {
            const size_t close = line.find(')', i);
            if (close == std::string::npos) break;
            i = close + 1;
            continue;
        }
        text.push_back(line[i++]);
    }
    return text;
}

void ParsePlainLrc(const std::string& document, std::vector<LyricLine>& out) {
    size_t pos = 0;
    while (pos <= document.size()) {
        const size_t newline = document.find('\n', pos);
        const std::string rawLine =
            newline == std::string::npos ? document.substr(pos) : document.substr(pos, newline - pos);
        pos = newline == std::string::npos ? document.size() + 1 : newline + 1;

        std::string line = rawLine;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::vector<int64_t> stamps;
        size_t cursor = 0;
        while (cursor < line.size() && line[cursor] == '[') {
            const size_t close = line.find(']', cursor);
            if (close == std::string::npos) break;
            const std::string token = line.substr(cursor + 1, close - cursor - 1);
            int64_t ms = 0;
            if (!ParseTimestamp(token, ms)) break; // metadata tag such as [ti:...]
            stamps.push_back(ms);
            cursor = close + 1;
        }

        if (stamps.empty()) continue;
        const std::string text = Trim(line.substr(cursor));
        for (int64_t stamp : stamps) out.push_back(LyricLine{stamp, text});
    }
    std::stable_sort(out.begin(), out.end(), [](const LyricLine& a, const LyricLine& b) { return a.timeMs < b.timeMs; });
}

void ParseKaraokeLrc(const std::string& document, std::vector<LyricLine>& out) {
    size_t pos = 0;
    while (pos <= document.size()) {
        const size_t newline = document.find('\n', pos);
        const std::string line =
            newline == std::string::npos ? document.substr(pos) : document.substr(pos, newline - pos);
        pos = newline == std::string::npos ? document.size() + 1 : newline + 1;
        std::string trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();
        if (!IsKaraokeLine(trimmed)) continue;

        const size_t close = trimmed.find(']');
        const std::string header = trimmed.substr(1, close - 1);
        const size_t comma = header.find(',');
        if (comma == std::string::npos) continue;
        const int64_t startMs = std::atoll(header.substr(0, comma).c_str());
        out.push_back(LyricLine{startMs, Trim(StripKaraokeMarkup(trimmed))});
    }
    std::stable_sort(out.begin(), out.end(), [](const LyricLine& a, const LyricLine& b) { return a.timeMs < b.timeMs; });
}

std::vector<LyricLine> ParsePayload(const std::string& payload, int depth);

std::vector<LyricLine> ParseJsonPayload(const std::string& payload, int depth) {
    std::vector<LyricLine> lines;
    bool ok = false;
    const Json root = Json::Parse(payload, &ok);
    if (!ok || !root.IsObject()) return lines;

    if (const Json* inner = root.Find("lyric")) {
        if (inner->IsString()) {
            const std::string text = inner->AsString();
            if (!text.empty() && text != payload) return ParsePayload(text, depth + 1);
        } else if (inner->IsArray()) {
            // Per-line object form: [{"ms":1234,"c":[{"tx":"..."}]}, ...]
            for (const Json& item : inner->Items()) {
                if (!item.IsObject()) continue;
                LyricLine line;
                if (const Json* ms = item.Find("ms")) line.timeMs = ms->AsInteger();
                else if (const Json* t = item.Find("t")) line.timeMs = t->AsInteger();
                std::string text;
                if (const Json* content = item.Find("c")) {
                    for (const Json& fragment : content->Items()) {
                        if (const Json* tx = fragment.Find("tx")) text += tx->AsString();
                    }
                } else if (const Json* tx = item.Find("tx")) {
                    text = tx->AsString();
                } else if (const Json* lyricText = item.Find("lyric")) {
                    text = lyricText->AsString();
                }
                line.text = Trim(text);
                lines.push_back(std::move(line));
            }
            return lines;
        }
    }

    if (const Json* yrc = root.Find("yrc")) {
        if (yrc->IsString()) return ParsePayload(yrc->AsString(), depth + 1);
        if (yrc->IsObject()) {
            if (const Json* text = yrc->Find("lyric"); text != nullptr && text->IsString()) {
                return ParsePayload(text->AsString(), depth + 1);
            }
        }
    }

    if (const Json* lrc = root.Find("lrc"); lrc != nullptr && lrc->IsString()) {
        return ParsePayload(lrc->AsString(), depth + 1);
    }
    if (const Json* text = root.Find("text"); text != nullptr && text->IsString()) {
        return ParsePayload(text->AsString(), depth + 1);
    }
    return lines;
}

std::vector<LyricLine> ParsePayload(const std::string& payload, int depth) {
    std::vector<LyricLine> lines;
    const std::string trimmed = Trim(payload);
    if (trimmed.empty() || depth > 6) return lines;

    if (trimmed.front() == '{') return ParseJsonPayload(trimmed, depth);

    if (trimmed.front() == '[') {
        // Karaoke lines begin with "[digits,digits]"; plain LRC with "[mm:ss".
        const size_t close = trimmed.find(']');
        const size_t comma = trimmed.find(',');
        const bool karaoke = close != std::string::npos && comma != std::string::npos && comma < close;
        if (karaoke) {
            ParseKaraokeLrc(trimmed, lines);
        } else {
            ParsePlainLrc(trimmed, lines);
        }
        return lines;
    }
    return lines;
}

void MergeTranslation(std::vector<LyricLine>& main, const std::vector<LyricLine>& translation) {
    if (translation.empty()) return;
    for (LyricLine& line : main) {
        const auto it = std::find_if(translation.begin(), translation.end(), [&](const LyricLine& candidate) {
            const int64_t delta = candidate.timeMs - line.timeMs;
            return delta >= -120 && delta <= 120;
        });
        if (it != translation.end() && !it->text.empty() && it->text != line.text) {
            line.text = line.text.empty() ? it->text : line.text + " / " + it->text;
        }
    }
}

} // namespace

std::vector<LyricLine> ParseLrcSidecar(const std::string& text) {
    std::string body = text;
    if (body.size() >= 3 && static_cast<uint8_t>(body[0]) == 0xEF && static_cast<uint8_t>(body[1]) == 0xBB &&
        static_cast<uint8_t>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }
    return ParsePayload(body, 0);
}

std::vector<LyricLine> ExtractLyrics(const std::string& metaJson) {
    std::vector<LyricLine> main;
    bool ok = false;
    const Json root = Json::Parse(metaJson, &ok);
    if (!ok || !root.IsObject()) return main;

    if (const Json* lyric = root.Find("lyric")) {
        main = ParsePayload(lyric->IsString() ? lyric->AsString() : std::string(), 0);
    }
    if (main.empty()) {
        for (const char* key : {"yrc", "lrc", "lyricText"}) {
            if (const Json* candidate = root.Find(key)) {
                main = ParsePayload(candidate->IsString() ? candidate->AsString() : std::string(), 0);
                if (!main.empty()) break;
            }
        }
    }

    std::vector<LyricLine> translation;
    if (const Json* tlyric = root.Find("tlyric")) {
        translation = ParsePayload(tlyric->IsString() ? tlyric->AsString() : std::string(), 0);
    }
    if (translation.empty()) {
        if (const Json* tlyric = root.Find("translatedLyric")) {
            translation = ParsePayload(tlyric->IsString() ? tlyric->AsString() : std::string(), 0);
        }
    }

    MergeTranslation(main, translation);
    return main;
}

std::string BuildLrcText(const std::vector<LyricLine>& lines) {
    std::string out;
    out.reserve(lines.size() * 40);
    for (const LyricLine& line : lines) {
        if (line.text.empty()) continue;
        const int64_t minutes = line.timeMs / 60000;
        const int64_t seconds = (line.timeMs % 60000) / 1000;
        const int64_t centis = (line.timeMs % 1000) / 10;
        char stamp[32] = {};
        std::snprintf(stamp, sizeof(stamp), "[%02lld:%02lld.%02lld]", static_cast<long long>(minutes),
                      static_cast<long long>(seconds), static_cast<long long>(centis));
        out += stamp;
        out += line.text;
        out.push_back('\n');
    }
    return out;
}

} // namespace ncmlib
