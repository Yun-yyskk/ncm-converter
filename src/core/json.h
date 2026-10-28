#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ncmlib {

// A deliberately small JSON reader: the NCM metadata blob only ever needs
// objects, arrays, strings, numbers and booleans.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;

    static Json Parse(const std::string& text, bool* ok = nullptr);

    Type GetType() const { return m_type; }
    bool IsNull() const { return m_type == Type::Null; }
    bool IsBool() const { return m_type == Type::Bool; }
    bool IsNumber() const { return m_type == Type::Number; }
    bool IsString() const { return m_type == Type::String; }
    bool IsArray() const { return m_type == Type::Array; }
    bool IsObject() const { return m_type == Type::Object; }

    const Json* Find(const std::string& key) const;
    std::string AsString(const std::string& fallback = {}) const;
    double AsNumber(double fallback = 0.0) const;
    int64_t AsInteger(int64_t fallback = 0) const;
    bool AsBool(bool fallback = false) const;

    const std::vector<Json>& Items() const { return m_items; }
    const std::map<std::string, Json>& Members() const { return m_members; }
    size_t Size() const;

    // Value construction is only used by JsonParser below; the members stay
    // public so the parser can build values without a friend declaration.
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    std::vector<Json> m_items;
    std::map<std::string, Json> m_members;
};

} // namespace ncmlib
