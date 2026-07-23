#pragma once

#include <cstddef>
#include <cstdint>

// Row-major 1bpp mask, MSB first. A set bit means "paint this pixel";
// the caller supplies black/paper so the same asset also supports reverse-fill
// selection states on the EPD framebuffer.
struct WqnBitmapAsset {
    uint8_t width;
    uint8_t height;
    uint8_t stride;
    uint16_t byte_count;
    const uint8_t* data;
};

inline bool WqnBitmapPixel(const WqnBitmapAsset& asset, uint8_t x, uint8_t y)
{
    if (x >= asset.width || y >= asset.height) {
        return false;
    }
    const size_t offset = static_cast<size_t>(y) * asset.stride + (x >> 3);
    return (asset.data[offset] & (0x80U >> (x & 7U))) != 0;
}
