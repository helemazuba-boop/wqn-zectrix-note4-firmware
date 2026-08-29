#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "esp_err.h"
#include "problem_pack.h"
#include "problem_store.h"
#include "storage.h"

namespace wqn {

enum class ProblemInput {
    kUp,
    kDown,
    kConfirm,
    kLongConfirm,
};

// 题目列表 -> 题目页（竖直环）-> 自评弹窗. Problem browsing lives inside the
// note screen (problem sets are mixed into the notebook list); `active` marks
// whether the problem layer currently owns the note screen's input/render.
enum class ProblemAppMode : uint8_t {
    kProblemList,
    kProblemView,
    kVerdict,
};

// Renderer-facing identity of the current ring segment. The ring is
// [题图1..P, 题面] while locked and [题图1..P, 题面, 答案面, 答案图1..M] once
// unlocked; ends joined, UP/DOWN wrap with the note-domain semantics.
enum class ProblemFace : uint8_t {
    kProblemImage,
    kBody,
    kAnswer,
    kSolutionImage,
};

enum class ProblemVerdictCommitState : uint8_t {
    kIdle,
    kPersisting,
    kCloudPending,
    kCloudAcknowledged,
    kFailed,
};

struct ProblemOutboxState {
    size_t pending_count = 0;
    size_t suspended_count = 0;
    size_t capacity = kProblemObservationOutboxCapacity;
};

struct ProblemAppState {
    bool initialized = false;
    bool active = false;
    ProblemAppMode mode = ProblemAppMode::kProblemList;

    // Which problem set is open: index into pack_index.sets plus display
    // copies that survive a staged index refresh.
    size_t set_order = 0;
    std::string set_id;
    std::string set_name;
    size_t list_selected = 0;
    size_t list_window_start = 0;

    // Ring position within the open problem (see ProblemFace).
    size_t ring_segment = 0;
    uint32_t body_scroll_lines = 0;
    uint32_t body_total_lines = 0;
    uint32_t answer_scroll_lines = 0;
    uint32_t answer_total_lines = 0;
    bool answer_unlocked = false;
    // 0=A对了 1=B还要想想 2=C错了 3=D跳过
    size_t verdict_selected = 0;

    WqnProblemEntry current;
    bool current_loaded = false;
    // Composed 题面 text: shell content followed by every part's own body
    // (the gaokao shell model keeps the real question text in parts[]).
    std::string body_text;
    // Composed 答案面 text (per part: label · 分值 · 正确答案).
    std::string answer_text;

    // Verdict outbox effect (mirrors the note observation effect handshake).
    bool verdict_effect_ready = false;
    DurableProblemObservation pending_verdict;
    bool advance_after_commit = false;
    ProblemVerdictCommitState commit_state = ProblemVerdictCommitState::kIdle;
    // [persist-worker] operation_id of the in-flight verdict persist submit; the
    // worker result is applied only if it still matches (and commit_state is
    // kPersisting), so a late result cannot mis-advance a reset/changed view.
    uint32_t pending_persist_operation_id = 0;
    ProblemOutboxState outbox;

    // Full-screen image plumbing (mirrors note_app's viewer; see note_app.h
    // for the expected/dispatched/loaded id rationale).
    bool image_request = false;
    bool image_in_flight = false;
    bool image_error = false;
    ImageRenderMode image_render_mode = ImageRenderMode::kGray16;
    bool image_expected_gray4 = false;
    // Which assets column and attachment the CURRENT image segment shows.
    bool image_is_solution = false;
    uint8_t image_index = 0;
    std::string image_expected_id;
    std::string image_dispatched_id;
    std::string image_loaded_id;
    std::shared_ptr<const std::vector<uint8_t>> image_wqni;
    int64_t image_dispatch_us = 0;
    int64_t image_view_entered_us = 0;

    ProblemPackIndex pack_index;
    // A cloud refresh never mutates the content backing an active browse; it
    // becomes current only after the problem layer is left.
    ProblemPackIndex pending_pack_index;
    bool pending_pack_index_ready = false;

    bool cloud_sync_requested = false;
    bool cloud_sync_failed = false;
    bool cloud_loaded_once = false;
    std::string message;
};

struct ProblemListRow {
    std::string title;
    std::string status_label;
};

struct ProblemAppSnapshot {
    bool active = false;
    ProblemAppMode mode = ProblemAppMode::kProblemList;
    std::string set_name;
    size_t list_selected = 0;
    size_t list_window_start = 0;
    std::vector<ProblemListRow> rows;

    // 题目页.
    std::string problem_id;  // wrapped-line cache key for the render layer
    std::string problem_title;
    ProblemFace face = ProblemFace::kBody;
    bool has_body = false;
    std::string body_text;
    std::string answer_text;
    uint32_t body_scroll_lines = 0;
    uint32_t answer_scroll_lines = 0;
    bool answer_unlocked = false;
    size_t position = 0;  // 1-based problem position within the set
    size_t total = 0;

    // Image segment.
    bool image_ready = false;
    bool image_error = false;
    bool image_is_solution = false;
    uint8_t image_ordinal = 0;  // 1-based within its kind, for the loading page
    uint8_t image_count = 0;
    std::string image_id;
    std::shared_ptr<const std::vector<uint8_t>> image_wqni;

    size_t verdict_selected = 0;
    ProblemVerdictCommitState commit_state = ProblemVerdictCommitState::kIdle;
    bool cloud_sync_failed = false;
    std::string status_line;
    std::string hint;
};

esp_err_t InitProblemApp(ProblemAppState* state);
esp_err_t HandleProblemAppInput(ProblemAppState* state, ProblemInput input);
void ApplyProblemPackIndex(
    ProblemAppState* state, ProblemPackIndex index, const std::string& message);
// Opens a problem set from the mixed notebook list. Returns false (with a
// message) when the set is not mounted or empty; the caller stays on the list.
bool ActivateProblemBrowse(ProblemAppState* state, const std::string& set_id);

// Image download request/result plumbing; same take/restore/apply shape as
// the note image path, executed by the problem cloud lane.
bool TakeProblemImageRequest(
    ProblemAppState* state,
    std::string* problem_id,
    bool* is_solution,
    uint8_t* image_index,
    std::string* image_id,
    bool* gray4);
void RestoreProblemImageRequest(ProblemAppState* state);
void ReleaseProblemImagePayload(ProblemAppState* state);
void SetProblemImageRenderMode(ProblemAppState* state, ImageRenderMode mode);
void ApplyProblemImageResult(
    ProblemAppState* state,
    esp_err_t result,
    const std::string& image_id,
    std::shared_ptr<const std::vector<uint8_t>> wqni);
bool ProblemImageLoadingGraceActive(const ProblemAppState& state, int64_t now_us);

// Verdict outbox effect handshake (mirrors TakeNoteObservationEffect).
bool TakeProblemVerdictEffect(
    ProblemAppState* state,
    const std::string& request_id,
    const std::string& occurred_at,
    uint32_t operation_id,
    DurableProblemObservation* observation);
void ApplyProblemVerdictCommitResult(ProblemAppState* state, esp_err_t result);

void RefreshProblemOutboxState(ProblemAppState* state);
ProblemAppSnapshot BuildProblemAppSnapshot(const ProblemAppState& state);
std::string ProblemAppStatusLine(const ProblemAppState& state);
std::string ProblemAppSignature(const ProblemAppState& state);
bool RunProblemPageStateSelfTest();

}  // namespace wqn
