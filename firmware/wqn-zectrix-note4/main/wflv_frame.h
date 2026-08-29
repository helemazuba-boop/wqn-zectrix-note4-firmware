#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wqn {

constexpr uint32_t kWflvMagic = 0x57464C56; // 'W','F','L','V'
constexpr uint16_t kWflvVersion = 2;
constexpr size_t kWflvHeaderBytes = 24;

constexpr uint16_t kWflvFlagStream = 0x0001;
constexpr uint16_t kWflvFlagFinal  = 0x0002;

inline void WriteLE16(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void WriteLE32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline void EncodeWflvHeader(uint8_t* dst, uint16_t flags, uint32_t seq, uint32_t sample_rate, uint32_t channels) {
    dst[0] = 'W';
    dst[1] = 'F';
    dst[2] = 'L';
    dst[3] = 'V';
    WriteLE16(dst + 4, kWflvVersion);
    WriteLE16(dst + 6, flags);
    WriteLE32(dst + 8, seq);
    WriteLE32(dst + 12, sample_rate);
    WriteLE32(dst + 16, channels);
    WriteLE32(dst + 20, 0); // reserved
}

} // namespace wqn
