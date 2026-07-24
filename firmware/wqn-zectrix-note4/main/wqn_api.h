#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "device_protocol/v3.h"
#include "device_protocol/note_study.h"
#include "device_protocol/word_study.h"

namespace wqn {

struct WqnAssetManifestItem {
    std::string role;
    std::string kind;
    std::string mime_type;
    std::string url;
    std::string sha256;
    int width = 0;
    int height = 0;
    int bytes = 0;
};

struct WqnProblem {
    std::string id;
    std::string title;
    std::string problem_type;
    std::string status;
    std::string subject_id;
    std::string subject_name;
    std::string updated_at;
    std::string next_review_at;
    std::string content_text;
    std::string solution_text;
    int asset_count = 0;
    int solution_asset_count = 0;
    std::vector<WqnAssetManifestItem> assets;
    std::vector<WqnAssetManifestItem> solution_assets;
};

struct WqnProblemIndexRequest {
    std::string cursor;
    std::string status;
    std::string subject_id;
    int limit = 50;
};

struct WqnProblemIndexPage {
    std::vector<WqnProblem> problems;
    std::string next_cursor;
    bool has_more = false;
    int total = 0;
};

struct WqnReviewResult {
    std::string problem_id;
    std::string selected_status;
    std::string reviewed_at;
    int duration_ms = 0;
};

struct WqnTodoItem {
    std::string id;
    std::string title;
    std::string status;
    std::string priority;
    std::string due_at;
    std::string reminder_at;
    std::string subject_name;
    std::string updated_at;
    std::string completed_at;
};

struct WqnTodoListPage {
    std::vector<WqnTodoItem> todos;
    std::string previous_cursor;
    std::string next_cursor;
    bool has_earlier = false;
    bool has_later = false;
    bool has_more = false;
    int total = 0;
    std::string server_time;
    std::string selected_todo_id;
    int selected_index = -1;
};

struct WqnTodoTimelineRequest {
    std::string cursor;
    int limit = 24;
};

struct WqnWordEntry {
    std::string id;
    std::string deck_id;
    std::string word;
    std::string normalized_word;
    std::string phonetic;
    std::string meaning;
    std::string example;
    std::string example_translation;
    std::string part_of_speech;
    std::string status;
    std::string due_at;
    bool deleted = false;
    int revision = 0;
};

struct WqnWordSearchRequest {
    std::string query;
    std::string prefix;
    int limit = 8;
};

struct WqnWordSearchResult {
    std::string prefix;
    std::vector<WqnWordEntry> words;
    std::vector<std::string> next_letters;
};

struct WqnWordPackManifestItem {
    std::string pack_id;
    std::string deck_id;
    std::string title;
    uint64_t revision = 0;
    uint64_t content_revision = 0;
    uint64_t pack_revision = 0;
    uint64_t change_sequence = 0;
    uint32_t schema_version = 0;
    std::string format;
    std::string compression;
    std::string sha256;
    std::string download_url;
    uint32_t entry_count = 0;
    uint32_t byte_size = 0;
    bool deleted = false;
};

struct WqnWordPackManifest {
    std::string server_time;
    uint64_t cursor = 0;
    bool has_more = false;
    std::vector<WqnWordPackManifestItem> packs;
};

// One notebook row of the note-study manifest. A pack is exactly one notebook's
// note set, so pack_id equals notebook_id. `has_pack` is false when the server
// returned a null pack (a notebook with no note content yet); `deleted` only
// appears in manifest deltas and is resolved away when the aggregate is merged.
struct WqnNotePackManifestNotebook {
    std::string notebook_id;
    std::string title;
    uint64_t change_sequence = 0;
    uint64_t content_revision = 0;
    bool deleted = false;
    bool has_pack = false;
    std::string pack_id;
    uint64_t pack_revision = 0;
    uint32_t schema_version = 0;
    uint32_t entry_count = 0;
    uint32_t byte_size = 0;
    std::string sha256;
    std::string download_url;
};

struct WqnNotePackManifest {
    uint64_t cursor = 0;
    bool has_more = false;
    std::vector<WqnNotePackManifestNotebook> notebooks;
};

struct WqnWordAiLookupRequest {
    std::string query;
    std::string prefix;
};

struct WqnWordAiLookupResult {
    WqnWordEntry word;
    std::string reply_text;
};

struct WqnAiAction {
    std::string type;
    std::string notebook_id;
    std::string note_id;
    std::string todo_id;
    std::string word_id;
    std::string deck_id;
    std::string word;
    std::string problem_set_id;
    std::string problem_id;
    std::string title;
    std::string status;
    std::string outcome;
    std::string due_at;
    std::string reminder_at;
};

struct WqnAiStatusTraceItem {
    std::string stage;
    std::string status;
    std::string detail;
    int elapsed_ms = 0;
};

struct WqnAiAsrSummary {
    std::string provider;
    std::string model;
    std::string status;
    std::string text;
    std::string request_id;
    int elapsed_ms = 0;
};

struct WqnAiFunctionCallSummary {
    std::string name;
    std::string status;
    std::string display;
    std::string action_type;
    std::string title;
};

struct WqnAiChatResponse {
    std::string transcript;
    std::string reply_text;
    std::string conversation_id;
    std::string error_code;
    std::string error_message;
    int latency_ms = 0;
    std::vector<WqnAiAction> actions;
    std::vector<WqnAiStatusTraceItem> status_trace;
    WqnAiAsrSummary asr;
    std::vector<WqnAiFunctionCallSummary> function_calls;
};

esp_err_t RunPairingFlowIfNeeded();
esp_err_t StartDeviceClaimV3(
    const protocol::v3::RequestMetadata& metadata,
    const std::string& hardware_id,
    const std::string& device_public_key,
    protocol::v3::ClaimStartData* data,
    protocol::v3::Error* error);
esp_err_t PollDeviceClaimV3(
    const protocol::v3::RequestMetadata& metadata,
    const std::string& claim_id,
    protocol::v3::ClaimPollData* data,
    protocol::v3::Error* error);
esp_err_t BootstrapDeviceControlV3(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    protocol::v3::BootstrapData* data,
    protocol::v3::Error* error);
esp_err_t SyncDeviceControlV3(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    protocol::v3::SyncData* data,
    protocol::v3::Error* error);
esp_err_t ProbeSyncAndClearTokenOnUnauthorized(const std::string& token);
esp_err_t SyncDueProblemIds(const std::string& token, std::vector<std::string>* due_problem_ids, int* total);
esp_err_t FetchProblems(const std::string& token, const std::vector<std::string>& problem_ids, std::vector<WqnProblem>* problems);
esp_err_t FetchProblemIndex(const std::string& token, const WqnProblemIndexRequest& request, WqnProblemIndexPage* page);
esp_err_t UploadReviewComplete(const std::string& token, const std::vector<WqnReviewResult>& results);
esp_err_t FetchTodoTimeline(const std::string& token, const WqnTodoTimelineRequest& request, WqnTodoListPage* page);
esp_err_t FetchTodoTimeline(const std::string& token, WqnTodoListPage* page);
esp_err_t FetchTodayPendingTodos(const std::string& token, WqnTodoListPage* page);
esp_err_t CompleteTodo(const std::string& token, const std::string& todo_id, WqnTodoItem* todo);
esp_err_t SearchWords(const std::string& token, const WqnWordSearchRequest& request, WqnWordSearchResult* result);
esp_err_t FetchWordPackManifest(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    uint64_t cursor,
    WqnWordPackManifest* manifest);
esp_err_t CreateWordStudySessionV1(
    const std::string& token,
    const protocol::word_study_v1::CreateSessionRequest& request,
    protocol::word_study_v1::SessionData* session,
    protocol::v3::Error* error);
esp_err_t FetchWordStudyCandidatePageV1(
    const std::string& token,
    const std::string& session_id,
    const protocol::word_study_v1::CandidatePageRequest& request,
    protocol::word_study_v1::CandidatePageData* page,
    protocol::v3::Error* error);
esp_err_t SubmitWordStudyObservationV1(
    const std::string& token,
    const protocol::word_study_v1::ObservationRequest& request,
    protocol::word_study_v1::ObservationData* observation,
    protocol::v3::Error* error,
    bool* transport_failure);
esp_err_t SkipWordStudyObservationV1(
    const std::string& token,
    const protocol::word_study_v1::ObservationRequest& request,
    protocol::word_study_v1::ObservationData* observation,
    protocol::v3::Error* error,
    bool* transport_failure);
using WqnHttpChunkSink = esp_err_t (*)(
    void* context,
    const uint8_t* bytes,
    size_t size);
esp_err_t DownloadWordPackStream(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    const WqnWordPackManifestItem& item,
    WqnHttpChunkSink sink,
    void* context);
// Fetches one page of the note-study manifest starting at `cursor`. Reuses the
// note-study-v1 protocol builder/parser and flattens the result into the
// storage-facing WqnNotePackManifest.
esp_err_t FetchNoteStudyManifest(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    uint64_t cursor,
    WqnNotePackManifest* manifest);
// Streams one notebook's note pack (application/x-ndjson) into `sink`. Bounded by
// the manifest byte_size and the note-study-v1 pack cap.
esp_err_t DownloadNotePackStream(
    const std::string& token,
    const protocol::v3::RequestMetadata& metadata,
    const WqnNotePackManifestNotebook& notebook,
    WqnHttpChunkSink sink,
    void* context);
esp_err_t CreateNoteStudySessionV1(
    const std::string& token,
    const protocol::note_study_v1::CreateSessionRequest& request,
    protocol::note_study_v1::SessionData* session,
    protocol::v3::Error* error);
esp_err_t FetchNoteStudyCandidatePageV1(
    const std::string& token,
    const std::string& session_id,
    const protocol::note_study_v1::CandidatePageRequest& request,
    protocol::note_study_v1::CandidatePageData* page,
    protocol::v3::Error* error);
esp_err_t LookupWordWithAi(const std::string& token, const WqnWordAiLookupRequest& request, WqnWordAiLookupResult* result);
esp_err_t SyncDueProblemsAndLog(const std::string& token);

// === v2 SSE streaming (Std/Pro tier, default path when WQN_AI_STREAMING_ENABLE=y) ===
//
// StreamParser callbacks are invoked from the audio-streaming task. Callbacks
// MUST return quickly (no blocking, no nested esp_http_client calls) and the
// same WqnSseEvent value must remain valid until the next callback returns.
//
struct WqnAiSseEvent {
    enum class Kind {
        kUnknown,
        kReady,
        kStage,
        kAsrDelta,
        kAsrComplete,
        kAsrFailed,
        kThinkingStart,
        kThinkingDelta,
        kThinkingDone,
        kTextStart,
        kTextDelta,
        kTextEnd,
        kToolStart,
        kToolResult,
        kToolError,
        kState,
        kTurnDone,
        kError,
        kFinal,
    };
    Kind kind = Kind::kUnknown;
    uint64_t event_id = 0;
    std::string raw_json;

    // Convenience fields parsed out of the SSE frame.
    std::string delta;
    std::string text;
    std::string full_text;
    std::string sentence_id;
    std::string tool_call_id;
    std::string tool_name;
    std::string tool_display;
    bool tool_ok = false;
    int tool_items_count = 0;
    int tool_elapsed_ms = 0;
    std::string stage;
    int elapsed_ms = 0;
    int text_chars = 0;
    int turn_id_offset = 0;
    std::string error_code;
    std::string error_message;
    std::string error_stage;
    std::string conversation_id;
    int latency_ms = 0;
    // final
    std::vector<WqnAiAction> actions;
    std::vector<WqnAiFunctionCallSummary> function_calls;
};

typedef void (*WqnAiSseCallback)(const WqnAiSseEvent& event, void* user_ctx);

struct WqnAiStreamRequest {
    std::string token;
    std::vector<int16_t> pcm;        // mono s16le 16 kHz
    int duration_ms = 0;
    std::string tier = "std";        // "std" | "pro"
    std::string conversation_id;     // optional
    std::string request_id;          // optional idempotency UUID, server generates if empty
    int timeout_ms = 0;              // 0 = use WQN_AI_SSE_TIMEOUT_MS
    WqnAiSseCallback callback = nullptr;
    void* user_ctx = nullptr;
    // Thinking controls sent as validated X-WQN headers on the raw PCM request.
    // The cloud owns any system-prompt augmentation and maps these bounded
    // values to provider-specific parameters.
    std::string reasoning_effort;    // "low"|"medium"|"high"; empty = don't send
    bool enable_thinking = true;
};

// Streams a SSE response from POST {WQN_API_BASE}{WQN_AI_SSE_REQUEST_PATH}.
//
// On ESP_OK the call has returned because either the server emitted `final` or
// the client received an error event.  On ESP_FAIL a transport/HTTP-level
// failure occurred before the stream could deliver any final frame; the caller
// should look at response->error_code / error_message.
esp_err_t UploadAiAudioChatStream(const WqnAiStreamRequest& request, WqnAiChatResponse* response);

// === Legacy v1 one-shot JSON (Std/Pro tier, kept behind WQN_AI_V1_FALLBACK) ===
//   This remains in case the v2 SSE server regresses; the firmware rebuild with
//   WQN_AI_STREAMING_ENABLE=n will pick up the legacy path.
esp_err_t UploadAiAudioChat(
    const std::string& token,
    const uint8_t* pcm_data,
    size_t pcm_size,
    int duration_ms,
    const std::string& conversation_id,
    const std::string& tier,
    WqnAiChatResponse* response);

esp_err_t ParseTodoListResponse(const std::string& body, WqnTodoListPage* page);
esp_err_t ParseTodoCompleteResponse(const std::string& body, WqnTodoItem* todo);
esp_err_t ParseWordSearchResponse(const std::string& body, WqnWordSearchResult* result);
esp_err_t ParseWordPackManifestResponse(const std::string& body, WqnWordPackManifest* manifest);
esp_err_t ParseWordAiLookupResponse(const std::string& body, WqnWordAiLookupResult* result);
esp_err_t ParseAiChatResponseBody(const std::string& body, WqnAiChatResponse* response);

}  // namespace wqn
