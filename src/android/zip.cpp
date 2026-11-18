#include "android/zip.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/log.h"

namespace nova::android {

// ---------------------------------------------------------------------------------------------------------------
// Inflate (RFC 1951), puff.c style.
namespace {
constexpr int kMaxBits = 15;
struct Huff {
  std::uint16_t count[kMaxBits + 1];
  std::uint16_t symbol[288];
};
struct InState {
  const std::uint8_t* in;
  std::size_t inlen, incnt = 0;
  std::uint32_t bitbuf = 0;
  int bitcnt = 0;
  std::uint8_t* out;
  std::size_t outlen, outcnt = 0;
  bool err = false;
  int Bits(int need) {
    std::uint64_t val = bitbuf;
    while (bitcnt < need) {
      if (incnt >= inlen) {
        err = true;
        return 0;
      }
      val |= static_cast<std::uint64_t>(in[incnt++]) << bitcnt;
      bitcnt += 8;
    }
    bitbuf = static_cast<std::uint32_t>(val >> need);
    bitcnt -= need;
    return static_cast<int>(val & ((1ull << need) - 1));
  }
};
int Decode(InState& s, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= kMaxBits; ++len) {
    code |= s.Bits(1);
    if (s.err) return -1;
    const int count = h.count[len];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}
int Construct(Huff& h, const std::uint16_t* length, int n) {
  std::memset(h.count, 0, sizeof h.count);
  for (int s = 0; s < n; ++s) h.count[length[s]]++;
  if (h.count[0] == n) return 0;
  int left = 1;
  for (int len = 1; len <= kMaxBits; ++len) {
    left <<= 1;
    left -= h.count[len];
    if (left < 0) return left;
  }
  std::uint16_t offs[kMaxBits + 1];
  offs[1] = 0;
  for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<std::uint16_t>(offs[len] + h.count[len]);
  for (int s = 0; s < n; ++s)
    if (length[s]) h.symbol[offs[length[s]]++] = static_cast<std::uint16_t>(s);
  return left;
}
const std::uint16_t kLBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::uint16_t kLExt[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t kDBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const std::uint16_t kDExt[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(InState& s, const Huff& lc, const Huff& dc) {
  for (;;) {
    int sym = Decode(s, lc);
    if (sym < 0) return false;
    if (sym < 256) {
      if (s.outcnt >= s.outlen) return false;
      s.out[s.outcnt++] = static_cast<std::uint8_t>(sym);
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      const int len = kLBase[sym] + s.Bits(kLExt[sym]);
      const int ds = Decode(s, dc);
      if (ds < 0 || ds >= 30) return false;
      const std::size_t dist = kDBase[ds] + static_cast<std::size_t>(s.Bits(kDExt[ds]));
      if (s.err || dist > s.outcnt || s.outcnt + len > s.outlen) return false;
      for (int i = 0; i < len; ++i, ++s.outcnt) s.out[s.outcnt] = s.out[s.outcnt - dist];
    }
  }
}
}  // namespace

bool Inflate(const std::uint8_t* src, std::size_t n, std::uint8_t* dst, std::size_t dst_len) {
  InState s{src, n};
  s.out = dst;
  s.outlen = dst_len;
  static Huff fixed_l, fixed_d;
  static const bool fixed_ready = [] {
    std::uint16_t lengths[288];
    int i = 0;
    for (; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    Construct(fixed_l, lengths, 288);
    for (i = 0; i < 30; ++i) lengths[i] = 5;
    Construct(fixed_d, lengths, 30);
    return true;
  }();
  (void)fixed_ready;
  int last;
  do {
    last = s.Bits(1);
    const int type = s.Bits(2);
    if (s.err) return false;
    if (type == 0) {
      s.bitbuf = 0;
      s.bitcnt = 0;
      if (s.incnt + 4 > s.inlen) return false;
      const unsigned len = s.in[s.incnt] | (s.in[s.incnt + 1] << 8);
      const unsigned nlen = s.in[s.incnt + 2] | (s.in[s.incnt + 3] << 8);
      s.incnt += 4;
      if (len != (~nlen & 0xFFFFu) || s.incnt + len > s.inlen || s.outcnt + len > s.outlen) return false;
      std::memcpy(s.out + s.outcnt, s.in + s.incnt, len);
      s.incnt += len;
      s.outcnt += len;
    } else if (type == 1) {
      if (!Codes(s, fixed_l, fixed_d)) return false;
    } else if (type == 2) {
      static const std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      const int nlen = s.Bits(5) + 257, ndist = s.Bits(5) + 1, ncode = s.Bits(4) + 4;
      if (s.err || nlen > 286 || ndist > 30) return false;
      std::uint16_t lengths[320] = {};
      for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<std::uint16_t>(s.Bits(3));
      Huff lencode, distcode;
      if (Construct(lencode, lengths, 19) != 0) return false;
      int idx = 0;
      while (idx < nlen + ndist) {
        int sym = Decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 16) {
          lengths[idx++] = static_cast<std::uint16_t>(sym);
        } else {
          int len = 0, rep;
          if (sym == 16) {
            if (!idx) return false;
            len = lengths[idx - 1];
            rep = 3 + s.Bits(2);
          } else if (sym == 17) {
            rep = 3 + s.Bits(3);
          } else {
            rep = 11 + s.Bits(7);
          }
          if (idx + rep > nlen + ndist) return false;
          while (rep--) lengths[idx++] = static_cast<std::uint16_t>(len);
        }
      }
      if (lengths[256] == 0) return false;
      const int el = Construct(lencode, lengths, nlen);
      if (el < 0 || (el > 0 && nlen - lencode.count[0] != 1)) return false;
      const int ed = Construct(distcode, lengths + nlen, ndist);
      if (ed < 0 || (ed > 0 && ndist - distcode.count[0] != 1)) return false;
      if (!Codes(s, lencode, distcode)) return false;
    } else {
      return false;
    }
  } while (!last);
  return s.outcnt == s.outlen;
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t n) {
  static const auto table = [] {
    struct T {
      std::uint32_t v[256];
    } t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t.v[i] = c;
    }
    return t;
  }();
  std::uint32_t c = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) c = table.v[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------------------------------------------
struct ZipArchive::File {
  HANDLE h = INVALID_HANDLE_VALUE;
  ~File() {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
  std::int64_t Size() const {
    LARGE_INTEGER li{};
    return GetFileSizeEx(h, &li) ? li.QuadPart : -1;
  }
  // Positioned read (no shared file pointer, so concurrent readers are fine). Count read, or -1.
  std::int64_t ReadAt(void* dst, std::size_t n, std::int64_t off) const {
    auto* p = static_cast<std::uint8_t*>(dst);
    std::size_t done = 0;
    while (done < n) {
      OVERLAPPED ov{};
      const std::int64_t o = off + static_cast<std::int64_t>(done);
      ov.Offset = static_cast<DWORD>(o);
      ov.OffsetHigh = static_cast<DWORD>(o >> 32);
      const DWORD want = static_cast<DWORD>(std::min<std::size_t>(n - done, 1u << 30));
      DWORD got = 0;
      if (!ReadFile(h, p + done, want, &got, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF) break;
        return -1;
      }
      if (!got) break;
      done += got;
    }
    return static_cast<std::int64_t>(done);
  }
};

namespace {
std::uint16_t Le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t Le32(const std::uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::wstring Widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
}  // namespace

ZipArchive::~ZipArchive() = default;

std::unique_ptr<ZipArchive> ZipArchive::Open(const std::string& path) {
  auto f = std::make_unique<File>();
  f->h = CreateFileW(Widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f->h == INVALID_HANDLE_VALUE) {
    log::Warn("apk: cannot open {} (error {})", path, GetLastError());
    return nullptr;
  }
  const std::int64_t size = f->Size();
  if (size < 22) return nullptr;
  const std::int64_t tail_n = std::min<std::int64_t>(size, 65536 + 22);
  std::vector<std::uint8_t> tail(static_cast<std::size_t>(tail_n));
  if (f->ReadAt(tail.data(), tail.size(), size - tail_n) != tail_n) return nullptr;
  std::int64_t eocd = -1;
  for (std::int64_t i = tail_n - 22; i >= 0; --i)
    if (Le32(&tail[i]) == 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd < 0) {
    log::Warn("apk: {} is not a zip (no end-of-central-directory record)", path);
    return nullptr;
  }
  const std::uint32_t cd_size = Le32(&tail[eocd + 12]), cd_off = Le32(&tail[eocd + 16]);
  if (cd_off == 0xFFFFFFFFu || static_cast<std::int64_t>(cd_off) + cd_size > size) {
    log::Warn("apk: {}: zip64 or an invalid central directory is not supported", path);
    return nullptr;
  }
  std::vector<std::uint8_t> cd(cd_size);
  if (f->ReadAt(cd.data(), cd.size(), cd_off) != static_cast<std::int64_t>(cd_size)) return nullptr;

  std::unique_ptr<ZipArchive> z(new ZipArchive());
  z->path_ = path;
  std::size_t off = 0;
  while (off + 46 <= cd.size() && Le32(&cd[off]) == 0x02014b50) {  // walk the directory, not the (16-bit) entry count
    const std::uint8_t* h = &cd[off];
    Entry e;
    e.method = Le16(h + 10);
    e.crc = Le32(h + 16);
    e.csize = Le32(h + 20);
    e.usize = Le32(h + 24);
    const std::uint16_t nl = Le16(h + 28), xl = Le16(h + 30), cl = Le16(h + 32);
    e.lho = Le32(h + 42);
    if (off + 46 + nl > cd.size()) break;
    e.name.assign(reinterpret_cast<const char*>(h + 46), nl);
    off += 46u + nl + xl + cl;
    if (!e.name.empty() && e.name.back() != '/') z->entries_.push_back(std::move(e));  // directories are implied by names
  }
  z->by_name_.reserve(z->entries_.size());
  for (std::uint32_t i = 0; i < z->entries_.size(); ++i) z->by_name_.emplace(z->entries_[i].name, i);
  z->file_ = std::move(f);
  log::Info("apk: {} opened ({} entries)", path, z->entries_.size());
  return z;
}

const ZipArchive::Entry* ZipArchive::Find(std::string_view name) const {
  const auto it = by_name_.find(name);
  return it == by_name_.end() ? nullptr : &entries_[it->second];
}

std::vector<std::string> ZipArchive::ListFiles(std::string_view dir) const {
  std::string prefix(dir);
  if (!prefix.empty()) prefix += '/';
  std::vector<std::string> out;
  for (const Entry& e : entries_) {
    if (e.name.size() <= prefix.size() || e.name.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string rest = e.name.substr(prefix.size());
    if (rest.find('/') == std::string::npos) out.push_back(rest);
  }
  return out;
}

std::int64_t ZipArchive::ReadRaw(void* dst, std::size_t n, std::int64_t off) const { return file_->ReadAt(dst, n, off); }

std::int64_t ZipArchive::DataOffset(const Entry& e) const {
  std::uint8_t lh[30];
  if (file_->ReadAt(lh, 30, e.lho) != 30 || Le32(lh) != 0x04034b50) return -1;
  return static_cast<std::int64_t>(e.lho) + 30 + Le16(lh + 26) + Le16(lh + 28);
}

std::shared_ptr<const std::vector<std::uint8_t>> ZipArchive::Load(const Entry& e) {
  const std::uint32_t idx = static_cast<std::uint32_t>(&e - entries_.data());
  {
    std::lock_guard lk(cache_mu_);
    if (auto it = cache_.find(idx); it != cache_.end())
      if (auto sp = it->second.lock()) return sp;
  }
  const std::int64_t data = DataOffset(e);
  if (data < 0) return nullptr;
  auto out = std::make_shared<std::vector<std::uint8_t>>(e.usize);
  if (e.method == 0) {
    if (e.usize && file_->ReadAt(out->data(), e.usize, data) != static_cast<std::int64_t>(e.usize)) return nullptr;
  } else if (e.method == 8) {
    std::vector<std::uint8_t> comp(e.csize);
    if (e.csize && file_->ReadAt(comp.data(), comp.size(), data) != static_cast<std::int64_t>(e.csize)) return nullptr;
    if (!Inflate(comp.data(), comp.size(), out->data(), out->size())) {
      log::Error("apk: cannot inflate {}", e.name);
      return nullptr;
    }
    if (Crc32(out->data(), out->size()) != e.crc) {
      log::Error("apk: CRC mismatch in {}", e.name);
      return nullptr;
    }
  } else {
    log::Warn("apk: {}: compression method {} is not supported", e.name, e.method);
    return nullptr;
  }
  std::lock_guard lk(cache_mu_);
  cache_[idx] = out;
  return out;
}
}  // namespace nova::android
