#pragma once
#include "wqn_bitmap_asset.h"

// AI content and scroll markers, 12x12
// ESP32 e-paper: 1-bit, black=1, rows packed MSB first.
// Generated; edit SVG masters or build_tools/generate_assets.py instead.

// M01: ai_thinking_marker, 12x12, 24 bytes
#define M01_AI_THINKING_MARKER_12_WIDTH  12
#define M01_AI_THINKING_MARKER_12_HEIGHT 12
#define M01_AI_THINKING_MARKER_12_STRIDE 2
static const uint8_t m01_ai_thinking_marker_12[] = {
    0x06, 0x00, 0x3F, 0x00, 0x7D, 0x80, 0x60, 0xE0, 0x60, 0x60, 0xC0, 0x30,
    0xC0, 0x60, 0x7F, 0xE0, 0x3F, 0x80, 0x01, 0x80, 0x00, 0x20, 0x00, 0x20
};
static const WqnBitmapAsset m01_ai_thinking_marker_12_asset = {
    12, 12, 2, sizeof(m01_ai_thinking_marker_12), m01_ai_thinking_marker_12
};

// M02: ai_tool_running, 12x12, 24 bytes
#define M02_AI_TOOL_RUNNING_12_WIDTH  12
#define M02_AI_TOOL_RUNNING_12_HEIGHT 12
#define M02_AI_TOOL_RUNNING_12_STRIDE 2
static const uint8_t m02_ai_tool_running_12[] = {
    0x01, 0x00, 0x01, 0xF0, 0x01, 0xE0, 0x01, 0xF0, 0x03, 0xF0, 0x07, 0x80,
    0x0E, 0x00, 0x1C, 0x00, 0x78, 0x00, 0x78, 0x00, 0x70, 0x00, 0x00, 0x00
};
static const WqnBitmapAsset m02_ai_tool_running_12_asset = {
    12, 12, 2, sizeof(m02_ai_tool_running_12), m02_ai_tool_running_12
};

// M03: status_success, 12x12, 24 bytes
#define M03_STATUS_SUCCESS_12_WIDTH  12
#define M03_STATUS_SUCCESS_12_HEIGHT 12
#define M03_STATUS_SUCCESS_12_STRIDE 2
static const uint8_t m03_status_success_12[] = {
    0x00, 0x00, 0x1F, 0x80, 0x3F, 0xC0, 0x70, 0xE0, 0x61, 0xE0, 0x73, 0xE0,
    0x7F, 0x60, 0x7E, 0x60, 0x7E, 0xE0, 0x3F, 0xC0, 0x1F, 0x80, 0x00, 0x00
};
static const WqnBitmapAsset m03_status_success_12_asset = {
    12, 12, 2, sizeof(m03_status_success_12), m03_status_success_12
};

// M04: status_error, 12x12, 24 bytes
#define M04_STATUS_ERROR_12_WIDTH  12
#define M04_STATUS_ERROR_12_HEIGHT 12
#define M04_STATUS_ERROR_12_STRIDE 2
static const uint8_t m04_status_error_12[] = {
    0x00, 0x00, 0x1F, 0x80, 0x3F, 0xC0, 0x79, 0xE0, 0x7F, 0xE0, 0x6F, 0x60,
    0x6F, 0x60, 0x7F, 0xE0, 0x79, 0xE0, 0x3F, 0xC0, 0x1F, 0x80, 0x00, 0x00
};
static const WqnBitmapAsset m04_status_error_12_asset = {
    12, 12, 2, sizeof(m04_status_error_12), m04_status_error_12
};

// M05: chevron_right, 12x12, 24 bytes
#define M05_CHEVRON_RIGHT_12_WIDTH  12
#define M05_CHEVRON_RIGHT_12_HEIGHT 12
#define M05_CHEVRON_RIGHT_12_STRIDE 2
static const uint8_t m05_chevron_right_12[] = {
    0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x1C, 0x00, 0x1F, 0x00, 0x1F, 0x80,
    0x1F, 0x80, 0x1F, 0x00, 0x1C, 0x00, 0x18, 0x00, 0x10, 0x00, 0x00, 0x00
};
static const WqnBitmapAsset m05_chevron_right_12_asset = {
    12, 12, 2, sizeof(m05_chevron_right_12), m05_chevron_right_12
};

// M06: chevron_down, 12x12, 24 bytes
#define M06_CHEVRON_DOWN_12_WIDTH  12
#define M06_CHEVRON_DOWN_12_HEIGHT 12
#define M06_CHEVRON_DOWN_12_STRIDE 2
static const uint8_t m06_chevron_down_12[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xE0, 0x3F, 0xC0, 0x1F, 0x80,
    0x0F, 0x00, 0x0F, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const WqnBitmapAsset m06_chevron_down_12_asset = {
    12, 12, 2, sizeof(m06_chevron_down_12), m06_chevron_down_12
};

// M07: chevron_up, 12x12, 24 bytes
#define M07_CHEVRON_UP_12_WIDTH  12
#define M07_CHEVRON_UP_12_HEIGHT 12
#define M07_CHEVRON_UP_12_STRIDE 2
static const uint8_t m07_chevron_up_12[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x0F, 0x00, 0x0F, 0x00,
    0x1F, 0x80, 0x3F, 0xC0, 0x3F, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const WqnBitmapAsset m07_chevron_up_12_asset = {
    12, 12, 2, sizeof(m07_chevron_up_12), m07_chevron_up_12
};
