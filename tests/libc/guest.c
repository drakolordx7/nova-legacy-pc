/* Guest side of the libc HLE test (tests/libc/libc_test.cpp): freestanding AArch64 code that calls bionic imports
 * through stub addresses. `d` is a zeroed RW scratch area owned by the host; each test_* returns 0 on success or the
 * number of the first failed check. */
typedef long i64;
typedef unsigned long u64;
typedef unsigned int u32;

enum {
#define X(n) S_##n,
#include "names.h"
#undef X
  NSTUBS
};

/* the stub table pointer is kept at scratch+512 (no .data/.bss in the flat guest image); every function binds `g` */
#define GT(p) u64 *g = *(u64 **)((char *)(p) + 512)
#define FN(name, ret, args) ((ret(*) args)g[S_##name])
#define CHECK(n, cond) do { if (!(cond)) return (n); } while (0)

struct timespec { i64 sec, nsec; };

/* ------------------------------------------------------------------ threads */
/* scratch layout: mutex @0 (40), cond @64 (48), counter @128, flag @136, dtor_hits @144, key @152, sem @160 (16),
   recursive mutex @192, thread results @256.. */
#define MTX(d) ((void *)((char *)(d)))
#define CND(d) ((void *)((char *)(d) + 64))
#define CNT(d) ((volatile i64 *)((char *)(d) + 128))
#define FLG(d) ((volatile i64 *)((char *)(d) + 136))
#define DTOR(d) ((volatile i64 *)((char *)(d) + 144))
#define KEY(d) ((u32 *)((char *)(d) + 152))
#define SEM(d) ((void *)((char *)(d) + 160))

static void *worker(void *arg) {
  GT(arg);
  char *d = (char *)arg;
  for (int i = 0; i < 2000; ++i) {
    FN(pthread_mutex_lock, int, (void *))(MTX(d));
    *CNT(d) = *CNT(d) + 1;
    FN(pthread_mutex_unlock, int, (void *))(MTX(d));
  }
  return (void *)0x1234;
}
static void *waiter(void *arg) {
  GT(arg);
  char *d = (char *)arg;
  FN(pthread_mutex_lock, int, (void *))(MTX(d));
  while (*FLG(d) == 0) FN(pthread_cond_wait, int, (void *, void *))(CND(d), MTX(d));
  *FLG(d) = 2;
  FN(pthread_mutex_unlock, int, (void *))(MTX(d));
  return 0;
}
static void key_dtor(void *v) { *DTOR(v) = *DTOR(v) + 1; }
static void *key_thread(void *arg) {
  GT(arg);
  FN(pthread_setspecific, int, (u32, const void *))(*KEY(arg), arg);
  return 0;
}
static void *sem_thread(void *arg) {
  GT(arg);
  FN(usleep, int, (u32))(20000);
  FN(sem_post, int, (void *))(SEM(arg));
  return 0;
}

int test_threads(u64 *stubs, char *d) {
  u64 *g = stubs;
  *(u64 **)((char *)d + 512) = stubs;
  u64 th[4];
  FN(pthread_mutex_init, int, (void *, const void *))(MTX(d), 0);
  FN(pthread_cond_init, int, (void *, const void *))(CND(d), 0);
  for (int i = 0; i < 4; ++i) CHECK(1, FN(pthread_create, int, (u64 *, const void *, void *(*)(void *), void *))(&th[i], 0, worker, d) == 0);
  for (int i = 0; i < 4; ++i) {
    void *ret = 0;
    CHECK(2, FN(pthread_join, int, (u64, void **))(th[i], &ret) == 0);
    CHECK(3, ret == (void *)0x1234);
  }
  CHECK(4, *CNT(d) == 8000);

  /* condition variable handshake */
  u64 w;
  CHECK(5, FN(pthread_create, int, (u64 *, const void *, void *(*)(void *), void *))(&w, 0, waiter, d) == 0);
  FN(usleep, int, (u32))(20000);
  FN(pthread_mutex_lock, int, (void *))(MTX(d));
  *FLG(d) = 1;
  FN(pthread_cond_signal, int, (void *))(CND(d));
  FN(pthread_mutex_unlock, int, (void *))(MTX(d));
  CHECK(6, FN(pthread_join, int, (u64, void **))(w, 0) == 0);
  CHECK(7, *FLG(d) == 2);

  /* timed wait on a condition nobody signals: ETIMEDOUT (110) after ~50 ms */
  struct timespec t0, ts;
  FN(clock_gettime, int, (int, struct timespec *))(0, &ts);
  t0 = ts;
  ts.nsec += 50000000;
  if (ts.nsec >= 1000000000) { ts.sec++; ts.nsec -= 1000000000; }
  FN(pthread_mutex_lock, int, (void *))(MTX(d));
  int r = FN(pthread_cond_timedwait, int, (void *, void *, const struct timespec *))(CND(d), MTX(d), &ts);
  FN(pthread_mutex_unlock, int, (void *))(MTX(d));
  CHECK(8, r == 110);
  struct timespec t1;
  FN(clock_gettime, int, (int, struct timespec *))(0, &t1);
  i64 ms = (t1.sec - t0.sec) * 1000 + (t1.nsec - t0.nsec) / 1000000;
  CHECK(9, ms >= 40 && ms < 1000);

  /* recursive mutex */
  char *rm = d + 192, attr[8];
  FN(pthread_mutexattr_init, int, (void *))(attr);
  FN(pthread_mutexattr_settype, int, (void *, int))(attr, 1);
  FN(pthread_mutex_init, int, (void *, const void *))(rm, attr);
  CHECK(10, FN(pthread_mutex_lock, int, (void *))(rm) == 0);
  CHECK(11, FN(pthread_mutex_lock, int, (void *))(rm) == 0);
  CHECK(12, FN(pthread_mutex_unlock, int, (void *))(rm) == 0);
  CHECK(13, FN(pthread_mutex_unlock, int, (void *))(rm) == 0);
  CHECK(14, FN(pthread_mutex_trylock, int, (void *))(MTX(d)) == 0);  /* free normal mutex */
  CHECK(15, FN(pthread_mutex_trylock, int, (void *))(MTX(d)) == 16); /* EBUSY */
  FN(pthread_mutex_unlock, int, (void *))(MTX(d));

  /* key destructor runs at thread exit, with the stored value */
  CHECK(16, FN(pthread_key_create, int, (u32 *, void (*)(void *)))(KEY(d), key_dtor) == 0);
  u64 k;
  FN(pthread_create, int, (u64 *, const void *, void *(*)(void *), void *))(&k, 0, key_thread, d);
  FN(pthread_join, int, (u64, void **))(k, 0);
  CHECK(17, *DTOR(d) == 1);
  CHECK(18, FN(pthread_getspecific, void *, (u32))(*KEY(d)) == 0); /* this thread never set it */

  /* semaphore: sem_wait blocks until the helper thread posts */
  FN(sem_init, int, (void *, int, u32))(SEM(d), 0, 0);
  u64 s;
  FN(pthread_create, int, (u64 *, const void *, void *(*)(void *), void *))(&s, 0, sem_thread, d);
  CHECK(19, FN(sem_wait, int, (void *))(SEM(d)) == 0);
  FN(pthread_join, int, (u64, void **))(s, 0);
  return 0;
}

/* ------------------------------------------------------------------ stdio on the VFS, files, directories */
struct stat_buf { u64 dev, ino; u32 mode, nlink, uid, gid; u64 rdev, pad; i64 size; int blksize, pad2; i64 blocks; i64 rest[7]; };
struct dirent_ { u64 ino; i64 off; unsigned short reclen; unsigned char type; char name[256]; };

int test_stdio(u64 *stubs, char *d) {
  u64 *g = stubs;
  *(u64 **)((char *)d + 512) = stubs;
  const char *dir = "/data/data/com.gameloft.android.ANMP.GloftNOHM/files";
  const char *path = "/data/data/com.gameloft.android.ANMP.GloftNOHM/files/t.txt";
  FN(mkdir, int, (const char *, u32))(dir, 0777);
  void *f = FN(fopen, void *, (const char *, const char *))(path, "w");
  CHECK(1, f != 0);
  CHECK(2, FN(fprintf, int, (void *, const char *, ...))(f, "x=%d y=%ld s=%s f=%.2f\n", -5, (i64)1 << 40, "hi", 3.14159) > 0);
  FN(fputs, int, (const char *, void *))("second line\n", f);
  CHECK(3, FN(fclose, int, (void *))(f) == 0);

  f = FN(fopen, void *, (const char *, const char *))(path, "r");
  CHECK(4, f != 0);
  char line[128];
  CHECK(5, FN(fgets, char *, (char *, int, void *))(line, sizeof line, f) == line);
  int x = 0; i64 y = 0; char s[16]; double fl = 0;
  CHECK(6, FN(sscanf, int, (const char *, const char *, ...))(line, "x=%d y=%ld s=%15s f=%lf", &x, &y, s, &fl) == 4);
  CHECK(7, x == -5 && y == ((i64)1 << 40) && s[0] == 'h' && s[1] == 'i' && s[2] == 0 && fl > 3.139 && fl < 3.141); /* printed with %.2f */
  i64 pos = FN(ftell, i64, (void *))(f);
  CHECK(8, pos == (i64)FN(strlen, u64, (const char *))(line));
  CHECK(9, FN(fseek, int, (void *, i64, int))(f, 0, 2) == 0);
  CHECK(10, FN(ftell, i64, (void *))(f) == pos + 12);
  CHECK(11, FN(fseek, int, (void *, i64, int))(f, pos, 0) == 0);
  char buf[16];
  CHECK(12, FN(fread, u64, (void *, u64, u64, void *))(buf, 1, 11, f) == 11 && buf[0] == 's');
  FN(fclose, int, (void *))(f);

  struct stat_buf st;
  CHECK(13, FN(stat, int, (const char *, void *))(path, &st) == 0);
  CHECK(14, st.size == pos + 12 && (st.mode & 0170000) == 0100000);
  CHECK(15, FN(stat, int, (const char *, void *))(dir, &st) == 0 && (st.mode & 0170000) == 0040000);
  CHECK(16, FN(access, int, (const char *, int))(path, 4) == 0);
  CHECK(17, FN(access, int, (const char *, int))("/data/data/nothing/here", 0) == -1 && *FN(__errno, int *, (void))() == 2);

  /* open/read/lseek on a descriptor, appending through "a" */
  f = FN(fopen, void *, (const char *, const char *))(path, "a");
  FN(fputs, int, (const char *, void *))("tail\n", f);
  FN(fclose, int, (void *))(f);
  int fd = FN(open, int, (const char *, int, ...))(path, 0 /* O_RDONLY */);
  CHECK(18, fd >= 3);
  CHECK(19, FN(lseek, i64, (int, i64, int))(fd, 0, 2) == pos + 12 + 5);
  CHECK(20, FN(lseek, i64, (int, i64, int))(fd, -5, 2) == pos + 12);
  CHECK(21, FN(read, i64, (int, void *, u64))(fd, buf, 4) == 4 && buf[0] == 't' && buf[3] == 'l');
  FN(close, int, (int))(fd);

  /* directory listing sees ".", "..", the file */
  void *dp = FN(opendir, void *, (const char *))(dir);
  CHECK(22, dp != 0);
  int n = 0, found = 0;
  struct dirent_ *e;
  while ((e = FN(readdir, struct dirent_ *, (void *))(dp)) != 0) {
    ++n;
    if (e->name[0] == 't' && e->name[1] == '.' && e->type == 8) found = 1;
  }
  FN(closedir, int, (void *))(dp);
  CHECK(23, n == 3 && found);
  CHECK(24, FN(unlink, int, (const char *))(path) == 0);
  CHECK(25, FN(fopen, void *, (const char *, const char *))(path, "r") == 0 && *FN(__errno, int *, (void))() == 2);
  /* paths outside the mapped roots do not exist */
  CHECK(26, FN(fopen, void *, (const char *, const char *))("/etc/passwd", "r") == 0);
  /* synthesised system files */
  f = FN(fopen, void *, (const char *, const char *))("/proc/cpuinfo", "r");
  CHECK(27, f != 0);
  CHECK(28, FN(fgets, char *, (char *, int, void *))(line, sizeof line, f) == line && line[0] == 'p');
  FN(fclose, int, (void *))(f);
  return 0;
}

/* ------------------------------------------------------------------ printf / scanf / strtod / qsort / math / time / wide */
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int test_format(u64 *stubs, char *d) {
  u64 *g = stubs;
  *(u64 **)((char *)d + 512) = stubs;
  char b[160];
  FN(snprintf, int, (char *, u64, const char *, ...))(b, sizeof b, "%5.2f|%ld|%s|%c|%#x|%08.3f|%-4d|%u", 3.14159, (i64)-9000000000L, "str", 'Z', 255, -2.5, 7, 4000000000u);
  const char *want = " 3.14|-9000000000|str|Z|0xff|-002.500|7   |4000000000";
  for (int i = 0; want[i]; ++i) CHECK(1, b[i] == want[i]);
  CHECK(2, b[FN(strlen, u64, (const char *))(want)] == 0);

  char *end;
  double v = FN(strtod, double, (const char *, char **))("  3.25e2xyz", &end);
  CHECK(3, v == 325.0 && end[0] == 'x');
  CHECK(4, FN(strtol, i64, (const char *, char **, int))("-0x1F", &end, 16) == -31 && *end == 0);
  CHECK(5, FN(strtol, i64, (const char *, char **, int))("99999999999999999999", &end, 10) == 0x7fffffffffffffffL && *FN(__errno, int *, (void))() == 34);

  int a, c; double fv; char w[8];
  CHECK(6, FN(sscanf, int, (const char *, const char *, ...))("10 0x20 3.5 abc", "%d %i %lf %7s", &a, &c, &fv, w) == 4);
  CHECK(7, a == 10 && c == 32 && fv == 3.5 && w[0] == 'a' && w[2] == 'c');

  int arr[9] = {5, 3, 9, 1, 7, 2, 8, 4, 6};
  FN(qsort, void, (void *, u64, u64, int (*)(const void *, const void *)))(arr, 9, 4, cmp_int);
  for (int i = 0; i < 9; ++i) CHECK(8, arr[i] == i + 1);

  /* libm (float in s regs) */
  float s = FN(sinf, float, (float))(1.0f), p = FN(powf, float, (float, float))(2.0f, 10.0f), m = FN(fmodf, float, (float, float))(7.5f, 2.0f);
  CHECK(9, s > 0.84147f && s < 0.84148f && p == 1024.0f && m == 1.5f);

  /* time */
  i64 t = 1000000000; /* 2001-09-09 01:46:40 UTC */
  int tm[14];
  FN(gmtime_r, void *, (const i64 *, void *))(&t, tm);
  FN(strftime, u64, (char *, u64, const char *, const void *))(b, sizeof b, "%F %T %a %j", tm);
  const char *wt = "2001-09-09 01:46:40 Sun 252";
  for (int i = 0; wt[i]; ++i) CHECK(10, b[i] == wt[i]);
  CHECK(11, FN(timegm, i64, (void *))(tm) == t);

  /* wide characters: 32-bit wchar_t, UTF-8 multibyte */
  u32 wb[16];
  FN(swprintf, int, (u32 *, u64, const u32 *, ...))(wb, 16, (const u32[]){'%', 'd', '-', '%', 'l', 's', 0}, 42, (const u32[]){0xE9, 'x', 0});
  CHECK(12, wb[0] == '4' && wb[1] == '2' && wb[2] == '-' && wb[3] == 0xE9 && wb[4] == 'x' && wb[5] == 0);
  CHECK(13, FN(wcslen, u64, (const u32 *))(wb) == 5);
  u32 wc = 0;
  CHECK(14, FN(mbrtowc, u64, (u32 *, const char *, u64, void *))(&wc, "\xC3\xA9", 2, 0) == 2 && wc == 0xE9);
  char mb[8];
  CHECK(15, FN(wcrtomb, u64, (char *, u32, void *))(mb, 0x20AC, 0) == 3 && (unsigned char)mb[0] == 0xE2);
  return 0;
}

/* ------------------------------------------------------------------ setjmp / longjmp on guest registers */
static void deep(void *jb, int n) {
  GT(jb);
  if (n == 0) FN(longjmp, void, (void *, int))(jb, 7);
  volatile char pad[64];
  pad[0] = (char)n;
  deep(jb, n - 1);
  pad[1] = pad[0];
}

int test_setjmp(u64 *stubs, char *d) {
  u64 *g = stubs;
  *(u64 **)((char *)d + 512) = stubs;
  volatile int hits = 0;
  void *jb = d;
  int r = FN(setjmp, int, (void *))(jb);
  if (r == 0) {
    hits = hits + 1;
    deep(jb, 5);
    return 1; /* not reached */
  }
  CHECK(2, r == 7);
  CHECK(3, hits == 1);
  /* longjmp(buf, 0) delivers 1 */
  r = FN(setjmp, int, (void *))(jb);
  if (r == 0) {
    hits = hits + 1;
    FN(longjmp, void, (void *, int))(jb, 0);
  }
  CHECK(4, r == 1 && hits == 2);
  return 0;
}
