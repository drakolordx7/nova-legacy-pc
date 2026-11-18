#include "audio/sink.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>

#include "audio/audio.h"
#include "core/log.h"

namespace nova::audio {
namespace {
std::mutex g_backend_mu;
bool g_backend_tried = false, g_backend_ok = false;
std::string g_driver;
}  // namespace

void SetDriver(const char* name) {
  std::lock_guard lk(g_backend_mu);
  g_driver = name ? name : "";
}

bool InitBackend() {
  std::lock_guard lk(g_backend_mu);
  if (g_backend_tried) return g_backend_ok;
  g_backend_tried = true;
  std::string drv = g_driver;
  if (drv.empty())
    if (const char* e = std::getenv("NOVA_AUDIO_DRIVER")) drv = e;
  if (!drv.empty()) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, drv.c_str());
  g_backend_ok = SDL_InitSubSystem(SDL_INIT_AUDIO);
  if (!g_backend_ok) {
    log::Warn("audio: SDL audio init failed ({}); output is silent but timed", SDL_GetError());
  } else {
    const char* cur = SDL_GetCurrentAudioDriver();
    log::Info("audio: SDL driver '{}'", cur ? cur : "?");
  }
  return g_backend_ok;
}

void QuitBackend() {
  std::lock_guard lk(g_backend_mu);
  if (g_backend_ok) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  g_backend_tried = g_backend_ok = false;
}

bool Sink::Open(std::uint32_t rate, std::uint32_t channels, std::uint32_t bits) {
  Close();
  SDL_AudioFormat fmt;
  switch (bits) {
    case 8: fmt = SDL_AUDIO_U8; break;
    case 16: fmt = SDL_AUDIO_S16LE; break;
    case 32: fmt = SDL_AUDIO_S32LE; break;
    default: return false;
  }
  if (rate == 0 || channels == 0 || channels > 8) return false;
  bytes_per_sec_ = rate * channels * (bits / 8);
  if (InitBackend()) {
    const SDL_AudioSpec spec{fmt, static_cast<int>(channels), static_cast<int>(rate)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) log::Warn("audio: cannot open an output stream ({}); using a silent timed output", SDL_GetError());
    else SDL_PauseAudioStreamDevice(stream_);
  }
  open_ = true;
  paused_ = true;
  fake_queued_ = 0;
  fake_last_ = std::chrono::steady_clock::now();
  return true;
}

void Sink::Close() {
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
  open_ = false;
}

void Sink::Advance() {
  const auto now = std::chrono::steady_clock::now();
  if (!paused_) {
    const double dt = std::chrono::duration<double>(now - fake_last_).count();
    fake_queued_ = std::max(0.0, fake_queued_ - dt * bytes_per_sec_);
  }
  fake_last_ = now;
}

void Sink::Put(const void* data, std::size_t bytes) {
  if (!bytes) return;
  if (stream_) {
    if (!SDL_PutAudioStreamData(stream_, data, static_cast<int>(bytes))) log::Warn("audio: put failed: {}", SDL_GetError());
  } else {
    Advance();
    fake_queued_ += static_cast<double>(bytes);
  }
}

std::size_t Sink::Queued() {
  if (stream_) {
    const int q = SDL_GetAudioStreamQueued(stream_);
    return q < 0 ? 0 : static_cast<std::size_t>(q);
  }
  Advance();
  return static_cast<std::size_t>(fake_queued_);
}

void Sink::Clear() {
  if (stream_) SDL_ClearAudioStream(stream_);
  else fake_queued_ = 0;
}

void Sink::SetPaused(bool paused) {
  if (paused == paused_) return;
  if (stream_) {
    if (paused) SDL_PauseAudioStreamDevice(stream_);
    else SDL_ResumeAudioStreamDevice(stream_);
  } else {
    Advance();  // settle the clock before the state flips
  }
  paused_ = paused;
}

void Sink::SetGain(float gain) {
  if (stream_) SDL_SetAudioStreamGain(stream_, std::max(0.0f, gain));
}
}  // namespace nova::audio
