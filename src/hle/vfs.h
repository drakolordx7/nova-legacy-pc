// Virtual file system under the bionic file API: maps Android paths onto a host data root, synthesises the system
// probes the game reads (/proc, /sys, /system/build.prop, /dev/urandom...), and owns the guest file-descriptor table.
//
//   /data/data/<pkg>/..                        -> <root>\internal\..
//   /sdcard/Android/data/<pkg>/..              -> <root>\external\..      (same for /storage/emulated/0/Android/data/<pkg>)
//   /sdcard/.., /storage/emulated/0/..         -> <root>\external\..
//   /system/fonts/..                           -> <root>\fonts\..
//   /data/local/tmp/.., /tmp/..                -> <root>\tmp\..
//   /proc/self/*, /proc/cpuinfo, ...           -> synthesised
//   anything else                              -> ENOENT
// Guest names are case-sensitive and the host is not: a lookup of "Foo.PNG" finds "foo.png" on the host, and a
// directory listing returns the host's spelling. Backslashes and Windows-illegal characters in a guest path make it
// unmappable (ENOENT).
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nova::vfs {

inline constexpr const char* kPackage = "com.gameloft.android.ANMP.GloftNOHM";

// Data root on the host. Default: <directory of nova.exe>\data. Call before the first guest file access.
void SetRoot(const std::filesystem::path& root);
std::filesystem::path Root();
// Extra mount: guest path prefix -> host directory (checked before the built-in rules).
void AddMount(std::string guest_prefix, const std::filesystem::path& host_dir);

// Guest cwd (default /data/data/<pkg>/files, so "../Documents/x" resolves next to the save dir).
std::string Cwd();
void SetCwd(std::string_view normalized);
// Absolute normalised guest path ("." and ".." resolved, duplicate slashes removed); relative paths use Cwd().
std::string Normalize(std::string_view path);
// Host path for a guest path (normalised internally); nullopt if the guest path is not mapped.
std::optional<std::filesystem::path> ToHost(std::string_view guest_path);

// ---- everything below is for the libc HLE (posix.cpp / stdio.cpp) ----

struct StatBuf {
  std::uint64_t dev = 0, ino = 0;
  std::uint32_t mode = 0, nlink = 1, uid = 0, gid = 0;
  std::uint64_t rdev = 0;
  std::int64_t size = 0;
  std::int32_t blksize = 4096;
  std::int64_t blocks = 0;
  std::int64_t atime = 0, atime_ns = 0, mtime = 0, mtime_ns = 0, ctime = 0, ctime_ns = 0;
};
inline constexpr std::uint32_t kIFMT = 0170000, kIFDIR = 0040000, kIFCHR = 0020000, kIFREG = 0100000, kIFIFO = 0010000,
                               kIFSOCK = 0140000;

// Linux/ARM open(2) flags.
inline constexpr int kO_ACCMODE = 3, kO_RDONLY = 0, kO_WRONLY = 1, kO_RDWR = 2, kO_CREAT = 0x40, kO_EXCL = 0x80,
                     kO_TRUNC = 0x200, kO_APPEND = 0x400, kO_NONBLOCK = 0x800, kO_DIRECTORY = 0x4000;

// An open file description. Operations return >= 0 or -errno.
class File {
 public:
  virtual ~File() = default;
  int flags = 0;
  std::string path;  // normalised guest path
  virtual std::int64_t Read(void* buf, std::uint64_t n) = 0;
  virtual std::int64_t Write(const void* buf, std::uint64_t n);
  virtual std::int64_t Seek(std::int64_t off, int whence);  // 0 SET, 1 CUR, 2 END
  virtual std::int64_t Pread(void* buf, std::uint64_t n, std::int64_t off);
  virtual std::int64_t Pwrite(const void* buf, std::uint64_t n, std::int64_t off);
  virtual int Stat(StatBuf& st) = 0;
  virtual int Truncate(std::int64_t size);
  virtual int Sync() { return 0; }
  virtual bool IsDir() const { return false; }
  virtual bool IsSocketLike() const { return false; }
};
using FilePtr = std::shared_ptr<File>;

// Descriptor table (0/1/2 are preinstalled, the host's stdio).
int Install(FilePtr f, int min_fd = 3);  // fd, or -EMFILE
FilePtr Get(int fd);
int Close(int fd);  // 0 or -EBADF
int Dup(int fd, int min_fd);
int Dup2(int fd, int newfd);

// Path operations: 0 on success, positive errno on failure.
int Open(std::string_view guest_path, int flags, int mode, FilePtr& out);
int StatPath(std::string_view guest_path, StatBuf& st);
int Mkdir(std::string_view guest_path);
int Rmdir(std::string_view guest_path);
int Unlink(std::string_view guest_path);
int Rename(std::string_view from, std::string_view to);
int Access(std::string_view guest_path, int mode);
int Truncate(std::string_view guest_path, std::int64_t size);
int Realpath(std::string_view guest_path, std::string& out);
// Free/total bytes of the volume holding the data root (statfs).
bool DiskSpace(std::uint64_t& total, std::uint64_t& avail);

struct DirEntry {
  std::string name;
  std::uint8_t type = 8;  // DT_REG 8, DT_DIR 4
  std::uint64_t ino = 0;
};
// Entries of a directory, "." and ".." first. 0 or positive errno.
int ListDir(std::string_view guest_path, std::vector<DirEntry>& out);

// Value of an Android system property of the spoofed phone ("" when unknown; *found tells them apart).
std::string SystemProperty(std::string_view name, bool* found = nullptr);

// Streams for pipe(), eventfd and friends (posix.cpp builds on these).
FilePtr MakeMemFile(std::string content, const std::string& path);
FilePtr MakeStdStream(int which);

}  // namespace nova::vfs
