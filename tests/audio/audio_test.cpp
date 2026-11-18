// OpenSL ES test: an aarch64 guest blob creates an engine, an output mix and a buffer-queue player the way
// vox::DriverAndroid does, enqueues one short (silent) buffer and starts playback. The buffer-queue callback must
// fire exactly once, on the audio guest thread. Output goes to SDL's "dummy" driver at master volume 0: no sound.
#include <chrono>
#include <cstddef>
#include <format>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "audio/audio.h"
#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"
#include "guest_blob.h"

using namespace nova;

namespace {
int g_fail = 0;
void Check(const char* name, bool ok, const std::string& detail = {}) {
  if (!ok) ++g_fail;
  std::printf("%s %-34s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
}

struct State {  // mirrors guest.c
  std::uint64_t eng, mix, player, eng_itf, play, bq;
  std::uint32_t cb_count, cb_queue_count, cb_play_index;
  std::uint64_t cb_tpidr, main_tpidr;
  std::uint32_t r[24];
  std::uint8_t pcm[4096];
};
struct Imports {
  std::uint64_t sl_create_engine, iid_engine, iid_play, iid_bq;
};
}  // namespace

int main() {
  audio::SetDriver("dummy");
  audio::SetMasterVolume(0.0f);
  GuestMemory::Init();
  const std::size_t sz = sizeof(guest_blob::kBytes);
  GuestMemory::Map(guest_blob::kBase, sz, kProtRW, "audio-code");
  std::memcpy(g2h(guest_blob::kBase), guest_blob::kBytes, sz);
  GuestMemory::Protect(guest_blob::kBase, sz, kProtRX);
  constexpr gaddr kData = 0x4800'0000ull;
  GuestMemory::Map(kData, 1 << 16, kProtRW, "audio-data");

  gaddr run = 0, finish = 0;
  for (const auto& s : guest_blob::kSyms) {
    if (!std::strcmp(s.name, "run")) run = s.addr;
    if (!std::strcmp(s.name, "finish")) finish = s.addr;
  }

  audio::RegisterHle();
  const gaddr state_addr = kData, imports_addr = kData + 0x2000;
  Imports im{hle::StubFor("slCreateEngine"), hle::FindData("SL_IID_ENGINE"), hle::FindData("SL_IID_PLAY"),
             hle::FindData("SL_IID_BUFFERQUEUE")};
  Check("IID data symbols defined", im.iid_engine && im.iid_play && im.iid_bq);
  std::memcpy(g2h(imports_addr), &im, sizeof im);
  auto* st = g2h<State>(state_addr);

  call_guest<void>(run, state_addr, imports_addr);

  Check("slCreateEngine", st->r[0] == 0, std::to_string(st->r[0]));
  Check("engine Realize + GetInterface", st->r[1] == 0 && st->r[2] == 0 && st->eng_itf != 0);
  Check("CreateOutputMix + Realize", st->r[3] == 0 && st->r[4] == 0);
  Check("CreateAudioPlayer + Realize", st->r[5] == 0 && st->r[6] == 0, std::format("{} {}", st->r[5], st->r[6]));
  Check("player GetInterface PLAY, BUFFERQUEUE", st->r[7] == 0 && st->r[8] == 0 && st->play && st->bq);
  Check("RegisterCallback, Enqueue, GetState", st->r[9] == 0 && st->r[10] == 0 && st->r[11] == 0 && st->r[12] == 1,
        std::format("count {}", st->r[12]));
  Check("SetPlayState(PLAYING)", st->r[13] == 0);
  Check("unused slot -> FEATURE_UNSUPPORTED", st->r[14] == 12, std::to_string(st->r[14]));
  Check("unknown IID -> FEATURE_UNSUPPORTED", st->r[15] == 12, std::to_string(st->r[15]));

  const auto t0 = std::chrono::steady_clock::now();
  auto count = [&] { return mem::R32(state_addr + offsetof(State, cb_count)); };
  while (count() == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  Check("callback fired", count() == 1, std::format("{} after {} ms", count(), ms));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  Check("callback fired exactly once", count() == 1, std::to_string(count()));
  Check("queue empty and playIndex 1 in the callback", st->cb_queue_count == 0 && st->cb_play_index == 1,
        std::format("count {} index {}", st->cb_queue_count, st->cb_play_index));
  Check("callback ran on another guest thread", st->cb_tpidr != 0 && st->cb_tpidr != st->main_tpidr,
        std::format("tp 0x{:x} vs 0x{:x}", st->cb_tpidr, st->main_tpidr));

  call_guest<void>(finish, state_addr);
  Check("stop + destroy", st->r[16] == 0 && st->r[17] == 1);
  audio::Shutdown();
  std::printf("%s\n", g_fail ? "AUDIO TEST FAILED" : "AUDIO TEST OK");
  std::fflush(stdout);
  std::_Exit(g_fail ? 1 : 0);  // skip static destruction while SDL and guest threads wind down
}
