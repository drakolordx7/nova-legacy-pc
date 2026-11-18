/* Guest side of the AAPCS64 marshalling test (tests/abi/abi_test.cpp): calls host handlers through stub addresses
 * with every argument/return shape the NOVA_HLE marshalling claims to support. */
typedef long i64;
typedef unsigned long u64;

struct S16 { i64 a, b; };            /* 16 bytes: two x regs */
struct S12 { int a, b, c; };         /* 12 bytes: two x regs, returned in x0:x1 */
struct V4f { float a, b, c, d; };    /* HFA of 4 floats: v0-v3 */
struct V2d { double a, b; };         /* HFA of 2 doubles */
struct Big { i64 a, b, c; };         /* 24 bytes: by reference / sret via x8 */

#define PUT_D(slot, v) do { double _d = (v); __builtin_memcpy(&out[slot], &_d, 8); } while (0)

/* Direct call of a variadic-callee stub with 20 alternating int64 / double values. */
typedef i64 (*var_fn)(const char*, int, ...);

/* A real variadic guest function that forwards its va_list to a host handler. */
typedef i64 (*vl_fn)(i64, __builtin_va_list);
i64 forward_vl(u64 host_vl, i64 n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  i64 r = ((vl_fn)host_vl)(n, ap);
  __builtin_va_end(ap);
  return r;
}

void run_all(u64* stubs, i64* out) {
  int k = 0;
  out[k++] = ((i64(*)(signed char, unsigned char, short, unsigned short, int, unsigned, i64, u64, i64, int))stubs[0])(
      -1, 200, -300, 60000, -5, 4000000000u, -7, 8, -9, -10);
  PUT_D(k++, ((double(*)(float, double, float, double, float, double, float, double, float, double))stubs[1])(
                 1.5f, 2.5, 3.5f, 4.5, 5.5f, 6.5, 7.5f, 8.5, 9.5f, 10.5));
  {
    struct S16 s = {100, 200};
    struct S16 r = ((struct S16(*)(struct S16, i64))stubs[2])(s, 7);
    out[k++] = r.a;
    out[k++] = r.b;
  }
  {
    struct S12 s = {1, 2, 3};
    struct S12 r = ((struct S12(*)(struct S12))stubs[3])(s);
    out[k++] = r.a * 10000 + r.b * 100 + r.c;
  }
  {
    struct V4f v = {1.f, 2.f, 3.f, 4.f};
    struct V4f r = ((struct V4f(*)(struct V4f, float))stubs[4])(v, 2.f);
    PUT_D(k++, (double)r.a + r.b * 10 + r.c * 100 + r.d * 1000);
  }
  {
    struct V2d v = {1.25, 2.5};
    struct V2d r = ((struct V2d(*)(struct V2d))stubs[5])(v);
    PUT_D(k++, r.a);
    PUT_D(k++, r.b);
  }
  {
    struct Big b = {1, 2, 3};
    struct Big r = ((struct Big(*)(struct Big, i64))stubs[6])(b, 100);
    out[k++] = r.a;
    out[k++] = r.b;
    out[k++] = r.c;
  }
  /* 7 ints fill x0-x6, so the 16-byte struct does not fit the remaining x7: it goes on the stack and NGRN = 8. */
  {
    struct S16 s = {1000, 2000};
    out[k++] = ((i64(*)(i64, i64, i64, i64, i64, i64, i64, struct S16, i64))stubs[7])(1, 2, 3, 4, 5, 6, 7, s, 9);
  }
  out[k++] = ((var_fn)stubs[8])("tag", 20, (i64)1, 1.5, (i64)2, 2.5, (i64)3, 3.5, (i64)4, 4.5, (i64)5, 5.5, (i64)6, 6.5,
                                (i64)7, 7.5, (i64)8, 8.5, (i64)9, 9.5, (i64)10, 10.5, (i64)11, 11.5, (i64)12, 12.5,
                                (i64)13, 13.5, (i64)14, 14.5, (i64)15, 15.5, (i64)16, 16.5, (i64)17, 17.5, (i64)18, 18.5,
                                (i64)19, 19.5, (i64)20, 20.5);
  out[k++] = forward_vl(stubs[9], 20, (i64)1, 1.5, (i64)2, 2.5, (i64)3, 3.5, (i64)4, 4.5, (i64)5, 5.5, (i64)6, 6.5,
                        (i64)7, 7.5, (i64)8, 8.5, (i64)9, 9.5, (i64)10, 10.5, (i64)11, 11.5, (i64)12, 12.5,
                        (i64)13, 13.5, (i64)14, 14.5, (i64)15, 15.5, (i64)16, 16.5, (i64)17, 17.5, (i64)18, 18.5,
                        (i64)19, 19.5, (i64)20, 20.5);
  /* pointers and bool, void return, float return */
  {
    char text[8] = {'h', 'i', 0};
    out[k++] = ((i64(*)(const char*, _Bool))stubs[10])(text, 1);
    ((void(*)(i64*))stubs[11])(&out[k++]);
    PUT_D(k++, (double)((float(*)(float))stubs[12])(2.5f));
  }
}
