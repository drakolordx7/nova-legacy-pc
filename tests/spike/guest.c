/* Tiny freestanding AArch64 guest code for the A64 spike (tests/spike/spike_test.cpp).
 * Built with: clang --target=aarch64-linux-android -ffreestanding -nostdlib -O2 -mno-outline-atomics
 * and linked at a fixed address; tools/build_guest.py turns it into a byte array + symbol table. */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
typedef unsigned char v16 __attribute__((vector_size(16), aligned(1)));

/* 1. Plain loads and stores of every width through fastmem (byte, 64-bit, 128-bit/NEON, float). */
u64 ls_test(u8* buf, u64 n) {
  for (u64 i = 0; i < n; i++) buf[i] = (u8)(i * 7 + 1);
  u64 sum = 0;
  for (u64 i = 0; i < n; i++) sum += buf[i];
  *(volatile u64*)(buf + n) = 0x1122334455667788ull;
  sum += *(volatile u64*)(buf + n) >> 56;          /* 0x11 */
  *(v16*)(buf + n + 16) = *(v16*)buf;               /* ldr q / str q */
  sum += ((v16*)(buf + n + 16))[0][3];              /* buf[3] */
  *(volatile float*)(buf + n + 40) = 1.5f;
  sum += (u64)(*(volatile float*)(buf + n + 40) * 2.0f);  /* 3 */
  return sum;
}

/* 2. Faulting accesses. */
u64 peek(volatile u64* p) { return *p; }
void poke(volatile u64* p, u64 v) { *p = v; }

/* 3. LL/SC. The compiler's atomic builtins expand to ldaxr/stlxr loops without LSE; the asm one is plain ldxr/stxr. */
void atomic_inc(volatile u64* p, u64 n) {
  for (u64 i = 0; i < n; i++) __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST);
}
void llsc_inc(volatile u64* p, u64 n) {
  for (u64 i = 0; i < n; i++) {
    u64 t, s;
    __asm__ volatile(
        "1: ldxr %0, [%2]\n"
        "   add %0, %0, #1\n"
        "   stxr %w1, %0, [%2]\n"
        "   cbnz %w1, 1b\n"
        : "=&r"(t), "=&r"(s)
        : "r"(p)
        : "memory");
  }
}
/* ldxr, clrex, stxr must report failure (status 1) and leave memory unchanged; returns status | (value << 8). */
u64 clrex_test(volatile u64* p) {
  u64 t, s;
  __asm__ volatile(
      "ldxr %0, [%2]\n"
      "clrex\n"
      "stxr %w1, %0, [%2]\n"
      : "=&r"(t), "=&r"(s)
      : "r"(p)
      : "memory");
  return s;
}

/* 4. SVC dispatch: fp is the stub of a host handler. It sees x0-x7 = 1..8 and v0-v7 = 1.5..8.5 (doubles) and
 * answers with x0 = sum of the ints and v0 = sum of the doubles. out[0] = x0, out[1] = bits of d0. */
typedef u64 (*host_u64_t)(u64, u64, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double);
typedef double (*host_dbl_t)(u64, u64, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double);
void svc_test(u64 fp, u64* out) {
  out[0] = ((host_u64_t)fp)(1, 2, 3, 4, 5, 6, 7, 8, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5);
  double d = ((host_dbl_t)fp)(1, 2, 3, 4, 5, 6, 7, 8, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5);
  out[1] = *(u64*)&d;
}

/* 5. Re-entrancy: reent_cb(ctx, val) calls the host handler ctx->host(ctx, ctx->cb, val-1), which calls back
 * reent_cb(ctx, val-1) through call_guest. Values live in callee-saved registers across the host call. */
struct ctx {
  u64 host;
  u64 cb;
};
u64 reent_cb(struct ctx* c, u64 val) {
  double keep = (double)val * 0.5;
  u64 a = val * 3, b = val * 5, d = val ^ 0xabcd;
  u64 r = 0;
  if (val > 0) r = ((u64(*)(struct ctx*, u64, u64))c->host)(c, c->cb, val - 1);
  if (a != val * 3 || b != val * 5 || d != (val ^ 0xabcd) || keep != (double)val * 0.5) return 0xdeadbeefull;
  return r + val;
}

/* 6. TPIDR_EL0 per thread. out[0]=tp, out[1]=[tp+8] (thread id), out[2]=[tp] (self), out[3]=tp after msr. */
u64 tp_thread(u64* out) {
  u64 tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  out[0] = tp;
  out[1] = ((u64*)tp)[1];
  out[2] = ((u64*)tp)[0];
  __asm__ volatile("msr tpidr_el0, %0" : : "r"((u64)out + 0x100));
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  out[3] = tp;
  return 0;
}

/* 7. Calling convention checks for call_guest: float math and stack-passed arguments. */
double fp_test(double a, double b) { return a * b + __builtin_sqrt(a); }
u64 many_args(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7, u64 a8, u64 a9, double f8) {
  return a0 + 2 * a1 + 3 * a2 + 4 * a3 + 5 * a4 + 6 * a5 + 7 * a6 + 8 * a7 + 9 * a8 + 10 * a9 + (u64)f8;
}

/* Thread entry for the two-thread LL/SC test: both atomic flavours on the same counter. */
u64 inc_thread(volatile u64* p) {
  atomic_inc(p, 200000);
  llsc_inc(p, 200000);
  return 0;
}
