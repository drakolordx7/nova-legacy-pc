/* Guest side of the fake-JNI test (tests/jni/jni_test.cpp): an AArch64 "native" that goes through the JNIEnv /
 * JavaVM function tables exactly like libNOVA.so does (table slot = index * 8). */
typedef long i64;
typedef unsigned long u64;
typedef int i32;

#define TBL(env) (*(void ***)(env))
#define SLOT(env, i) (TBL(env)[i])

typedef u64 (*FindClass_t)(void *, const char *);
typedef u64 (*GetStaticMethodID_t)(void *, u64, const char *, const char *);
typedef u64 (*NewStringUTF_t)(void *, const char *);
typedef u64 (*CallStaticObjectMethod_t)(void *, u64, u64, ...);
typedef i32 (*CallStaticIntMethod_t)(void *, u64, u64, ...);
typedef i32 (*CallStaticIntMethodV_t)(void *, u64, u64, __builtin_va_list);
typedef i32 (*CallStaticIntMethodA_t)(void *, u64, u64, const u64 *);
typedef const char *(*GetStringUTFChars_t)(void *, u64, unsigned char *);
typedef void (*ReleaseStringUTFChars_t)(void *, u64, const char *);
typedef i32 (*GetStringLength_t)(void *, u64);
typedef i32 (*GetStringUTFLength_t)(void *, u64);
typedef u64 (*NewIntArray_t)(void *, i32);
typedef i32 *(*GetIntArrayElements_t)(void *, u64, unsigned char *);
typedef void (*ReleaseIntArrayElements_t)(void *, u64, i32 *, i32);
typedef i32 (*GetArrayLength_t)(void *, u64);
typedef unsigned char (*ExceptionCheck_t)(void *);
typedef void (*ExceptionClear_t)(void *);
typedef u64 (*NewGlobalRef_t)(void *, u64);
typedef void (*DeleteGlobalRef_t)(void *, u64);
typedef i32 (*GetObjectRefType_t)(void *, u64);
typedef i32 (*GetEnv_t)(void *, void **, i32);

/* A real variadic guest function that forwards its va_list: tests the AAPCS64 va_list struct reader. */
i32 call_v(void *env, u64 cls, u64 mid, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, mid);
  i32 r = ((CallStaticIntMethodV_t)SLOT(env, 130))(env, cls, mid, ap);
  __builtin_va_end(ap);
  return r;
}

static i32 slen(const char *s) {
  i32 n = 0;
  while (s[n]) ++n;
  return n;
}

void run_all(void *env, void *vm, i64 *out, char *buf) {
  int k = 0;
  const u64 cls = ((FindClass_t)SLOT(env, 6))(env, "test/Calc");
  out[k++] = cls != 0;
  const u64 calc = ((GetStaticMethodID_t)SLOT(env, 113))(env, cls, "calc", "(IDJLjava/lang/String;)Ljava/lang/String;");
  const u64 mix = ((GetStaticMethodID_t)SLOT(env, 113))(env, cls, "mix", "(IJD)I");
  const u64 sum = ((GetStaticMethodID_t)SLOT(env, 113))(env, cls, "sum", "([I)I");
  out[k++] = calc != 0 && mix != 0 && sum != 0;

  /* NewStringUTF with a 2-byte UTF-8 character; CallStaticObjectMethod with int, double, long, object varargs. */
  const u64 str = ((NewStringUTF_t)SLOT(env, 167))(env, "h\xC3\xA9llo");
  const u64 r = ((CallStaticObjectMethod_t)SLOT(env, 114))(env, cls, calc, 7, 2.5, 1234567890123L, str);
  out[k++] = r != 0;
  const char *chars = ((GetStringUTFChars_t)SLOT(env, 169))(env, r, 0);
  const i32 n = slen(chars);
  for (i32 i = 0; i <= n; ++i) buf[i] = chars[i];
  ((ReleaseStringUTFChars_t)SLOT(env, 170))(env, r, chars);
  out[k++] = n;
  out[k++] = ((GetStringUTFLength_t)SLOT(env, 168))(env, str); /* 6 bytes */
  out[k++] = ((GetStringLength_t)SLOT(env, 164))(env, str);    /* 5 chars */

  /* va_list flavour (CallStaticIntMethodV) and jvalue[] flavour (CallStaticIntMethodA). */
  out[k++] = call_v(env, cls, mix, 3, 4L, 2.5);
  u64 jv[3];
  jv[0] = 3;
  jv[1] = 4;
  const double d = 2.5;
  __builtin_memcpy(&jv[2], &d, 8);
  out[k++] = ((CallStaticIntMethodA_t)SLOT(env, 131))(env, cls, mix, jv);

  /* Primitive array: Get<T>ArrayElements is a direct pointer into guest memory the host reads back. */
  const u64 arr = ((NewIntArray_t)SLOT(env, 179))(env, 4);
  i32 *el = ((GetIntArrayElements_t)SLOT(env, 187))(env, arr, 0);
  for (i32 i = 0; i < 4; ++i) el[i] = i + 1;
  ((ReleaseIntArrayElements_t)SLOT(env, 195))(env, arr, el, 0);
  out[k++] = ((GetArrayLength_t)SLOT(env, 171))(env, arr);
  out[k++] = ((CallStaticIntMethod_t)SLOT(env, 129))(env, cls, sum, arr); /* the "..." flavour */

  /* A class the real APK lacks: FindClass returns NULL with a pending exception. */
  const u64 missing = ((FindClass_t)SLOT(env, 6))(env, "test/Missing");
  out[k++] = missing == 0;
  out[k++] = ((ExceptionCheck_t)SLOT(env, 228))(env);
  ((ExceptionClear_t)SLOT(env, 17))(env);
  out[k++] = ((ExceptionCheck_t)SLOT(env, 228))(env);

  /* References. */
  const u64 g = ((NewGlobalRef_t)SLOT(env, 21))(env, cls);
  out[k++] = ((GetObjectRefType_t)SLOT(env, 232))(env, g); /* 2 = global */
  ((DeleteGlobalRef_t)SLOT(env, 22))(env, g);

  /* JavaVM: GetEnv from an attached thread returns the same JNIEnv. */
  void *env2 = 0;
  void **vt = *(void ***)vm;
  const i32 rc = ((GetEnv_t)vt[6])(vm, &env2, 0x10006);
  out[k++] = rc == 0 && env2 == env;
}
