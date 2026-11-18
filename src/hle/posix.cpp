// POSIX file / directory / memory-mapping / socket-stub layer over the VFS. Return conventions follow libc:
// -1 + errno for the syscall wrappers, errno values returned directly only where POSIX says so.
// bionic arm64 layouts used here: struct stat (128 bytes, asm-generic), struct dirent (280 bytes), struct statfs (120),
// struct epoll_event (16, not packed), struct pollfd (8), struct addrinfo, struct hostent.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "hle/heap.h"
#include "hle/libc_internal.h"
#include "hle/vfs.h"

namespace nova::hle::libc {
namespace {
using std::int32_t;
using std::int64_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

int64_t Fail(Cpu& cpu, int e) {
  SetErrno(cpu, e);
  return -1;
}
// Result of a File op (>= 0 or -errno) -> libc convention.
int64_t Ret(Cpu& cpu, int64_t r) { return r < 0 ? Fail(cpu, static_cast<int>(-r)) : r; }
int64_t RetErr(Cpu& cpu, int e) { return e ? Fail(cpu, e) : 0; }

std::string GStr(const char* s) { return s ? std::string(s) : std::string(); }

// ------------------------------------------------------------------------------------------ wait hub (pipes, eventfd, epoll)
struct Hub {
  std::mutex mu;
  std::condition_variable cv;
} g_hub;

class Waitable : public vfs::File {
 public:
  virtual uint32_t Ready() = 0;  // EPOLLIN 1 / EPOLLOUT 4 / EPOLLHUP 0x10 bits; called with g_hub.mu held
};

struct PipeState {
  std::deque<uint8_t> data;
  bool write_closed = false, read_closed = false;
};
class PipeRead : public Waitable {
 public:
  explicit PipeRead(std::shared_ptr<PipeState> s) : s_(std::move(s)) { flags = vfs::kO_RDONLY; path = "pipe:[r]"; }
  ~PipeRead() override {
    std::lock_guard lk(g_hub.mu);
    s_->read_closed = true;
    g_hub.cv.notify_all();
  }
  int64_t Read(void* buf, uint64_t n) override {
    std::unique_lock lk(g_hub.mu);
    for (;;) {
      if (!s_->data.empty()) {
        const uint64_t k = std::min<uint64_t>(n, s_->data.size());
        for (uint64_t i = 0; i < k; ++i) {
          static_cast<uint8_t*>(buf)[i] = s_->data.front();
          s_->data.pop_front();
        }
        return static_cast<int64_t>(k);
      }
      if (s_->write_closed) return 0;
      if (flags & vfs::kO_NONBLOCK) return -err::AGAIN;
      g_hub.cv.wait(lk);
    }
  }
  int Stat(vfs::StatBuf& st) override {
    st = vfs::StatBuf{};
    st.mode = vfs::kIFIFO | 0600;
    return 0;
  }
  uint32_t Ready() override { return (!s_->data.empty() ? 1u : 0u) | (s_->write_closed ? 0x10u : 0u); }
  bool IsSocketLike() const override { return true; }

 private:
  std::shared_ptr<PipeState> s_;
};
class PipeWrite : public Waitable {
 public:
  explicit PipeWrite(std::shared_ptr<PipeState> s) : s_(std::move(s)) { flags = vfs::kO_WRONLY; path = "pipe:[w]"; }
  ~PipeWrite() override {
    std::lock_guard lk(g_hub.mu);
    s_->write_closed = true;
    g_hub.cv.notify_all();
  }
  int64_t Read(void*, uint64_t) override { return -err::BADF; }
  int64_t Write(const void* buf, uint64_t n) override {
    std::lock_guard lk(g_hub.mu);
    if (s_->read_closed) return -err::PIPE;
    s_->data.insert(s_->data.end(), static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + n);
    g_hub.cv.notify_all();
    return static_cast<int64_t>(n);
  }
  int Stat(vfs::StatBuf& st) override {
    st = vfs::StatBuf{};
    st.mode = vfs::kIFIFO | 0600;
    return 0;
  }
  uint32_t Ready() override { return 4; }

 private:
  std::shared_ptr<PipeState> s_;
};

class EventFile : public Waitable {
 public:
  EventFile(uint64_t init, int fl) : count_(init) {
    flags = fl;
    path = "anon_inode:[eventfd]";
  }
  int64_t Read(void* buf, uint64_t n) override {
    if (n < 8) return -err::INVAL;
    std::unique_lock lk(g_hub.mu);
    while (count_ == 0) {
      if (flags & vfs::kO_NONBLOCK) return -err::AGAIN;
      g_hub.cv.wait(lk);
    }
    std::memcpy(buf, &count_, 8);
    count_ = 0;
    return 8;
  }
  int64_t Write(const void* buf, uint64_t n) override {
    if (n < 8) return -err::INVAL;
    uint64_t v;
    std::memcpy(&v, buf, 8);
    std::lock_guard lk(g_hub.mu);
    count_ += v;
    g_hub.cv.notify_all();
    return 8;
  }
  int Stat(vfs::StatBuf& st) override {
    st = vfs::StatBuf{};
    st.mode = 0600;
    return 0;
  }
  uint32_t Ready() override { return (count_ ? 1u : 0u) | 4u; }

 private:
  uint64_t count_;
};

// A socket that never connects: the game is offline, so every connect fails fast.
class SockFile : public vfs::File {
 public:
  SockFile(int dom, int type) : dom_(dom), type_(type) {
    flags = vfs::kO_RDWR;
    path = "socket:[0]";
  }
  int64_t Read(void*, uint64_t) override { return -err::NOTCONN; }
  int64_t Write(const void*, uint64_t) override { return -err::NOTCONN; }
  int Stat(vfs::StatBuf& st) override {
    st = vfs::StatBuf{};
    st.mode = vfs::kIFSOCK | 0777;
    return 0;
  }
  bool IsSocketLike() const override { return true; }
  int dom_, type_;
};

struct EpollEntry {
  uint32_t events;
  uint64_t data;
};
class EpollFile : public vfs::File {
 public:
  EpollFile() { path = "anon_inode:[eventpoll]"; }
  int64_t Read(void*, uint64_t) override { return -err::INVAL; }
  int Stat(vfs::StatBuf& st) override {
    st = vfs::StatBuf{};
    st.mode = 0600;
    return 0;
  }
  std::map<int, EpollEntry> watch;  // guarded by g_hub.mu
};

// ------------------------------------------------------------------------------------------ struct marshalling
void WriteStat(void* dst, const vfs::StatBuf& s) {
  auto* p = static_cast<uint8_t*>(dst);
  std::memset(p, 0, 128);
  auto put = [&](size_t off, auto v) { std::memcpy(p + off, &v, sizeof v); };
  put(0, s.dev);
  put(8, s.ino);
  put(16, s.mode);
  put(20, s.nlink);
  put(24, s.uid);
  put(28, s.gid);
  put(32, s.rdev);
  put(48, s.size);
  put(56, s.blksize);
  put(64, s.blocks);
  put(72, s.atime);
  put(80, s.atime_ns);
  put(88, s.mtime);
  put(96, s.mtime_ns);
  put(104, s.ctime);
  put(112, s.ctime_ns);
}

bool FillStatfs(uint8_t* p) {
  uint64_t total = 0, avail = 0;
  if (!vfs::DiskSpace(total, avail)) return false;
  std::memset(p, 0, 120);
  auto put = [&](size_t off, uint64_t v) { std::memcpy(p + off, &v, 8); };
  put(0, 0xEF53);  // EXT4_SUPER_MAGIC
  put(8, 4096);
  put(16, total / 4096);
  put(24, avail / 4096);
  put(32, avail / 4096);
  put(40, 1 << 20);
  put(48, 1 << 19);
  put(64, 255);
  put(72, 4096);
  return true;
}

// ------------------------------------------------------------------------------------------ files
int32_t h_open(Cpu& cpu, const char* path, int32_t flags, Varargs va) {
  if (!path) return static_cast<int32_t>(Fail(cpu, err::FAULT));
  uint32_t mode = 0;
  if (flags & vfs::kO_CREAT) mode = va.Next<uint32_t>();
  vfs::FilePtr f;
  const int e = vfs::Open(path, flags, static_cast<int>(mode), f);
  if (e) return static_cast<int32_t>(Fail(cpu, e));
  f->flags = flags;
  const int fd = vfs::Install(std::move(f));
  return fd < 0 ? static_cast<int32_t>(Fail(cpu, -fd)) : fd;
}
int32_t h_close(Cpu& cpu, int32_t fd) {
  const int r = vfs::Close(fd);
  return r < 0 ? static_cast<int32_t>(Fail(cpu, -r)) : 0;
}
int64_t h_read(Cpu& cpu, int32_t fd, void* buf, uint64_t n) {
  auto f = vfs::Get(fd);
  if (!f) return Fail(cpu, err::BADF);
  if (!buf && n) return Fail(cpu, err::FAULT);
  return Ret(cpu, f->Read(buf, n));
}
int64_t h_write(Cpu& cpu, int32_t fd, const void* buf, uint64_t n) {
  auto f = vfs::Get(fd);
  if (!f) return Fail(cpu, err::BADF);
  if (!buf && n) return Fail(cpu, err::FAULT);
  return Ret(cpu, f->Write(buf, n));
}
int64_t h_pread(Cpu& cpu, int32_t fd, void* buf, uint64_t n, int64_t off) {
  auto f = vfs::Get(fd);
  return f ? Ret(cpu, f->Pread(buf, n, off)) : Fail(cpu, err::BADF);
}
int64_t h_pwrite(Cpu& cpu, int32_t fd, const void* buf, uint64_t n, int64_t off) {
  auto f = vfs::Get(fd);
  return f ? Ret(cpu, f->Pwrite(buf, n, off)) : Fail(cpu, err::BADF);
}
int64_t h_lseek(Cpu& cpu, int32_t fd, int64_t off, int32_t whence) {
  auto f = vfs::Get(fd);
  return f ? Ret(cpu, f->Seek(off, whence)) : Fail(cpu, err::BADF);
}
int32_t h_fsync(Cpu& cpu, int32_t fd) {
  auto f = vfs::Get(fd);
  return f ? static_cast<int32_t>(Ret(cpu, f->Sync())) : static_cast<int32_t>(Fail(cpu, err::BADF));
}
int32_t h_ftruncate(Cpu& cpu, int32_t fd, int64_t size) {
  auto f = vfs::Get(fd);
  return f ? static_cast<int32_t>(Ret(cpu, f->Truncate(size))) : static_cast<int32_t>(Fail(cpu, err::BADF));
}
int32_t h_truncate(Cpu& cpu, const char* path, int64_t size) {
  return static_cast<int32_t>(RetErr(cpu, path ? vfs::Truncate(path, size) : err::FAULT));
}
int32_t h_dup(Cpu& cpu, int32_t fd) {
  const int r = vfs::Dup(fd, 0);
  return r < 0 ? static_cast<int32_t>(Fail(cpu, -r)) : r;
}
int32_t h_dup2(Cpu& cpu, int32_t fd, int32_t nfd) {
  const int r = vfs::Dup2(fd, nfd);
  return r < 0 ? static_cast<int32_t>(Fail(cpu, -r)) : r;
}
int32_t h_pipe(Cpu& cpu, int32_t* fds) {
  if (!fds) return static_cast<int32_t>(Fail(cpu, err::FAULT));
  auto st = std::make_shared<PipeState>();
  const int r = vfs::Install(std::make_shared<PipeRead>(st));
  if (r < 0) return static_cast<int32_t>(Fail(cpu, -r));
  const int w = vfs::Install(std::make_shared<PipeWrite>(st));
  if (w < 0) {
    vfs::Close(r);
    return static_cast<int32_t>(Fail(cpu, -w));
  }
  fds[0] = r;
  fds[1] = w;
  return 0;
}
int32_t h_pipe2(Cpu& cpu, int32_t* fds, int32_t flags) {
  const int32_t r = h_pipe(cpu, fds);
  if (r == 0 && (flags & vfs::kO_NONBLOCK)) {
    vfs::Get(fds[0])->flags |= vfs::kO_NONBLOCK;
    vfs::Get(fds[1])->flags |= vfs::kO_NONBLOCK;
  }
  return r;
}
int64_t h_sendfile(Cpu& cpu, int32_t out_fd, int32_t in_fd, int64_t* offset, uint64_t count) {
  auto in = vfs::Get(in_fd), out = vfs::Get(out_fd);
  if (!in || !out) return Fail(cpu, err::BADF);
  std::vector<uint8_t> buf(1 << 16);
  int64_t total = 0;
  while (static_cast<uint64_t>(total) < count) {
    const uint64_t want = std::min<uint64_t>(buf.size(), count - total);
    const int64_t r = offset ? in->Pread(buf.data(), want, *offset + total) : in->Read(buf.data(), want);
    if (r < 0) return total ? total : Fail(cpu, static_cast<int>(-r));
    if (r == 0) break;
    const int64_t w = out->Write(buf.data(), static_cast<uint64_t>(r));
    if (w < 0) return total ? total : Fail(cpu, static_cast<int>(-w));
    total += w;
    if (w < r) break;
  }
  if (offset) *offset += total;
  return total;
}
int32_t h_fcntl(Cpu& cpu, int32_t fd, int32_t cmd, Varargs va) {
  auto f = vfs::Get(fd);
  if (!f) return static_cast<int32_t>(Fail(cpu, err::BADF));
  switch (cmd) {
    case 0:     // F_DUPFD
    case 1030: {  // F_DUPFD_CLOEXEC
      const int r = vfs::Dup(fd, va.Next<int32_t>());
      return r < 0 ? static_cast<int32_t>(Fail(cpu, -r)) : r;
    }
    case 1:  // F_GETFD
    case 2:  // F_SETFD
      return 0;
    case 3: return f->flags;  // F_GETFL
    case 4: {                  // F_SETFL: only O_NONBLOCK / O_APPEND are changeable
      const int32_t nf = va.Next<int32_t>();
      const int32_t mask = vfs::kO_NONBLOCK | vfs::kO_APPEND;
      f->flags = (f->flags & ~mask) | (nf & mask);
      return 0;
    }
    default: return static_cast<int32_t>(Fail(cpu, err::INVAL));
  }
}
int32_t h_ioctl(Cpu& cpu, int32_t fd, uint64_t req, Varargs va) {
  auto f = vfs::Get(fd);
  if (!f) return static_cast<int32_t>(Fail(cpu, err::BADF));
  switch (req) {
    case 0x5421: {  // FIONBIO
      const uint64_t p = va.NextInt();
      if (p && mem::R32(p)) f->flags |= vfs::kO_NONBLOCK;
      else f->flags &= ~vfs::kO_NONBLOCK;
      return 0;
    }
    case 0x541B: {  // FIONREAD
      if (const uint64_t p = va.NextInt()) mem::W32(p, 0);
      return 0;
    }
    case 0x8912: {  // SIOCGIFCONF: no network interfaces (offline)
      if (const uint64_t p = va.NextInt()) mem::W32(p, 0);
      return 0;
    }
    case 0x8915: return static_cast<int32_t>(Fail(cpu, err::ADDRNOTAVAIL));  // SIOCGIFADDR
    default: return static_cast<int32_t>(Fail(cpu, err::NOTTY));
  }
}
int32_t h_isatty(Cpu& cpu, int32_t fd) {
  SetErrno(cpu, vfs::Get(fd) ? err::NOTTY : err::BADF);
  return 0;
}
int32_t h_tcattr(Cpu& cpu, int32_t) { return static_cast<int32_t>(Fail(cpu, err::NOTTY)); }

// ------------------------------------------------------------------------------------------ paths
int32_t h_stat(Cpu& cpu, const char* path, void* buf) {
  if (!path || !buf) return static_cast<int32_t>(Fail(cpu, err::FAULT));
  vfs::StatBuf st;
  const int e = vfs::StatPath(path, st);
  if (e) return static_cast<int32_t>(Fail(cpu, e));
  WriteStat(buf, st);
  return 0;
}
int32_t h_fstat(Cpu& cpu, int32_t fd, void* buf) {
  auto f = vfs::Get(fd);
  if (!f) return static_cast<int32_t>(Fail(cpu, err::BADF));
  vfs::StatBuf st;
  const int r = f->Stat(st);
  if (r < 0) return static_cast<int32_t>(Fail(cpu, -r));
  WriteStat(buf, st);
  return 0;
}
int32_t h_statfs(Cpu& cpu, const char* path, uint8_t* buf) {
  vfs::StatBuf st;
  const int e = path ? vfs::StatPath(path, st) : err::FAULT;
  if (e) return static_cast<int32_t>(Fail(cpu, e));
  return FillStatfs(buf) ? 0 : static_cast<int32_t>(Fail(cpu, err::IO));
}
int32_t h_fstatfs(Cpu& cpu, int32_t fd, uint8_t* buf) {
  if (!vfs::Get(fd)) return static_cast<int32_t>(Fail(cpu, err::BADF));
  return FillStatfs(buf) ? 0 : static_cast<int32_t>(Fail(cpu, err::IO));
}
int32_t h_access(Cpu& cpu, const char* path, int32_t mode) {
  return static_cast<int32_t>(RetErr(cpu, path ? vfs::Access(path, mode) : err::FAULT));
}
int32_t h_mkdir(Cpu& cpu, const char* path, uint32_t) { return static_cast<int32_t>(RetErr(cpu, path ? vfs::Mkdir(path) : err::FAULT)); }
int32_t h_rmdir(Cpu& cpu, const char* path) { return static_cast<int32_t>(RetErr(cpu, path ? vfs::Rmdir(path) : err::FAULT)); }
int32_t h_unlink(Cpu& cpu, const char* path) { return static_cast<int32_t>(RetErr(cpu, path ? vfs::Unlink(path) : err::FAULT)); }
int32_t h_remove(Cpu& cpu, const char* path) {
  if (!path) return static_cast<int32_t>(Fail(cpu, err::FAULT));
  vfs::StatBuf st;
  const int e = vfs::StatPath(path, st);
  if (e) return static_cast<int32_t>(Fail(cpu, e));
  return static_cast<int32_t>(RetErr(cpu, (st.mode & vfs::kIFMT) == vfs::kIFDIR ? vfs::Rmdir(path) : vfs::Unlink(path)));
}
int32_t h_rename(Cpu& cpu, const char* a, const char* b) {
  return static_cast<int32_t>(RetErr(cpu, a && b ? vfs::Rename(a, b) : err::FAULT));
}
int32_t h_chdir(Cpu& cpu, const char* path) {
  if (!path) return static_cast<int32_t>(Fail(cpu, err::FAULT));
  vfs::StatBuf st;
  const int e = vfs::StatPath(path, st);
  if (e) return static_cast<int32_t>(Fail(cpu, e));
  if ((st.mode & vfs::kIFMT) != vfs::kIFDIR) return static_cast<int32_t>(Fail(cpu, err::NOTDIR));
  vfs::SetCwd(vfs::Normalize(path));
  return 0;
}
uint64_t h_getcwd(Cpu& cpu, char* buf, uint64_t size) {
  const std::string c = vfs::Cwd();
  gaddr out = buf ? h2g(buf) : 0;
  if (!buf) out = heap::Alloc(std::max<uint64_t>(size, c.size() + 1));
  else if (size < c.size() + 1) {
    SetErrno(cpu, err::RANGE);
    return 0;
  }
  if (!out) return 0;
  std::memcpy(g2h(out), c.c_str(), c.size() + 1);
  return out;
}
int32_t h_chmod(Cpu& cpu, const char* path, uint32_t) {
  vfs::StatBuf st;
  return static_cast<int32_t>(RetErr(cpu, path ? vfs::StatPath(path, st) : err::FAULT));
}
std::atomic<uint32_t> g_umask{022};
uint32_t h_umask(uint32_t m) { return g_umask.exchange(m & 0777); }
uint64_t h_realpath(Cpu& cpu, const char* path, char* resolved) {
  if (!path) {
    SetErrno(cpu, err::INVAL);
    return 0;
  }
  std::string out;
  const int e = vfs::Realpath(path, out);
  if (e) {
    SetErrno(cpu, e);
    return 0;
  }
  gaddr dst = resolved ? h2g(resolved) : heap::Alloc(out.size() + 1);
  if (!dst) return 0;
  std::memcpy(g2h(dst), out.c_str(), out.size() + 1);
  return dst;
}
int32_t h_mkstemp(Cpu& cpu, char* tmpl) {
  if (!tmpl) return static_cast<int32_t>(Fail(cpu, err::FAULT));
  const size_t n = std::strlen(tmpl);
  if (n < 6 || std::strcmp(tmpl + n - 6, "XXXXXX") != 0) return static_cast<int32_t>(Fail(cpu, err::INVAL));
  static const char kAlpha[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  for (int attempt = 0; attempt < 100; ++attempt) {
    uint64_t r = static_cast<uint64_t>(MonotonicNs()) * 6364136223846793005ull + attempt * 1442695040888963407ull;
    for (int i = 0; i < 6; ++i) {
      tmpl[n - 6 + i] = kAlpha[(r >> (i * 8)) % 36];
    }
    vfs::FilePtr f;
    const int e = vfs::Open(tmpl, vfs::kO_RDWR | vfs::kO_CREAT | vfs::kO_EXCL, 0600, f);
    if (e == err::EXIST) continue;
    if (e) return static_cast<int32_t>(Fail(cpu, e));
    f->flags = vfs::kO_RDWR;
    const int fd = vfs::Install(std::move(f));
    return fd < 0 ? static_cast<int32_t>(Fail(cpu, -fd)) : fd;
  }
  return static_cast<int32_t>(Fail(cpu, err::EXIST));
}

// ------------------------------------------------------------------------------------------ directories
struct DirState {
  std::vector<vfs::DirEntry> entries;
  size_t idx = 0;
};
std::mutex g_dir_mu;
std::unordered_map<uint64_t, std::shared_ptr<DirState>> g_dirs;  // DIR* (guest block address) -> state
constexpr gsize kDirBlock = 16 + 280;

uint64_t h_opendir(Cpu& cpu, const char* path) {
  if (!path) {
    SetErrno(cpu, err::FAULT);
    return 0;
  }
  auto st = std::make_shared<DirState>();
  const int e = vfs::ListDir(path, st->entries);
  if (e) {
    SetErrno(cpu, e);
    return 0;
  }
  const gaddr blk = heap::Alloc(kDirBlock);
  if (!blk) {
    SetErrno(cpu, err::NOMEM);
    return 0;
  }
  std::memset(g2h(blk), 0, kDirBlock);
  std::lock_guard lk(g_dir_mu);
  g_dirs[blk] = std::move(st);
  return blk;
}
uint64_t h_readdir(Cpu& cpu, uint64_t dir) {
  std::shared_ptr<DirState> st;
  {
    std::lock_guard lk(g_dir_mu);
    auto it = g_dirs.find(dir);
    if (it != g_dirs.end()) st = it->second;
  }
  if (!st) {
    SetErrno(cpu, err::BADF);
    return 0;
  }
  if (st->idx >= st->entries.size()) return 0;
  const vfs::DirEntry& e = st->entries[st->idx++];
  const gaddr d = dir + 16;
  std::memset(g2h(d), 0, 280);
  mem::W64(d + 0, e.ino ? e.ino : st->idx);
  mem::W64(d + 8, st->idx);
  mem::W<uint16_t>(d + 16, 280);
  mem::W<uint8_t>(d + 18, e.type);
  const size_t n = std::min<size_t>(e.name.size(), 255);
  std::memcpy(g2h(d + 19), e.name.data(), n);
  return d;
}
int32_t h_closedir(Cpu& cpu, uint64_t dir) {
  {
    std::lock_guard lk(g_dir_mu);
    if (!g_dirs.erase(dir)) return static_cast<int32_t>(Fail(cpu, err::BADF));
  }
  heap::Free(dir);
  return 0;
}
void h_rewinddir(uint64_t dir) {
  std::lock_guard lk(g_dir_mu);
  auto it = g_dirs.find(dir);
  if (it != g_dirs.end()) it->second->idx = 0;
}

// ------------------------------------------------------------------------------------------ memory mapping
constexpr uint64_t kMapFailed = ~0ull;
uint64_t MapFail(Cpu& cpu, int e) {
  SetErrno(cpu, e);
  return kMapFailed;
}
uint64_t h_mmap(Cpu& cpu, uint64_t addr, uint64_t len, int32_t prot, int32_t flags, int32_t fd, int64_t off) {
  if (!len) return MapFail(cpu, err::INVAL);
  const bool anon = (flags & 0x20) != 0, fixed = (flags & 0x10) != 0;
  vfs::FilePtr file;
  if (!anon) {
    file = vfs::Get(fd);
    if (!file) return MapFail(cpu, err::BADF);
  }
  const gsize size = (len + kPageSize - 1) & ~(kPageSize - 1);
  const uint8_t gp = static_cast<uint8_t>(prot & 7);
  gaddr base = 0;
  if (fixed && addr) {
    if (GuestMemory::Map(addr, size, kProtRW, "mmap")) {
      base = addr;
    } else if (GuestMemory::RangeHas(addr, size, kProtRead)) {
      GuestMemory::Protect(addr, size, kProtRW);
      std::memset(g2h(addr), 0, size);
      base = addr;
    } else {
      return MapFail(cpu, err::NOMEM);
    }
  } else {
    base = GuestMemory::MapAnywhere(Zone::Anon, size, kProtRW, "mmap");
    if (!base) return MapFail(cpu, err::NOMEM);
  }
  if (file) {  // a private copy of the file contents (read-only data, packs); shared writes are not propagated
    int64_t got = 0;
    while (static_cast<uint64_t>(got) < len) {
      const int64_t r = file->Pread(g2h(base + got), std::min<uint64_t>(len - got, 1u << 28), off + got);
      if (r <= 0) break;
      got += r;
    }
  }
  if (gp != kProtRW) GuestMemory::Protect(base, size, gp);
  return base;
}
int32_t h_munmap(Cpu& cpu, uint64_t addr, uint64_t len) {
  if (!len || (addr & (kPageSize - 1))) return static_cast<int32_t>(Fail(cpu, err::INVAL));
  const gsize size = (len + kPageSize - 1) & ~(kPageSize - 1);
  return GuestMemory::Unmap(addr, size) ? 0 : static_cast<int32_t>(Fail(cpu, err::INVAL));
}
int32_t h_mprotect(Cpu& cpu, uint64_t addr, uint64_t len, int32_t prot) {
  if (addr & (kPageSize - 1)) return static_cast<int32_t>(Fail(cpu, err::INVAL));
  const gsize size = (len + kPageSize - 1) & ~(kPageSize - 1);
  return GuestMemory::Protect(addr, size, static_cast<uint8_t>(prot & 7)) ? 0 : static_cast<int32_t>(Fail(cpu, err::NOMEM));
}
int32_t h_zero3(uint64_t, uint64_t, int32_t) { return 0; }

// ------------------------------------------------------------------------------------------ sockets (offline)
bool IsSock(const vfs::FilePtr& f) { return f && dynamic_cast<SockFile*>(f.get()) != nullptr; }
int32_t h_socket(Cpu& cpu, int32_t dom, int32_t type, int32_t) {
  if (dom != 1 && dom != 2 && dom != 10) return static_cast<int32_t>(Fail(cpu, err::AFNOSUPPORT));
  auto s = std::make_shared<SockFile>(dom, type & 0xF);
  if (type & vfs::kO_NONBLOCK) s->flags |= vfs::kO_NONBLOCK;
  const int fd = vfs::Install(std::move(s));
  return fd < 0 ? static_cast<int32_t>(Fail(cpu, -fd)) : fd;
}
int32_t SockOp(Cpu& cpu, int32_t fd, int e) {
  auto f = vfs::Get(fd);
  if (!f) return static_cast<int32_t>(Fail(cpu, err::BADF));
  if (!IsSock(f)) return static_cast<int32_t>(Fail(cpu, err::NOTSOCK));
  return e ? static_cast<int32_t>(Fail(cpu, e)) : 0;
}
int32_t h_bind(Cpu& cpu, int32_t fd, uint64_t, uint32_t) { return SockOp(cpu, fd, 0); }
int32_t h_listen(Cpu& cpu, int32_t fd, int32_t) { return SockOp(cpu, fd, 0); }
int32_t h_accept(Cpu& cpu, int32_t fd, uint64_t, uint64_t) { return SockOp(cpu, fd, err::NETDOWN); }
int32_t h_connect(Cpu& cpu, int32_t fd, uint64_t, uint32_t) { return SockOp(cpu, fd, err::NETUNREACH); }
int64_t h_send(Cpu& cpu, int32_t fd, uint64_t, uint64_t, int32_t) { return SockOp(cpu, fd, err::NOTCONN); }
int64_t h_sendto(Cpu& cpu, int32_t fd, uint64_t, uint64_t, int32_t, uint64_t, uint32_t) { return SockOp(cpu, fd, err::NETUNREACH); }
int64_t h_recv(Cpu& cpu, int32_t fd, uint64_t, uint64_t, int32_t) { return SockOp(cpu, fd, err::NOTCONN); }
int64_t h_recvfrom(Cpu& cpu, int32_t fd, uint64_t, uint64_t, int32_t, uint64_t, uint64_t) { return SockOp(cpu, fd, err::NOTCONN); }
int64_t h_sendmsg(Cpu& cpu, int32_t fd, uint64_t, int32_t) { return SockOp(cpu, fd, err::NETUNREACH); }
int64_t h_recvmsg(Cpu& cpu, int32_t fd, uint64_t, int32_t) { return SockOp(cpu, fd, err::NOTCONN); }
int32_t h_shutdown(Cpu& cpu, int32_t fd, int32_t) { return SockOp(cpu, fd, 0); }
int32_t h_setsockopt(Cpu& cpu, int32_t fd, int32_t, int32_t, uint64_t, uint32_t) { return SockOp(cpu, fd, 0); }
int32_t h_getsockopt(Cpu& cpu, int32_t fd, int32_t, int32_t, uint64_t val, uint64_t lenp) {
  const int32_t r = SockOp(cpu, fd, 0);
  if (r == 0 && val && lenp) {  // SO_ERROR and friends: 0
    const uint32_t n = std::min<uint32_t>(mem::R32(lenp), 4);
    std::memset(g2h(val), 0, n);
    mem::W32(lenp, n);
  }
  return r;
}
int32_t h_getsockname(Cpu& cpu, int32_t fd, uint64_t addr, uint64_t lenp) {
  const int32_t r = SockOp(cpu, fd, 0);
  if (r == 0 && addr && lenp) {
    std::memset(g2h(addr), 0, std::min<uint32_t>(mem::R32(lenp), 16));
    mem::W<uint16_t>(addr, 2);
    mem::W32(lenp, 16);
  }
  return r;
}
int32_t h_getpeername(Cpu& cpu, int32_t fd, uint64_t, uint64_t) { return SockOp(cpu, fd, err::NOTCONN); }

// Sleeps `ms` (-1 = forever) on the hub until woken or timed out. Caller holds the lock.
void HubWait(std::unique_lock<std::mutex>& lk, int64_t deadline_ns) {
  if (deadline_ns < 0) g_hub.cv.wait(lk);
  else g_hub.cv.wait_until(lk, std::chrono::steady_clock::now() + std::chrono::nanoseconds(std::max<int64_t>(0, deadline_ns - MonotonicNs())));
}

int32_t h_select(Cpu& cpu, int32_t nfds, uint8_t* r, uint8_t* w, uint8_t* e, const Timeval* tv) {
  (void)cpu;
  (void)nfds;
  std::unique_lock lk(g_hub.mu);
  const int64_t deadline = tv ? MonotonicNs() + tv->sec * 1000000000ll + tv->usec * 1000 : -1;
  // Nothing real can become ready (no live sockets, pipes/eventfd are not selectable here): honour the timeout.
  while (deadline < 0 || MonotonicNs() < deadline) HubWait(lk, deadline);
  for (uint8_t* s : {r, w, e})
    if (s) std::memset(s, 0, 128);
  return 0;
}
int32_t h_poll(Cpu& cpu, uint8_t* fds, uint64_t n, int32_t timeout_ms) {
  (void)cpu;
  const int64_t deadline = timeout_ms < 0 ? -1 : MonotonicNs() + int64_t{timeout_ms} * 1000000;
  std::unique_lock lk(g_hub.mu);
  for (;;) {
    int32_t ready = 0;
    for (uint64_t i = 0; i < n; ++i) {
      uint8_t* p = fds + i * 8;
      int32_t fd;
      int16_t events, revents = 0;
      std::memcpy(&fd, p, 4);
      std::memcpy(&events, p + 4, 2);
      if (fd >= 0) {
        auto f = vfs::Get(fd);
        if (!f) revents = 0x20;  // POLLNVAL
        else if (auto* wf = dynamic_cast<Waitable*>(f.get())) {
          const uint32_t rd = wf->Ready();
          if ((events & 1) && (rd & 1)) revents |= 1;
          if ((events & 4) && (rd & 4)) revents |= 4;
          if (rd & 0x10) revents |= 0x10;
        } else if (!f->IsSocketLike()) {
          revents = events & (1 | 4);  // regular files are always ready
        }
      }
      std::memcpy(p + 6, &revents, 2);
      if (revents) ++ready;
    }
    if (ready || timeout_ms == 0) return ready;
    if (deadline >= 0 && MonotonicNs() >= deadline) return 0;
    HubWait(lk, deadline);
  }
}

// epoll: level-triggered emulation over the Waitable objects (eventfd / pipes); sockets never become ready.
int32_t h_epoll_create(Cpu& cpu, int32_t) {
  const int fd = vfs::Install(std::make_shared<EpollFile>());
  return fd < 0 ? static_cast<int32_t>(Fail(cpu, -fd)) : fd;
}
int32_t h_epoll_create1(Cpu& cpu, int32_t) { return h_epoll_create(cpu, 1); }
int32_t h_epoll_ctl(Cpu& cpu, int32_t epfd, int32_t op, int32_t fd, const uint8_t* ev) {
  auto ep = std::dynamic_pointer_cast<EpollFile>(vfs::Get(epfd));
  if (!ep) return static_cast<int32_t>(Fail(cpu, err::BADF));
  if (!vfs::Get(fd)) return static_cast<int32_t>(Fail(cpu, err::BADF));
  std::lock_guard lk(g_hub.mu);
  if (op == 2) {
    if (!ep->watch.erase(fd)) return static_cast<int32_t>(Fail(cpu, err::NOENT));
    return 0;
  }
  EpollEntry e{};
  if (ev) {
    std::memcpy(&e.events, ev, 4);
    std::memcpy(&e.data, ev + 8, 8);
  }
  if (op == 1) {
    if (ep->watch.count(fd)) return static_cast<int32_t>(Fail(cpu, err::EXIST));
    ep->watch[fd] = e;
  } else if (op == 3) {
    if (!ep->watch.count(fd)) return static_cast<int32_t>(Fail(cpu, err::NOENT));
    ep->watch[fd] = e;
  } else {
    return static_cast<int32_t>(Fail(cpu, err::INVAL));
  }
  return 0;
}
int32_t h_epoll_wait(Cpu& cpu, int32_t epfd, uint8_t* out, int32_t maxev, int32_t timeout_ms) {
  auto ep = std::dynamic_pointer_cast<EpollFile>(vfs::Get(epfd));
  if (!ep) return static_cast<int32_t>(Fail(cpu, err::BADF));
  if (maxev <= 0) return static_cast<int32_t>(Fail(cpu, err::INVAL));
  const int64_t deadline = timeout_ms < 0 ? -1 : MonotonicNs() + int64_t{timeout_ms} * 1000000;
  // A guest thread polling with timeout 0 would spin a host core at ~1M calls/s (there are no real fds, so the
  // answer is almost always "nothing"). Count consecutive empty polls per thread and back off: a few free polls,
  // then yields, then short sleeps. Any non-empty or blocking call resets the streak.
  static thread_local uint32_t t_empty_polls = 0;
  std::unique_lock lk(g_hub.mu);
  for (;;) {
    int32_t n = 0;
    for (const auto& [fd, e] : ep->watch) {
      auto f = vfs::Get(fd);
      auto* wf = dynamic_cast<Waitable*>(f.get());
      if (!wf) continue;
      const uint32_t rd = wf->Ready() & (e.events | 0x10);
      if (!rd) continue;
      if (n < maxev) {
        std::memset(out + n * 16, 0, 16);
        std::memcpy(out + n * 16, &rd, 4);
        std::memcpy(out + n * 16 + 8, &e.data, 8);
      }
      if (++n >= maxev) break;
    }
    if (n) {
      t_empty_polls = 0;
      return n;
    }
    if (timeout_ms == 0) {
      const uint32_t streak = ++t_empty_polls;
      lk.unlock();
      if (streak > 256) SleepNs(100000);
      else if (streak > 16) SwitchToThread();
      return 0;
    }
    t_empty_polls = 0;
    if (deadline >= 0 && MonotonicNs() >= deadline) return 0;
    HubWait(lk, deadline);
  }
}
int32_t h_eventfd(Cpu& cpu, uint32_t init, int32_t flags) {
  const int fd = vfs::Install(std::make_shared<EventFile>(init, (flags & 0x800) ? vfs::kO_NONBLOCK : 0));
  return fd < 0 ? static_cast<int32_t>(Fail(cpu, -fd)) : fd;
}

// ------------------------------------------------------------------------------------------ name resolution (numeric only)
bool ParseIPv4(const char* s, uint32_t& net) {
  unsigned parts[4];
  int n = 0;
  uint64_t acc = 0;
  bool have = false;
  for (const char* p = s;; ++p) {
    if (*p >= '0' && *p <= '9') {
      acc = acc * 10 + (*p - '0');
      have = true;
      if (acc > 255) return false;
    } else if ((*p == '.' || *p == 0) && have && n < 4) {
      parts[n++] = static_cast<unsigned>(acc);
      acc = 0;
      have = false;
      if (*p == 0) break;
    } else {
      return false;
    }
  }
  if (n != 4) return false;
  net = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);  // network byte order read little-endian
  return true;
}
uint32_t h_inet_addr(const char* s) {
  uint32_t a;
  return s && ParseIPv4(s, a) ? a : 0xFFFFFFFFu;
}
uint64_t h_inet_ntoa(uint32_t in) {
  const gaddr b = ThreadBuffer(4, 32);
  std::snprintf(g2h<char>(b), 32, "%u.%u.%u.%u", in & 255, (in >> 8) & 255, (in >> 16) & 255, in >> 24);
  return b;
}
uint64_t h_inet_ntop(Cpu& cpu, int32_t af, const uint8_t* src, char* dst, uint32_t size) {
  std::string s;
  if (af == 2) {
    s = std::to_string(src[0]) + "." + std::to_string(src[1]) + "." + std::to_string(src[2]) + "." + std::to_string(src[3]);
  } else if (af == 10) {
    uint16_t w[8];
    for (int i = 0; i < 8; ++i) w[i] = static_cast<uint16_t>((src[2 * i] << 8) | src[2 * i + 1]);
    int bs = -1, bl = 0;
    for (int i = 0; i < 8;) {
      if (w[i]) {
        ++i;
        continue;
      }
      int j = i;
      while (j < 8 && !w[j]) ++j;
      if (j - i > bl && j - i >= 2) bs = i, bl = j - i;
      i = j;
    }
    char b[8];
    for (int i = 0; i < 8; ++i) {
      if (i == bs) {
        s += "::";
        i += bl - 1;
        continue;
      }
      if (!s.empty() && s.back() != ':') s += ":";
      std::snprintf(b, sizeof b, "%x", w[i]);
      s += b;
    }
  } else {
    SetErrno(cpu, err::AFNOSUPPORT);
    return 0;
  }
  if (s.size() + 1 > size) {
    SetErrno(cpu, err::NOSPC);
    return 0;
  }
  std::memcpy(dst, s.c_str(), s.size() + 1);
  return h2g(dst);
}
uint64_t h_if_indextoname(Cpu& cpu, uint32_t, char*) {
  SetErrno(cpu, 6);  // ENXIO
  return 0;
}
int32_t h_gethostname(Cpu& cpu, char* buf, uint64_t len) {
  static const char name[] = "localhost";
  if (len < sizeof name) return static_cast<int32_t>(Fail(cpu, err::NAMETOOLONG));
  std::memcpy(buf, name, sizeof name);
  return 0;
}
// EAI_* are positive in bionic: EAI_NONAME 8, EAI_SERVICE 9, EAI_FAMILY 5
int32_t h_getaddrinfo(const char* node, const char* service, const uint8_t* hints, uint64_t* res) {
  int32_t family = 0, socktype = 0, protocol = 0, flags = 0;
  if (hints) {
    std::memcpy(&flags, hints, 4);
    std::memcpy(&family, hints + 4, 4);
    std::memcpy(&socktype, hints + 8, 4);
    std::memcpy(&protocol, hints + 12, 4);
  }
  if (family != 0 && family != 2) return 5;  // only IPv4 literals
  uint32_t addr = 0;
  if (!node) addr = (flags & 1) ? 0 : 0x0100007Fu;  // AI_PASSIVE: INADDR_ANY, else loopback
  else if (std::strcmp(node, "localhost") == 0) addr = 0x0100007Fu;
  else if (!ParseIPv4(node, addr)) return 8;
  uint16_t port = 0;
  if (service) {
    char* end;
    const unsigned long p = std::strtoul(service, &end, 10);
    if (*end == 0 && end != service) port = static_cast<uint16_t>(p);
    else if (std::strcmp(service, "http") == 0) port = 80;
    else if (std::strcmp(service, "https") == 0) port = 443;
    else return 9;
  }
  const gaddr blk = heap::Alloc(48 + 16);
  if (!blk) return 6;
  std::memset(g2h(blk), 0, 64);
  mem::W32(blk + 0, static_cast<uint32_t>(flags));
  mem::W32(blk + 4, 2);
  mem::W32(blk + 8, static_cast<uint32_t>(socktype ? socktype : 1));
  mem::W32(blk + 12, static_cast<uint32_t>(protocol));
  mem::W32(blk + 16, 16);
  mem::W64(blk + 32, blk + 48);  // ai_addr -> sockaddr_in inside the same block
  mem::W<uint16_t>(blk + 48, 2);
  mem::W<uint16_t>(blk + 50, static_cast<uint16_t>((port << 8) | (port >> 8)));
  mem::W32(blk + 52, addr);
  *res = blk;
  return 0;
}
void h_freeaddrinfo(uint64_t res) {
  while (res) {
    const uint64_t next = mem::R64(res + 40);
    heap::Free(res);
    res = next;
  }
}
uint64_t h_gethostbyname(const char* name) {
  uint32_t a = 0;
  if (!name || (std::strcmp(name, "localhost") != 0 && !ParseIPv4(name, a))) return 0;
  if (std::strcmp(name, "localhost") == 0) a = 0x0100007Fu;
  const gaddr b = ThreadBuffer(5, 128);
  std::memset(g2h(b), 0, 128);
  const gaddr nm = b + 64, list = b + 40, ent = b + 80;
  std::strncpy(g2h<char>(nm), name, 40);
  mem::W64(b + 0, nm);
  mem::W64(b + 8, b + 56);  // aliases: {NULL}
  mem::W32(b + 16, 2);
  mem::W32(b + 20, 4);
  mem::W64(b + 24, list);
  mem::W64(list, ent);
  mem::W64(list + 8, 0);
  mem::W32(ent, a);
  return b;
}
uint64_t h_getpwuid(uint32_t) { return 0; }
int32_t h_system(const char*) { return -1; }
}  // namespace

void RegisterPosix() {
  NOVA_HLE("open", h_open);
  NOVA_HLE("close", h_close);
  NOVA_HLE("read", h_read);
  NOVA_HLE("write", h_write);
  NOVA_HLE("pread", h_pread);
  NOVA_HLE("pread64", h_pread);
  NOVA_HLE("pwrite", h_pwrite);
  NOVA_HLE("pwrite64", h_pwrite);
  NOVA_HLE("lseek", h_lseek);
  NOVA_HLE("lseek64", h_lseek);
  NOVA_HLE("fsync", h_fsync);
  NOVA_HLE("fdatasync", h_fsync);
  NOVA_HLE("ftruncate", h_ftruncate);
  NOVA_HLE("ftruncate64", h_ftruncate);
  NOVA_HLE("truncate", h_truncate);
  NOVA_HLE("dup", h_dup);
  NOVA_HLE("dup2", h_dup2);
  NOVA_HLE("pipe", h_pipe);
  NOVA_HLE("pipe2", h_pipe2);
  NOVA_HLE("sendfile", h_sendfile);
  NOVA_HLE("fcntl", h_fcntl);
  NOVA_HLE("ioctl", h_ioctl);
  NOVA_HLE("isatty", h_isatty);
  NOVA_HLE("tcgetattr", h_tcattr);
  NOVA_HLE("tcsetattr", h_tcattr);

  NOVA_HLE("stat", h_stat);
  NOVA_HLE("lstat", h_stat);
  NOVA_HLE("fstat", h_fstat);
  NOVA_HLE("statfs", h_statfs);
  NOVA_HLE("fstatfs", h_fstatfs);
  NOVA_HLE("access", h_access);
  NOVA_HLE("mkdir", h_mkdir);
  NOVA_HLE("rmdir", h_rmdir);
  NOVA_HLE("unlink", h_unlink);
  NOVA_HLE("remove", h_remove);
  NOVA_HLE("rename", h_rename);
  NOVA_HLE("chdir", h_chdir);
  NOVA_HLE("getcwd", h_getcwd);
  NOVA_HLE("chmod", h_chmod);
  NOVA_HLE("umask", h_umask);
  NOVA_HLE("realpath", h_realpath);
  NOVA_HLE("mkstemp", h_mkstemp);

  NOVA_HLE("opendir", h_opendir);
  NOVA_HLE("readdir", h_readdir);
  NOVA_HLE("closedir", h_closedir);
  NOVA_HLE("rewinddir", h_rewinddir);

  NOVA_HLE("mmap", h_mmap);
  NOVA_HLE("mmap64", h_mmap);
  NOVA_HLE("munmap", h_munmap);
  NOVA_HLE("mprotect", h_mprotect);
  NOVA_HLE("madvise", h_zero3);
  NOVA_HLE("msync", h_zero3);

  NOVA_HLE("socket", h_socket);
  NOVA_HLE("bind", h_bind);
  NOVA_HLE("listen", h_listen);
  NOVA_HLE("accept", h_accept);
  NOVA_HLE("connect", h_connect);
  NOVA_HLE("send", h_send);
  NOVA_HLE("sendto", h_sendto);
  NOVA_HLE("recv", h_recv);
  NOVA_HLE("recvfrom", h_recvfrom);
  NOVA_HLE("sendmsg", h_sendmsg);
  NOVA_HLE("recvmsg", h_recvmsg);
  NOVA_HLE("shutdown", h_shutdown);
  NOVA_HLE("setsockopt", h_setsockopt);
  NOVA_HLE("getsockopt", h_getsockopt);
  NOVA_HLE("getsockname", h_getsockname);
  NOVA_HLE("getpeername", h_getpeername);
  NOVA_HLE("select", h_select);
  NOVA_HLE("poll", h_poll);
  NOVA_HLE("epoll_create", h_epoll_create);
  NOVA_HLE("epoll_create1", h_epoll_create1);
  NOVA_HLE("epoll_ctl", h_epoll_ctl);
  NOVA_HLE("epoll_wait", h_epoll_wait);
  NOVA_HLE("eventfd", h_eventfd);

  NOVA_HLE_FAST("inet_addr", h_inet_addr);
  NOVA_HLE("inet_ntoa", h_inet_ntoa);
  NOVA_HLE("inet_ntop", h_inet_ntop);
  NOVA_HLE("if_indextoname", h_if_indextoname);
  NOVA_HLE("gethostname", h_gethostname);
  NOVA_HLE("getaddrinfo", h_getaddrinfo);
  NOVA_HLE("freeaddrinfo", h_freeaddrinfo);
  NOVA_HLE("gethostbyname", h_gethostbyname);
  NOVA_HLE("getpwuid", h_getpwuid);
  NOVA_HLE("system", h_system);
}
}  // namespace nova::hle::libc
