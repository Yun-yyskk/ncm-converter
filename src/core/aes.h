#pragma once

#include <cstdint>
#include <vector>

namespace ncmlib {

// AES-128 in ECB mode, used by the NCM container for both the RC4 seed and the
// metadata blob. Returns false when BCrypt rejects the operation.
bool Aes128EcbDecrypt(const uint8_t key[16], const uint8_t* data, size_t length, std::vector<uint8_t>& out);

} // namespace ncmlib
