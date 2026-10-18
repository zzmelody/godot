#pragma once

#include <array>
#include <cstdint>

#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"

#ifdef WINDOWS_ENABLED
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

namespace cooker_hash {

// Hash the actual bytes on every call. This only changes the implementation
// of SHA-256; it introduces no file-identity or timestamp validation cache.
class Sha256 {
 public:
  Sha256() = default;
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  ~Sha256() {
#ifdef WINDOWS_ENABLED
    if (native_) {
      BCryptDestroyHash(native_);
    }
#endif
  }

  Error Start() {
#ifdef WINDOWS_ENABLED
    struct Provider {
      BCRYPT_ALG_HANDLE handle = nullptr;
      Provider() {
        if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM,
                                       nullptr, 0) < 0) {
          handle = nullptr;
        }
      }
      ~Provider() {
        if (handle) {
          BCryptCloseAlgorithmProvider(handle, 0);
        }
      }
    };
    static const Provider provider;
    if (native_) {
      BCryptDestroyHash(native_);
      native_ = nullptr;
    }
    if (provider.handle &&
        BCryptCreateHash(provider.handle, &native_, nullptr, 0, nullptr, 0,
                         0) >= 0) {
      return OK;
    }
    native_ = nullptr;
#endif
    return portable_.start();
  }

  Error Update(const uint8_t* bytes, uint64_t size) {
#ifdef WINDOWS_ENABLED
    if (native_) {
      while (size) {
        const ULONG count = static_cast<ULONG>(MIN(size, uint64_t(65536)));
        // CNG's input type is mutable; HashData only reads these bytes.
        if (BCryptHashData(native_, const_cast<PUCHAR>(bytes), count, 0) < 0) {
          return FAILED;
        }
        bytes += count;
        size -= count;
      }
      return OK;
    }
#endif
    return portable_.update(bytes, size);
  }

  Error Finish(uint8_t* output) {
#ifdef WINDOWS_ENABLED
    if (native_) {
      return BCryptFinishHash(native_, output, 32, 0) < 0 ? FAILED : OK;
    }
#endif
    return portable_.finish(output);
  }

 private:
  CryptoCore::SHA256Context portable_;
#ifdef WINDOWS_ENABLED
  BCRYPT_HASH_HANDLE native_ = nullptr;
#endif
};

inline String HashFile(const String& path) {
  const Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
  ERR_FAIL_COND_V(file.is_null(), String());
  const uint64_t length = file->get_length();
  Sha256 hash;
  ERR_FAIL_COND_V(hash.Start() != OK, String());
  std::array<uint8_t, 65536> block;
  uint64_t remaining = length;
  while (remaining) {
    const uint32_t count = static_cast<uint32_t>(
        MIN(remaining, static_cast<uint64_t>(block.size())));
    ERR_FAIL_COND_V(file->get_buffer(block.data(), count) != count ||
                        hash.Update(block.data(), count) != OK,
                    String());
    remaining -= count;
  }
  ERR_FAIL_COND_V(file->get_length() != length, String());
  std::array<uint8_t, 32> digest;
  ERR_FAIL_COND_V(hash.Finish(digest.data()) != OK, String());
  return String::hex_encode_buffer(digest.data(), digest.size());
}

}  // namespace cooker_hash
