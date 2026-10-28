#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncmlib {

// Decodes standard or URL-safe base64. Characters outside the alphabet (such as
// line breaks produced by wrapped JSON) are ignored.
std::vector<uint8_t> Base64Decode(const std::string& input);

} // namespace ncmlib
