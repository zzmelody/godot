#include "content_hash.h"

#include <map>
#include <utility>

#include "core/config/project_settings.h"

namespace cooker_hash {
namespace {

#ifdef WINDOWS_ENABLED
struct PinnedGeometry {
  HANDLE file = INVALID_HANDLE_VALUE;
  BY_HANDLE_FILE_INFORMATION identity = {};
  String sha256;
  String md5;

  ~PinnedGeometry() {
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
    }
  }

  Error Hash() {
    LARGE_INTEGER length;
    ERR_FAIL_COND_V(!GetFileSizeEx(file, &length) || length.QuadPart < 0,
                    ERR_FILE_CANT_READ);
    Sha256 sha;
    CryptoCore::MD5Context md;
    ERR_FAIL_COND_V(sha.Start() != OK || md.start() != OK, FAILED);
    std::array<uint8_t, 65536> block;
    uint64_t remaining = static_cast<uint64_t>(length.QuadPart);
    while (remaining != 0) {
      const DWORD count = static_cast<DWORD>(
          MIN(remaining, static_cast<uint64_t>(block.size())));
      DWORD received = 0;
      ERR_FAIL_COND_V(
          !ReadFile(file, block.data(), count, &received, nullptr) ||
              received != count,
          ERR_FILE_CANT_READ);
      ERR_FAIL_COND_V(sha.Update(block.data(), count) != OK ||
                          md.update(block.data(), count) != OK,
                      FAILED);
      remaining -= count;
    }
    std::array<uint8_t, 32> sha_bytes;
    std::array<uint8_t, 16> md_bytes;
    ERR_FAIL_COND_V(
        sha.Finish(sha_bytes.data()) != OK || md.finish(md_bytes.data()) != OK,
        FAILED);
    sha256 = String::hex_encode_buffer(sha_bytes.data(), sha_bytes.size());
    md5 = String::hex_encode_buffer(md_bytes.data(), md_bytes.size());
    return OK;
  }
};
#endif

String HashUncachedFile(const String& path) {
  const Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
  ERR_FAIL_COND_V(file.is_null(), String());
  const uint64_t length = file->get_length();
  Sha256 hash;
  ERR_FAIL_COND_V(hash.Start() != OK, String());
  std::array<uint8_t, 65536> block;
  uint64_t remaining = length;
  while (remaining != 0) {
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

}  // namespace

struct Batch::State {
#ifdef WINDOWS_ENABLED
  std::map<String, std::unique_ptr<PinnedGeometry>> files;

  Error Get(const String& path, PinnedGeometry*& result) {
    String absolute = ProjectSettings::get_singleton()
                          ->globalize_path(path)
                          .simplify_path()
                          .replace_char('/', '\\');
    if (!absolute.is_network_share_path() && !absolute.begins_with(R"(\\?\)")) {
      absolute = R"(\\?\)" + absolute;
    }
    PinnedGeometry opened;
    // The handle pins these bytes, rather than trusting a timestamp or
    // filename. Opening again on a hit also detects replacement of an ancestor
    // directory.
    opened.file =
        CreateFileW(reinterpret_cast<LPCWSTR>(absolute.utf16().get_data()),
                    GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    ERR_FAIL_COND_V_MSG(opened.file == INVALID_HANDLE_VALUE, ERR_FILE_CANT_OPEN,
                        "Cannot pin shared geometry input: " + path);
    ERR_FAIL_COND_V(!GetFileInformationByHandle(opened.file, &opened.identity),
                    ERR_FILE_CANT_READ);
    const auto found = files.find(absolute);
    if (found != files.end()) {
      const auto& previous = found->second->identity;
      const auto& current = opened.identity;
      if (previous.dwVolumeSerialNumber == current.dwVolumeSerialNumber &&
          previous.nFileIndexHigh == current.nFileIndexHigh &&
          previous.nFileIndexLow == current.nFileIndexLow) {
        result = found->second.get();
        return OK;
      }
    }
    const Error error = opened.Hash();
    ERR_FAIL_COND_V(error != OK, error);
    auto stored = std::make_unique<PinnedGeometry>();
    stored->file = opened.file;
    opened.file = INVALID_HANDLE_VALUE;
    stored->identity = opened.identity;
    stored->sha256 = std::move(opened.sha256);
    stored->md5 = std::move(opened.md5);
    result = stored.get();
    files[absolute] = std::move(stored);
    return OK;
  }
#endif
};

thread_local Batch* Batch::current_ = nullptr;

Batch::Batch() : state_(std::make_unique<State>()), previous_(current_) {
  current_ = this;
}

Batch::~Batch() {
  current_ = previous_;
}

Error PinGeometryFile(const String& path, String& digest) {
  digest = String();
#ifdef WINDOWS_ENABLED
  if (Batch::current_ && path.get_extension() == "vgp") {
    PinnedGeometry* pinned = nullptr;
    const Error error = Batch::current_->state_->Get(path, pinned);
    ERR_FAIL_COND_V(error != OK, error);
    digest = pinned->sha256;
  }
#endif
  return OK;
}

String HashFile(const String& path) {
  String digest;
  ERR_FAIL_COND_V(PinGeometryFile(path, digest) != OK, String());
  return digest.is_empty() ? HashUncachedFile(path) : digest;
}

String Md5File(const String& path) {
#ifdef WINDOWS_ENABLED
  if (Batch::current_ && path.get_extension() == "vgp") {
    PinnedGeometry* pinned = nullptr;
    ERR_FAIL_COND_V(Batch::current_->state_->Get(path, pinned) != OK, String());
    return pinned->md5;
  }
#endif
  return FileAccess::get_md5(path);
}

}  // namespace cooker_hash
