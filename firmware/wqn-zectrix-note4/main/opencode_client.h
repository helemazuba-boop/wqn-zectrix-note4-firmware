#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio_capture.h"
#include "esp_err.h"

namespace wqn {

struct OpenCodeSessionInfo {
    std::string id;
    std::string title;
    int64_t updated_at = 0;
};

struct OpenCodeResult {
    int http_status = 0;
    std::string error_code;
    std::string detail;
};

enum class OpenCodeEventKind : uint8_t {
    kAccepted,
    kStatus,
    kTextDelta,
    kText,
    kTool,
    kPermission,
    kError,
};

struct OpenCodeEvent {
    OpenCodeEventKind kind = OpenCodeEventKind::kStatus;
    std::string status;
    std::string text;
    std::string tool;
    std::string preview;
    std::string permission_id;
};

using OpenCodeEventCallback = void (*)(const OpenCodeEvent& event, void* ctx);

esp_err_t ListOpenCodeSessions(
    const std::string& token,
    std::vector<OpenCodeSessionInfo>* sessions,
    OpenCodeResult* result);
esp_err_t TranscribeOpenCodeAudio(
    const std::string& token,
    const AudioCaptureChunk& audio,
    std::string* transcript,
    OpenCodeResult* result);
esp_err_t RunOpenCodePrompt(
    const std::string& token,
    const std::string& session_id,
    const std::string& prompt,
    OpenCodeEventCallback callback,
    void* callback_ctx,
    OpenCodeResult* result);

}  // namespace wqn
