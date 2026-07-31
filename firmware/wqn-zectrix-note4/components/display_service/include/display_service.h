#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

#include "font_fmt.h"  // lv_font_t for DrawTextWithFont

namespace wqn {

constexpr int kEpdWidth = 400;
constexpr int kEpdHeight = 300;
constexpr int kEpdBytesPerRow = kEpdWidth / 8;
constexpr int kEpdFramebufferSize = kEpdBytesPerRow * kEpdHeight;

// Initializes the Note4 4.2" EPD backend and allocates the 1bpp framebuffer.
esp_err_t InitEpdDisplay();

// Rendering executes on DisplayService's task. The framebuffer remains
// private to the service; clients receive drawing operations, not a pointer.
void ClearEpdFramebuffer(bool white = true);
void DrawEpdPixel(int x, int y, bool black);
// Copies a full pre-rendered 1bpp frame (kEpdFramebufferSize bytes, same
// row-major/MSB-first/1=white layout as the framebuffer) in one memcpy.
// Server-rendered note images (WQNI payloads) display through this.
void BlitEpdFramebuffer(const uint8_t* bitmap, size_t size);

esp_err_t DrawUtf8Text(int x, int y, const char* text, bool black = true);
int MeasureUtf8TextWidth(const char* text);

// Draw / measure text with an arbitrary LVGL-format font (e.g. font_zectrix_48_1).
// Iterates UTF-8 codepoints; missing glyphs are skipped (zero width, nothing drawn).
void DrawTextWithFont(int x, int y, const lv_font_t* font, const char* text, bool black = true);
int MeasureTextWithFont(const lv_font_t* font, const char* text);
void DrawTextWithFontCentered(int x, int y, int width, const lv_font_t* font, const char* text, bool black = true);
std::string TruncateUtf8TextToWidth(const std::string& text, int max_width_px);
std::vector<std::string> WrapUtf8TextToWidth(const std::string& text, int max_width_px, size_t max_lines);

// Sends the framebuffer to the panel and performs a blocking refresh. Local
// partial-window refresh can be disabled for page-level commits that are too
// broad for the panel's hot partial path.
esp_err_t RefreshEpdFull(bool allow_local_partial = true, bool force_full_refresh = false);

esp_err_t PrepareDisplayForSleep(int64_t deadline_us);
void RollbackDisplayAfterSleepAbort();
void NoteEpdActivity();
void PowerOffEpdAfterIdleIfNeeded();

// [epd-owner] RAII lock over the WHOLE clear->draw->refresh sequence of one
// frame. RefreshEpdFull() only serializes the transmit; a caller that clears
// and draws the framebuffer across several service calls (RenderFrameToEpd)
// must hold this for the entire sequence so idle cleanup or another render
// cannot interleave on the shared framebuffer. Recursive: the inner
// RefreshEpdFull re-enters the same mutex on this task harmlessly. Construct
// on the stack; the destructor releases on every early return.
class EpdFrameTransaction {
public:
    EpdFrameTransaction();
    ~EpdFrameTransaction();
    EpdFrameTransaction(const EpdFrameTransaction&) = delete;
    EpdFrameTransaction& operator=(const EpdFrameTransaction&) = delete;
    // True only when the mutex existed AND was acquired. Callers MUST check
    // this before drawing: the mutex handle can be null on allocation failure.
    bool locked() const { return locked_; }

private:
    // Opaque SemaphoreHandle_t captured at construction (void* to keep FreeRTOS
    // types out of this public header); released by the destructor iff locked.
    void* mutex_ = nullptr;
    bool locked_ = false;
};

}  // namespace wqn
