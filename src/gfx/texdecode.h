// Software decoders for compressed textures -> RGBA8. Used when ANGLE does not support the format (PVRTC, ATC) or as
// a fallback for ETC1.
// Ported from Zombiewood's hle/gles/texdecode (MIT, (c) 2026 ChrisDutary).
#pragma once
#include <cstdint>
#include <vector>

namespace nova::gfx::texdecode {

// GL formats (gl2ext.h) we know how to decode.
constexpr std::uint32_t kETC1_RGB8 = 0x8D64;                 // GL_ETC1_RGB8_OES
constexpr std::uint32_t kPVRTC_RGB_4BPP = 0x8C00;            // GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG
constexpr std::uint32_t kPVRTC_RGB_2BPP = 0x8C01;
constexpr std::uint32_t kPVRTC_RGBA_4BPP = 0x8C02;
constexpr std::uint32_t kPVRTC_RGBA_2BPP = 0x8C03;
constexpr std::uint32_t kATC_RGB = 0x8C92;                   // GL_ATC_RGB_AMD
constexpr std::uint32_t kATC_RGBA_EXPLICIT = 0x8C93;         // GL_ATC_RGBA_EXPLICIT_ALPHA_AMD
constexpr std::uint32_t kATC_RGBA_INTERPOLATED = 0x87EE;     // GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD

bool IsPVRTC(std::uint32_t fmt);
bool IsATC(std::uint32_t fmt);
bool CanDecode(std::uint32_t fmt);  // ETC1, PVRTC or ATC

// Size in bytes of the compressed data of a w x h image (0 if the format is unknown).
std::uint32_t CompressedSize(std::uint32_t fmt, int w, int h);

// Decodes `src` (size bytes) to RGBA8 (w*h*4 bytes, row 0 = first row of the data, as GL expects). Returns false if
// the format is unknown or the data is too short.
bool Decode(std::uint32_t fmt, const std::uint8_t* src, std::uint32_t size, int w, int h,
            std::vector<std::uint8_t>& out_rgba);

// A single block, exposed for tests.
void DecodeETC1Block(const std::uint8_t block[8], std::uint8_t out_rgba[4 * 4 * 4]);  // row major, 4x4

}  // namespace nova::gfx::texdecode
