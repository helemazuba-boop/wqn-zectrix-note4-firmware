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
// True only when the RAM previous-frame mirror represents the physical panel.
// Deep sleep/cold initialization clears this state; region-only drawing must
// fall back to a complete frame until a successful refresh synchronizes it.
bool IsEpdFramebufferSynchronized();

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

// Full-screen 4bpp WQNI image refresh using the SSD2683 vendor-calibrated
// sixteen-level waveform. This is a full refresh only; callers must provide
// exactly kEpdGray4PayloadSize bytes (two pixels per source byte).
constexpr size_t kEpdGray4RowBytes = kEpdWidth / 2;
constexpr size_t kEpdGray4PayloadSize = kEpdGray4RowBytes * kEpdHeight;
esp_err_t RefreshEpdGray16(const uint8_t* gray4, size_t size);

esp_err_t PrepareDisplayForSleep(int64_t deadline_us);

// [power-fix] User-initiated power-off: white-clear + forced full refresh
// (owner task), then rail power-off. Same Pending/Claimed channel and
// deadline semantics as PrepareDisplayForSleep; a refresh failure still cuts
// the rail. Called by the PowerCoordinator before the final latch cut.
esp_err_t PrepareDisplayForShutdown(int64_t deadline_us);
void RollbackDisplayAfterSleepAbort();
// [epd-owner] The EPD refresh task registers itself as the panel owner at
// startup. PrepareDisplayForSleep then runs the power-off ON THAT TASK: when a
// different task (the power coordinator) calls it, the request is posted to a
// Pending/Claimed state machine and the owner services it from its command
// point; when the owner itself calls (defensive: e.g. an emergency raised on
// the EPD task), it executes locally. Pass nullptr to clear (task deleted).
void RegisterEpdOwnerTask(void* owner_task_handle);
// [epd-owner] Called by the EPD owner task at its idle command point. Claims a
// Pending sleep-prep request (Pending->Claimed CAS), runs the power-off, and
// publishes the generation-tagged result. Returns true iff a request was
// claimed this call (the caller then skips idle maintenance this round so a
// 1-3 s cleanup cannot make the power side miss its deadline). Must run only
// outside an EpdFrameTransaction (the idle command point guarantees this).
bool ServiceDisplaySleepPrepCommand();
void NoteEpdActivity();
// [epd-owner] Monotonic counter bumped by NoteEpdActivity. The idle-maintenance
// command samples it before yielding to the EPD task and the EPD task re-checks
// it just before running maintenance: a change means activity happened in
// between, so the (1-3 s) cleanup full refresh is skipped rather than run on
// top of a fresh frame. Acquire-load pairs with the release bump.
uint32_t GetEpdActivityGeneration();
// [epd-owner] True when idle maintenance is actually due: the idle-off feature
// is enabled, activity exists, the idle deadline has elapsed, and the rail has
// NOT already been cut. RequestEpdIdleMaintenance gates on this so an idle UI
// poll does not wake the EPD task every cycle (edge-trigger the request only
// when it flips to due).
bool IsEpdIdleMaintenanceDue();
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
