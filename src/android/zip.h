// Read-only ZIP (APK) reader with a built-in inflate, used for the AAssetManager. Entry names are matched exactly
// (Android asset paths are case sensitive). Thread-safe: reads are positioned (no shared file pointer).
// Adapted from Zombiewood's hle/libc/zip.cpp (MIT, (c) 2026 ChrisDutary).
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nova::android {

// RFC 1951 raw inflate into a buffer of exactly `dst_len` bytes. True only if the stream is valid and fills it.
bool Inflate(const std::uint8_t* src, std::size_t n, std::uint8_t* dst, std::size_t dst_len);
std::uint32_t Crc32(const std::uint8_t* data, std::size_t n);

class ZipArchive {
 public:
  struct Entry {
    std::string name;
    std::uint16_t method = 0;  // 0 stored, 8 deflate
    std::uint32_t crc = 0, csize = 0, usize = 0, lho = 0;
  };

  // `path` is UTF-8. Null (and a log line) if the file is missing or is not a ZIP.
  static std::unique_ptr<ZipArchive> Open(const std::string& path);
  ~ZipArchive();

  const Entry* Find(std::string_view name) const;
  // Names of the files (not directories) directly inside `dir` ("" = root, no trailing slash), in archive order.
  std::vector<std::string> ListFiles(std::string_view dir) const;

  // Offset in the archive of the entry's data (after its local header); -1 on error.
  std::int64_t DataOffset(const Entry& e) const;
  // Reads n bytes of the archive at `off`; the count read, or -1.
  std::int64_t ReadRaw(void* dst, std::size_t n, std::int64_t off) const;
  // The whole uncompressed entry. Deflated entries are inflated and CRC-checked; the result is cached while
  // somebody holds it. Null on error.
  std::shared_ptr<const std::vector<std::uint8_t>> Load(const Entry& e);

 private:
  ZipArchive() = default;
  struct File;
  std::unique_ptr<File> file_;
  std::string path_;
  std::vector<Entry> entries_;
  std::unordered_map<std::string_view, std::uint32_t> by_name_;
  std::mutex cache_mu_;
  std::unordered_map<std::uint32_t, std::weak_ptr<const std::vector<std::uint8_t>>> cache_;
};
}  // namespace nova::android
