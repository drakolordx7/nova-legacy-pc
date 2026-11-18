// One playback output: an SDL3 audio stream on the default device, or (when no device can be opened) a wall-clock
// stand-in that drains at the real rate, so the game's buffer-queue callbacks keep firing on a machine without audio.
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>

struct SDL_AudioStream;

namespace nova::audio {
class Sink {
 public:
  Sink() = default;
  ~Sink() { Close(); }
  Sink(const Sink&) = delete;
  Sink& operator=(const Sink&) = delete;

  // PCM with bits 8 (unsigned) / 16 / 32 (signed little endian). The sink starts paused. False for other formats.
  bool Open(std::uint32_t rate, std::uint32_t channels, std::uint32_t bits);
  void Close();
  bool is_open() const { return open_; }
  bool is_real() const { return stream_ != nullptr; }

  void Put(const void* data, std::size_t bytes);
  // Bytes handed to Put and not yet taken by the device (source format).
  std::size_t Queued();
  void Clear();
  void SetPaused(bool paused);
  void SetGain(float gain);

 private:
  void Advance();  // fake clock only

  SDL_AudioStream* stream_ = nullptr;
  bool open_ = false;
  bool paused_ = true;
  std::uint32_t bytes_per_sec_ = 0;
  // Fake clock.
  double fake_queued_ = 0;
  std::chrono::steady_clock::time_point fake_last_{};
};

// Initialises the SDL audio subsystem once; false if it cannot (sinks then use the fake clock).
bool InitBackend();
void QuitBackend();
}  // namespace nova::audio
