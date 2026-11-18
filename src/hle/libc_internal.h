// Shared pieces of the bionic LP64 HLE (not public API): Linux errno numbers, tiny helpers, and the per-area
// registration functions that RegisterAll() calls. Handler names are bionic's; guest `long`/`size_t`/`time_t`/`off_t`
// are 64-bit, so handlers use int64_t/uint64_t and never host `long`.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "cpu/cpu.h"
#include "cpu/hle_abi.h"

namespace nova::hle::libc {

// Linux/bionic errno values (never the host's).
namespace err {
inline constexpr int PERM = 1, NOENT = 2, SRCH = 3, INTR = 4, IO = 5, BADF = 9, CHILD = 10, AGAIN = 11, NOMEM = 12,
                     ACCES = 13, FAULT = 14, BUSY = 16, EXIST = 17, XDEV = 18, NODEV = 19, NOTDIR = 20, ISDIR = 21,
                     INVAL = 22, NFILE = 23, MFILE = 24, NOTTY = 25, FBIG = 27, NOSPC = 28, SPIPE = 29, ROFS = 30,
                     PIPE = 32, DOM = 33, RANGE = 34, DEADLK = 35, NAMETOOLONG = 36, NOSYS = 38, NOTEMPTY = 39,
                     NOTSOCK = 88, OPNOTSUPP = 95, AFNOSUPPORT = 97, ADDRNOTAVAIL = 99, NETDOWN = 100,
                     NETUNREACH = 101, CONNRESET = 104, NOTCONN = 107, TIMEDOUT = 110, CONNREFUSED = 111,
                     HOSTUNREACH = 113, INPROGRESS = 115, OVERFLOW_ = 75, ILSEQ = 84;
}

inline void SetErrno(Cpu& c, int e) { mem::W<std::int32_t>(c.errno_addr(), e); }
inline void SetErrno(int e) { SetErrno(Cpu::Current(), e); }

struct Timespec {
  std::int64_t sec, nsec;
};
struct Timeval {
  std::int64_t sec, usec;
};

// A NUL-terminated string interned in guest memory for the process lifetime (strerror, tm_zone, environment...).
gaddr InternString(std::string_view s);
// Per-thread scratch guest buffer number `slot` (0..7) of at least `size` bytes (localtime, asctime, ...).
gaddr ThreadBuffer(unsigned slot, gsize size);

// bionic's strerror text for a Linux errno ("Unknown error N" beyond the table).
std::string ErrorText(int e);

// Time helpers (time.cpp / runtime.cpp).
std::int64_t RealtimeNs();   // CLOCK_REALTIME in ns since the Unix epoch
std::int64_t MonotonicNs();  // CLOCK_MONOTONIC in ns
void SleepNs(std::int64_t ns);
std::string StrftimeImpl(const char* fmt, const std::int32_t* tm_fields, std::int64_t gmtoff, const char* zone);

// Program-exit plumbing: flush every open FILE (stdio.cpp), run atexit handlers (runtime.cpp).
void FlushAllStreams();
void RunAtExit(std::uint64_t dso);

// ---- stdio hooks used by the wide-character functions ----
int StreamGetc(gaddr fp);
int StreamUngetc(int c, gaddr fp);
int StreamPutc(int c, gaddr fp);
int StreamWrite(gaddr fp, const void* data, std::uint64_t n);  // bytes written, or -1

// ---- scanf engine (format.cpp) ----
struct ScanInput {
  virtual ~ScanInput() = default;
  virtual int Get() = 0;          // next byte or -1 at EOF
  virtual void Unget(int c) = 0;  // give back the last byte read
};
int ScanfCore(ScanInput& in, const char* fmt, VaList& va);

// ---- long double (IEEE binary128 in the guest), done in software ----
Vec128 DoubleToQuad(double d);

// ---- per-area registration ----
void RegisterPthread();
void RegisterRuntime();  // process/runtime, stdlib, environment, signals, setjmp, props, mmap, sysconf, atexit
void RegisterString();
void RegisterWchar();
void RegisterTime();
void RegisterMath();
void RegisterPosix();  // fd layer, dirs, stat, sockets/epoll/eventfd stubs
void RegisterStdio();

}  // namespace nova::hle::libc
