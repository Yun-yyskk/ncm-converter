#include "pch.h"

#include "core/json.h"

namespace ncmlib {
namespace {

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : m_text(text) {}

    Json ParseDocument() {
        SkipSpace();
        Json value = ParseValue(0);
        SkipSpace();
        if (m_pos != m_text.size()) m_ok = false;
        return value;
    }

    bool Ok() const { return m_ok; }

private:
    const std::string& m_text;
    size_t m_pos = 0;
    bool m_ok = true;

    void SkipSpace() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++m_pos;
            } else {
                break;
            }
        }
    }

    bool Eat(char expected) {
        if (m_pos < m_text.size() && m_text[m_pos] == expected) {
            ++m_pos;
            return true;
        }
        m_ok = false;
        return false;
    }

    Json ParseValue(int depth) {
        if (depth > 64) {
            m_ok = false;
            return Json{};
        }
        SkipSpace();
        if (m_pos >= m_text.size()) {
            m_ok = false;
            return Json{};
        }
        switch (m_text[m_pos]) {
        case '{': return ParseObject(depth);
        case '[': return ParseArray(depth);
        case '"': {
            Json value;
            value.m_type = Json::Type::String;
            value.m_string = ParseString();
            return value;
        }
        case 't': return ParseLiteral("true", true);
        case 'f': return ParseLiteral("false", false);
        case 'n': {
            if (m_text.compare(m_pos, 4, "null") == 0) {
                m_pos += 4;
            } else {
                m_ok = false;
            }
            return Json{};
        }
        default: return ParseNumber();
        }
    }

    Json ParseLiteral(const char* literal, bool value) {
        const size_t len = strlen(literal);
        if (m_text.compare(m_pos, len, literal) == 0) {
            m_pos += len;
            Json result;
            result.m_type = Json::Type::Bool;
            result.m_bool = value;
            return result;
        }
        m_ok = false;
        return Json{};
    }

    Json ParseNumber() {
        const size_t start = m_pos;
        if (m_pos < m_text.size() && (m_text[m_pos] == '-' || m_text[m_pos] == '+')) ++m_pos;
        while (m_pos < m_text.size() && ((m_text[m_pos] >= '0' && m_text[m_pos] <= '9') || m_text[m_pos] == '.' ||
                                         m_text[m_pos] == 'e' || m_text[m_pos] == 'E' || m_text[m_pos] == '-' ||
                                         m_text[m_pos] == '+')) {
            ++m_pos;
        }
        if (m_pos == start) {
            m_ok = false;
            return Json{};
        }
        Json result;
        result.m_type = Json::Type::Number;
        try {
            result.m_number = std::stod(m_text.substr(start, m_pos - start));
        } catch (...) {
            result.m_number = 0.0;
        }
        return result;
    }

    static uint32_t ReadHex4(const std::string& text, size_t pos) {
        uint32_t value = 0;
        for (size_t i = 0; i < 4; ++i) {
            const char c = text[pos + i];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
        }
        return value;
    }

    std::string ParseString() {
        std::string out;
        if (!Eat('"')) return out;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_pos >= m_text.size()) break;
            const char esc = m_text[m_pos++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (m_pos + 4 > m_text.size()) {
                    m_ok = false;
                    return out;
                }
                uint32_t cp = ReadHex4(m_text, m_pos);
                m_pos += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 6 <= m_text.size() && m_text[m_pos] == '\\' &&
                    m_text[m_pos + 1] == 'u') {
                    const uint32_t low = ReadHex4(m_text, m_pos + 2);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        m_pos += 6;
                    }
                }
                AppendUtf8(out, cp);
                break;
            }
            default: out.push_back(esc); break;
            }
        }
        m_ok = false;
        return out;
    }

    Json ParseArray(int depth) {
        Json result;
        result.m_type = Json::Type::Array;
        if (!Eat('[')) return result;
        SkipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return result;
        }
        while (m_ok) {
            result.m_items.push_back(ParseValue(depth + 1));
            SkipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                ++m_pos;
                SkipSpace();
                continue;
            }
            Eat(']');
            break;
        }
        return result;
    }

    Json ParseObject(int depth) {
        Json result;
        result.m_type = Json::Type::Object;
        if (!Eat('{')) return result;
        SkipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return result;
        }
        while (m_ok) {
            SkipSpace();
            const std::string key = ParseString();
            SkipSpace();
            if (!Eat(':')) break;
            result.m_members[key] = ParseValue(depth + 1);
            SkipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            Eat('}');
            break;
        }
        return result;
    }
};

Json Json::Parse(const std::string& text, bool* ok) {
    JsonParser parser(text);
    Json value = parser.ParseDocument();
    if (ok != nullptr) *ok = parser.Ok();
    return value;
}

const Json* Json::Find(const std::string& key) const {
    if (m_type != Type::Object) return nullptr;
    const auto it = m_members.find(key);
    return it == m_members.end() ? nullptr : &it->second;
}

std::string Json::AsString(const std::string& fallback) const {
    return m_type == Type::String ? m_string : fallback;
}

double Json::AsNumber(double fallback) const {
    return m_type == Type::Number ? m_number : fallback;
}

int64_t Json::AsInteger(int64_t fallback) const {
    return m_type == Type::Number ? static_cast<int64_t>(m_number) : fallback;
}

bool Json::AsBool(bool fallback) const {
    return m_type == Type::Bool ? m_bool : fallback;
}

size_t Json::Size() const {
    if (m_type == Type::Array) return m_items.size();
    if (m_type == Type::Object) return m_members.size();
    return 0;
}

} // namespace ncmlib
