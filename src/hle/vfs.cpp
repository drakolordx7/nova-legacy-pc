#include "hle/vfs.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "core/log.h"
#include "cpu/guest_memory.h"
#include "hle/libc_internal.h"

namespace nova::vfs {
namespace fs = std::filesystem;
namespace err = nova::hle::libc::err;
using std::int64_t;
using std::uint64_t;

// ------------------------------------------------------------------------------------------------ configuration
namespace {
std::mutex g_cfg_mu;
std::optional<fs::path> g_root;
struct Mount {
  std::string prefix;
  fs::path dir;
};
std::vector<Mount> g_mounts;
std::string g_cwd;
std::once_flag g_dirs_once;

constexpr unsigned kCores = 8;  // the spoofed phone has 8 cores everywhere (sysconf, /proc/cpuinfo, /sys/devices/system/cpu)

std::wstring Widen(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
std::string Narrow(std::wstring_view w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

int ErrFromWin(DWORD e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_DRIVE: return err::NOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT: return err::ACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return err::EXIST;
    case ERROR_DIRECTORY: return err::NOTDIR;
    case ERROR_DIR_NOT_EMPTY: return err::NOTEMPTY;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return err::NOSPC;
    case ERROR_NEGATIVE_SEEK:
    case ERROR_INVALID_PARAMETER: return err::INVAL;
    case ERROR_NOT_SAME_DEVICE: return err::XDEV;
    case ERROR_FILENAME_EXCED_RANGE: return err::NAMETOOLONG;
    case ERROR_TOO_MANY_OPEN_FILES: return err::MFILE;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return err::NOMEM;
    case ERROR_BROKEN_PIPE: return err::PIPE;
    default: return err::IO;
  }
}

fs::path ExeDir() {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  fs::path p(std::wstring(buf, n));
  return p.parent_path();
}
fs::path RootLocked() {
  if (!g_root) g_root = ExeDir() / L"data";
  return *g_root;
}

bool ValidComponent(std::string_view c) {
  for (unsigned char ch : c)
    if (ch < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '|' || ch == '?' || ch == '*' || ch == '\\')
      return false;
  return true;
}
}  // namespace

void SetRoot(const fs::path& root) {
  std::lock_guard lk(g_cfg_mu);
  g_root = root;
}
fs::path Root() {
  std::lock_guard lk(g_cfg_mu);
  return RootLocked();
}
void AddMount(std::string guest_prefix, const fs::path& host_dir) {
  std::lock_guard lk(g_cfg_mu);
  while (guest_prefix.size() > 1 && guest_prefix.back() == '/') guest_prefix.pop_back();
  g_mounts.push_back({std::move(guest_prefix), host_dir});
}
std::string Cwd() {
  std::lock_guard lk(g_cfg_mu);
  return g_cwd.empty() ? std::string("/data/data/") + kPackage + "/files" : g_cwd;
}
void SetCwd(std::string_view n) {
  std::lock_guard lk(g_cfg_mu);
  g_cwd = std::string(n);
}

std::string Normalize(std::string_view path) {
  std::string full;
  if (path.empty() || path[0] != '/') {
    full = Cwd();
    full.push_back('/');
  }
  full.append(path);
  std::vector<std::string_view> parts;
  std::size_t i = 0;
  while (i <= full.size()) {
    std::size_t j = full.find('/', i);
    if (j == std::string::npos) j = full.size();
    std::string_view c(full.data() + i, j - i);
    if (c.empty() || c == ".") {
    } else if (c == "..") {
      if (!parts.empty()) parts.pop_back();
    } else {
      parts.push_back(c);
    }
    i = j + 1;
  }
  std::string out;
  for (auto p : parts) {
    out.push_back('/');
    out.append(p);
  }
  return out.empty() ? "/" : out;
}

namespace {
// Rest of `norm` below `prefix` ("" for the prefix itself), or nullopt if `norm` is not inside it.
std::optional<std::string_view> Below(std::string_view norm, std::string_view prefix) {
  if (norm.size() < prefix.size() || norm.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
  if (norm.size() == prefix.size()) return std::string_view();
  if (norm[prefix.size()] != '/') return std::nullopt;
  return norm.substr(prefix.size() + 1);
}

fs::path Join(fs::path base, std::string_view rest, bool& ok) {
  ok = true;
  std::size_t i = 0;
  while (i < rest.size()) {
    std::size_t j = rest.find('/', i);
    if (j == std::string::npos) j = rest.size();
    std::string_view c = rest.substr(i, j - i);
    if (!ValidComponent(c)) {
      ok = false;
      return {};
    }
    base /= Widen(c);
    i = j + 1;
  }
  return base;
}

void EnsureDirs() {
  std::call_once(g_dirs_once, [] {
    const fs::path r = Root();
    std::error_code ec;
    for (const wchar_t* d : {L"internal\\files", L"internal\\cache", L"external\\files", L"fonts", L"tmp"})
      fs::create_directories(r / d, ec);
  });
}
}  // namespace

namespace {
// Maps a normalised guest path; *base (optional) receives the host folder of the mount / rule that matched.
std::optional<fs::path> MapPath(const std::string& norm, fs::path* base) {
  bool ok = true;
  {
    std::lock_guard lk(g_cfg_mu);
    const Mount* best = nullptr;
    for (const Mount& m : g_mounts)
      if (Below(norm, m.prefix) && (!best || m.prefix.size() > best->prefix.size())) best = &m;
    if (best) {
      if (base) *base = best->dir;
      fs::path p = Join(best->dir, *Below(norm, best->prefix), ok);
      if (!ok) return std::nullopt;
      return p;
    }
  }
  EnsureDirs();
  const fs::path root = Root();
  const std::string pkg = kPackage;
  struct Rule {
    std::string prefix;
    const wchar_t* sub;
  };
  static const char* kExt[] = {"/sdcard", "/storage/emulated/0", "/storage/emulated/legacy", "/storage/self/primary",
                               "/mnt/sdcard", "/storage/sdcard0", "/mnt/shell/emulated/0"};
  std::vector<Rule> rules;
  rules.push_back({"/data/data/" + pkg, L"internal"});
  rules.push_back({"/data/user/0/" + pkg, L"internal"});
  for (const char* e : kExt) {
    rules.push_back({std::string(e) + "/Android/data/" + pkg, L"external"});
    rules.push_back({std::string(e) + "/Android/obb/" + pkg, L"external\\obb"});
  }
  for (const char* e : kExt) rules.push_back({e, L"external"});
  rules.push_back({"/system/fonts", L"fonts"});
  rules.push_back({"/data/local/tmp", L"tmp"});
  rules.push_back({"/tmp", L"tmp"});
  rules.push_back({"/cache", L"tmp"});
  for (const Rule& r : rules) {
    if (auto rest = Below(norm, r.prefix)) {
      if (base) *base = root / r.sub;
      fs::path p = Join(root / r.sub, *rest, ok);
      if (!ok) return std::nullopt;
      return p;
    }
  }
  return std::nullopt;
}

// Creates the host folder behind the mount / rule that `norm` falls under (a mount root that does not exist yet must
// not fail a create). Never creates anything below it: a missing parent is ENOENT, as on Linux.
void EnsureMountRoot(const std::string& norm) {
  fs::path base;
  if (!MapPath(norm, &base) || base.empty()) return;
  std::error_code ec;
  if (!fs::exists(base, ec)) fs::create_directories(base, ec);
}
}  // namespace

std::optional<fs::path> ToHost(std::string_view guest_path) { return MapPath(Normalize(guest_path), nullptr); }

// ------------------------------------------------------------------------------------------------ synthesised system files
namespace {
std::string PropValue(std::string_view k);

std::string CpuInfo() {
  std::string s;
  for (unsigned i = 0; i < kCores; ++i) {
    const bool big = i >= 4;
    s += "processor\t: " + std::to_string(i) + "\n";
    s += "BogoMIPS\t: 38.40\n";
    s += "Features\t: fp asimd evtstrm crc32\n";
    s += "CPU implementer\t: 0x51\n";
    s += "CPU architecture: 8\n";
    s += "CPU variant\t: 0xd\n";
    s += std::string("CPU part\t: ") + (big ? "0x802" : "0x801") + "\n";
    s += "CPU revision\t: 4\n\n";
  }
  s += "Hardware\t: Qualcomm Technologies, Inc SDM845\n";
  return s;
}
std::string MemInfo() {
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof m;
  GlobalMemoryStatusEx(&m);
  const uint64_t total = std::min<uint64_t>(m.ullTotalPhys / 1024, 8ull << 20);
  const uint64_t avail = std::min<uint64_t>(m.ullAvailPhys / 1024, total);
  char b[512];
  std::snprintf(b, sizeof b,
                "MemTotal:       %8llu kB\nMemFree:        %8llu kB\nMemAvailable:   %8llu kB\nBuffers:        %8d kB\n"
                "Cached:         %8llu kB\nSwapCached:            0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n",
                static_cast<unsigned long long>(total), static_cast<unsigned long long>(avail),
                static_cast<unsigned long long>(avail), 4096, static_cast<unsigned long long>(avail / 4));
  return b;
}
std::string SelfStatm() {
  // size resident shared text lib data dt (pages)
  const uint64_t vm_pages = 1 << 16, rss_pages = 1 << 15;
  char b[128];
  std::snprintf(b, sizeof b, "%llu %llu 2048 1 0 %llu 0\n", static_cast<unsigned long long>(vm_pages),
                static_cast<unsigned long long>(rss_pages), static_cast<unsigned long long>(rss_pages / 2));
  return b;
}
std::string SelfMaps() {
  std::string s;
  char b[256];
  for (const auto& r : GuestMemory::Regions()) {
    std::snprintf(b, sizeof b, "%08llx-%08llx %c%c%cp 00000000 00:00 0 %s\n", static_cast<unsigned long long>(r.start),
                  static_cast<unsigned long long>(r.start + r.size), (r.prot & kProtRead) ? 'r' : '-',
                  (r.prot & kProtWrite) ? 'w' : '-', (r.prot & kProtExec) ? 'x' : '-', r.name.c_str());
    s += b;
  }
  return s;
}
std::string BuildProp() {
  std::string s;
  for (const char* k : {"ro.build.id", "ro.build.display.id", "ro.build.version.release", "ro.build.version.sdk",
                        "ro.build.fingerprint", "ro.product.model", "ro.product.brand", "ro.product.name",
                        "ro.product.device", "ro.product.manufacturer", "ro.product.board", "ro.board.platform",
                        "ro.hardware", "ro.product.cpu.abi", "ro.product.cpu.abilist", "ro.build.type", "ro.build.tags",
                        "ro.arch"})
    s += std::string(k) + "=" + PropValue(k) + "\n";
  return s;
}
std::string FreqFile(unsigned cpu, std::string_view what) {
  const bool big = cpu >= 4;
  if (what == "scaling_cur_freq") return big ? "2803200\n" : "1766400\n";
  if (what == "cpuinfo_max_freq" || what == "scaling_max_freq") return big ? "2803200\n" : "1766400\n";
  return big ? "825600\n" : "300000\n";  // cpuinfo_min_freq / scaling_min_freq
}

using Gen = std::function<std::string()>;
const std::map<std::string, Gen>& Synth() {
  static const std::map<std::string, Gen> table = [] {
    std::map<std::string, Gen> t;
    t["/proc/cpuinfo"] = CpuInfo;
    t["/proc/meminfo"] = MemInfo;
    t["/proc/self/statm"] = SelfStatm;
    t["/proc/self/maps"] = SelfMaps;
    t["/proc/self/cmdline"] = [] { return std::string(kPackage) + std::string(1, '\0'); };
    t["/proc/self/status"] = [] {
      return std::string("Name:\t") + "nova" + "\nState:\tR (running)\nVmRSS:\t  262144 kB\nThreads:\t" +
             std::to_string(std::max<std::size_t>(1, nova::threads::Count())) + "\n";
    };
    t["/proc/version"] = [] { return std::string("Linux version 4.9.112 (android-build) #1 SMP PREEMPT\n"); };
    const std::string range = "0-" + std::to_string(kCores - 1) + "\n";
    for (const char* n : {"present", "possible", "online"}) t[std::string("/sys/devices/system/cpu/") + n] = [range] { return range; };
    for (unsigned i = 0; i < kCores; ++i) {
      const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/";
      for (const char* f : {"scaling_cur_freq", "cpuinfo_max_freq", "cpuinfo_min_freq", "scaling_max_freq", "scaling_min_freq"})
        t[base + f] = [i, f] { return FreqFile(i, f); };
      t["/sys/devices/system/cpu/cpu" + std::to_string(i) + "/online"] = [] { return std::string("1\n"); };
    }
    t["/sys/class/thermal/thermal_zone0/temp"] = [] { return std::string("36000\n"); };
    t["/sys/class/thermal/thermal_zone0/type"] = [] { return std::string("cpu-0-0-usr\n"); };
    t["/sys/bus/soc/devices/soc0/family"] = [] { return std::string("Snapdragon\n"); };
    t["/system/build.prop"] = BuildProp;
    t["/etc/hosts"] = [] { return std::string("127.0.0.1 localhost\n"); };
    return t;
  }();
  return table;
}

bool IsSynthDir(const std::string& norm) {
  if (norm == "/proc" || norm == "/sys" || norm == "/proc/self") return true;
  const std::string pre = norm == "/" ? "/" : norm + "/";
  for (const auto& [k, g] : Synth())
    if (k.compare(0, pre.size(), pre) == 0) return true;
  return false;
}
bool IsSynthNamespace(const std::string& norm) {
  for (const char* p : {"/proc", "/sys", "/dev", "/etc", "/vendor", "/proc/self"})
    if (Below(norm, p)) return true;
  return Below(norm, "/system") && !Below(norm, "/system/fonts");
}
std::vector<DirEntry> ListSynthDir(const std::string& norm) {
  std::set<std::string> dirs, files;
  const std::string pre = norm + "/";
  for (const auto& [k, g] : Synth()) {
    if (k.compare(0, pre.size(), pre) != 0) continue;
    const std::string rest = k.substr(pre.size());
    const std::size_t s = rest.find('/');
    if (s == std::string::npos) files.insert(rest);
    else dirs.insert(rest.substr(0, s));
  }
  std::vector<DirEntry> out;
  for (const auto& d : dirs) out.push_back({d, 4, 0});
  for (const auto& f : files) out.push_back({f, 8, 0});
  return out;
}

// ---- property table shared with __system_property_get ----
std::string PropValue(std::string_view k) {
  static const std::map<std::string, std::string, std::less<>> props = {
      {"ro.build.id", "TP1A.220624.014"},
      {"ro.build.display.id", "TP1A.220624.014.S918BXXU2AWF1"},
      {"ro.build.version.release", "13"},
      {"ro.build.version.sdk", "33"},
      {"ro.build.fingerprint", "samsung/dm3qxxx/dm3q:13/TP1A.220624.014/S918BXXU2AWF1:user/release-keys"},
      {"ro.product.model", "SM-S918B"},
      {"ro.product.brand", "samsung"},
      {"ro.product.name", "dm3qxxx"},
      {"ro.product.device", "dm3q"},
      {"ro.product.manufacturer", "samsung"},
      {"ro.product.board", "kalama"},
      {"ro.board.platform", "kalama"},
      {"ro.hardware", "qcom"},
      {"ro.product.cpu.abi", "arm64-v8a"},
      {"ro.product.cpu.abilist", "arm64-v8a,armeabi-v7a,armeabi"},
      {"ro.product.cpu.abilist64", "arm64-v8a"},
      {"ro.build.type", "user"},
      {"ro.build.tags", "release-keys"},
      {"ro.arch", "arm64"},
      {"ro.debuggable", "0"},
      {"ro.secure", "1"},
      {"persist.sys.language", "en"},
      {"persist.sys.country", "US"},
      {"ro.build.version.codename", "REL"},
      {"ro.build.version.incremental", "1810090000"},
      {"ro.opengles.version", "196609"},
  };
  auto it = props.find(k);
  return it == props.end() ? std::string() : it->second;
}
}  // namespace

std::string SystemProperty(std::string_view name, bool* found) {
  std::string v = PropValue(name);
  if (found) *found = !v.empty();
  return v;
}

// ------------------------------------------------------------------------------------------------ File base
std::int64_t File::Write(const void*, std::uint64_t) { return -err::BADF; }
std::int64_t File::Seek(std::int64_t, int) { return -err::SPIPE; }
std::int64_t File::Pread(void*, std::uint64_t, std::int64_t) { return -err::SPIPE; }
std::int64_t File::Pwrite(const void*, std::uint64_t, std::int64_t) { return -err::SPIPE; }
int File::Truncate(std::int64_t) { return -err::INVAL; }

namespace {
int64_t FtToUnixSec(const FILETIME& ft, int64_t* ns) {
  const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  const int64_t u = static_cast<int64_t>(t) - 116444736000000000ll;
  int64_t sec = u / 10000000, rem = u % 10000000;
  if (rem < 0) {
    rem += 10000000;
    --sec;
  }
  if (ns) *ns = rem * 100;
  return sec;
}

void FillFromInfo(const BY_HANDLE_FILE_INFORMATION& i, StatBuf& st) {
  const bool dir = (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const bool ro = (i.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
  st.dev = i.dwVolumeSerialNumber;
  st.ino = (static_cast<uint64_t>(i.nFileIndexHigh) << 32) | i.nFileIndexLow;
  st.mode = dir ? (kIFDIR | (ro ? 0555 : 0755)) : (kIFREG | (ro ? 0444 : 0644));
  st.nlink = i.nNumberOfLinks ? i.nNumberOfLinks : 1;
  st.size = dir ? 4096 : static_cast<int64_t>((static_cast<uint64_t>(i.nFileSizeHigh) << 32) | i.nFileSizeLow);
  st.blksize = 4096;
  st.blocks = (st.size + 511) / 512;
  st.atime = FtToUnixSec(i.ftLastAccessTime, &st.atime_ns);
  st.mtime = FtToUnixSec(i.ftLastWriteTime, &st.mtime_ns);
  st.ctime = FtToUnixSec(i.ftCreationTime, &st.ctime_ns);
}

void FillSynthStat(StatBuf& st, bool dir, int64_t size, uint32_t mode_override = 0) {
  st = StatBuf{};
  st.dev = 0x1d;
  st.mode = mode_override ? mode_override : (dir ? (kIFDIR | 0555) : (kIFREG | 0444));
  st.size = size;
  st.blksize = 4096;
  const int64_t now = hle::libc::RealtimeNs() / 1000000000ll;
  st.atime = st.mtime = st.ctime = now;
}

// ---- host file ----
class HostFile : public File {
 public:
  HostFile(HANDLE h, int fl, std::string p) : h_(h) {
    flags = fl;
    path = std::move(p);
  }
  ~HostFile() override {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
  }
  int64_t Read(void* buf, uint64_t n) override {
    if ((flags & kO_ACCMODE) == kO_WRONLY) return -err::BADF;
    std::lock_guard lk(mu_);
    return ReadLocked(buf, n);
  }
  int64_t Write(const void* buf, uint64_t n) override {
    if ((flags & kO_ACCMODE) == kO_RDONLY) return -err::BADF;
    std::lock_guard lk(mu_);
    if (flags & kO_APPEND) SetFilePointerEx(h_, LARGE_INTEGER{}, nullptr, FILE_END);
    return WriteLocked(buf, n);
  }
  int64_t Seek(int64_t off, int whence) override {
    if (whence < 0 || whence > 2) return -err::INVAL;
    std::lock_guard lk(mu_);
    LARGE_INTEGER to, out;
    to.QuadPart = off;
    if (!SetFilePointerEx(h_, to, &out, static_cast<DWORD>(whence))) return -ErrFromWin(GetLastError());
    return out.QuadPart;
  }
  int64_t Pread(void* buf, uint64_t n, int64_t off) override {
    if ((flags & kO_ACCMODE) == kO_WRONLY) return -err::BADF;
    std::lock_guard lk(mu_);
    LARGE_INTEGER cur, to;
    to.QuadPart = 0;
    SetFilePointerEx(h_, to, &cur, FILE_CURRENT);
    to.QuadPart = off;
    SetFilePointerEx(h_, to, nullptr, FILE_BEGIN);
    const int64_t r = ReadLocked(buf, n);
    SetFilePointerEx(h_, cur, nullptr, FILE_BEGIN);
    return r;
  }
  int64_t Pwrite(const void* buf, uint64_t n, int64_t off) override {
    if ((flags & kO_ACCMODE) == kO_RDONLY) return -err::BADF;
    std::lock_guard lk(mu_);
    LARGE_INTEGER cur, to;
    to.QuadPart = 0;
    SetFilePointerEx(h_, to, &cur, FILE_CURRENT);
    to.QuadPart = off;
    SetFilePointerEx(h_, to, nullptr, FILE_BEGIN);
    const int64_t r = WriteLocked(buf, n);
    SetFilePointerEx(h_, cur, nullptr, FILE_BEGIN);
    return r;
  }
  int Stat(StatBuf& st) override {
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle(h_, &i)) return -ErrFromWin(GetLastError());
    FillFromInfo(i, st);
    return 0;
  }
  int Truncate(int64_t size) override {
    if ((flags & kO_ACCMODE) == kO_RDONLY) return -err::INVAL;
    if (size < 0) return -err::INVAL;
    FILE_END_OF_FILE_INFO e;
    e.EndOfFile.QuadPart = size;
    if (!SetFileInformationByHandle(h_, FileEndOfFileInfo, &e, sizeof e)) return -ErrFromWin(GetLastError());
    return 0;
  }
  int Sync() override { return FlushFileBuffers(h_) ? 0 : -err::IO; }

 private:
  int64_t ReadLocked(void* buf, uint64_t n) {
    uint64_t total = 0;
    while (total < n) {
      const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(n - total, 1u << 30));
      DWORD got = 0;
      if (!ReadFile(h_, static_cast<char*>(buf) + total, chunk, &got, nullptr)) return total ? static_cast<int64_t>(total) : -ErrFromWin(GetLastError());
      total += got;
      if (got < chunk) break;
    }
    return static_cast<int64_t>(total);
  }
  int64_t WriteLocked(const void* buf, uint64_t n) {
    uint64_t total = 0;
    while (total < n) {
      const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(n - total, 1u << 30));
      DWORD put = 0;
      if (!WriteFile(h_, static_cast<const char*>(buf) + total, chunk, &put, nullptr)) return total ? static_cast<int64_t>(total) : -ErrFromWin(GetLastError());
      total += put;
    }
    return static_cast<int64_t>(total);
  }
  HANDLE h_;
  std::mutex mu_;
};

// ---- directory (host or synthesised) ----
class DirFile : public File {
 public:
  DirFile(std::string p, int fl) {
    path = std::move(p);
    flags = fl;
  }
  int64_t Read(void*, uint64_t) override { return -err::ISDIR; }
  int Stat(StatBuf& st) override {
    const int e = StatPath(path, st);
    return e ? -e : 0;
  }
  bool IsDir() const override { return true; }
};

// ---- read-only content ----
class MemFile : public File {
 public:
  MemFile(std::string c, std::string p) : data_(std::move(c)) { path = std::move(p); }
  int64_t Read(void* buf, uint64_t n) override {
    std::lock_guard lk(mu_);
    if (pos_ >= data_.size()) return 0;
    const uint64_t k = std::min<uint64_t>(n, data_.size() - pos_);
    std::memcpy(buf, data_.data() + pos_, k);
    pos_ += k;
    return static_cast<int64_t>(k);
  }
  int64_t Seek(int64_t off, int whence) override {
    std::lock_guard lk(mu_);
    int64_t base = whence == 0 ? 0 : (whence == 1 ? static_cast<int64_t>(pos_) : static_cast<int64_t>(data_.size()));
    if (whence < 0 || whence > 2 || base + off < 0) return -err::INVAL;
    pos_ = static_cast<uint64_t>(base + off);
    return base + off;
  }
  int64_t Pread(void* buf, uint64_t n, int64_t off) override {
    std::lock_guard lk(mu_);
    if (off < 0) return -err::INVAL;
    if (static_cast<uint64_t>(off) >= data_.size()) return 0;
    const uint64_t k = std::min<uint64_t>(n, data_.size() - static_cast<uint64_t>(off));
    std::memcpy(buf, data_.data() + off, k);
    return static_cast<int64_t>(k);
  }
  int Stat(StatBuf& st) override {
    FillSynthStat(st, false, static_cast<int64_t>(data_.size()));
    return 0;
  }

 private:
  std::string data_;
  uint64_t pos_ = 0;
  std::mutex mu_;
};

// ---- /dev nodes ----
class DevFile : public File {
 public:
  enum Kind { Null, Zero, Random };
  DevFile(Kind k, std::string p, int fl) : kind_(k) {
    path = std::move(p);
    flags = fl;
  }
  int64_t Read(void* buf, uint64_t n) override {
    if (kind_ == Null) return 0;
    if (kind_ == Zero) {
      std::memset(buf, 0, n);
      return static_cast<int64_t>(n);
    }
    uint64_t done = 0;
    while (done < n) {
      const ULONG c = static_cast<ULONG>(std::min<uint64_t>(n - done, 1u << 20));
      BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf) + done, c, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
      done += c;
    }
    return static_cast<int64_t>(n);
  }
  int64_t Write(const void*, uint64_t n) override { return static_cast<int64_t>(n); }
  int64_t Seek(int64_t, int) override { return 0; }
  int Stat(StatBuf& st) override {
    FillSynthStat(st, false, 0, kIFCHR | 0666);
    st.rdev = kind_ == Null ? 0x0103 : (kind_ == Zero ? 0x0105 : 0x0109);
    return 0;
  }

 private:
  Kind kind_;
};

// ---- the standard streams ----
class StdStream : public File {
 public:
  explicit StdStream(int w) : which_(w) {
    path = which_ == 0 ? "/dev/stdin" : (which_ == 1 ? "/dev/stdout" : "/dev/stderr");
    flags = which_ == 0 ? kO_RDONLY : kO_WRONLY;
  }
  int64_t Read(void*, uint64_t) override { return 0; }  // stdin is EOF
  int64_t Write(const void* buf, uint64_t n) override {
    if (which_ == 0) return -err::BADF;
    std::FILE* f = which_ == 1 ? stdout : stderr;
    const std::size_t w = std::fwrite(buf, 1, n, f);
    if (which_ == 2) std::fflush(f);
    return static_cast<int64_t>(w);
  }
  int Stat(StatBuf& st) override {
    FillSynthStat(st, false, 0, kIFCHR | 0620);
    st.rdev = 0x8800 + which_;
    return 0;
  }

 private:
  int which_;
};

// ---- descriptor table ----
std::mutex g_fd_mu;
std::vector<FilePtr> g_fds;
constexpr std::size_t kMaxFds = 4096;

void InitFdsLocked() {
  if (!g_fds.empty()) return;
  g_fds.resize(3);
  for (int i = 0; i < 3; ++i) g_fds[static_cast<std::size_t>(i)] = std::make_shared<StdStream>(i);
}

HANDLE OpenHost(const fs::path& p, int flags, int& e, bool& is_dir) {
  is_dir = false;
  const DWORD attrs = GetFileAttributesW(p.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    is_dir = true;
    return INVALID_HANDLE_VALUE;
  }
  const int acc = flags & kO_ACCMODE;
  DWORD access = acc == kO_RDONLY ? GENERIC_READ : (acc == kO_WRONLY ? GENERIC_WRITE : (GENERIC_READ | GENERIC_WRITE));
  DWORD disp;
  if ((flags & kO_CREAT) && (flags & kO_EXCL)) disp = CREATE_NEW;
  else if ((flags & kO_CREAT) && (flags & kO_TRUNC) && acc != kO_RDONLY) disp = CREATE_ALWAYS;
  else if (flags & kO_CREAT) disp = OPEN_ALWAYS;
  else if ((flags & kO_TRUNC) && acc != kO_RDONLY) disp = TRUNCATE_EXISTING;
  else disp = OPEN_EXISTING;
  HANDLE h = CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) e = ErrFromWin(GetLastError());
  return h;
}
}  // namespace

FilePtr MakeMemFile(std::string content, const std::string& path) { return std::make_shared<MemFile>(std::move(content), path); }
FilePtr MakeStdStream(int which) { return std::make_shared<StdStream>(which); }

int Install(FilePtr f, int min_fd) {
  std::lock_guard lk(g_fd_mu);
  InitFdsLocked();
  for (std::size_t i = static_cast<std::size_t>(std::max(min_fd, 0)); i < kMaxFds; ++i) {
    if (i >= g_fds.size()) g_fds.resize(i + 1);
    if (!g_fds[i]) {
      g_fds[i] = std::move(f);
      return static_cast<int>(i);
    }
  }
  return -err::MFILE;
}
FilePtr Get(int fd) {
  std::lock_guard lk(g_fd_mu);
  InitFdsLocked();
  if (fd < 0 || static_cast<std::size_t>(fd) >= g_fds.size()) return nullptr;
  return g_fds[static_cast<std::size_t>(fd)];
}
int Close(int fd) {
  FilePtr f;
  {
    std::lock_guard lk(g_fd_mu);
    InitFdsLocked();
    if (fd < 0 || static_cast<std::size_t>(fd) >= g_fds.size() || !g_fds[static_cast<std::size_t>(fd)]) return -err::BADF;
    f = std::move(g_fds[static_cast<std::size_t>(fd)]);
    if (fd < 3) g_fds[static_cast<std::size_t>(fd)] = std::make_shared<StdStream>(fd);  // closing a std fd keeps it usable
  }
  return 0;  // `f` released here: the host handle closes when the last reference goes
}
int Dup(int fd, int min_fd) {
  FilePtr f = Get(fd);
  if (!f) return -err::BADF;
  return Install(f, min_fd);
}
int Dup2(int fd, int newfd) {
  FilePtr f = Get(fd);
  if (!f) return -err::BADF;
  if (newfd < 0 || static_cast<std::size_t>(newfd) >= kMaxFds) return -err::BADF;
  if (fd == newfd) return newfd;
  std::lock_guard lk(g_fd_mu);
  if (static_cast<std::size_t>(newfd) >= g_fds.size()) g_fds.resize(static_cast<std::size_t>(newfd) + 1);
  g_fds[static_cast<std::size_t>(newfd)] = std::move(f);
  return newfd;
}

int Open(std::string_view guest_path, int flags, int mode, FilePtr& out) {
  (void)mode;
  if (guest_path.empty()) return err::NOENT;
  const std::string norm = Normalize(guest_path);
  const int acc = flags & kO_ACCMODE;
  // devices and synthesised system files
  if (norm == "/dev/null") return out = std::make_shared<DevFile>(DevFile::Null, norm, flags), 0;
  if (norm == "/dev/zero") return out = std::make_shared<DevFile>(DevFile::Zero, norm, flags), 0;
  if (norm == "/dev/urandom" || norm == "/dev/random") return out = std::make_shared<DevFile>(DevFile::Random, norm, flags), 0;
  if (IsSynthNamespace(norm)) {
    auto it = Synth().find(norm);
    if (it != Synth().end()) {
      if (acc != kO_RDONLY || (flags & kO_CREAT)) return err::ACCES;
      out = std::make_shared<MemFile>(it->second(), norm);
      out->flags = flags;
      return 0;
    }
    if (IsSynthDir(norm)) {
      if (acc != kO_RDONLY) return err::ISDIR;
      out = std::make_shared<DirFile>(norm, flags);
      return 0;
    }
    return err::NOENT;
  }
  auto host = ToHost(norm);
  if (!host) return err::NOENT;
  if (flags & kO_CREAT) EnsureMountRoot(norm);
  int e = 0;
  bool is_dir = false;
  HANDLE h = OpenHost(*host, flags, e, is_dir);
  if (is_dir) {
    if ((flags & kO_CREAT) && (flags & kO_EXCL)) return err::EXIST;
    if (acc != kO_RDONLY) return err::ISDIR;
    out = std::make_shared<DirFile>(norm, flags);
    return 0;
  }
  if (h == INVALID_HANDLE_VALUE) return e ? e : err::IO;
  if (flags & kO_DIRECTORY) {
    CloseHandle(h);
    return err::NOTDIR;
  }
  out = std::make_shared<HostFile>(h, flags, norm);
  return 0;
}

int StatPath(std::string_view guest_path, StatBuf& st) {
  const std::string norm = Normalize(guest_path);
  if (norm == "/dev/null" || norm == "/dev/zero" || norm == "/dev/urandom" || norm == "/dev/random") {
    FilePtr f;
    const int e = Open(norm, kO_RDONLY, 0, f);
    return e ? e : -f->Stat(st);
  }
  if (IsSynthNamespace(norm)) {
    auto it = Synth().find(norm);
    if (it != Synth().end()) {
      FillSynthStat(st, false, static_cast<int64_t>(it->second().size()));
      return 0;
    }
    if (IsSynthDir(norm)) {
      FillSynthStat(st, true, 0);
      return 0;
    }
    return err::NOENT;
  }
  auto host = ToHost(norm);
  if (!host) return err::NOENT;
  HANDLE h = CreateFileW(host->c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return ErrFromWin(GetLastError());
  BY_HANDLE_FILE_INFORMATION i;
  const BOOL ok = GetFileInformationByHandle(h, &i);
  const DWORD le = GetLastError();
  CloseHandle(h);
  if (!ok) return ErrFromWin(le);
  FillFromInfo(i, st);
  return 0;
}

int Mkdir(std::string_view guest_path) {
  const std::string norm = Normalize(guest_path);
  if (IsSynthNamespace(norm)) return IsSynthDir(norm) || Synth().count(norm) ? err::EXIST : err::ACCES;
  auto host = ToHost(norm);
  if (!host) return err::NOENT;
  if (CreateDirectoryW(host->c_str(), nullptr)) return 0;
  const DWORD e = GetLastError();
  if (e == ERROR_PATH_NOT_FOUND) {  // the mount root itself may not exist yet; create it, then retry once
    EnsureMountRoot(norm);
    if (CreateDirectoryW(host->c_str(), nullptr)) return 0;
  }
  return ErrFromWin(GetLastError());
}
int Rmdir(std::string_view guest_path) {
  auto host = ToHost(guest_path);
  if (!host || IsSynthNamespace(Normalize(guest_path))) return err::NOENT;
  const DWORD a = GetFileAttributesW(host->c_str());
  if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return err::NOTDIR;
  return RemoveDirectoryW(host->c_str()) ? 0 : ErrFromWin(GetLastError());
}
int Unlink(std::string_view guest_path) {
  const std::string norm = Normalize(guest_path);
  auto host = ToHost(norm);
  if (!host || IsSynthNamespace(norm)) return err::NOENT;
  const DWORD a = GetFileAttributesW(host->c_str());
  if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return err::ISDIR;
  if (DeleteFileW(host->c_str())) return 0;
  const DWORD e = GetLastError();
  if (e == ERROR_ACCESS_DENIED && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
    SetFileAttributesW(host->c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if (DeleteFileW(host->c_str())) return 0;
  }
  return ErrFromWin(GetLastError());
}
int Rename(std::string_view from, std::string_view to) {
  auto a = ToHost(from), b = ToHost(to);
  if (!a || IsSynthNamespace(Normalize(from))) return err::NOENT;
  if (!b || IsSynthNamespace(Normalize(to))) return err::ACCES;
  if (MoveFileExW(a->c_str(), b->c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
  return ErrFromWin(GetLastError());
}
int Access(std::string_view guest_path, int mode) {
  StatBuf st;
  const int e = StatPath(guest_path, st);
  if (e) return e;
  if ((mode & 2) && !(st.mode & 0222)) return err::ACCES;
  const std::string norm = Normalize(guest_path);
  if ((mode & 2) && IsSynthNamespace(norm) && norm.rfind("/dev/", 0) != 0) return err::ACCES;
  return 0;
}
int Truncate(std::string_view guest_path, int64_t size) {
  FilePtr f;
  int e = Open(guest_path, kO_WRONLY, 0, f);
  if (e) return e;
  const int r = f->Truncate(size);
  return r < 0 ? -r : 0;
}
int Realpath(std::string_view guest_path, std::string& out) {
  StatBuf st;
  const std::string norm = Normalize(guest_path);
  const int e = StatPath(norm, st);
  if (e) return e;
  out = norm;
  return 0;
}
bool DiskSpace(uint64_t& total, uint64_t& avail) {
  ULARGE_INTEGER a{}, t{}, f{};
  const fs::path r = Root();
  std::error_code ec;
  fs::create_directories(r, ec);
  if (!GetDiskFreeSpaceExW(r.c_str(), &a, &t, &f)) return false;
  total = t.QuadPart;
  avail = a.QuadPart;
  return true;
}

int ListDir(std::string_view guest_path, std::vector<DirEntry>& out) {
  const std::string norm = Normalize(guest_path);
  out.clear();
  out.push_back({".", 4, 1});
  out.push_back({"..", 4, 2});
  if (IsSynthNamespace(norm)) {
    if (!IsSynthDir(norm)) return Synth().count(norm) ? err::NOTDIR : err::NOENT;
    for (auto& e : ListSynthDir(norm)) out.push_back(std::move(e));
    return 0;
  }
  auto host = ToHost(norm);
  if (!host) return err::NOENT;
  const DWORD a = GetFileAttributesW(host->c_str());
  if (a == INVALID_FILE_ATTRIBUTES) return ErrFromWin(GetLastError());
  if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return err::NOTDIR;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW((*host / L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
  if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : ErrFromWin(GetLastError());
  uint64_t n = 10;
  do {
    const std::wstring_view nm = fd.cFileName;
    if (nm == L"." || nm == L"..") continue;
    out.push_back({Narrow(nm), static_cast<uint8_t>((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8), ++n});
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return 0;
}
}  // namespace nova::vfs
