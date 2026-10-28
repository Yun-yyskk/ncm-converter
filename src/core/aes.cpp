#include "pch.h"

#include "core/aes.h"

#include <bcrypt.h>

namespace ncmlib {
namespace {

bool Succeeded(NTSTATUS status) {
    return status >= 0;
}

class AlgorithmHandle {
public:
    AlgorithmHandle() = default;
    ~AlgorithmHandle() {
        if (m_handle != nullptr) BCryptCloseAlgorithmProvider(m_handle, 0);
    }
    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;

    BCRYPT_ALG_HANDLE* Address() { return &m_handle; }
    BCRYPT_ALG_HANDLE Get() const { return m_handle; }

private:
    BCRYPT_ALG_HANDLE m_handle = nullptr;
};

class KeyHandle {
public:
    KeyHandle() = default;
    ~KeyHandle() {
        if (m_handle != nullptr) BCryptDestroyKey(m_handle);
    }
    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;

    BCRYPT_KEY_HANDLE* Address() { return &m_handle; }
    BCRYPT_KEY_HANDLE Get() const { return m_handle; }

private:
    BCRYPT_KEY_HANDLE m_handle = nullptr;
};

} // namespace

bool Aes128EcbDecrypt(const uint8_t key[16], const uint8_t* data, size_t length, std::vector<uint8_t>& out) {
    out.clear();
    if (length == 0 || (length % 16) != 0) return false;

    AlgorithmHandle algorithm;
    if (!Succeeded(BCryptOpenAlgorithmProvider(algorithm.Address(), BCRYPT_AES_ALGORITHM, nullptr, 0))) return false;

    if (!Succeeded(BCryptSetProperty(algorithm.Get(), BCRYPT_CHAINING_MODE,
                                     reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
                                     static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_ECB)), 0))) {
        return false;
    }

    KeyHandle symmetricKey;
    if (!Succeeded(BCryptGenerateSymmetricKey(algorithm.Get(), symmetricKey.Address(), nullptr, 0,
                                              reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(key)), 16, 0))) {
        return false;
    }

    out.resize(length);
    ULONG produced = 0;
    const NTSTATUS status = BCryptDecrypt(symmetricKey.Get(), reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(data)),
                                          static_cast<ULONG>(length), nullptr, nullptr, 0, out.data(),
                                          static_cast<ULONG>(out.size()), &produced, 0);
    if (!Succeeded(status) || produced != length) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace ncmlib
