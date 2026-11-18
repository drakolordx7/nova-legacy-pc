// OpenSL ES HLE: slCreateEngine and the guest-visible COM-style interface objects.
//
// An interface (SLObjectItf, SLEngineItf, SLPlayItf, SLBufferQueueItf) is a pointer to a 16-byte guest block whose
// first 8 bytes point at a vtable (a guest table of `svc` stub addresses). The game calls straight through it. The
// host side of a block is looked up in a table keyed by the block's guest address, never read back from guest memory.
//
// Vtable layouts (OpenSLES.h): Object 10 slots, Engine 15, Play 12, BufferQueue 4. Slots the game never calls point at
// a shared "unsupported" handler: it returns SL_RESULT_FEATURE_UNSUPPORTED and logs the slot once.
//
// Playback: every player owns an output Sink (an SDL3 stream). One audio thread, a proper guest thread, moves enqueued
// data into the sinks and, when the device has taken a buffer, calls the game's buffer-queue callback with call_guest.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/audio.h"
#include "audio/sink.h"
#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle_abi.h"
#include "hle/heap.h"

namespace nova::audio {
namespace {
using namespace std::chrono_literals;

// SLresult values.
constexpr std::uint32_t kOk = 0, kPrecond = 1, kParam = 2, kMemory = 3, kBufferInsufficient = 7, kContentUnsupported = 9,
                        kFeatureUnsupported = 12;
// SL_OBJECT_STATE_*, SL_PLAYSTATE_*.
constexpr std::uint32_t kObjUnrealized = 1, kObjRealized = 2;
constexpr std::uint32_t kPlayStopped = 1, kPlayPaused = 2, kPlayPlaying = 3;
// Data locator / format types.
constexpr std::uint32_t kLocOutputMix = 4, kLocBufferQueue = 6, kLocAndroidSimpleBQ = 0x800007BD, kFmtPcm = 2;
constexpr std::uint32_t kTimeUnknown = 0xFFFFFFFFu;
constexpr std::size_t kMaxEnqueueBytes = 64u << 20;

struct Guid {  // SLInterfaceID_
  std::uint32_t a;
  std::uint16_t b, c, d;
  std::uint8_t e[6];
};
static_assert(sizeof(Guid) == 16);

enum class Iid { Unknown, Object, Engine, Play, BufferQueue };
struct IidDef {
  Iid id;
  const char* name;
  Guid guid;
};
// Values from OpenSLES.h (SL_IID_*).
const IidDef kIids[] = {
    {Iid::Object, "SL_IID_OBJECT", {0x79216360, 0xddd7, 0x11db, 0xac16, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {Iid::Engine, "SL_IID_ENGINE", {0x8d97c260, 0xddd4, 0x11db, 0x958f, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {Iid::Play, "SL_IID_PLAY", {0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
    {Iid::BufferQueue, "SL_IID_BUFFERQUEUE", {0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}},
};

enum class Kind : std::uint32_t { Object, Engine, Play, BufferQueue };
constexpr int kKinds = 4;
enum class ObjType { Engine, OutputMix, AudioPlayer };

struct Buf {
  std::vector<std::uint8_t> data;
  std::uint64_t end = 0;  // value of Player::sunk once the whole buffer has been handed to the sink
  bool submitted = false;
};

struct Player {
  std::uint32_t rate = 44100, channels = 2, bits = 16, frame_bytes = 4;
  std::uint32_t max_buffers = 2;
  Sink sink;
  std::deque<Buf> queue;
  std::uint64_t sunk = 0;      // bytes handed to the sink since the last stop
  std::uint64_t completed = 0;  // buffers finished (BufferQueue playIndex)
  std::uint64_t played_bytes = 0;  // bytes of the finished buffers since the last stop
  std::uint32_t play_state = kPlayStopped;
  gaddr cb = 0, cb_ctx = 0, bq_itf = 0;
  int inflight = 0;  // callbacks being run by the audio thread
  bool dead = false;
};

struct Obj {
  std::uint32_t id = 0;
  ObjType type = ObjType::Engine;
  std::uint32_t state = kObjUnrealized;
  gaddr itf[kKinds] = {};
  std::shared_ptr<Player> player;
};

struct ItfInfo {
  std::uint32_t obj_id;
  Kind kind;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::map<std::uint32_t, std::shared_ptr<Obj>> g_objects;
std::unordered_map<gaddr, ItfInfo> g_itfs;
std::uint32_t g_next_id = 1;
float g_master = 1.0f;
bool g_paused = false;
std::set<std::string> g_logged;

gaddr g_vtbl[kKinds] = {};

std::mutex g_thread_mu;
threads::ThreadId g_thread = 0;
std::atomic<bool> g_stop{false};
std::atomic<threads::ThreadId> g_audio_tid{0};

void LogOnce(const std::string& key, const std::string& msg) {
  std::lock_guard lk(g_mu);
  if (g_logged.insert(key).second) log::Warn("opensl: {}", msg);
}

// ---------------------------------------------------------------------------------------------------------------
// Audio thread
struct Fire {
  std::shared_ptr<Player> p;
  gaddr cb, itf, ctx;
};

// Caller holds g_mu.
void Pump(Player& p, std::vector<Fire>& fires, const std::shared_ptr<Player>& sp) {
  if (p.dead || !p.sink.is_open() || p.play_state != kPlayPlaying || g_paused) return;
  for (Buf& b : p.queue) {
    if (b.submitted) continue;
    p.sink.Put(b.data.data(), b.data.size());
    p.sunk += b.data.size();
    b.end = p.sunk;
    b.submitted = true;
  }
  const std::uint64_t queued = std::min<std::uint64_t>(p.sink.Queued(), p.sunk);
  const std::uint64_t consumed = p.sunk - queued;
  while (!p.queue.empty() && p.queue.front().submitted && p.queue.front().end <= consumed) {
    p.played_bytes += p.queue.front().data.size();
    p.queue.pop_front();
    ++p.completed;
    if (p.cb) {
      ++p.inflight;
      fires.push_back({sp, p.cb, p.bq_itf, p.cb_ctx});
    }
  }
}

void AudioThreadMain(Cpu& cpu) {
  g_audio_tid = cpu.thread_id();
  std::vector<Fire> fires;
  while (!g_stop.load()) {
    fires.clear();
    {
      std::unique_lock lk(g_mu);
      bool playing = false;
      for (auto& [id, o] : g_objects) {
        if (!o->player) continue;
        Pump(*o->player, fires, o->player);
        playing |= o->player->play_state == kPlayPlaying;
      }
      if (fires.empty()) g_cv.wait_for(lk, playing && !g_paused ? 2ms : 100ms);
    }
    for (Fire& f : fires) {
      bool fault = false;
      try {
        cpu.Call<void>(f.cb, f.itf, f.ctx);
      } catch (const GuestFault& e) {
        log::Error("opensl: the buffer-queue callback faulted; callbacks disabled for this player\n{}", e.what());
        fault = true;
      }
      std::lock_guard lk(g_mu);
      if (fault) f.p->cb = 0;
      --f.p->inflight;
      g_cv.notify_all();
    }
  }
  g_audio_tid = 0;
  cpu.set_x(0, 0);
}

void EnsureThread() {
  std::lock_guard lk(g_thread_mu);
  if (g_thread && threads::IsAlive(g_thread)) return;
  if (g_thread) threads::Join(g_thread);
  g_stop = false;
  threads::CreateParams cp;
  cp.entry = hle::StubFor("__nova_audio_thread");
  cp.name = "nova-audio";
  g_thread = threads::Create(cp);
  if (!g_thread) log::Error("opensl: could not start the audio thread");
}

// ---------------------------------------------------------------------------------------------------------------
// Objects and interface blocks
std::shared_ptr<Obj> NewObj(ObjType type) {  // caller holds g_mu
  auto o = std::make_shared<Obj>();
  o->id = g_next_id++;
  o->type = type;
  for (int k = 0; k < kKinds; ++k) {
    const gaddr a = hle::heap::Alloc(16);
    mem::W64(a, g_vtbl[k]);
    mem::W64(a + 8, 0);
    o->itf[k] = a;
    g_itfs[a] = {o->id, static_cast<Kind>(k)};
  }
  g_objects[o->id] = o;
  return o;
}

std::shared_ptr<Obj> FindObj(gaddr self, Kind want) {  // caller holds g_mu
  const auto it = g_itfs.find(self);
  if (it == g_itfs.end() || it->second.kind != want) return nullptr;
  const auto o = g_objects.find(it->second.obj_id);
  return o == g_objects.end() ? nullptr : o->second;
}

Iid IidOf(gaddr iid) {
  if (!iid) return Iid::Unknown;
  Guid g;
  std::memcpy(&g, g2h(iid), sizeof g);
  for (const IidDef& d : kIids)
    if (!std::memcmp(&g, &d.guid, sizeof g)) return d.id;
  return Iid::Unknown;
}

std::string GuidString(gaddr iid) {
  if (!iid) return "NULL";
  Guid g;
  std::memcpy(&g, g2h(iid), sizeof g);
  return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", g.a, g.b, g.c, g.d, g.e[0], g.e[1],
                     g.e[2], g.e[3], g.e[4], g.e[5]);
}

// Reads the interface-id / required arrays of a Create* call. False (and `res` set) when a required interface is
// not available. `supported` tells which Iid values the new object can provide.
bool CheckRequested(std::uint32_t n, gaddr ids, gaddr required, std::initializer_list<Iid> supported, std::uint32_t& res) {
  for (std::uint32_t i = 0; i < n && ids; ++i) {
    const gaddr iid = mem::R64(ids + 8ull * i);
    const Iid k = IidOf(iid);
    const bool ok = k != Iid::Unknown && std::find(supported.begin(), supported.end(), k) != supported.end();
    if (ok) continue;
    const bool req = required && mem::R<std::uint32_t>(required + 4ull * i) != 0;  // SLboolean is 32-bit
    if (req) {
      LogOnce("req-" + GuidString(iid), "required interface " + GuidString(iid) + " is not supported");
      res = kFeatureUnsupported;
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// SLObjectItf
std::uint32_t ObjRealize(std::uint64_t self, std::uint32_t /*async*/) {
  std::unique_lock lk(g_mu);
  auto o = FindObj(self, Kind::Object);
  if (!o) return kParam;
  if (o->state != kObjUnrealized) return kPrecond;
  if (o->player && !o->player->sink.Open(o->player->rate, o->player->channels, o->player->bits)) return kContentUnsupported;
  if (o->player) o->player->sink.SetGain(g_master);
  o->state = kObjRealized;
  lk.unlock();
  if (o->player) EnsureThread();
  return kOk;
}

std::uint32_t ObjGetState(std::uint64_t self, std::uint32_t* state) {
  std::lock_guard lk(g_mu);
  auto o = FindObj(self, Kind::Object);
  if (!o || !state) return kParam;
  *state = o->state;
  return kOk;
}

std::uint32_t ObjGetInterface(std::uint64_t self, std::uint64_t iid, std::uint64_t* out) {
  std::lock_guard lk(g_mu);
  auto o = FindObj(self, Kind::Object);
  if (!o || !iid || !out) return kParam;
  const Iid k = IidOf(iid);
  Kind want;
  bool ok = false;
  switch (k) {
    case Iid::Object: want = Kind::Object; ok = true; break;
    case Iid::Engine: want = Kind::Engine; ok = o->type == ObjType::Engine; break;
    case Iid::Play: want = Kind::Play; ok = o->type == ObjType::AudioPlayer; break;
    case Iid::BufferQueue: want = Kind::BufferQueue; ok = o->type == ObjType::AudioPlayer; break;
    default: want = Kind::Object; break;
  }
  if (!ok) {
    *out = 0;
    if (g_logged.insert("gi-" + GuidString(iid)).second)
      log::Warn("opensl: GetInterface({}) is not supported on this object", GuidString(iid));
    return kFeatureUnsupported;
  }
  if (o->state != kObjRealized && k != Iid::Object) {
    *out = 0;
    return kPrecond;
  }
  *out = o->itf[static_cast<int>(want)];
  return kOk;
}

std::uint32_t ObjDestroy(std::uint64_t self) {
  std::shared_ptr<Obj> o;
  {
    std::unique_lock lk(g_mu);
    o = FindObj(self, Kind::Object);
    if (!o) return kParam;
    if (o->player) {
      Player& p = *o->player;
      p.dead = true;
      p.cb = 0;
      // Let a callback that is running finish (the game can destroy the player while one is in flight). Not when
      // called from inside the callback itself.
      if (g_audio_tid.load() != threads::CurrentId()) g_cv.wait_for(lk, 500ms, [&] { return p.inflight == 0; });
      p.sink.SetPaused(true);
      p.sink.Close();
    }
    for (int k = 0; k < kKinds; ++k) g_itfs.erase(o->itf[k]);
    g_objects.erase(o->id);
  }
  for (int k = 0; k < kKinds; ++k) hle::heap::Free(o->itf[k]);
  return kOk;
}

// ---------------------------------------------------------------------------------------------------------------
// SLEngineItf
std::uint32_t EngineCreateOutputMix(std::uint64_t self, std::uint64_t* out, std::uint32_t n, std::uint64_t ids, std::uint64_t req) {
  std::lock_guard lk(g_mu);
  if (!FindObj(self, Kind::Engine)) return kParam;
  if (!out) return kParam;
  std::uint32_t res = kOk;
  if (!CheckRequested(n, ids, req, {Iid::Object}, res)) return res;
  *out = NewObj(ObjType::OutputMix)->itf[static_cast<int>(Kind::Object)];
  return kOk;
}

std::uint32_t EngineCreateAudioPlayer(std::uint64_t self, std::uint64_t* out, std::uint64_t src, std::uint64_t snk, std::uint32_t n,
                                      std::uint64_t ids, std::uint64_t req) {
  std::lock_guard lk(g_mu);
  if (!FindObj(self, Kind::Engine)) return kParam;
  if (!out || !src || !snk) return kParam;
  std::uint32_t res = kOk;
  if (!CheckRequested(n, ids, req, {Iid::Object, Iid::Play, Iid::BufferQueue}, res)) return res;

  // SLDataSource {pLocator, pFormat}: a buffer queue of PCM.
  const gaddr loc = mem::R64(src), fmt = mem::R64(src + 8);
  if (!loc || !fmt) return kParam;
  const std::uint32_t loc_type = mem::R32(loc);
  if (loc_type != kLocBufferQueue && loc_type != kLocAndroidSimpleBQ) {
    LogOnce("loc", std::format("data source locator type 0x{:x} is not supported", loc_type));
    return kContentUnsupported;
  }
  const std::uint32_t num_buffers = mem::R32(loc + 4);
  if (mem::R32(fmt) != kFmtPcm) {
    LogOnce("fmt", std::format("data format type {} is not PCM", mem::R32(fmt)));
    return kContentUnsupported;
  }
  // SLDataFormat_PCM {formatType, numChannels, samplesPerSec (milliHz), bitsPerSample, containerSize, channelMask, endianness}
  const std::uint32_t channels = mem::R32(fmt + 4), mhz = mem::R32(fmt + 8), bits = mem::R32(fmt + 12), container = mem::R32(fmt + 16);
  if ((bits != 8 && bits != 16 && bits != 32) || container != bits || channels < 1 || channels > 8 || mhz < 1000) {
    LogOnce("pcm", std::format("PCM format {} ch {} mHz {} bit/{} container is not supported", channels, mhz, bits, container));
    return kContentUnsupported;
  }
  // SLDataSink {pLocator, pFormat}: the output mix.
  const gaddr sloc = mem::R64(snk);
  if (!sloc || mem::R32(sloc) != kLocOutputMix) return kParam;
  const auto mix = FindObj(mem::R64(sloc + 8), Kind::Object);
  if (!mix || mix->type != ObjType::OutputMix) return kParam;

  auto o = NewObj(ObjType::AudioPlayer);
  auto p = std::make_shared<Player>();
  p->rate = mhz / 1000;
  p->channels = channels;
  p->bits = bits;
  p->frame_bytes = channels * (bits / 8);
  p->max_buffers = std::max<std::uint32_t>(1, num_buffers);
  p->bq_itf = o->itf[static_cast<int>(Kind::BufferQueue)];
  o->player = std::move(p);
  *out = o->itf[static_cast<int>(Kind::Object)];
  return kOk;
}

// ---------------------------------------------------------------------------------------------------------------
// SLPlayItf
std::uint32_t PlaySetPlayState(std::uint64_t self, std::uint32_t state) {
  bool start = false;
  {
    std::lock_guard lk(g_mu);
    auto o = FindObj(self, Kind::Play);
    if (!o || !o->player) return kParam;
    Player& p = *o->player;
    if (state < kPlayStopped || state > kPlayPlaying) return kParam;
    switch (state) {
      case kPlayPlaying:
        p.sink.SetPaused(g_paused);
        start = true;
        break;
      case kPlayPaused:
        p.sink.SetPaused(true);
        break;
      case kPlayStopped:  // back to the start: whatever was queued stays and replays from its first byte
        p.sink.SetPaused(true);
        p.sink.Clear();
        p.sunk = 0;
        p.played_bytes = 0;
        for (Buf& b : p.queue) b.submitted = false;
        break;
    }
    p.play_state = state;
  }
  if (start) EnsureThread();
  g_cv.notify_all();
  return kOk;
}

std::uint32_t PlayGetPlayState(std::uint64_t self, std::uint32_t* state) {
  std::lock_guard lk(g_mu);
  auto o = FindObj(self, Kind::Play);
  if (!o || !o->player || !state) return kParam;
  *state = o->player->play_state;
  return kOk;
}

std::uint32_t PlayGetDuration(std::uint64_t self, std::uint32_t* ms) {
  std::lock_guard lk(g_mu);
  if (!FindObj(self, Kind::Play) || !ms) return kParam;
  *ms = kTimeUnknown;
  return kOk;
}

std::uint32_t PlayGetPosition(std::uint64_t self, std::uint32_t* ms) {
  std::lock_guard lk(g_mu);
  auto o = FindObj(self, Kind::Play);
  if (!o || !o->player || !ms) return kParam;
  Player& p = *o->player;
  std::uint64_t bytes = p.played_bytes;
  if (!p.queue.empty() && p.queue.front().submitted) {
    const std::uint64_t consumed = p.sunk - std::min<std::uint64_t>(p.sink.Queued(), p.sunk);
    const std::uint64_t start = p.queue.front().end - p.queue.front().data.size();
    if (consumed > start) bytes += consumed - start;
  }
  *ms = static_cast<std::uint32_t>(bytes / p.frame_bytes * 1000 / p.rate);
  return kOk;
}

// ---------------------------------------------------------------------------------------------------------------
// SLBufferQueueItf
std::uint32_t BqEnqueue(std::uint64_t self, const void* data, std::uint32_t size) {
  {
    std::lock_guard lk(g_mu);
    auto o = FindObj(self, Kind::BufferQueue);
    if (!o || !o->player || !data || !size || size > kMaxEnqueueBytes) return kParam;
    Player& p = *o->player;
    if (o->state != kObjRealized || p.dead) return kPrecond;
    if (p.queue.size() >= p.max_buffers) return kBufferInsufficient;
    Buf b;
    b.data.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);  // the game reuses its buffer
    p.queue.push_back(std::move(b));
  }
  g_cv.notify_all();
  return kOk;
}

std::uint32_t BqClear(std::uint64_t self) {
  std::lock_guard lk(g_mu);
  auto o = FindObj(self, Kind::BufferQueue);
  if (!o || !o->player) return kParam;
  Player& p = *o->player;
  p.queue.clear();
  p.sink.Clear();
  p.sunk = 0;
  return kOk;
}

struct BqState {
  std::uint32_t count, play_index;
};
std::uint32_t BqGetState(std::uint64_t self, BqState* st) {
  std::lock_guard lk(g_mu);
  auto o = FindObj(self, Kind::BufferQueue);
  if (!o || !o->player || !st) return kParam;
  st->count = static_cast<std::uint32_t>(o->player->queue.size());
  st->play_index = static_cast<std::uint32_t>(o->player->completed);
  return kOk;
}

std::uint32_t BqRegisterCallback(std::uint64_t self, std::uint64_t cb, std::uint64_t ctx) {
  std::lock_guard lk(g_mu);
  auto o = FindObj(self, Kind::BufferQueue);
  if (!o || !o->player) return kParam;
  o->player->cb = cb;
  o->player->cb_ctx = ctx;
  return kOk;
}

// ---------------------------------------------------------------------------------------------------------------
// Everything the game does not call.
void Unsupported(Cpu& cpu) {
  LogOnce(std::string("unsup-") + cpu.current_import(), std::string(cpu.current_import()) + " is not supported");
  cpu.set_x(0, kFeatureUnsupported);
}

// slCreateEngine(SLObjectItf* pEngine, SLuint32 numOptions, const SLEngineOption*, SLuint32 numInterfaces,
//                const SLInterfaceID*, const SLboolean*)
std::uint32_t SlCreateEngine(std::uint64_t* out, std::uint32_t /*nopt*/, std::uint64_t /*opts*/, std::uint32_t n, std::uint64_t ids,
                             std::uint64_t req) {
  if (!out) return kParam;
  std::lock_guard lk(g_mu);
  std::uint32_t res = kOk;
  if (!CheckRequested(n, ids, req, {Iid::Object, Iid::Engine}, res)) return res;
  *out = NewObj(ObjType::Engine)->itf[static_cast<int>(Kind::Object)];
  return kOk;
}

struct SlotName {
  const char* name;
  gaddr stub;
};

// Builds a vtable of `n` slots; `impl[i]` is the stub for slot i or 0 for "unsupported".
gaddr BuildVtable(const char* itf, const std::vector<const char*>& names, const std::vector<gaddr>& impl) {
  const gaddr table = hle::DefineData(std::string("__nova_sl_vtbl_") + itf, names.size() * 8, 16);
  for (std::size_t i = 0; i < names.size(); ++i) {
    gaddr stub = i < impl.size() ? impl[i] : 0;
    if (!stub) {
      const std::string n = std::string("__nova_sl_") + itf + "_" + names[i];
      NOVA_HLE_RAW(n, Unsupported);
      stub = hle::StubFor(n);
    }
    mem::W64(table + 8 * i, stub);
  }
  return table;
}

#define SLFN(itf, name, fn) NOVA_HLE_THUNK("__nova_sl_" itf "_" name, fn)

void DefineIid(const char* sym, Iid id) {
  const gaddr var = hle::DefineData(sym, 8, 8);
  const gaddr guid = hle::DefineData(std::string("__nova_sl_guid_") + sym, 16, 16);
  for (const IidDef& d : kIids)
    if (d.id == id) std::memcpy(g2h(guid), &d.guid, 16);
  mem::W64(var, guid);  // `const SLInterfaceID SL_IID_X` is a pointer to the GUID
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
void RegisterHle() {
  GuestMemory::Init();
  NOVA_HLE_RAW("__nova_audio_thread", AudioThreadMain);

  g_vtbl[static_cast<int>(Kind::Object)] =
      BuildVtable("Object", {"Realize", "Resume", "GetState", "GetInterface", "RegisterCallback", "AbortAsyncOperation", "Destroy",
                             "SetPriority", "GetPriority", "SetLossOfControlInterfaces"},
                  {SLFN("Object", "Realize", ObjRealize), 0, SLFN("Object", "GetState", ObjGetState),
                   SLFN("Object", "GetInterface", ObjGetInterface), 0, 0, SLFN("Object", "Destroy", ObjDestroy)});
  g_vtbl[static_cast<int>(Kind::Engine)] = BuildVtable(
      "Engine",
      {"CreateLEDDevice", "CreateVibraDevice", "CreateAudioPlayer", "CreateAudioRecorder", "CreateMidiPlayer", "CreateListener",
       "Create3DGroup", "CreateOutputMix", "CreateMetadataExtractor", "CreateExtensionObject", "QueryNumSupportedInterfaces",
       "QuerySupportedInterfaces", "QueryNumSupportedExtensions", "QuerySupportedExtension", "IsExtensionSupported"},
      {0, 0, SLFN("Engine", "CreateAudioPlayer", EngineCreateAudioPlayer), 0, 0, 0, 0,
       SLFN("Engine", "CreateOutputMix", EngineCreateOutputMix)});
  g_vtbl[static_cast<int>(Kind::Play)] = BuildVtable(
      "Play",
      {"SetPlayState", "GetPlayState", "GetDuration", "GetPosition", "RegisterCallback", "SetCallbackEventsMask",
       "GetCallbackEventsMask", "SetMarkerPosition", "ClearMarkerPosition", "GetMarkerPosition", "SetPositionUpdatePeriod",
       "GetPositionUpdatePeriod"},
      {SLFN("Play", "SetPlayState", PlaySetPlayState), SLFN("Play", "GetPlayState", PlayGetPlayState),
       SLFN("Play", "GetDuration", PlayGetDuration), SLFN("Play", "GetPosition", PlayGetPosition)});
  g_vtbl[static_cast<int>(Kind::BufferQueue)] = BuildVtable(
      "BufferQueue", {"Enqueue", "Clear", "GetState", "RegisterCallback"},
      {SLFN("BufferQueue", "Enqueue", BqEnqueue), SLFN("BufferQueue", "Clear", BqClear),
       SLFN("BufferQueue", "GetState", BqGetState), SLFN("BufferQueue", "RegisterCallback", BqRegisterCallback)});

  NOVA_HLE("slCreateEngine", SlCreateEngine);
  DefineIid("SL_IID_ENGINE", Iid::Engine);
  DefineIid("SL_IID_PLAY", Iid::Play);
  DefineIid("SL_IID_BUFFERQUEUE", Iid::BufferQueue);
}

void SetMasterVolume(float volume) {
  std::lock_guard lk(g_mu);
  g_master = std::clamp(volume, 0.0f, 1.0f);
  for (auto& [id, o] : g_objects)
    if (o->player) o->player->sink.SetGain(g_master);
}

float MasterVolume() {
  std::lock_guard lk(g_mu);
  return g_master;
}

void SetPaused(bool paused) {
  {
    std::lock_guard lk(g_mu);
    g_paused = paused;
    for (auto& [id, o] : g_objects)
      if (o->player && o->player->play_state == kPlayPlaying) o->player->sink.SetPaused(paused);
  }
  g_cv.notify_all();
}

void Shutdown() {
  threads::ThreadId t;
  {
    std::lock_guard lk(g_thread_mu);
    t = g_thread;
    g_thread = 0;
  }
  g_stop = true;
  g_cv.notify_all();
  if (t) threads::Join(t);
  std::lock_guard lk(g_mu);
  for (auto& [id, o] : g_objects)
    if (o->player) {
      o->player->sink.SetPaused(true);
      o->player->sink.Close();
    }
}
}  // namespace nova::audio
