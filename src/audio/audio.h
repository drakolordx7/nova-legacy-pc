// OpenSL ES for the guest (libOpenSLES imports) on top of SDL3 audio streams.
// The game (vox::DriverAndroid::_InitOSL) uses exactly: slCreateEngine, SLObject {Realize, GetState, GetInterface,
// Destroy}, SLEngine {CreateAudioPlayer, CreateOutputMix}, SLPlay {SetPlayState}, SLBufferQueue {Enqueue, GetState,
// RegisterCallback} with one 16-bit stereo 44.1 kHz PCM buffer-queue player. Everything else in the OpenSL vtables
// answers SL_RESULT_FEATURE_UNSUPPORTED and is logged once.
#pragma once

namespace nova::audio {
// Registers slCreateEngine and defines the SL_IID_ENGINE / SL_IID_PLAY / SL_IID_BUFFERQUEUE data imports. Call before
// loading the module (the loader resolves data imports at load time).
void RegisterHle();

// Master gain applied to every player (0..1, clamped). Default 1.
void SetMasterVolume(float volume);
float MasterVolume();
// Pauses / resumes every player's output (their SetPlayState values are untouched). Callbacks stop while paused.
void SetPaused(bool paused);

// Stops the audio thread and closes all outputs. Safe to call twice; later playback restarts the thread.
void Shutdown();

// Test / diagnostics: forces the SDL audio driver ("dummy" is silent). Must precede the first player. Also read from
// the NOVA_AUDIO_DRIVER environment variable.
void SetDriver(const char* name);
}  // namespace nova::audio
