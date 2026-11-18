// bionic stdio over the VFS. A guest FILE* is a 152-byte guest block (sizeof(sFILE) on LP64; the game only ever
// touches the three __sF entries by address, everything else is opaque) that keys a host Stream: buffered reads,
// buffered writes (standard streams are write-through), pushback for ungetc, and funopen() callbacks into the guest.
// __sF[0..2] (stdin/stdout/stderr) are fixed guest addresses with fd 0/1/2 in the _file field (offset 18).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "hle/format.h"
#include "hle/heap.h"
#include "hle/libc_internal.h"
#include "hle/vfs.h"

namespace nova::hle::libc {
namespace {
using std::int16_t;
using std::int32_t;
using std::int64_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

constexpr uint64_t kFileSize = 152;  // sizeof(FILE) on bionic LP64
constexpr std::size_t kBufSize = 16384;
constexpr int32_t kEOF = -1;
constexpr uint32_t kWEOF = 0xFFFFFFFFu;

struct Stream {
  vfs::FilePtr file;
  gaddr guest = 0;
  int fd = -1;  // descriptor in the VFS table (closed by fclose), -1 for funopen / standard streams
  bool can_read = false, can_write = false, append = false, unbuffered = false, is_std = false;
  bool eof = false, error = false;
  std::vector<uint8_t> rbuf;
  std::size_t rpos = 0, rend = 0;
  std::vector<uint8_t> wbuf;
  std::mutex mu;
};
using StreamPtr = std::shared_ptr<Stream>;

std::mutex g_tab_mu;
std::unordered_map<gaddr, StreamPtr> g_streams;

StreamPtr Lookup(gaddr fp) {
  std::lock_guard lk(g_tab_mu);
  auto it = g_streams.find(fp);
  return it == g_streams.end() ? nullptr : it->second;
}
void SetFileFields(gaddr fp, int fd, bool r, bool w) {
  std::memset(g2h(fp), 0, kFileSize);
  mem::W<int16_t>(fp + 16, static_cast<int16_t>((r && w) ? 0x0010 : (r ? 0x0004 : 0x0008)));  // __SRW / __SRD / __SWR
  mem::W<int16_t>(fp + 18, static_cast<int16_t>(fd));
}

// ------------------------------------------------------------------------------------------ stream core (mu held)
bool FlushW(Stream& s) {
  if (s.wbuf.empty()) return true;
  std::size_t done = 0;
  while (done < s.wbuf.size()) {
    const int64_t w = s.file->Write(s.wbuf.data() + done, s.wbuf.size() - done);
    if (w <= 0) {
      s.error = true;
      s.wbuf.clear();
      return false;
    }
    done += static_cast<std::size_t>(w);
  }
  s.wbuf.clear();
  return true;
}
void DropR(Stream& s) {
  if (s.rend > s.rpos) s.file->Seek(-static_cast<int64_t>(s.rend - s.rpos), 1);
  s.rpos = s.rend = 0;
}
bool FillR(Stream& s) {
  if (!FlushW(s)) return false;
  if (s.rbuf.size() < kBufSize) s.rbuf.resize(kBufSize);
  s.rpos = s.rend = 0;
  const int64_t r = s.file->Read(s.rbuf.data(), s.rbuf.size());
  if (r < 0) {
    s.error = true;
    return false;
  }
  if (r == 0) {
    s.eof = true;
    return false;
  }
  s.rend = static_cast<std::size_t>(r);
  return true;
}
int GetC(Stream& s) {
  if (!s.can_read) {
    s.error = true;
    return kEOF;
  }
  if (s.rpos >= s.rend && !FillR(s)) return kEOF;
  return s.rbuf[s.rpos++];
}
void UngetC(Stream& s, int c) {
  if (s.rpos > 0) {
    s.rbuf[--s.rpos] = static_cast<uint8_t>(c);
  } else {
    s.rbuf.insert(s.rbuf.begin(), static_cast<uint8_t>(c));
    s.rend = s.rend + 1;
    if (s.rbuf.size() < s.rend) s.rbuf.resize(s.rend);
  }
  s.eof = false;
}
std::size_t ReadBytes(Stream& s, void* dst, std::size_t n) {
  if (!s.can_read) {
    s.error = true;
    return 0;
  }
  auto* out = static_cast<uint8_t*>(dst);
  std::size_t got = 0;
  while (got < n) {
    if (s.rpos < s.rend) {
      const std::size_t k = std::min(n - got, s.rend - s.rpos);
      std::memcpy(out + got, s.rbuf.data() + s.rpos, k);
      s.rpos += k;
      got += k;
      continue;
    }
    if (n - got >= kBufSize) {  // big read: straight into the destination
      if (!FlushW(s)) break;
      const int64_t r = s.file->Read(out + got, n - got);
      if (r < 0) {
        s.error = true;
        break;
      }
      if (r == 0) {
        s.eof = true;
        break;
      }
      got += static_cast<std::size_t>(r);
      continue;
    }
    if (!FillR(s)) break;
  }
  return got;
}
std::size_t WriteBytes(Stream& s, const void* src, std::size_t n) {
  if (!s.can_write) {
    s.error = true;
    return 0;
  }
  if (s.rend > s.rpos || s.rend) DropR(s);
  if (s.unbuffered || n >= kBufSize) {
    if (!FlushW(s)) return 0;
    std::size_t done = 0;
    while (done < n) {
      const int64_t w = s.file->Write(static_cast<const uint8_t*>(src) + done, n - done);
      if (w <= 0) {
        s.error = true;
        return done;
      }
      done += static_cast<std::size_t>(w);
    }
    return n;
  }
  if (s.wbuf.size() + n > kBufSize && !FlushW(s)) return 0;
  s.wbuf.insert(s.wbuf.end(), static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + n);
  return n;
}
int64_t TellS(Stream& s) {
  if (s.append) FlushW(s);
  const int64_t p = s.file->Seek(0, 1);
  if (p < 0) return p;
  return p - static_cast<int64_t>(s.rend - s.rpos) + static_cast<int64_t>(s.wbuf.size());
}
int SeekS(Stream& s, int64_t off, int whence) {
  if (!FlushW(s)) return -err::IO;
  if (whence == 1) off -= static_cast<int64_t>(s.rend - s.rpos);
  s.rpos = s.rend = 0;
  const int64_t r = s.file->Seek(off, whence);
  if (r < 0) return static_cast<int>(r);
  s.eof = false;
  return 0;
}

// ------------------------------------------------------------------------------------------ funopen
class CallbackFile : public vfs::File {
 public:
  CallbackFile(uint64_t cookie, uint64_t rd, uint64_t wr, uint64_t sk, uint64_t cl)
      : cookie_(cookie), rd_(rd), wr_(wr), sk_(sk), cl_(cl) {
    path = "funopen";
    flags = vfs::kO_RDWR;
  }
  ~CallbackFile() override { Close(); }
  void Close() {
    if (closed_.exchange(true)) return;
    if (cl_) Cpu::Current().Call<int32_t>(cl_, cookie_);
    if (scratch_) heap::Free(scratch_);
    scratch_ = 0;
  }
  int64_t Read(void* buf, uint64_t n) override {
    if (!rd_) return -err::BADF;
    n = std::min<uint64_t>(n, kBufSize);
    gaddr g = Guest(buf, n);
    const int32_t r = Cpu::Current().Call<int32_t>(rd_, cookie_, g, static_cast<int32_t>(n));
    if (r > 0 && g == scratch_) std::memcpy(buf, g2h(scratch_), static_cast<size_t>(r));
    if (r > 0) pos_ += r;
    return r < 0 ? -err::IO : r;
  }
  int64_t Write(const void* buf, uint64_t n) override {
    if (!wr_) return -err::BADF;
    n = std::min<uint64_t>(n, kBufSize);
    gaddr g = Guest(buf, n);
    if (g == scratch_) std::memcpy(g2h(scratch_), buf, n);
    const int32_t r = Cpu::Current().Call<int32_t>(wr_, cookie_, g, static_cast<int32_t>(n));
    if (r > 0) pos_ += r;
    return r < 0 ? -err::IO : r;
  }
  int64_t Seek(int64_t off, int whence) override {
    if (!sk_) return -err::SPIPE;
    // ftell is a kFast handler and must not call into the guest: answer a pure position query (SEEK_CUR, 0) from the
    // position tracked here (starts at 0, follows read/write/seek).
    if (whence == 1 && off == 0) return pos_;
    const int64_t r = Cpu::Current().Call<int64_t>(sk_, cookie_, off, static_cast<int32_t>(whence));
    if (r >= 0) pos_ = r;
    return r < 0 ? -err::INVAL : r;
  }
  int Stat(vfs::StatBuf& st) override {
    st = vfs::StatBuf{};
    st.mode = vfs::kIFREG | 0444;
    return 0;
  }

 private:
  gaddr Guest(const void* buf, uint64_t n) {
    if (GuestMemory::Contains(buf)) return h2g(buf);
    if (!scratch_) scratch_ = heap::Alloc(kBufSize);
    (void)n;
    return scratch_;
  }
  uint64_t cookie_, rd_, wr_, sk_, cl_;
  gaddr scratch_ = 0;
  int64_t pos_ = 0;
  std::atomic<bool> closed_{false};
};

// ------------------------------------------------------------------------------------------ opening
bool ParseMode(const char* mode, int& flags, bool& r, bool& w, bool& app) {
  if (!mode) return false;
  r = w = app = false;
  bool plus = false;
  const char m = *mode;
  for (const char* p = mode + 1; *p; ++p)
    if (*p == '+') plus = true;
  if (m == 'r') {
    r = true;
    w = plus;
    flags = plus ? vfs::kO_RDWR : vfs::kO_RDONLY;
  } else if (m == 'w') {
    w = true;
    r = plus;
    flags = (plus ? vfs::kO_RDWR : vfs::kO_WRONLY) | vfs::kO_CREAT | vfs::kO_TRUNC;
  } else if (m == 'a') {
    w = true;
    r = plus;
    app = true;
    flags = (plus ? vfs::kO_RDWR : vfs::kO_WRONLY) | vfs::kO_CREAT | vfs::kO_APPEND;
  } else {
    return false;
  }
  return true;
}

gaddr Register(StreamPtr s, int fd_field) {
  const gaddr fp = heap::Alloc(kFileSize);
  if (!fp) return 0;
  SetFileFields(fp, fd_field, s->can_read, s->can_write);
  s->guest = fp;
  std::lock_guard lk(g_tab_mu);
  g_streams[fp] = std::move(s);
  return fp;
}

uint64_t h_fopen(Cpu& cpu, const char* path, const char* mode) {
  int flags;
  bool r, w, app;
  if (!path || !ParseMode(mode, flags, r, w, app)) {
    SetErrno(cpu, err::INVAL);
    return 0;
  }
  vfs::FilePtr f;
  const int e = vfs::Open(path, flags, 0666, f);
  if (e) {
    SetErrno(cpu, e);
    return 0;
  }
  f->flags = flags;
  const int fd = vfs::Install(f);
  if (fd < 0) {
    SetErrno(cpu, -fd);
    return 0;
  }
  auto s = std::make_shared<Stream>();
  s->file = std::move(f);
  s->fd = fd;
  s->can_read = r;
  s->can_write = w;
  s->append = app;
  const gaddr fp = Register(s, fd);
  if (!fp) {
    vfs::Close(fd);
    SetErrno(cpu, err::NOMEM);
  }
  return fp;
}
uint64_t h_fdopen(Cpu& cpu, int32_t fd, const char* mode) {
  int flags;
  bool r, w, app;
  if (!ParseMode(mode, flags, r, w, app)) {
    SetErrno(cpu, err::INVAL);
    return 0;
  }
  auto f = vfs::Get(fd);
  if (!f) {
    SetErrno(cpu, err::BADF);
    return 0;
  }
  auto s = std::make_shared<Stream>();
  s->file = f;
  s->fd = fd;
  s->can_read = r;
  s->can_write = w;
  s->append = app;
  return Register(s, fd);
}
uint64_t h_freopen(Cpu& cpu, const char* path, const char* mode, uint64_t fp) {
  StreamPtr s = Lookup(fp);
  int flags;
  bool r, w, app;
  if (!s || !ParseMode(mode, flags, r, w, app)) {
    SetErrno(cpu, s ? err::INVAL : err::BADF);
    return 0;
  }
  std::lock_guard lk(s->mu);
  FlushW(*s);
  s->rpos = s->rend = 0;
  if (path) {
    vfs::FilePtr f;
    const int e = vfs::Open(path, flags, 0666, f);
    if (e) {
      SetErrno(cpu, e);
      return 0;
    }
    f->flags = flags;
    if (s->fd > 2) vfs::Close(s->fd);
    if (s->is_std) {
      s->is_std = false;
      s->unbuffered = false;
    }
    s->fd = vfs::Install(f);
    s->file = std::move(f);
  }
  s->can_read = r;
  s->can_write = w;
  s->append = app;
  s->eof = s->error = false;
  SetFileFields(fp, s->fd < 0 ? -1 : s->fd, r, w);
  return fp;
}
uint64_t h_funopen(uint64_t cookie, uint64_t rd, uint64_t wr, uint64_t sk, uint64_t cl) {
  auto s = std::make_shared<Stream>();
  s->file = std::make_shared<CallbackFile>(cookie, rd, wr, sk, cl);
  s->can_read = rd != 0;
  s->can_write = wr != 0;
  return Register(s, -1);
}
uint64_t h_tmpfile(Cpu& cpu) {
  char name[64];
  vfs::FilePtr f;
  int e = err::EXIST;
  std::string path;
  for (int i = 0; i < 100 && e == err::EXIST; ++i) {
    std::snprintf(name, sizeof name, "/data/local/tmp/tmp.%llx%d", static_cast<unsigned long long>(MonotonicNs()), i);
    path = name;
    e = vfs::Open(path, vfs::kO_RDWR | vfs::kO_CREAT | vfs::kO_EXCL, 0600, f);
  }
  if (e) {
    SetErrno(cpu, e);
    return 0;
  }
  f->flags = vfs::kO_RDWR;
  const int fd = vfs::Install(f);
  vfs::Unlink(path);  // the data goes away when the last handle closes
  auto s = std::make_shared<Stream>();
  s->file = std::move(f);
  s->fd = fd;
  s->can_read = s->can_write = true;
  return Register(s, fd);
}
uint64_t h_tmpnam(char* buf) {
  static std::atomic<uint32_t> counter{0};
  gaddr dst = buf ? h2g(buf) : ThreadBuffer(6, 64);
  std::snprintf(g2h<char>(dst), 64, "/data/local/tmp/tmp%llx.%u", static_cast<unsigned long long>(MonotonicNs() & 0xFFFFFF), counter++);
  return dst;
}

int32_t h_fclose(Cpu& cpu, uint64_t fp) {
  StreamPtr s;
  {
    std::lock_guard lk(g_tab_mu);
    auto it = g_streams.find(fp);
    if (it == g_streams.end()) {
      SetErrno(cpu, err::BADF);
      return kEOF;
    }
    s = it->second;
  }
  int32_t rc = 0;
  {
    std::lock_guard lk(s->mu);
    if (!FlushW(*s)) rc = kEOF;
    if (s->is_std) return rc;  // the __sF entries stay valid: closing a standard stream only flushes it
    if (auto* cb = dynamic_cast<CallbackFile*>(s->file.get())) cb->Close();
    s->file.reset();
    if (s->fd > 2) vfs::Close(s->fd);
  }
  {
    std::lock_guard lk(g_tab_mu);
    g_streams.erase(fp);
  }
  heap::Free(fp);
  return rc;
}
int32_t h_fflush(uint64_t fp) {
  if (!fp) {
    FlushAllStreams();
    return 0;
  }
  StreamPtr s = Lookup(fp);
  if (!s) return kEOF;
  std::lock_guard lk(s->mu);
  return FlushW(*s) ? 0 : kEOF;
}

// ------------------------------------------------------------------------------------------ reading / writing
#define WITH_STREAM(fp, fail_value)            \
  StreamPtr s = Lookup(fp);                    \
  if (!s) {                                    \
    SetErrno(err::BADF);                       \
    return fail_value;                         \
  }                                            \
  std::lock_guard lk(s->mu)

uint64_t h_fread(void* dst, uint64_t size, uint64_t n, uint64_t fp) {
  WITH_STREAM(fp, 0);
  if (!size || !n) return 0;
  return ReadBytes(*s, dst, size * n) / size;
}
uint64_t h_fwrite(const void* src, uint64_t size, uint64_t n, uint64_t fp) {
  WITH_STREAM(fp, 0);
  if (!size || !n) return 0;
  return WriteBytes(*s, src, size * n) / size;
}
int32_t h_fgetc(uint64_t fp) {
  WITH_STREAM(fp, kEOF);
  return GetC(*s);
}
int32_t h_fputc(int32_t c, uint64_t fp) {
  WITH_STREAM(fp, kEOF);
  const uint8_t b = static_cast<uint8_t>(c);
  return WriteBytes(*s, &b, 1) == 1 ? b : kEOF;
}
int32_t h_getchar() {
  StreamPtr s = Lookup(FindData("__sF"));
  if (!s) return kEOF;
  std::lock_guard lk(s->mu);
  return GetC(*s);
}
int32_t h_putchar(int32_t c) {
  StreamPtr s = Lookup(FindData("__sF") + kFileSize);
  if (!s) return kEOF;
  std::lock_guard lk(s->mu);
  const uint8_t b = static_cast<uint8_t>(c);
  return WriteBytes(*s, &b, 1) == 1 ? b : kEOF;
}
int32_t h_ungetc(int32_t c, uint64_t fp) {
  WITH_STREAM(fp, kEOF);
  if (c == kEOF || !s->can_read) return kEOF;
  if (s->wbuf.size()) FlushW(*s);
  UngetC(*s, c);
  return c & 0xFF;
}
uint64_t h_fgets(char* buf, int32_t n, uint64_t fp) {
  WITH_STREAM(fp, 0);
  if (n <= 0) return 0;
  int32_t i = 0;
  while (i < n - 1) {
    const int c = GetC(*s);
    if (c == kEOF) break;
    buf[i++] = static_cast<char>(c);
    if (c == '\n') break;
  }
  if (i == 0) return 0;
  buf[i] = 0;
  return h2g(buf);
}
int32_t h_fputs(const char* str, uint64_t fp) {
  WITH_STREAM(fp, kEOF);
  const std::size_t n = std::strlen(str);
  return WriteBytes(*s, str, n) == n ? 1 : kEOF;
}
int32_t h_puts(const char* str) {
  StreamPtr s = Lookup(FindData("__sF") + kFileSize);
  if (!s) return kEOF;
  std::lock_guard lk(s->mu);
  const std::size_t n = std::strlen(str);
  const bool ok = WriteBytes(*s, str, n) == n && WriteBytes(*s, "\n", 1) == 1;
  return ok ? 1 : kEOF;
}
int32_t h_feof(uint64_t fp) {
  WITH_STREAM(fp, 0);
  return s->eof ? 1 : 0;
}
int32_t h_ferror(uint64_t fp) {
  WITH_STREAM(fp, 0);
  return s->error ? 1 : 0;
}
void h_clearerr(uint64_t fp) {
  StreamPtr s = Lookup(fp);
  if (!s) return;
  std::lock_guard lk(s->mu);
  s->eof = s->error = false;
}
int32_t h_fileno(Cpu& cpu, uint64_t fp) {
  StreamPtr s = Lookup(fp);
  if (!s || s->fd < 0) {
    SetErrno(cpu, err::BADF);
    return -1;
  }
  return s->fd;
}
int32_t h_fseek(Cpu& cpu, uint64_t fp, int64_t off, int32_t whence) {
  WITH_STREAM(fp, -1);
  if (whence < 0 || whence > 2) {
    SetErrno(cpu, err::INVAL);
    return -1;
  }
  const int r = SeekS(*s, off, whence);
  if (r < 0) {
    SetErrno(cpu, -r);
    return -1;
  }
  return 0;
}
int64_t h_ftell(Cpu& cpu, uint64_t fp) {
  WITH_STREAM(fp, -1);
  const int64_t p = TellS(*s);
  if (p < 0) {
    SetErrno(cpu, static_cast<int>(-p));
    return -1;
  }
  return p;
}
void h_rewind(uint64_t fp) {
  StreamPtr s = Lookup(fp);
  if (!s) return;
  std::lock_guard lk(s->mu);
  SeekS(*s, 0, 0);
  s->eof = s->error = false;
}
int32_t h_fgetpos(Cpu& cpu, uint64_t fp, int64_t* pos) {
  const int64_t p = h_ftell(cpu, fp);
  if (p < 0) return -1;
  *pos = p;
  return 0;
}
int32_t h_fsetpos(Cpu& cpu, uint64_t fp, const int64_t* pos) { return h_fseek(cpu, fp, *pos, 0); }
int32_t h_setvbuf(uint64_t fp, void*, int32_t mode, uint64_t) {
  StreamPtr s = Lookup(fp);
  if (!s) return -1;
  std::lock_guard lk(s->mu);
  if (!s->is_std) s->unbuffered = (mode == 2);  // _IONBF
  return 0;
}
void h_setbuf(uint64_t fp, void* buf) { h_setvbuf(fp, buf, buf ? 0 : 2, kBufSize); }

// ------------------------------------------------------------------------------------------ printf family
int32_t WriteFormatted(uint64_t fp, const std::string& str) {
  WITH_STREAM(fp, -1);
  if (WriteBytes(*s, str.data(), str.size()) != str.size()) return -1;
  return static_cast<int32_t>(str.size());
}
int32_t h_fprintf(uint64_t fp, const char* fmt, Varargs va) { return WriteFormatted(fp, FormatPrintf(fmt, va)); }
int32_t h_vfprintf(uint64_t fp, const char* fmt, VaList va) { return WriteFormatted(fp, FormatPrintf(fmt, va)); }
int32_t h_printf(const char* fmt, Varargs va) { return WriteFormatted(FindData("__sF") + kFileSize, FormatPrintf(fmt, va)); }
int32_t h_vprintf(const char* fmt, VaList va) { return WriteFormatted(FindData("__sF") + kFileSize, FormatPrintf(fmt, va)); }
int32_t StoreAllocated(char** out, const std::string& s) {
  const gaddr p = heap::Alloc(s.size() + 1);
  if (!p) return -1;
  std::memcpy(g2h(p), s.c_str(), s.size() + 1);
  *reinterpret_cast<uint64_t*>(out) = p;  // `out` is a guest char**: store the guest address, not a host pointer
  return static_cast<int32_t>(s.size());
}
int32_t h_vasprintf(char** out, const char* fmt, VaList va) { return StoreAllocated(out, FormatPrintf(fmt, va)); }
int32_t h_asprintf(char** out, const char* fmt, Varargs va) { return StoreAllocated(out, FormatPrintf(fmt, va)); }

// ------------------------------------------------------------------------------------------ scanf family
struct StringInput : ScanInput {
  explicit StringInput(const char* s) : p(s) {}
  int Get() override { return *p ? static_cast<uint8_t>(*p++) : (++past, -1); }
  void Unget(int c) override {
    if (c < 0) --past;
    else --p;
  }
  const char* p;
  int past = 0;
};
struct StreamInput : ScanInput {
  explicit StreamInput(Stream& st) : s(st) {}
  int Get() override { return GetC(s); }
  void Unget(int c) override {
    if (c >= 0) UngetC(s, c);
  }
  Stream& s;
};
int32_t h_sscanf(const char* str, const char* fmt, Varargs va) {
  StringInput in(str);
  return ScanfCore(in, fmt, va);
}
int32_t h_vsscanf(const char* str, const char* fmt, VaList va) {
  StringInput in(str);
  return ScanfCore(in, fmt, va);
}
int32_t h_fscanf(uint64_t fp, const char* fmt, Varargs va) {
  WITH_STREAM(fp, kEOF);
  StreamInput in(*s);
  return ScanfCore(in, fmt, va);
}
int32_t h_vfscanf(uint64_t fp, const char* fmt, VaList va) {
  WITH_STREAM(fp, kEOF);
  StreamInput in(*s);
  return ScanfCore(in, fmt, va);
}

// ------------------------------------------------------------------------------------------ wide characters (UTF-8 on the wire)
uint32_t h_getwc(uint64_t fp) {
  WITH_STREAM(fp, kWEOF);
  const int c = GetC(*s);
  if (c == kEOF) return kWEOF;
  if (c < 0x80) return static_cast<uint32_t>(c);
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  uint32_t cp = extra == 3 ? (c & 7) : extra == 2 ? (c & 15) : (c & 31);
  if (extra == 0) return static_cast<uint32_t>(c);  // stray continuation byte: pass through
  while (extra--) {
    const int d = GetC(*s);
    if (d == kEOF || (d & 0xC0) != 0x80) {
      if (d != kEOF) UngetC(*s, d);
      return kWEOF;
    }
    cp = (cp << 6) | (d & 0x3F);
  }
  return cp;
}
void EncodeUtf8(uint32_t cp, std::string& out) {
  if (cp < 0x80) out.push_back(static_cast<char>(cp));
  else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
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
uint32_t h_putwc(uint32_t wc, uint64_t fp) {
  WITH_STREAM(fp, kWEOF);
  std::string u;
  EncodeUtf8(wc, u);
  return WriteBytes(*s, u.data(), u.size()) == u.size() ? wc : kWEOF;
}
uint32_t h_ungetwc(uint32_t wc, uint64_t fp) {
  WITH_STREAM(fp, kWEOF);
  if (wc == kWEOF || !s->can_read) return kWEOF;
  std::string u;
  EncodeUtf8(wc, u);
  FlushW(*s);
  for (std::size_t i = u.size(); i-- > 0;) UngetC(*s, static_cast<uint8_t>(u[i]));
  return wc;
}
}  // namespace

// hooks for the wide-character module
int StreamGetc(gaddr fp) {
  StreamPtr s = Lookup(fp);
  if (!s) return kEOF;
  std::lock_guard lk(s->mu);
  return GetC(*s);
}
int StreamUngetc(int c, gaddr fp) {
  StreamPtr s = Lookup(fp);
  if (!s || c == kEOF) return kEOF;
  std::lock_guard lk(s->mu);
  UngetC(*s, c);
  return c;
}
int StreamPutc(int c, gaddr fp) {
  StreamPtr s = Lookup(fp);
  if (!s) return kEOF;
  std::lock_guard lk(s->mu);
  const uint8_t b = static_cast<uint8_t>(c);
  return WriteBytes(*s, &b, 1) == 1 ? b : kEOF;
}
int StreamWrite(gaddr fp, const void* data, uint64_t n) {
  StreamPtr s = Lookup(fp);
  if (!s) return -1;
  std::lock_guard lk(s->mu);
  return WriteBytes(*s, data, n) == n ? static_cast<int>(n) : -1;
}
void FlushAllStreams() {
  std::vector<StreamPtr> all;
  {
    std::lock_guard lk(g_tab_mu);
    for (auto& [k, v] : g_streams) all.push_back(v);
  }
  for (auto& s : all) {
    std::lock_guard lk(s->mu);
    FlushW(*s);
  }
}

namespace {
void h_perror(const char* msg) {
  StreamPtr s = Lookup(FindData("__sF") + 2 * kFileSize);
  if (!s) return;
  std::string line;
  if (msg && *msg) line = std::string(msg) + ": ";
  line += ErrorText(mem::R<int32_t>(Cpu::Current().errno_addr())) + "\n";
  std::lock_guard lk(s->mu);
  WriteBytes(*s, line.data(), line.size());
}
}  // namespace

void RegisterStdio() {
  const gaddr sf = DefineData("__sF", 3 * kFileSize, 16);
  for (int i = 0; i < 3; ++i) {
    auto s = std::make_shared<Stream>();
    s->file = vfs::MakeStdStream(i);
    s->can_read = i == 0;
    s->can_write = i != 0;
    s->unbuffered = true;
    s->is_std = true;
    s->fd = i;
    s->guest = sf + i * kFileSize;
    SetFileFields(s->guest, i, s->can_read, s->can_write);
    std::lock_guard lk(g_tab_mu);
    g_streams[s->guest] = std::move(s);
  }

  NOVA_HLE("fopen", h_fopen);
  NOVA_HLE("fdopen", h_fdopen);
  NOVA_HLE("freopen", h_freopen);
  NOVA_HLE("funopen", h_funopen);
  NOVA_HLE("tmpfile", h_tmpfile);
  NOVA_HLE("tmpnam", h_tmpnam);
  NOVA_HLE("fclose", h_fclose);
  NOVA_HLE("fflush", h_fflush);
  NOVA_HLE("fread", h_fread);
  NOVA_HLE("fwrite", h_fwrite);
  NOVA_HLE("fgetc", h_fgetc);
  NOVA_HLE("getc", h_fgetc);
  NOVA_HLE("fputc", h_fputc);
  NOVA_HLE("putc", h_fputc);
  NOVA_HLE("getchar", h_getchar);
  NOVA_HLE("putchar", h_putchar);
  NOVA_HLE("ungetc", h_ungetc);
  NOVA_HLE("fgets", h_fgets);
  NOVA_HLE("fputs", h_fputs);
  NOVA_HLE("puts", h_puts);
  NOVA_HLE_FAST("feof", h_feof);
  NOVA_HLE_FAST("ferror", h_ferror);
  NOVA_HLE("clearerr", h_clearerr);
  NOVA_HLE_FAST("fileno", h_fileno);
  NOVA_HLE("fseek", h_fseek);
  NOVA_HLE("fseeko", h_fseek);
  NOVA_HLE("fseeko64", h_fseek);
  NOVA_HLE_FAST("ftell", h_ftell);
  NOVA_HLE_FAST("ftello", h_ftell);
  NOVA_HLE_FAST("ftello64", h_ftell);
  NOVA_HLE("rewind", h_rewind);
  NOVA_HLE("fgetpos", h_fgetpos);
  NOVA_HLE("fsetpos", h_fsetpos);
  NOVA_HLE("setvbuf", h_setvbuf);
  NOVA_HLE("setbuf", h_setbuf);
  NOVA_HLE("perror", h_perror);

  NOVA_HLE("fprintf", h_fprintf);
  NOVA_HLE("vfprintf", h_vfprintf);
  NOVA_HLE("printf", h_printf);
  NOVA_HLE("vprintf", h_vprintf);
  NOVA_HLE("vasprintf", h_vasprintf);
  NOVA_HLE("asprintf", h_asprintf);
  NOVA_HLE("sscanf", h_sscanf);
  NOVA_HLE("vsscanf", h_vsscanf);
  NOVA_HLE("fscanf", h_fscanf);
  NOVA_HLE("vfscanf", h_vfscanf);

  NOVA_HLE("getwc", h_getwc);
  NOVA_HLE("fgetwc", h_getwc);
  NOVA_HLE("putwc", h_putwc);
  NOVA_HLE("fputwc", h_putwc);
  NOVA_HLE("ungetwc", h_ungetwc);
}
}  // namespace nova::hle::libc
