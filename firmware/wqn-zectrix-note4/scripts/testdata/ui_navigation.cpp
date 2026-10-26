#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ui/navigation_model.h"
#include "ui/assets/navigation_icons.h"

#define CONFIG_WQN_AI_ENABLE 1
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_ARG = 1, ESP_ERR_NO_MEM = 2;
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
int allocations = 0, releases = 0;
bool allocation_allowed = true;
void* heap_caps_malloc(size_t size, int caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!allocation_allowed) return nullptr;
    ++allocations; return std::malloc(size);
}
void heap_caps_free(void* pointer) { if (pointer) ++releases; std::free(pointer); }
const char* esp_err_to_name(int) { return "test"; }
template <class... Args> void TestLog(Args...) {}
#define ESP_LOGW(...) TestLog(__VA_ARGS__)
#define ESP_LOGI(...) TestLog(__VA_ARGS__)
#define ESP_RETURN_ON_FALSE(condition, error, ...) do { if (!(condition)) return (error); } while (0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { const int err = (expr); if (err) return err; } while (0)

int checks = 0;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::abort(); } ++checks; } while (0)
struct Counters {
    int clamp = 0, note = 0, problem = 0, reset = 0, flash = 0;
    int disconnect = 0, leave = 0, list = 0, observe = 0;
    int todos = 0, words = 0, notes = 0, problems = 0, summary = 0, disarm = 0;
} calls;
std::array<uint8_t, 400 * 300> pixels;

namespace wqn {
enum class AiTier { kFlash, kStd, kAgent };
enum class AiSessionStatus { kIdle, kPreparingCapture, kListening, kTranscribing, kAwaitingConfirmation, kAwaitingDecision };
enum class AiFeaturePhase { kIdle, kRecording };
@@SETTINGS_DIALOG@@
enum class WordObservationCommitState { kIdle, kPersisting };
enum class NoteObservationCommitState { kIdle, kPersisting };
enum class ProblemVerdictCommitState { kIdle, kPersisting };
struct WordState {
    struct Session {
        WordObservationCommitState commit_state = WordObservationCommitState::kIdle;
        bool batch_flush_requested = false;
        int batch_retry_after_ms = 99;
    } session;
    bool buffered = false, cloud_sync_requested = false, scope_reset_pending_valid = false;
    std::string message, scoped_deck_id, scoped_deck_title;
    std::string scope_reset_pending_deck_id, scope_reset_pending_deck_title;
    unsigned scope_reset_save_op_id = 0;
};
struct NoteState {
    struct { NoteObservationCommitState commit_state = NoteObservationCommitState::kIdle; } session;
    bool cloud_sync_requested = false;
    std::string message;
};
struct ProblemState {
    bool active = false, cloud_sync_requested = false;
    ProblemVerdictCommitState commit_state = ProblemVerdictCommitState::kIdle;
    std::string message;
};
struct TimeState { bool editing = false; };
struct UiState {
    UiScreen screen = UiScreen::kHome;
    size_t selected_home_task = 0;
    NavigationState navigation;
    WordState word_app;
    NoteState note_app;
    ProblemState problem_app;
    TimeState time_app;
    struct { AiTier tier = AiTier::kStd; AiSessionStatus status = AiSessionStatus::kIdle; } ai;
    struct {
        bool session_locked = true, stream_active = false, option = false;
        std::string current_session_id;
        struct { AiFeaturePhase phase = AiFeaturePhase::kIdle; } ui;
    } agent;
    struct { SettingsDialog dialog = SettingsDialog::kNone; bool sync_confirm_pending = false; unsigned volume_save_op_id = 0; } settings;
    struct { bool active = false; } status_edit;
    struct { bool flash_ptt_started = false, agent_ptt_started = false, std_ptt_started = false; int last_ai_confirm_tap_ms = 0; } gestures;
};
struct UiFrame {
    UiScreen screen = UiScreen::kHome;
    NavigationState navigation;
    bool prefer_full_refresh = false;
    struct { AiTier tier = AiTier::kStd; } ai;
};
bool provisioning = false;
bool IsProvisioningActive() { return provisioning; }
bool HasBufferedWordObservations(const WordState& word) { return word.buffered; }
bool TimeAppIsEditingValue(const TimeState& time) { return time.editing; }
void DisarmTimeAppAction(TimeState*) { ++calls.disarm; }
void ClampUiSelection(UiState*) { ++calls.clamp; }
void ReleaseNoteImagePayload(NoteState*) { ++calls.note; }
void ReleaseProblemImagePayload(ProblemState*) { ++calls.problem; }
void StopFlashSession() { ++calls.flash; }
void LeaveStdAiTier() {}
void LeaveOpenCodeAgentTier() { ++calls.leave; }
int RequestOpenCodeSessionList() { ++calls.list; return ESP_OK; }
int ObserveOpenCodeSession() { ++calls.observe; return ESP_OK; }
namespace stdpro_ws { void RequestIdleDisconnect() { ++calls.disconnect; } }
namespace services {
enum class SyncContentDomain { kWordPacks, kNotePacks, kProblemPacks };
void RequestContentRefresh(SyncContentDomain) {}
}
constexpr int kEpdWidth = 400, kEpdHeight = 300, kEpdBytesPerRow = 50;
constexpr size_t kEpdGray4PayloadSize = 60000;
std::array<uint8_t, 15000> framebuffer;
uint8_t* g_framebuffer = framebuffer.data();
@@READBACK@@
void ClearEpdFramebuffer(bool white) {
    framebuffer.fill(white ? 0xff : 0);
    pixels.fill(!white);
}
void DrawEpdPixel(int x, int y, bool ink) {
    assert(x >= 0 && x < 400 && y >= 0 && y < 300);
    pixels[y * 400 + x] = ink;
    const uint8_t mask = 0x80U >> (x & 7);
    if (ink) framebuffer[y * 50 + x / 8] &= ~mask;
    else framebuffer[y * 50 + x / 8] |= mask;
}
std::vector<uint8_t> gray_presented;
int RefreshEpdGray16(const uint8_t* data, size_t size) {
    gray_presented.assign(data, data + size); return ESP_OK;
}
int hardware_refreshes = 0;
bool hardware_local = false, hardware_full = false;
int hardware_result = ESP_OK;
int RefreshEpdFull(bool local, bool full) {
    ++hardware_refreshes; hardware_local = local; hardware_full = full; return hardware_result;
}
}

namespace device_ui_internal {
unsigned SubmitWordSessionReset(const std::string&) { ++calls.reset; return 7; }
}
namespace wqn {
@@TRANSITIONS@@
}

namespace device_ui_internal {
@@SCHEDULE@@
@@GEOMETRY@@
enum class AgentOptionMode { kNone, kPermission };
AgentOptionMode AgentOptionModeFor(const decltype(wqn::UiState::agent)& agent) {
    return agent.option ? AgentOptionMode::kPermission : AgentOptionMode::kNone;
}
void RefreshTodosFromCloud(wqn::UiState*) { ++calls.todos; }
bool QueueWordReviewRefresh() { ++calls.words; return true; }
bool QueueNotePackSync() { ++calls.notes; return true; }
bool QueueProblemPackSync() { ++calls.problems; return true; }
bool IsWordCloudBusy() { return false; }
bool IsNoteCloudBusy() { return false; }
void BuildHomeSummary(wqn::UiState*) { ++calls.summary; }
@@INPUT@@
RefreshSchedule Dispatch(const wqn::ButtonEvent& event, int64_t event_time_ms, wqn::UiState* state) {
@@BRIDGE@@
    return RefreshSchedule::kNone;
}

struct Label { std::string text; bool ink; };
std::vector<Label> labels;
void FillRect(int x, int y, int width, int height, bool ink) {
    for (int yy = y; yy < y + height; ++yy) for (int xx = x; xx < x + width; ++xx) {
        wqn::DrawEpdPixel(xx, yy, ink);
    }
}
void FillRoundedRect(int, int, int, int, int, bool black = true);
@@ROUND@@

@@LINES@@
@@OUTLINE@@
enum class SelectionStyle { kInvert };
void DrawSelectionDecoration(int x, int y, int w, int h, SelectionStyle) { FillRoundedRect(x, y, w, h, 4); }
void DrawWqnBitmapAsset(int x, int y, const WqnBitmapAsset& asset, bool ink) {
    for (int yy = 0; yy < asset.height; ++yy) for (int xx = 0; xx < asset.width; ++xx)
        if (WqnBitmapPixel(asset, xx, yy)) FillRect(x + xx, y + yy, 1, 1, ink);
}
int DrawCenteredText(int, int, int, const std::string& text, bool ink) { labels.push_back({text, ink}); return ESP_OK; }
@@VIEW@@
@@GRAY@@
constexpr const char* kTag = "host";
wqn::UiScreen g_last_rendered_screen = wqn::UiScreen::kHome;
const char* RefreshScheduleName(RefreshSchedule) { return "test"; }
bool ScheduleAllowsWindowedPartial(RefreshSchedule schedule) {
    return schedule == RefreshSchedule::kClock || schedule == RefreshSchedule::kTimer || schedule == RefreshSchedule::kConfig;
}
@@REFRESH@@
}

using namespace wqn;
using namespace device_ui_internal;
int64_t tick = 1000;
NavigationAction Input(NavigationState& state, UiScreen screen, ButtonId button, ButtonEventType type, bool can_open = true, bool repeat = false) {
    ButtonEvent event;
    event.button = button; event.type = type; event.repeat = repeat;
    return ReduceNavigationInput(&state, screen, event, ++tick, can_open);
}
void Open(NavigationState& state, UiScreen screen) {
    Input(state, screen, ButtonId::kUp, ButtonEventType::kPress);
    CHECK(Input(state, screen, ButtonId::kUp, ButtonEventType::kLongPress) == NavigationAction::kRepaint);
    CHECK(Input(state, screen, ButtonId::kUp, ButtonEventType::kLongPress, true, true) == NavigationAction::kConsumed);
    CHECK(Input(state, screen, ButtonId::kUp, ButtonEventType::kLongRelease) == NavigationAction::kConsumed);
    Input(state, screen, ButtonId::kUp, ButtonEventType::kRelease);
}
NavigationAction Click(NavigationState& state, UiScreen screen, ButtonId button, bool double_press = false) {
    Input(state, screen, button, ButtonEventType::kPress);
    auto action = Input(state, screen, button, double_press ? ButtonEventType::kDoublePress : ButtonEventType::kShortPress);
    Input(state, screen, button, ButtonEventType::kRelease);
    return action;
}

void CheckInputAndTransitions() {
    for (const auto& origin : kNavigationPages) for (size_t destination = 0; destination < kNavigationPages.size(); ++destination) {
        calls = {};
        UiState state;
        state.screen = origin.screen;
        Open(state.navigation, origin.screen);
        CHECK(state.navigation.selected == NavigationPageIndex(origin.screen));
        for (size_t step = 0; state.navigation.selected != destination && step < 7; ++step) {
            CHECK(Click(state.navigation, origin.screen, ButtonId::kDownPower, step % 2) == NavigationAction::kRepaint);
            CHECK(state.screen == origin.screen);
            CHECK(calls.clamp == 0 && calls.todos == 0 && calls.list == 0);
        }
        CHECK(Click(state.navigation, origin.screen, ButtonId::kConfirm) == NavigationAction::kConfirm);
        CHECK(!state.navigation.visible);
        CHECK(NavigateUiToScreen(&state, kNavigationPages[destination].screen));
        CHECK(state.screen == kNavigationPages[destination].screen);
        CHECK(calls.clamp == (origin.screen != state.screen));
        CHECK(calls.note == (origin.screen == UiScreen::kNote && state.screen != origin.screen));
        CHECK(calls.problem == calls.note);
        CHECK(calls.leave == (origin.screen == UiScreen::kAi && state.screen != origin.screen));
        CHECK(AdjacentNavigationPage(AdjacentNavigationPage(origin.screen, 1), -1) == origin.screen);
    }
    NavigationState nav;
    Open(nav, UiScreen::kHome);
    CHECK(Click(nav, UiScreen::kHome, ButtonId::kUp) == NavigationAction::kRepaint && nav.selected == 6);
    CHECK(Click(nav, UiScreen::kHome, ButtonId::kDownPower) == NavigationAction::kRepaint && nav.selected == 0);
    Input(nav, UiScreen::kHome, ButtonId::kDownPower, ButtonEventType::kPress);
    CHECK(Input(nav, UiScreen::kHome, ButtonId::kDownPower, ButtonEventType::kLongPress) == NavigationAction::kRepaint);
    CHECK(!nav.visible);
    CHECK(Input(nav, UiScreen::kHome, ButtonId::kDownPower, ButtonEventType::kLongPress, true, true) == NavigationAction::kConsumed);
    Input(nav, UiScreen::kHome, ButtonId::kDownPower, ButtonEventType::kRelease);
    CHECK(Input(nav, UiScreen::kHome, ButtonId::kUp, ButtonEventType::kLongPress, false) == NavigationAction::kPass);
    CHECK(Input(nav, UiScreen::kProvisioning, ButtonId::kUp, ButtonEventType::kLongPress) == NavigationAction::kPass);
    Open(nav, UiScreen::kAi);
    CHECK(Click(nav, UiScreen::kAi, ButtonId::kConfirm) == NavigationAction::kConfirm);
    CHECK(Input(nav, UiScreen::kAi, ButtonId::kConfirm, ButtonEventType::kPress) == NavigationAction::kConsumed);
    CHECK(Input(nav, UiScreen::kAi, ButtonId::kConfirm, ButtonEventType::kHoldPress) == NavigationAction::kConsumed);
    CHECK(Input(nav, UiScreen::kAi, ButtonId::kConfirm, ButtonEventType::kDoublePress) == NavigationAction::kConsumed);
    Input(nav, UiScreen::kAi, ButtonId::kConfirm, ButtonEventType::kRelease);
    tick += kButtonDoublePressWindowMs + 1;
    CHECK(Input(nav, UiScreen::kAi, ButtonId::kConfirm, ButtonEventType::kPress) == NavigationAction::kPass);
    // A Confirm already held when the menu opens belongs to the old page.
    nav = {};
    Input(nav, UiScreen::kWord, ButtonId::kConfirm, ButtonEventType::kPress);
    Open(nav, UiScreen::kWord);
    CHECK(Input(nav, UiScreen::kWord, ButtonId::kConfirm, ButtonEventType::kShortPress) == NavigationAction::kConsumed);
    CHECK(nav.visible);
    Input(nav, UiScreen::kWord, ButtonId::kConfirm, ButtonEventType::kRelease);
    // A fresh long Confirm inside the menu never starts PTT or confirms.
    Input(nav, UiScreen::kWord, ButtonId::kConfirm, ButtonEventType::kPress);
    CHECK(Input(nav, UiScreen::kWord, ButtonId::kConfirm, ButtonEventType::kHoldPress) == NavigationAction::kConsumed);
    CHECK(Input(nav, UiScreen::kWord, ButtonId::kConfirm, ButtonEventType::kLongPress) == NavigationAction::kConsumed);
    CHECK(Input(nav, UiScreen::kWord, ButtonId::kConfirm, ButtonEventType::kLongRelease) == NavigationAction::kConsumed);
    Input(nav, UiScreen::kWord, ButtonId::kConfirm, ButtonEventType::kRelease);
    CHECK(nav.visible);
}

void CheckGatesAndEffects() {
    UiState state;
    for (int domain = 0; domain < 4; ++domain) {
        state = {}; calls = {};
        state.screen = domain < 2 ? UiScreen::kWord : UiScreen::kNote;
        if (domain == 0) state.word_app.session.commit_state = WordObservationCommitState::kPersisting;
        if (domain == 1) state.word_app.buffered = true;
        if (domain == 2) state.note_app.session.commit_state = NoteObservationCommitState::kPersisting;
        if (domain == 3) { state.problem_app.active = true; state.problem_app.commit_state = ProblemVerdictCommitState::kPersisting; }
        const UiScreen before = state.screen;
        CHECK(!NavigateUiToScreen(&state, UiScreen::kHome));
        CHECK(state.screen == before && calls.clamp == 0 && calls.reset == 0);
        CHECK(NavigateUiToScreen(&state, before));
        CHECK(domain < 2 ? !state.word_app.message.empty() : !state.note_app.message.empty());
        if (domain < 2) CHECK(state.word_app.session.batch_flush_requested);
    }
    state = {}; calls = {}; state.screen = UiScreen::kWord; state.word_app.scoped_deck_id = "deck";
    CHECK(NavigateUiToScreen(&state, UiScreen::kHome));
    CHECK(calls.reset == 1 && state.word_app.scoped_deck_id.empty());
    CHECK(state.word_app.scope_reset_pending_valid && state.word_app.scope_reset_save_op_id == 7);
    state = {}; calls = {}; state.screen = UiScreen::kAi; state.ai.tier = AiTier::kFlash;
    CHECK(NavigateUiToScreen(&state, UiScreen::kHome));
    CHECK(calls.flash == 1 && calls.disconnect == 1 && calls.leave == 1);
    state = {}; calls = {}; state.ai.tier = AiTier::kAgent;
    CHECK(NavigateUiToScreen(&state, UiScreen::kAi)); CHECK(calls.list == 1);
    state = {}; calls = {}; state.ai.tier = AiTier::kAgent; state.agent.current_session_id = "session";
    CHECK(NavigateUiToScreen(&state, UiScreen::kAi)); CHECK(calls.observe == 1);

    state = {}; CHECK(CanOpenNavigation(state));
    provisioning = true; CHECK(!CanOpenNavigation(state)); provisioning = false;
    // Every settings subpage is a full-page surface, not an input modal.
    state.screen = UiScreen::kSettings; CHECK(CanOpenNavigation(state));
    for (int dialog = 1; dialog <= static_cast<int>(SettingsDialog::kProvisioningResult); ++dialog) {
        state = {}; state.screen = UiScreen::kSettings;
        state.settings.dialog = static_cast<SettingsDialog>(dialog);
        state.settings.sync_confirm_pending = true;
        state.settings.volume_save_op_id = 47; // An admitted worker keeps ownership.
        CHECK(CanOpenNavigation(state));
        CHECK(Dispatch({ButtonId::kDownPower, ButtonEventType::kPress}, ++tick, &state) == RefreshSchedule::kNone);
        CHECK(Dispatch({ButtonId::kDownPower, ButtonEventType::kLongPress}, ++tick, &state) == RefreshSchedule::kNavigation);
        CHECK(state.navigation.visible && state.screen == UiScreen::kSettings);
        CHECK(state.settings.dialog == static_cast<SettingsDialog>(dialog));
        Dispatch({ButtonId::kDownPower, ButtonEventType::kLongRelease}, ++tick, &state);
        Dispatch({ButtonId::kDownPower, ButtonEventType::kRelease}, ++tick, &state);
        state.navigation.selected = NavigationPageIndex(UiScreen::kHome);
        Dispatch({ButtonId::kConfirm, ButtonEventType::kPress}, ++tick, &state);
        CHECK(Dispatch({ButtonId::kConfirm, ButtonEventType::kShortPress}, ++tick, &state) == RefreshSchedule::kCommit);
        CHECK(state.screen == UiScreen::kHome && !state.navigation.visible);
        CHECK(!state.settings.sync_confirm_pending && state.settings.volume_save_op_id == 47);
        CHECK(state.settings.dialog == static_cast<SettingsDialog>(dialog));
        provisioning = true; CHECK(!CanOpenNavigation(state)); provisioning = false;
    }
    state = {}; state.screen = UiScreen::kTime; state.time_app.editing = true; CHECK(!CanOpenNavigation(state));
    for (int mode = 0; mode < 6; ++mode) {
        state = {}; state.screen = UiScreen::kAi;
        if (mode == 0) state.status_edit.active = true;
        if (mode == 1) state.gestures.flash_ptt_started = true;
        if (mode == 2) state.ai.status = AiSessionStatus::kPreparingCapture;
        if (mode == 3) state.ai.status = AiSessionStatus::kListening;
        state.ai.tier = mode >= 4 ? AiTier::kAgent : AiTier::kStd;
        if (mode == 4) state.agent.option = true;
        if (mode == 5) state.agent.ui.phase = AiFeaturePhase::kRecording;
        CHECK(!CanOpenNavigation(state));
    }
    for (auto pending : {AiSessionStatus::kTranscribing, AiSessionStatus::kAwaitingConfirmation, AiSessionStatus::kAwaitingDecision}) {
        state = {}; state.screen = UiScreen::kAi; state.ai.tier = AiTier::kStd; state.ai.status = pending;
        CHECK(!CanOpenNavigation(state));
    }
    state = {}; state.screen = UiScreen::kAi; state.gestures.std_ptt_started = true;
    CHECK(!CanOpenNavigation(state));
    state = {}; state.screen = UiScreen::kAi; state.ai.tier = AiTier::kAgent;
    state.agent.session_locked = false;
    CHECK(CanOpenNavigation(state)); // Empty session lists have no local Back gesture.
    // Run the real input bridge: opening and previewing never enter Todo;
    // confirming enters it once, with no calls for intermediate pages.
    state = {}; calls = {};
    ButtonEvent event; event.button = ButtonId::kUp; event.type = ButtonEventType::kLongPress;
    CHECK(Dispatch(event, 1000, &state) == RefreshSchedule::kNavigation);
    CHECK(state.navigation.visible && calls.summary == 0);
    event.type = ButtonEventType::kRelease; Dispatch(event, 1100, &state);
    event.button = ButtonId::kDownPower; event.type = ButtonEventType::kShortPress;
    for (int i = 0; i < 4; ++i) Dispatch(event, 1200 + i * 100, &state);
    CHECK(state.screen == UiScreen::kHome && calls.todos == 0 && calls.words == 0 && calls.notes == 0);
    event.button = ButtonId::kConfirm;
    CHECK(Dispatch(event, 1800, &state) == RefreshSchedule::kCommit);
    CHECK(state.screen == UiScreen::kTodo && calls.todos == 1 && calls.summary == 1);
    state = {}; calls = {}; state.screen = UiScreen::kWord;
    state.word_app.session.commit_state = WordObservationCommitState::kPersisting;
    event.button = ButtonId::kUp; event.type = ButtonEventType::kLongPress;
    CHECK(Dispatch(event, 3000, &state) == RefreshSchedule::kNavigation);
    event.type = ButtonEventType::kRelease; Dispatch(event, 3100, &state);
    event.type = ButtonEventType::kShortPress;
    Dispatch(event, 3200, &state); Dispatch(event, 3300, &state);
    event.button = ButtonId::kConfirm;
    CHECK(Dispatch(event, 3400, &state) == RefreshSchedule::kNavigation);
    CHECK(state.screen == UiScreen::kWord && !state.navigation.visible);
    CHECK(!state.word_app.message.empty() && calls.clamp == 0 && calls.summary == 0);
}

void CheckComposition() {
    for (size_t candidate = 0; candidate < kNavigationPages.size(); ++candidate) {
        pixels.fill(1); labels.clear();
        UiFrame frame; frame.screen = UiScreen::kNote; frame.navigation.visible = true;
        frame.navigation.selected = candidate;
        CHECK(DrawNavigationOverlay(frame) == ESP_OK);
        CHECK(labels.size() == 7);
        for (size_t i = 0; i < labels.size(); ++i) {
            CHECK(labels[i].text == kNavigationPages[i].label);
            CHECK(labels[i].ink == (i != candidate));
        }
        bool outside_unchanged = true;
        for (int y = 0; y < 300; ++y) for (int x = 0; x < 400; ++x)
            if (x < 8 || x >= 392 || y < 214 || y >= 292)
                outside_unchanged &= pixels[y * 400 + x] == 1;
        CHECK(outside_unchanged);
        CHECK(pixels[285 * 400 + 200] == 1);
        CHECK(pixels[285 * 400 + 44] == 0);
    }
    UiFrame frame; frame.prefer_full_refresh = true;
    labels.clear();
    g_last_rendered_screen = frame.screen;
    CHECK(RefreshFrame(frame, RefreshSchedule::kNavigation) == ESP_OK);
    CHECK(!hardware_full && !hardware_local && labels.empty());
    frame.navigation.visible = true;
    CHECK(RefreshFrame(frame, RefreshSchedule::kNavigation) == ESP_OK);
    CHECK(labels.size() == 7 && !hardware_full && !hardware_local);
    CHECK(RefreshFrame(frame, RefreshSchedule::kCommit) == ESP_OK && hardware_full);
    frame.prefer_full_refresh = false; frame.screen = UiScreen::kNote;
    CHECK(RefreshFrame(frame, RefreshSchedule::kNavigation) == ESP_OK && hardware_full);
}

void CheckGrayComposition() {
    std::vector<uint8_t> source(wqn::kEpdGray4PayloadSize);
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>(i);
    const auto original = source;
    UiFrame frame; frame.screen = UiScreen::kNote;
    CHECK(RefreshNavigationGrayImage(frame, source.data(), source.size()) == ESP_OK);
    CHECK(gray_presented == source && allocations == 0);
    frame.navigation.visible = true; frame.navigation.selected = 5;
    wqn::ClearEpdFramebuffer(false); // Dirty old image must not leak into corners.
    CHECK(RefreshNavigationGrayImage(frame, source.data(), source.size()) == ESP_OK);
    CHECK(allocations == 1 && releases == 1 && source == original);
    bool outside_unchanged = true;
    for (int y = 0; y < 300; ++y) for (int x = 0; x < 400; ++x) {
        if (x >= 8 && x < 392 && y >= 214 && y < 292) continue;
        const size_t i = (y * 400 + x) / 2;
        const int shift = (x & 1) ? 0 : 4;
        outside_unchanged &= ((gray_presented[i] >> shift) & 15) == ((source[i] >> shift) & 15);
    }
    CHECK(outside_unchanged);
    bool all_ink_composed = true;
    for (int y = 214; y < 292; ++y) for (int x = 8; x < 392; ++x) {
        if (!wqn::ReadEpdPixel(x, y)) continue;
        const int shift = (x & 1) ? 0 : 4;
        all_ink_composed &= ((gray_presented[(y * 400 + x) / 2] >> shift) & 15) == 0;
    }
    CHECK(all_ink_composed); // Includes the actual midpoint-circle outline.
    CHECK(!wqn::ReadEpdPixel(8, 214));
    CHECK(gray_presented[(214 * 400 + 8) / 2] == source[(214 * 400 + 8) / 2]);
    CHECK((gray_presented[(230 * 400 + 282) / 2] >> 4) == 0); // AI focus fill.
    CHECK((gray_presented[(230 * 400 + 22) / 2] >> 4) == 15); // Other tile is white.
    CHECK(!wqn::ReadEpdPixel(-1, 0) && !wqn::ReadEpdPixel(400, 0));
    CHECK(!wqn::ReadEpdPixel(0, -1) && !wqn::ReadEpdPixel(0, 300));
    frame.navigation.visible = false;
    CHECK(RefreshNavigationGrayImage(frame, source.data(), source.size()) == ESP_OK);
    CHECK(gray_presented == source && allocations == 1 && releases == 1);
    frame.navigation.visible = true;
    CHECK(RefreshNavigationGrayImage(frame, nullptr, source.size()) == ESP_ERR_INVALID_ARG);
    allocation_allowed = false;
    CHECK(RefreshNavigationGrayImage(frame, source.data(), source.size()) == ESP_ERR_NO_MEM);
    CHECK(releases == allocations && source == original);
}

int main() {
    CheckInputAndTransitions();
    CheckGatesAndEffects();
    CheckComposition();
    CheckGrayComposition();
    std::printf("PASS: navigation input, transitions, modal gates, composition and refresh (%d assertions)\n", checks);
}
