/* Guest side of the OpenSL ES test (tests/audio/audio_test.cpp): does what vox::DriverAndroid::_InitOSL does, through
 * the interface vtables, with the vtable layouts of OpenSLES.h. The host passes the addresses of the imports (the
 * slCreateEngine stub and the SL_IID_* data symbols, as a GOT entry would). */
typedef unsigned int u32;
typedef unsigned long u64;
typedef unsigned short u16;
typedef unsigned char u8;

typedef const struct ObjVt* const* Obj;
typedef const struct EngVt* const* Eng;
typedef const struct PlayVt* const* Play;
typedef const struct BqVt* const* Bq;

struct ObjVt {
  u32 (*Realize)(Obj, u32);
  u32 (*Resume)(Obj, u32);
  u32 (*GetState)(Obj, u32*);
  u32 (*GetInterface)(Obj, const void*, void*);
  void* RegisterCallback;
  void* AbortAsyncOperation;
  void (*Destroy)(Obj);
  void* SetPriority;
  void* GetPriority;
  void* SetLossOfControlInterfaces;
};
struct EngVt {
  u32 (*CreateLEDDevice)(Eng, void*, u32, u32, const void*, const void*);
  void* CreateVibraDevice;
  u32 (*CreateAudioPlayer)(Eng, Obj*, void*, void*, u32, const void*, const void*);
  void* CreateAudioRecorder;
  void* CreateMidiPlayer;
  void* CreateListener;
  void* Create3DGroup;
  u32 (*CreateOutputMix)(Eng, Obj*, u32, const void*, const void*);
  void* rest[5];
};
struct PlayVt {
  u32 (*SetPlayState)(Play, u32);
  u32 (*GetPlayState)(Play, u32*);
  void* rest[10];
};
struct BqState {
  u32 count, play_index;
};
struct BqVt {
  u32 (*Enqueue)(Bq, const void*, u32);
  u32 (*Clear)(Bq);
  u32 (*GetState)(Bq, struct BqState*);
  u32 (*RegisterCallback)(Bq, void (*)(Bq, void*), void*);
};

struct Imports {
  u32 (*slCreateEngine)(Obj*, u32, const void*, u32, const void*, const void*);
  const void* const* iid_engine;
  const void* const* iid_play;
  const void* const* iid_bq;
};

struct State {
  Obj eng, mix, player;
  Eng eng_itf;
  Play play;
  Bq bq;
  u32 cb_count, cb_queue_count, cb_play_index;
  u64 cb_tpidr, main_tpidr;
  u32 r[24];
  u8 pcm[4096];
};

static u64 tpidr(void) {
  u64 v;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
  return v;
}

/* The buffer-queue callback, as the game's CallbackOSL: counts, and looks at the queue. */
static void on_buffer_done(Bq bq, void* ctx) {
  struct State* s = (struct State*)ctx;
  struct BqState st;
  st.count = 99;
  st.play_index = 99;
  (*bq)->GetState(bq, &st);
  s->cb_queue_count = st.count;
  s->cb_play_index = st.play_index;
  s->cb_tpidr = tpidr();
  s->cb_count = s->cb_count + 1;
}

void run(struct State* s, const struct Imports* im) {
  int k = 0;
  s->main_tpidr = tpidr();
  s->r[k++] = im->slCreateEngine(&s->eng, 0, 0, 0, 0, 0);                              /* 0 */
  s->r[k++] = (*s->eng)->Realize(s->eng, 0);                                           /* 1 */
  s->r[k++] = (*s->eng)->GetInterface(s->eng, *im->iid_engine, &s->eng_itf);          /* 2 */
  Obj mix = 0;
  s->r[k++] = (*s->eng_itf)->CreateOutputMix(s->eng_itf, &mix, 0, 0, 0);               /* 3 */
  s->mix = mix;
  s->r[k++] = (*s->mix)->Realize(s->mix, 0);                                           /* 4 */

  /* SLDataLocator_BufferQueue {6, 2}, SLDataFormat_PCM {2, 2 ch, 44100000 mHz, 16, 16, 3, 2 (little endian)} */
  u32 src_loc[2];
  src_loc[0] = 6;
  src_loc[1] = 2;
  u32 pcm[7];
  pcm[0] = 2;
  pcm[1] = 2;
  pcm[2] = 44100000;
  pcm[3] = 16;
  pcm[4] = 16;
  pcm[5] = 3;
  pcm[6] = 2;
  void* src[2];
  src[0] = src_loc;
  src[1] = pcm;
  /* SLDataLocator_OutputMix {4, mix} */
  u64 snk_loc[2];
  snk_loc[0] = 4;
  snk_loc[1] = (u64)s->mix;
  void* snk[2];
  snk[0] = snk_loc;
  snk[1] = 0;
  const void* ids[1];
  ids[0] = *im->iid_bq;
  u32 req[1];
  req[0] = 1;
  Obj player = 0;
  s->r[k++] = (*s->eng_itf)->CreateAudioPlayer(s->eng_itf, &player, src, snk, 1, ids, req); /* 5 */
  s->player = player;
  s->r[k++] = (*s->player)->Realize(s->player, 0);                                     /* 6 */
  s->r[k++] = (*s->player)->GetInterface(s->player, *im->iid_play, &s->play);         /* 7 */
  s->r[k++] = (*s->player)->GetInterface(s->player, *im->iid_bq, &s->bq);             /* 8 */
  s->r[k++] = (*s->bq)->RegisterCallback(s->bq, on_buffer_done, s);                    /* 9 */
  s->r[k++] = (*s->bq)->Enqueue(s->bq, s->pcm, sizeof s->pcm);                         /* 10 */
  struct BqState st;
  st.count = 99;
  st.play_index = 99;
  s->r[k++] = (*s->bq)->GetState(s->bq, &st);                                          /* 11 */
  s->r[k++] = st.count;                                                                /* 12: 1 before play */
  s->r[k++] = (*s->play)->SetPlayState(s->play, 3);                                    /* 13 */
  /* unused slots and unknown interfaces answer SL_RESULT_FEATURE_UNSUPPORTED (12) */
  s->r[k++] = (*s->eng_itf)->CreateLEDDevice(s->eng_itf, 0, 0, 0, 0, 0);               /* 14 */
  u32 junk[4];
  junk[0] = junk[1] = junk[2] = junk[3] = 0x12345678;
  void* out = 0;
  s->r[k++] = (*s->mix)->GetInterface(s->mix, junk, &out);                             /* 15 */
}

void finish(struct State* s) {
  int k = 16;
  u32 ps = 0;
  (*s->play)->SetPlayState(s->play, 1);
  s->r[k++] = (*s->play)->GetPlayState(s->play, &ps);                                  /* 16 */
  s->r[k++] = ps;                                                                      /* 17: stopped (1) */
  (*s->player)->Destroy(s->player);
  (*s->mix)->Destroy(s->mix);
  (*s->eng)->Destroy(s->eng);
}
