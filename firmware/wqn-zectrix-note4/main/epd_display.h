#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace wqn {

constexpr int kEpdWidth = 400;
constexpr int kEpdHeight = 300;
constexpr int kEpdBytesPerRow = kEpdWidth / 8;
constexpr int kEpdFramebufferSize = kEpdBytesPerRow * kEpdHeight;

// Initializes the Note4 4.2" EPD backend and allocates the 1bpp framebuffer.
esp_err_t InitEpdDisplay();

// Framebuffer format is row-major 1bpp, MSB first, where 1 is white and 0 is black.
uint8_t* GetEpdFramebuffer();
const uint8_t* GetEpdFramebufferConst();
size_t GetEpdFramebufferSize();

void ClearEpdFramebuffer(bool white = true);
void DrawEpdPixel(int x, int y, bool black);

esp_err_t DrawUtf8Text(int x, int y, const char* text, bool black = true);
int MeasureUtf8TextWidth(const char* text);
std::string TruncateUtf8TextToWidth(const std::string& text, int max_width_px);
std::vector<std::string> WrapUtf8TextToWidth(const std::string& text, int max_width_px, size_t max_lines);

// Sends the framebuffer to the panel and performs a blocking refresh. Local
// partial-window refresh can be disabled for page-level commits that are too
// broad for the panel's hot partial path.
esp_err_t RefreshEpdFull(bool allow_local_partial = true, bool force_full_refresh = false);

// Powers down only the EPD rail on GPIO6. Board-level GPIO17 is intentionally untouched.
void PowerOffEpd();

// Returns true if the EPD is currently being initialized or is mid-refresh.
// Used by the power manager to prevent cutting EPD power during active SPI traffic.
bool IsEpdBusy();

}  // namespace wqn
