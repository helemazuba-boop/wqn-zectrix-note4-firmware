// Device UI typography tokens - single source of truth for text metrics.
// Replaces scattered 18 / 9 / 22 line-height magic numbers and duplicate width
// constants. See ESP32DOC/wqn-cloud-relay/docs/13-ui-design-language.md §13.4.1 / §13.4.4.
//
// Pure constants only, no includes required.

#pragma once

namespace device_ui_internal {

// ---- Font metrics (fixed: single 16px CJK + 5x7 ASCII) ----
constexpr int kCjkFontHeight = 16;
constexpr int kCjkLineHeight = 18;              // single source for the 18px line height (was duplicated in 4 places)
constexpr int kAsciiCellWidth = 6;

// ---- Text layout ----
constexpr int kTextWidth = 388;                 // = kEpdWidth - 12; was duplicated in text_wrappers.cpp + ui_render.cpp
constexpr int kMaxWrapLines = 4;                // was duplicated in page_home.cpp + ui_render.cpp
constexpr char kEllipsis[] = "...";             // unified overflow signal

}  // namespace device_ui_internal
