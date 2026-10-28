#include "pch.h"

#include "core/base64.h"

namespace ncmlib {
namespace {

int DecodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

} // namespace

std::vector<uint8_t> Base64Decode(const std::string& input) {
    std::vector<uint8_t> out;
    out.reserve(input.size() * 3 / 4 + 4);

    uint32_t acc = 0;
    int bits = 0;
    for (char ch : input) {
        if (ch == '=') break;
        const int value = DecodeChar(ch);
        if (value < 0) continue;
        acc = ((acc << 6) | static_cast<uint32_t>(value)) & 0xFFFFFF;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

} // namespace ncmlib
