#include "problem_app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "display_service.h"

namespace {

constexpr char kTag[] = "problem_app";

using wqn::protocol::problem_study_v1::ProblemStatus;
using wqn::protocol::problem_study_v1::ReviewAction;

// Visible rows on the problem title list; mirrors page_problem.cpp geometry
// (same list layout as the note lists).
constexpr size_t kProblemListVisibleRows = 8;
// Body/answer faces show 13 wrapped lines (no image entry row: attachments
// live on their own ring segments); must match page_problem.cpp.
constexpr uint32_t kProblemFaceVisibleLines = 13;
constexpr uint32_t kProblemFaceScrollStep = 4;

// Edge-triggered viewport (see note_app.cpp UpdateListViewport for the
// repaint rationale).
void UpdateListViewport(size_t selected, size_t count, size_t* start)
{
    constexpr size_t visible = kProblemListVisibleRows;
    if (count <= visible) {
        *start = 0;
        return;
    }
    const size_t max_start = count - visible;
    if (*start > max_start) *start = max_start;
    if (selected < *start) {
        *start = selected + 1 >= visible ? selected + 1 - visible : 0;
    } else if (selected >= *start + visible) {
        *start = std::min(selected, max_start);
    }
}

size_t ProblemImageCount(const wqn::ProblemAppState& state)
{
    return state.current.image_ids.size();
}

size_t SolutionImageCount(const wqn::ProblemAppState& state)
{
    return state.current.solution_image_ids.size();
}

// Ring layout: [题图 0..P-1, 题面 P] locked; unlocked appends [答案面 P+1,
// 答案图 P+2..P+1+M].
size_t BodySegment(const wqn::ProblemAppState& state)
{
    return ProblemImageCount(state);
}

size_t AnswerSegment(const wqn::ProblemAppState& state)
{
    return ProblemImageCount(state) + 1;
}

wqn::ProblemFace SegmentFace(const wqn::ProblemAppState& state, size_t segment)
{
    const size_t images = ProblemImageCount(state);
    if (segment < images) return wqn::ProblemFace::kProblemImage;
    if (segment == images) return wqn::ProblemFace::kBody;
    if (state.answer_unlocked && segment == images + 1) {
        return wqn::ProblemFace::kAnswer;
    }
    return wqn::ProblemFace::kSolutionImage;
}

uint32_t FaceMaxScroll(uint32_t total_lines)
{
    return total_lines > kProblemFaceVisibleLines
        ? total_lines - kProblemFaceVisibleLines
        : 0;
}

const wqn::ProblemPackSet* OpenSet(const wqn::ProblemAppState& state)
{
    if (state.set_order >= state.pack_index.sets.size()) return nullptr;
    const wqn::ProblemPackSet& set = state.pack_index.sets[state.set_order];
    if (set.set_id != state.set_id) return nullptr;
    return &set;
}

const char* ProblemStatusLabel(uint8_t status)
{
    switch (static_cast<ProblemStatus>(status)) {
        case ProblemStatus::kWrong: return "还错着";
        case ProblemStatus::kNeedsReview: return "要复习";
        case ProblemStatus::kMastered: return "已掌握";
    }
    return "";
}

// 题面: the gaokao shell model splits the text across the shell's shared
// stem (content_text, may be empty) and every part's own body. Rendering only
// the shell stem dropped the actual questions -- single-part problems (whose
// stem is usually empty) showed nothing at all. A lone part without label or
// marks degrades to plain stem + body with no "第N问" heading.
std::string ComposeBodyText(const wqn::WqnProblemEntry& entry)
{
    std::string text;
    if (!entry.content_text.empty()) {
        text = entry.content_text;
    }
    const bool single_plain_part = entry.parts.size() == 1 &&
        entry.parts.front().label.empty() && entry.parts.front().full_marks == 0;
    for (const wqn::WqnProblemPackPart& part : entry.parts) {
        std::string section;
        if (!single_plain_part) {
            section = "第" + std::to_string(part.index) + "问";
            if (!part.label.empty()) {
                section += " · " + part.label;
            }
            if (part.full_marks > 0) {
                section += " · " + std::to_string(part.full_marks) + "分";
            }
            if (!part.content_text.empty()) {
                section.push_back('\n');
                section += part.content_text;
            }
        } else {
            section = part.content_text;
        }
        if (section.empty()) continue;
        if (!text.empty()) {
            text += "\n\n";
        }
        text += section;
    }
    return text;
}

// 答案面: 逐问 label · 分值 · 正确答案, one blank line between parts.
std::string ComposeAnswerText(const wqn::WqnProblemEntry& entry)
{
    std::string text;
    for (const wqn::WqnProblemPackPart& part : entry.parts) {
        std::string heading = "第" + std::to_string(part.index) + "问";
        if (!part.label.empty()) {
            heading += " · " + part.label;
        }
        if (part.full_marks > 0) {
            heading += " · " + std::to_string(part.full_marks) + "分";
        }
        text += heading;
        text.push_back('\n');
        text += "答案：";
        text += part.answer_text.empty() ? "（见解析图）" : part.answer_text;
        text.push_back('\n');
        text.push_back('\n');
    }
    if (!text.empty()) {
        text.pop_back();
    }
    return text;
}

void ResetProblemImageViewer(wqn::ProblemAppState* state)
{
    state->image_request = false;
    state->image_in_flight = false;
    state->image_error = false;
    state->image_is_solution = false;
    state->image_index = 0;
    state->image_expected_id.clear();
    state->image_dispatched_id.clear();
    state->image_loaded_id.clear();
    state->image_wqni.reset();
    state->image_dispatch_us = 0;
}

// Points the viewer at the current image segment's content hash: a hit on the
// in-memory payload is immediate; otherwise the pump hands the id to the
// problem cloud lane (shared ni_ SPIFFS cache first, then download).
void RequestCurrentProblemImage(wqn::ProblemAppState* state)
{
    const auto& ids = state->image_is_solution
        ? state->current.solution_image_ids
        : state->current.image_ids;
    if (state->image_index >= ids.size()) {
        state->image_error = true;
        return;
    }
    const std::string& id = ids[state->image_index];
    state->image_expected_id = id;
    state->image_error = false;
    if (state->image_loaded_id == id && state->image_wqni != nullptr) {
        state->image_request = false;
        return;
    }
    if (!state->image_in_flight) {
        state->image_request = true;
    }
}

void EnterImageSegment(wqn::ProblemAppState* state, size_t segment)
{
    state->ring_segment = segment;
    const size_t images = ProblemImageCount(*state);
    if (segment < images) {
        state->image_is_solution = false;
        state->image_index = static_cast<uint8_t>(segment);
    } else {
        state->image_is_solution = true;
        state->image_index = static_cast<uint8_t>(segment - images - 2);
    }
    state->image_view_entered_us = esp_timer_get_time();
    RequestCurrentProblemImage(state);
    state->message.clear();
}

void EnterBodySegment(wqn::ProblemAppState* state, uint32_t scroll_lines)
{
    state->ring_segment = BodySegment(*state);
    state->body_scroll_lines = scroll_lines;
    state->image_request = false;
    state->image_error = false;
    state->message.clear();
}

void EnterAnswerSegment(wqn::ProblemAppState* state, uint32_t scroll_lines)
{
    state->ring_segment = AnswerSegment(*state);
    state->answer_scroll_lines = scroll_lines;
    state->image_request = false;
    state->image_error = false;
    state->message.clear();
}

// Best-effort body load for the selected problem. Only touches storage when a
// matching pack entry exists, so the reducer stays testable with an empty
// index.
void LoadCurrentProblem(wqn::ProblemAppState* state)
{
    state->current = wqn::WqnProblemEntry{};
    state->current_loaded = false;
    state->body_text.clear();
    state->answer_text.clear();
    state->answer_unlocked = false;
    state->body_scroll_lines = 0;
    state->answer_scroll_lines = 0;
    state->body_total_lines = 0;
    state->answer_total_lines = 0;
    state->verdict_selected = 0;
    ResetProblemImageViewer(state);
    const wqn::ProblemPackSet* set = OpenSet(*state);
    if (set == nullptr || state->list_selected >= set->entry_count) {
        state->ring_segment = 0;
        return;
    }
    const size_t entry_index = set->entry_begin + state->list_selected;
    if (entry_index >= state->pack_index.entries.size()) {
        state->ring_segment = 0;
        return;
    }
    const wqn::ProblemPackIndexEntry& entry = state->pack_index.entries[entry_index];
    const esp_err_t read_result = wqn::ReadProblemPackEntry(entry, &state->current);
    if (read_result == ESP_OK) {
        state->current_loaded = true;
        // Precompute wrapped line counts with the SAME width as the renderer
        // (page_problem.cpp: kContentW - 14 = 370 px) so scroll clamps match.
        state->body_text = ComposeBodyText(state->current);
        state->body_total_lines = static_cast<uint32_t>(
            wqn::WrapUtf8TextToWidth(state->body_text, 370, 4096).size());
        state->answer_text = ComposeAnswerText(state->current);
        state->answer_total_lines = static_cast<uint32_t>(
            wqn::WrapUtf8TextToWidth(state->answer_text, 370, 4096).size());
        ESP_LOGI(
            kTag, "problem opened: id=%.8s images=%u/%u parts=%u body_bytes=%u",
            state->current.problem_id.c_str(),
            static_cast<unsigned>(state->current.image_ids.size()),
            static_cast<unsigned>(state->current.solution_image_ids.size()),
            static_cast<unsigned>(state->current.parts.size()),
            static_cast<unsigned>(state->current.content_text.size()));
    } else {
        ESP_LOGW(
            kTag, "problem open read failed: id=%.8s err=%s",
            entry.problem_id, esp_err_to_name(read_result));
    }
    // The problem opens on the 题面; attachments sit above it on the ring.
    state->ring_segment = BodySegment(*state);
}

void InstallProblemPackIndex(
    wqn::ProblemAppState* state,
    wqn::ProblemPackIndex index,
    const std::string& message)
{
    const bool has_manifest = index.has_manifest;
    const bool pack_error = index.pack_error;
    const std::string status_message = index.status_message;
    state->pack_index = std::move(index);
    state->cloud_loaded_once = has_manifest;
    state->cloud_sync_failed = pack_error;
    state->cloud_sync_requested = !has_manifest || pack_error;
    state->message = !message.empty() ? message : status_message;
    if (state->message.empty()) {
        state->message = state->pack_index.sets.empty() ? "错题未同步" : "错题已就绪";
    }
}

void ActivatePendingProblemPackIndex(wqn::ProblemAppState* state)
{
    if (state == nullptr || state->active || !state->pending_pack_index_ready) {
        return;
    }
    wqn::ProblemPackIndex pending = std::move(state->pending_pack_index);
    state->pending_pack_index = {};
    state->pending_pack_index_ready = false;
    InstallProblemPackIndex(state, std::move(pending), "错题更新已启用");
}

void ExitToProblemList(wqn::ProblemAppState* state)
{
    state->current = wqn::WqnProblemEntry{};
    state->current_loaded = false;
    state->body_text.clear();
    state->answer_text.clear();
    state->answer_unlocked = false;
    ResetProblemImageViewer(state);
    state->mode = wqn::ProblemAppMode::kProblemList;
    state->message.clear();
}

void HandleProblemListInput(wqn::ProblemAppState* state, wqn::ProblemInput input)
{
    const wqn::ProblemPackSet* set = OpenSet(*state);
    const size_t count = set != nullptr ? set->entry_count : 0;
    switch (input) {
        case wqn::ProblemInput::kUp:
            if (state->list_selected > 0) {
                --state->list_selected;
                state->message.clear();
            } else {
                state->message = "已到顶部";
            }
            UpdateListViewport(state->list_selected, count, &state->list_window_start);
            break;
        case wqn::ProblemInput::kDown:
            if (count > 0 && state->list_selected + 1 < count) {
                ++state->list_selected;
                state->message.clear();
            } else {
                state->message = "已到底部";
            }
            UpdateListViewport(state->list_selected, count, &state->list_window_start);
            break;
        case wqn::ProblemInput::kConfirm:
            if (count == 0 || state->list_selected >= count) {
                state->message = "该错题本暂无题目";
                break;
            }
            LoadCurrentProblem(state);
            state->mode = wqn::ProblemAppMode::kProblemView;
            state->message = state->current_loaded ? "" : "内容未同步";
            break;
        case wqn::ProblemInput::kLongConfirm:
            // Leave the problem layer: back to the mixed notebook list.
            state->active = false;
            state->message.clear();
            ActivatePendingProblemPackIndex(state);
            break;
    }
}

void HandleProblemViewInput(wqn::ProblemAppState* state, wqn::ProblemInput input)
{
    const size_t images = ProblemImageCount(*state);
    const size_t solution_images = SolutionImageCount(*state);
    const wqn::ProblemFace face = SegmentFace(*state, state->ring_segment);
    const uint32_t body_max = FaceMaxScroll(state->body_total_lines);
    const uint32_t answer_max = FaceMaxScroll(state->answer_total_lines);
    switch (input) {
        case wqn::ProblemInput::kUp:
            if (face == wqn::ProblemFace::kBody) {
                if (state->body_scroll_lines > 0) {
                    state->body_scroll_lines =
                        state->body_scroll_lines > kProblemFaceScrollStep
                            ? state->body_scroll_lines - kProblemFaceScrollStep
                            : 0;
                    state->message.clear();
                } else if (images > 0) {
                    // Up from the 题面 top reaches the image directly above
                    // it: the LAST problem attachment.
                    EnterImageSegment(state, images - 1);
                } else if (state->answer_unlocked) {
                    // No problem images: wrap backwards around the ring.
                    if (solution_images > 0) {
                        EnterImageSegment(state, AnswerSegment(*state) + solution_images);
                    } else {
                        EnterAnswerSegment(state, answer_max);
                    }
                } else {
                    state->body_scroll_lines = body_max;
                    state->message.clear();
                }
            } else if (face == wqn::ProblemFace::kAnswer) {
                if (state->answer_scroll_lines > 0) {
                    state->answer_scroll_lines =
                        state->answer_scroll_lines > kProblemFaceScrollStep
                            ? state->answer_scroll_lines - kProblemFaceScrollStep
                            : 0;
                    state->message.clear();
                } else {
                    EnterBodySegment(state, body_max);
                }
            } else if (face == wqn::ProblemFace::kProblemImage) {
                if (state->ring_segment > 0) {
                    EnterImageSegment(state, state->ring_segment - 1);
                } else if (state->answer_unlocked) {
                    if (solution_images > 0) {
                        EnterImageSegment(state, AnswerSegment(*state) + solution_images);
                    } else {
                        EnterAnswerSegment(state, answer_max);
                    }
                } else {
                    EnterBodySegment(state, body_max);
                }
            } else {  // kSolutionImage
                if (state->ring_segment > AnswerSegment(*state) + 1) {
                    EnterImageSegment(state, state->ring_segment - 1);
                } else {
                    EnterAnswerSegment(state, answer_max);
                }
            }
            break;
        case wqn::ProblemInput::kDown:
            if (face == wqn::ProblemFace::kBody) {
                if (state->body_scroll_lines < body_max) {
                    state->body_scroll_lines = std::min<uint32_t>(
                        state->body_scroll_lines + kProblemFaceScrollStep, body_max);
                    state->message.clear();
                } else if (state->answer_unlocked) {
                    EnterAnswerSegment(state, 0);
                } else if (images > 0) {
                    // Down past the 题面 end wraps around the ring to the
                    // first problem image at the very top.
                    EnterImageSegment(state, 0);
                } else {
                    state->body_scroll_lines = 0;
                    state->message.clear();
                }
            } else if (face == wqn::ProblemFace::kAnswer) {
                if (state->answer_scroll_lines < answer_max) {
                    state->answer_scroll_lines = std::min<uint32_t>(
                        state->answer_scroll_lines + kProblemFaceScrollStep, answer_max);
                    state->message.clear();
                } else if (solution_images > 0) {
                    EnterImageSegment(state, AnswerSegment(*state) + 1);
                } else if (images > 0) {
                    EnterImageSegment(state, 0);
                } else {
                    EnterBodySegment(state, 0);
                }
            } else if (face == wqn::ProblemFace::kProblemImage) {
                if (state->ring_segment + 1 < images) {
                    EnterImageSegment(state, state->ring_segment + 1);
                } else {
                    EnterBodySegment(state, 0);
                }
            } else {  // kSolutionImage
                if (state->ring_segment < AnswerSegment(*state) + solution_images) {
                    EnterImageSegment(state, state->ring_segment + 1);
                } else if (images > 0) {
                    EnterImageSegment(state, 0);
                } else {
                    EnterBodySegment(state, 0);
                }
            }
            break;
        case wqn::ProblemInput::kConfirm:
            if (state->commit_state == wqn::ProblemVerdictCommitState::kPersisting) {
                // The previous verdict is still committing; ignore the window.
                break;
            }
            if (!state->answer_unlocked) {
                if (!state->current_loaded) {
                    state->message = "内容未同步";
                    break;
                }
                // Unlock: extend the ring and jump to the answer face's first
                // screen.
                state->answer_unlocked = true;
                EnterAnswerSegment(state, 0);
            } else {
                state->mode = wqn::ProblemAppMode::kVerdict;
                state->verdict_selected = 0;
                state->message.clear();
            }
            break;
        case wqn::ProblemInput::kLongConfirm:
            ExitToProblemList(state);
            break;
    }
}

ReviewAction VerdictAction(size_t selected)
{
    switch (selected) {
        case 0: return ReviewAction::kCorrect;
        case 1: return ReviewAction::kHesitant;
        case 2: return ReviewAction::kWrong;
        default: return ReviewAction::kSkip;
    }
}

void HandleVerdictInput(wqn::ProblemAppState* state, wqn::ProblemInput input)
{
    switch (input) {
        case wqn::ProblemInput::kUp:
            state->verdict_selected = (state->verdict_selected + 3) % 4;
            break;
        case wqn::ProblemInput::kDown:
            state->verdict_selected = (state->verdict_selected + 1) % 4;
            break;
        case wqn::ProblemInput::kConfirm: {
            if (state->commit_state == wqn::ProblemVerdictCommitState::kPersisting) {
                break;
            }
            if (state->current.problem_id.size() != 36) {
                state->mode = wqn::ProblemAppMode::kProblemView;
                state->message = "内容未同步";
                break;
            }
            wqn::DurableProblemObservation pending;
            pending.problem_id = state->current.problem_id;
            pending.action = VerdictAction(state->verdict_selected);
            state->pending_verdict = std::move(pending);
            state->verdict_effect_ready = true;
            state->commit_state = wqn::ProblemVerdictCommitState::kPersisting;
            state->advance_after_commit = true;
            state->mode = wqn::ProblemAppMode::kProblemView;
            state->message = "正在记录…";
            break;
        }
        case wqn::ProblemInput::kLongConfirm:
            // Escape hatch: back to the problem without recording anything.
            state->mode = wqn::ProblemAppMode::kProblemView;
            state->message.clear();
            break;
    }
}

// Advances to the next problem in fixed pack order after a committed verdict;
// past the last problem the round ends back on the title list.
void AdvanceAfterVerdict(wqn::ProblemAppState* state)
{
    const wqn::ProblemPackSet* set = OpenSet(*state);
    const size_t count = set != nullptr ? set->entry_count : 0;
    if (count > 0 && state->list_selected + 1 < count) {
        ++state->list_selected;
        UpdateListViewport(state->list_selected, count, &state->list_window_start);
        LoadCurrentProblem(state);
        state->mode = wqn::ProblemAppMode::kProblemView;
        state->message = state->current_loaded ? "已记录，下一题" : "内容未同步";
    } else {
        ExitToProblemList(state);
        state->message = "本轮已完成";
    }
}

}  // namespace

namespace wqn {

esp_err_t InitProblemApp(ProblemAppState* state)
{
    if (state == nullptr) return ESP_ERR_INVALID_ARG;
    if (state->initialized) return ESP_OK;

    const esp_err_t storage_result = InitProblemPackStorage();
    if (storage_result != ESP_OK) {
        state->message = "错题分区不可用";
    } else {
        ProblemPackIndex index;
        const esp_err_t index_result = LoadProblemPackIndex(&index);
        const std::string index_message = index.status_message;
        InstallProblemPackIndex(state, std::move(index), index_message);
        if (index_result != ESP_OK) {
            ESP_LOGW(kTag, "load local problem pack index failed: %s",
                     esp_err_to_name(index_result));
        }
    }

    ProblemOutboxSnapshot outbox;
    if (ReadProblemOutboxSnapshot(&outbox) == ESP_OK) {
        state->outbox.pending_count = outbox.pending_count;
        state->outbox.capacity = outbox.capacity;
    }

    state->initialized = true;
    state->cloud_sync_requested =
        !state->cloud_loaded_once || state->pack_index.pack_error;
    return ESP_OK;
}

esp_err_t HandleProblemAppInput(ProblemAppState* state, ProblemInput input)
{
    if (state == nullptr) return ESP_ERR_INVALID_ARG;
    if (!state->initialized) {
        ESP_RETURN_ON_ERROR(InitProblemApp(state), kTag, "init problem app");
    }
    if (!state->active) return ESP_OK;
    // [persist-worker] While a verdict is committing (kPersisting covers the
    // whole Prepare -> worker-apply window, wider than persist busy), leaving
    // the current view could drop the problem layer or switch sets; the late
    // result would then AdvanceAfterVerdict() against the wrong set/list. Block
    // the escape until the terminal result is applied; every other input stays
    // responsive.
    if (input == ProblemInput::kLongConfirm &&
        state->commit_state == ProblemVerdictCommitState::kPersisting) {
        state->message = "正在保存，请稍后离开";
        return ESP_OK;
    }
    switch (state->mode) {
        case ProblemAppMode::kProblemList:
            HandleProblemListInput(state, input);
            break;
        case ProblemAppMode::kProblemView:
            HandleProblemViewInput(state, input);
            break;
        case ProblemAppMode::kVerdict:
            HandleVerdictInput(state, input);
            break;
    }
    return ESP_OK;
}

void ApplyProblemPackIndex(
    ProblemAppState* state, ProblemPackIndex index, const std::string& message)
{
    if (state == nullptr) return;
    // A refresh while browsing stages for the next return to the mixed list.
    if (state->active) {
        state->pending_pack_index = std::move(index);
        state->pending_pack_index_ready = true;
        return;
    }
    InstallProblemPackIndex(state, std::move(index), message);
}

bool ActivateProblemBrowse(ProblemAppState* state, const std::string& set_id)
{
    if (state == nullptr || set_id.size() != 36) return false;
    // [persist-worker] Second line of defense behind the LongConfirm gate: an
    // unexpected entry path must not switch sets (resetting set_order and
    // list_selected) while a verdict commit is still in flight, or the late
    // result would advance the wrong set.
    if (state->commit_state == ProblemVerdictCommitState::kPersisting) {
        state->message = "正在保存，请稍后";
        return false;
    }
    if (!state->initialized) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(InitProblemApp(state));
    }
    ActivatePendingProblemPackIndex(state);
    for (size_t index = 0; index < state->pack_index.sets.size(); ++index) {
        const ProblemPackSet& set = state->pack_index.sets[index];
        if (set.set_id != set_id) continue;
        if (!set.has_pack || set.entry_count == 0) {
            state->message = "该错题本暂无题目";
            return false;
        }
        state->active = true;
        state->mode = ProblemAppMode::kProblemList;
        state->set_order = index;
        state->set_id = set.set_id;
        state->set_name = set.name;
        state->list_selected = 0;
        state->list_window_start = 0;
        state->message = "选择要练的题";
        return true;
    }
    state->message = "错题本尚未同步";
    return false;
}

// Defined below ApplyProblemImageResult; the pump-side take path uses it to
// heal dropped or mismatched fetch results.
void EnsureProblemImageRequest(ProblemAppState* state);

bool TakeProblemImageRequest(
    ProblemAppState* state,
    std::string* problem_id,
    bool* is_solution,
    uint8_t* image_index,
    std::string* image_id)
{
    if (state == nullptr || problem_id == nullptr || is_solution == nullptr ||
        image_index == nullptr || image_id == nullptr) {
        return false;
    }
    EnsureProblemImageRequest(state);
    const bool on_image_segment = state->active &&
        state->mode == ProblemAppMode::kProblemView &&
        (SegmentFace(*state, state->ring_segment) == ProblemFace::kProblemImage ||
         SegmentFace(*state, state->ring_segment) == ProblemFace::kSolutionImage);
    if (!state->image_request || state->image_in_flight || !on_image_segment ||
        state->image_expected_id.empty() ||
        state->current.problem_id.size() != 36) {
        return false;
    }
    *problem_id = state->current.problem_id;
    *is_solution = state->image_is_solution;
    *image_index = state->image_index;
    *image_id = state->image_expected_id;
    state->image_request = false;
    state->image_in_flight = true;
    state->image_dispatch_us = esp_timer_get_time();
    state->image_dispatched_id = *image_id;
    ESP_LOGI(kTag, "problem image fetch dispatched: id=%.12s kind=%s index=%u",
             image_id->c_str(), *is_solution ? "solution" : "assets",
             static_cast<unsigned>(*image_index));
    return true;
}

void RestoreProblemImageRequest(ProblemAppState* state)
{
    if (state == nullptr || !state->image_in_flight) return;
    state->image_in_flight = false;
    state->image_request = true;
}

// Drops the 15 KB image payload when the user leaves the note screen (the
// problem layer rides on it); re-entry reloads via the shared ni_ cache.
void ReleaseProblemImagePayload(ProblemAppState* state)
{
    if (state == nullptr) return;
    state->image_wqni.reset();
    state->image_loaded_id.clear();
}

void ApplyProblemImageResult(
    ProblemAppState* state,
    esp_err_t result,
    const std::string& image_id,
    std::shared_ptr<const std::vector<uint8_t>> wqni)
{
    if (state == nullptr) return;
    state->image_in_flight = false;
    if (result == ESP_OK && wqni != nullptr) {
        state->image_loaded_id = image_id;
        state->image_wqni = std::move(wqni);
        if (state->image_expected_id == image_id) {
            state->image_error = false;
        }
        return;
    }
    // Attribute empty-id failures to the id recorded at dispatch (see
    // ApplyNoteImageResult for the poisoning rationale).
    const std::string& failed_id =
        image_id.empty() ? state->image_dispatched_id : image_id;
    if (!state->image_request && failed_id == state->image_expected_id) {
        state->image_error = true;
        ESP_LOGW(kTag, "problem image fetch failed: %s id=%.12s",
                 esp_err_to_name(result), failed_id.c_str());
    }
}

// Self-healing for the image segments: a dropped/mismatched result re-arms
// the request; a wedged fetch times out past 20 s. Failed fetches set
// image_error and are NOT re-armed.
void EnsureProblemImageRequest(ProblemAppState* state)
{
    if (state == nullptr || !state->active ||
        state->mode != ProblemAppMode::kProblemView ||
        state->image_request || state->image_error ||
        state->image_expected_id.empty()) {
        return;
    }
    const ProblemFace face = SegmentFace(*state, state->ring_segment);
    if (face != ProblemFace::kProblemImage && face != ProblemFace::kSolutionImage) {
        return;
    }
    if (state->image_in_flight) {
        constexpr int64_t kImageFetchTimeoutUs = 20LL * 1000 * 1000;
        if (state->image_dispatch_us <= 0 ||
            esp_timer_get_time() - state->image_dispatch_us < kImageFetchTimeoutUs) {
            return;
        }
        ESP_LOGW(kTag, "problem image fetch timed out; re-arming: id=%.12s",
                 state->image_expected_id.c_str());
        state->image_in_flight = false;
        state->image_request = true;
        return;
    }
    if (state->image_loaded_id == state->image_expected_id &&
        state->image_wqni != nullptr) {
        return;  // already showing
    }
    ESP_LOGW(kTag, "problem image request re-armed: id=%.12s",
             state->image_expected_id.c_str());
    state->image_request = true;
}

bool ProblemImageLoadingGraceActive(const ProblemAppState& state, int64_t now_us)
{
    if (!state.active || state.mode != ProblemAppMode::kProblemView ||
        state.image_error) {
        return false;
    }
    const ProblemFace face = SegmentFace(state, state.ring_segment);
    if (face != ProblemFace::kProblemImage && face != ProblemFace::kSolutionImage) {
        return false;
    }
    if (state.image_loaded_id == state.image_expected_id &&
        state.image_wqni != nullptr) {
        return false;  // payload ready: paint it now
    }
    constexpr int64_t kProblemImageGraceUs = 400LL * 1000;
    return state.image_view_entered_us > 0 &&
        now_us - state.image_view_entered_us < kProblemImageGraceUs;
}

bool TakeProblemVerdictEffect(
    ProblemAppState* state,
    const std::string& request_id,
    const std::string& occurred_at,
    uint32_t operation_id,
    DurableProblemObservation* observation)
{
    if (state == nullptr || observation == nullptr ||
        !state->verdict_effect_ready || request_id.empty() ||
        occurred_at.empty()) {
        return false;
    }
    auto& pending = state->pending_verdict;
    if (pending.request_id.empty()) {
        // Assigned exactly once so a queue-full retry stays idempotent.
        pending.request_id = request_id;
        pending.occurred_at = occurred_at;
    }
    if (pending.problem_id.size() != 36) {
        state->verdict_effect_ready = false;
        state->commit_state = ProblemVerdictCommitState::kFailed;
        state->message = "记录无效";
        return false;
    }
    state->verdict_effect_ready = false;
    // Bind this dispatch so a late worker result after a reset / newer submit
    // is rejected instead of applied (mirrors word/note).
    state->pending_persist_operation_id = operation_id;
    *observation = pending;
    return true;
}

void ApplyProblemVerdictCommitResult(ProblemAppState* state, esp_err_t result)
{
    if (state == nullptr) return;
    // The bound dispatch is consumed either way; clearing keeps the expected-id
    // invariant tight (the kPersisting guard at the caller is the primary one).
    state->pending_persist_operation_id = 0;
    if (result != ESP_OK) {
        state->commit_state = ProblemVerdictCommitState::kFailed;
        state->advance_after_commit = false;
        state->message = result == ESP_ERR_NO_MEM ? "记录空间已满" : "记录未保存";
        return;
    }
    state->pending_verdict = {};
    state->commit_state = ProblemVerdictCommitState::kCloudPending;
    if (state->outbox.pending_count < state->outbox.capacity) {
        ++state->outbox.pending_count;
    }
    if (state->advance_after_commit) {
        state->advance_after_commit = false;
        AdvanceAfterVerdict(state);
    }
}

void RefreshProblemOutboxState(ProblemAppState* state)
{
    if (state == nullptr) return;
    ProblemOutboxSnapshot snapshot;
    if (ReadProblemOutboxSnapshot(&snapshot) != ESP_OK) return;
    state->outbox.pending_count = snapshot.pending_count;
    state->outbox.capacity = snapshot.capacity;
    if (snapshot.pending_count == 0 &&
        state->commit_state == ProblemVerdictCommitState::kCloudPending) {
        state->commit_state = ProblemVerdictCommitState::kCloudAcknowledged;
    }
}

ProblemAppSnapshot BuildProblemAppSnapshot(const ProblemAppState& state)
{
    ProblemAppSnapshot snapshot;
    snapshot.active = state.active;
    if (!state.active) return snapshot;
    snapshot.mode = state.mode;
    snapshot.set_name = state.set_name;
    snapshot.list_selected = state.list_selected;
    snapshot.list_window_start = state.list_window_start;
    snapshot.cloud_sync_failed = state.cloud_sync_failed;

    const ProblemPackSet* set = OpenSet(state);
    const size_t count = set != nullptr ? set->entry_count : 0;
    snapshot.total = count;
    if (state.mode == ProblemAppMode::kProblemList && set != nullptr) {
        snapshot.rows.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const size_t entry_index = set->entry_begin + index;
            if (entry_index >= state.pack_index.entries.size()) break;
            const ProblemPackIndexEntry& entry = state.pack_index.entries[entry_index];
            ProblemListRow row;
            row.title = entry.title;
            row.status_label = ProblemStatusLabel(entry.status);
            snapshot.rows.push_back(std::move(row));
        }
    }

    if (state.mode == ProblemAppMode::kProblemView ||
        state.mode == ProblemAppMode::kVerdict) {
        snapshot.position = state.list_selected + 1;
        snapshot.has_body = state.current_loaded;
        snapshot.problem_id = state.current.problem_id;
        snapshot.problem_title = state.current.title;
        snapshot.body_text = state.body_text;
        snapshot.answer_text = state.answer_text;
        snapshot.body_scroll_lines = state.body_scroll_lines;
        snapshot.answer_scroll_lines = state.answer_scroll_lines;
        snapshot.answer_unlocked = state.answer_unlocked;
        snapshot.face = SegmentFace(state, state.ring_segment);
        if (snapshot.face == ProblemFace::kProblemImage ||
            snapshot.face == ProblemFace::kSolutionImage) {
            snapshot.image_is_solution = state.image_is_solution;
            snapshot.image_ordinal = static_cast<uint8_t>(state.image_index + 1);
            snapshot.image_count = static_cast<uint8_t>(std::min<size_t>(
                state.image_is_solution ? state.current.solution_image_ids.size()
                                        : state.current.image_ids.size(),
                protocol::problem_study_v1::kMaxImagesPerRow));
            snapshot.image_id = state.image_expected_id;
            snapshot.image_error = state.image_error;
            snapshot.image_ready = !state.image_error &&
                state.image_wqni != nullptr &&
                state.image_loaded_id == state.image_expected_id &&
                !state.image_expected_id.empty();
            if (snapshot.image_ready) {
                snapshot.image_wqni = state.image_wqni;
            }
        }
    }
    snapshot.verdict_selected = state.verdict_selected;
    snapshot.commit_state = state.commit_state;
    snapshot.status_line = ProblemAppStatusLine(state);
    switch (state.mode) {
        case ProblemAppMode::kProblemList:
            snapshot.hint = "确认打开 · 长按返回";
            break;
        case ProblemAppMode::kProblemView:
            snapshot.hint = state.answer_unlocked
                ? "上下浏览 · 确认自评"
                : "上下浏览 · 确认看答案";
            break;
        case ProblemAppMode::kVerdict:
            snapshot.hint = "上下选择 · 确认提交";
            break;
    }
    return snapshot;
}

std::string ProblemAppStatusLine(const ProblemAppState& state)
{
    if (!state.message.empty()) return state.message;
    switch (state.mode) {
        case ProblemAppMode::kProblemList:
            return "选择要练的题";
        case ProblemAppMode::kProblemView:
            return state.current_loaded ? "" : "内容未同步";
        case ProblemAppMode::kVerdict:
            return "这道题做得怎么样？";
    }
    return "";
}

std::string ProblemAppSignature(const ProblemAppState& state)
{
    // Compact identity for the render layer to detect meaningful frame
    // changes; every navigation/unlock/verdict/image transition must land
    // here or the dedup pipeline freezes the page.
    char buffer[128] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u",
        static_cast<unsigned>(state.active ? 1 : 0),
        static_cast<unsigned>(state.mode),
        static_cast<unsigned>(state.list_selected),
        static_cast<unsigned>(state.ring_segment),
        static_cast<unsigned>(state.body_scroll_lines),
        static_cast<unsigned>(state.answer_scroll_lines),
        static_cast<unsigned>(state.answer_unlocked ? 1 : 0),
        static_cast<unsigned>(state.verdict_selected),
        static_cast<unsigned>(state.commit_state),
        static_cast<unsigned>(state.image_error ? 1 : 0),
        static_cast<unsigned>(state.pack_index.sets.size()));
    return std::string(buffer);
}

namespace {

struct ProblemPageFixture {
    ProblemAppState* state = nullptr;
    ProblemPageFixture() : state(new (std::nothrow) ProblemAppState()) {}
    ~ProblemPageFixture() { delete state; }
    explicit operator bool() const { return state != nullptr; }
    ProblemAppState& get() { return *state; }
};

// Builds a browsable in-memory fixture: one set of `count` problems whose
// entries have no pack file backing (current_loaded stays false, which the
// ring navigation must tolerate).
void PopulateFixture(ProblemAppState* state, size_t count)
{
    state->initialized = true;
    ProblemPackSet set;
    set.set_id = "11111111-1111-4111-8111-111111111111";
    set.name = "错题本";
    set.has_pack = true;
    set.entry_begin = 0;
    set.entry_count = count;
    state->pack_index.sets.push_back(set);
    for (size_t index = 0; index < count; ++index) {
        ProblemPackIndexEntry entry = {};
        std::snprintf(
            entry.problem_id, sizeof(entry.problem_id),
            "00000000-0000-4000-8000-%012u", static_cast<unsigned>(index + 100));
        std::snprintf(entry.set_id, sizeof(entry.set_id), "%s", set.set_id.c_str());
        std::snprintf(entry.title, sizeof(entry.title), "题目 %u",
                      static_cast<unsigned>(index + 1));
        entry.status = static_cast<uint8_t>(ProblemStatus::kWrong);
        state->pack_index.entries.push_back(entry);
    }
}

}  // namespace

bool RunProblemPageStateSelfTest()
{
    auto require = [](bool condition, const char* label) {
        if (!condition) ESP_LOGE(kTag, "problem page self-test failed: %s", label);
        return condition;
    };

    // Inactive layer ignores input; empty-list navigation stays bounded.
    ProblemPageFixture idle;
    if (!idle) return require(false, "allocate idle fixture");
    idle.get().initialized = true;
    for (size_t index = 0; index < 100; ++index) {
        const ProblemInput input = (index % 2 == 0) ? ProblemInput::kDown : ProblemInput::kUp;
        if (HandleProblemAppInput(&idle.get(), input) != ESP_OK) return false;
    }
    if (!require(!idle.get().active, "inactive layer stays inactive")) return false;

    // Activation requires a mounted, non-empty set.
    ProblemPageFixture activate;
    if (!activate) return require(false, "allocate activation fixture");
    activate.get().initialized = true;
    if (!require(
            !ActivateProblemBrowse(
                &activate.get(), "11111111-1111-4111-8111-111111111111"),
            "unknown set rejected")) {
        return false;
    }
    PopulateFixture(&activate.get(), 3);
    if (!require(
            ActivateProblemBrowse(
                &activate.get(), "11111111-1111-4111-8111-111111111111"),
            "mounted set activates") ||
        !require(activate.get().mode == ProblemAppMode::kProblemList,
                 "activation lands on the list")) {
        return false;
    }

    // Confirm opens the 题面; unlock extends the ring and jumps to the answer
    // face; a second confirm opens the verdict dialog with A preselected.
    ProblemAppState& a = activate.get();
    if (HandleProblemAppInput(&a, ProblemInput::kConfirm) != ESP_OK ||
        !require(a.mode == ProblemAppMode::kProblemView, "confirm opens the problem") ||
        !require(!a.answer_unlocked, "problem opens locked")) {
        return false;
    }
    // current_loaded is false (no pack file behind the fixture), so unlock is
    // refused -- the answer face never appears for unsynced content.
    if (HandleProblemAppInput(&a, ProblemInput::kConfirm) != ESP_OK ||
        !require(!a.answer_unlocked, "unsynced content cannot unlock")) {
        return false;
    }
    a.current_loaded = true;
    a.current.problem_id = "00000000-0000-4000-8000-000000000100";
    if (HandleProblemAppInput(&a, ProblemInput::kConfirm) != ESP_OK ||
        !require(a.answer_unlocked, "confirm unlocks the answer") ||
        !require(
            SegmentFace(a, a.ring_segment) == ProblemFace::kAnswer,
            "unlock jumps to the answer face") ||
        !require(a.answer_scroll_lines == 0, "unlock starts at the answer top")) {
        return false;
    }
    if (HandleProblemAppInput(&a, ProblemInput::kConfirm) != ESP_OK ||
        !require(a.mode == ProblemAppMode::kVerdict, "confirm opens the verdict") ||
        !require(a.verdict_selected == 0, "verdict defaults to A")) {
        return false;
    }

    // UP/DOWN cycle through the four options; confirm arms exactly one
    // durable verdict with the selected action.
    if (HandleProblemAppInput(&a, ProblemInput::kUp) != ESP_OK ||
        !require(a.verdict_selected == 3, "verdict UP wraps to D") ||
        HandleProblemAppInput(&a, ProblemInput::kDown) != ESP_OK ||
        !require(a.verdict_selected == 0, "verdict DOWN wraps to A") ||
        HandleProblemAppInput(&a, ProblemInput::kDown) != ESP_OK ||
        HandleProblemAppInput(&a, ProblemInput::kConfirm) != ESP_OK ||
        !require(a.commit_state == ProblemVerdictCommitState::kPersisting,
                 "verdict arms a durable commit") ||
        !require(a.verdict_effect_ready, "verdict arms the effect") ||
        !require(a.pending_verdict.action == ReviewAction::kHesitant,
                 "B maps to hesitant") ||
        !require(a.mode == ProblemAppMode::kProblemView, "verdict returns to view")) {
        return false;
    }

    // Take assigns the request id exactly once and binds the dispatch's
    // operation id; while the commit is persisting, LongConfirm must not leave
    // the view (a late result would advance the wrong set) and set switches
    // are refused.
    DurableProblemObservation observation;
    if (!require(
            TakeProblemVerdictEffect(
                &a, "req_problem_selftest_01", "2026-07-26T00:00:00.000Z", 1u,
                &observation),
            "take verdict effect") ||
        !require(observation.action == ReviewAction::kHesitant, "effect action") ||
        !require(
            observation.problem_id == "00000000-0000-4000-8000-000000000100",
            "effect problem id") ||
        !require(a.pending_persist_operation_id == 1u,
                 "take binds the dispatch operation id")) {
        return false;
    }
    const size_t set_before = a.set_order;
    if (HandleProblemAppInput(&a, ProblemInput::kLongConfirm) != ESP_OK ||
        !require(a.mode == ProblemAppMode::kProblemView,
                 "long-press is blocked while persisting") ||
        !require(a.active, "problem layer stays active while persisting") ||
        !require(a.set_order == set_before, "set unchanged while persisting") ||
        !require(!ActivateProblemBrowse(&a, "00000000-0000-4000-8000-0000000000aa"),
                 "set switch refused while persisting") ||
        !require(a.set_order == set_before, "set unchanged after refused switch")) {
        return false;
    }
    const size_t before = a.list_selected;
    ApplyProblemVerdictCommitResult(&a, ESP_OK);
    if (!require(a.commit_state == ProblemVerdictCommitState::kCloudPending,
                 "commit result moves to cloud pending") ||
        !require(a.pending_persist_operation_id == 0,
                 "apply clears the bound operation id") ||
        !require(a.list_selected == before + 1, "commit advances to the next problem") ||
        !require(a.mode == ProblemAppMode::kProblemView, "advance stays in the view") ||
        !require(!a.answer_unlocked, "next problem opens locked")) {
        return false;
    }

    // Long-press escape chain: view -> list -> mixed notebook list.
    if (HandleProblemAppInput(&a, ProblemInput::kLongConfirm) != ESP_OK ||
        !require(a.mode == ProblemAppMode::kProblemList, "long-press exits to list") ||
        HandleProblemAppInput(&a, ProblemInput::kLongConfirm) != ESP_OK ||
        !require(!a.active, "long-press exits the problem layer")) {
        return false;
    }

    // The last problem's verdict ends the round back on the title list.
    ProblemPageFixture tail;
    if (!tail) return require(false, "allocate tail fixture");
    PopulateFixture(&tail.get(), 1);
    ProblemAppState& t = tail.get();
    if (!require(
            ActivateProblemBrowse(&t, "11111111-1111-4111-8111-111111111111"),
            "tail set activates")) {
        return false;
    }
    if (HandleProblemAppInput(&t, ProblemInput::kConfirm) != ESP_OK) return false;
    t.current_loaded = true;
    t.current.problem_id = "00000000-0000-4000-8000-000000000100";
    if (HandleProblemAppInput(&t, ProblemInput::kConfirm) != ESP_OK ||
        HandleProblemAppInput(&t, ProblemInput::kConfirm) != ESP_OK ||
        HandleProblemAppInput(&t, ProblemInput::kConfirm) != ESP_OK) {
        return false;
    }
    ApplyProblemVerdictCommitResult(&t, ESP_OK);
    if (!require(t.mode == ProblemAppMode::kProblemList, "last verdict ends the round") ||
        !require(t.active, "round end stays on the title list")) {
        return false;
    }

    // A failed local commit is terminal for the verdict (kFailed) and does
    // not advance.
    ProblemPageFixture fail;
    if (!fail) return require(false, "allocate failure fixture");
    PopulateFixture(&fail.get(), 2);
    ProblemAppState& f = fail.get();
    if (!require(
            ActivateProblemBrowse(&f, "11111111-1111-4111-8111-111111111111"),
            "failure set activates")) {
        return false;
    }
    if (HandleProblemAppInput(&f, ProblemInput::kConfirm) != ESP_OK) return false;
    f.current_loaded = true;
    f.current.problem_id = "00000000-0000-4000-8000-000000000100";
    if (HandleProblemAppInput(&f, ProblemInput::kConfirm) != ESP_OK ||
        HandleProblemAppInput(&f, ProblemInput::kConfirm) != ESP_OK ||
        HandleProblemAppInput(&f, ProblemInput::kConfirm) != ESP_OK) {
        return false;
    }
    ApplyProblemVerdictCommitResult(&f, ESP_FAIL);
    if (!require(f.commit_state == ProblemVerdictCommitState::kFailed,
                 "failed commit is terminal") ||
        !require(f.list_selected == 0, "failed commit does not advance")) {
        return false;
    }
    return true;
}

}  // namespace wqn
