#include "ui/ui_gates_selftest.h"

#include <new>

#include "button_input.h"
#include "esp_log.h"
#include "note_app.h"
#include "problem_app.h"
#include "ui_internal.h"
#include "ui_model.h"
#include "word_app.h"

namespace {

constexpr char kTag[] = "ui_gates";

using UiState = wqn::UiState;
using UiStateRef = wqn::UiState&;

// A wqn::UiState is 6576 bytes and this self-test runs on the IDF main task,
// whose stack is CONFIG_ESP_MAIN_TASK_STACK_SIZE = 8192. A stack local would
// leave ~1.6 KiB for the entire reducer chain underneath it, which is exactly
// what overflowed the device into a reboot loop on the first HIL run (the
// top-navigation case drives one of the deepest chains). The state therefore
// lives in a static buffer and is reconstructed per case with placement new:
// no stack cost, no heap, and every case starts pristine.
alignas(UiState) static unsigned char g_state_bytes[sizeof(UiState)];
static UiState* g_state = nullptr;

UiStateRef FreshState()
{
    if (g_state != nullptr) {
        g_state->~UiState();
    }
    g_state = new (g_state_bytes) UiState();
    return *g_state;
}
// Every case drives the real button entry point with a hand-built UiState, so
// what is under test is the production reducer and not a copy of its logic.
// Nothing here may set up a state whose gate is expected to FAIL: the un-gated
// path performs real storage work (session clears, an NVS cursor write) or a
// factory reset, and this self-test runs before any of that is safe.

wqn::ButtonEvent Press(wqn::ButtonId button, wqn::ButtonEventType type)
{
    wqn::ButtonEvent event;
    event.button = button;
    event.type = type;
    event.duration_ms = type == wqn::ButtonEventType::kLongPress ? 1000 : 50;
    event.occurred_at_ms = 0;
    event.seq = 0;
    event.repeat = false;
    return event;
}

bool Case(const char* name, bool passed)
{
    if (!passed) {
        ESP_LOGE(kTag, "gate self-test failed: %s", name);
    }
    return passed;
}

// Reports the FIRST failing sub-condition, so a broken gate names the exact
// property that broke in the boot log instead of only the case name. The
// word-row case failed once with a 38-character deck id (the loader wants
// exactly 36) and the log could only say "this case failed", which cost a
// round trip to diagnose.
bool Check(const char* name, const char* condition, bool passed)
{
    if (!passed) {
        ESP_LOGE(kTag, "gate self-test failed: %s: %s", name, condition);
    }
    return passed;
}

// AGENTS 4.2: the settings-page Confirm must refuse while a word observation is
// mid-commit, and must not open the dialog it would otherwise open.
bool CheckSettingsRowConfirmGateDuringWordPersist()
{
    wqn::UiState& state = FreshState();
    state.screen = wqn::UiScreen::kSettings;
    state.settings.selected = 0;
    state.settings.dialog = wqn::SettingsDialog::kNone;
    state.word_app.session.commit_state = wqn::WordObservationCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kConfirm, wqn::ButtonEventType::kShortPress), 0, &state);

    const char* name = "settings-row Confirm refused while a word commit is persisting";
    return Case(
        name,
        Check(name, "no dialog opened", state.settings.dialog == wqn::SettingsDialog::kNone) &&
            Check(name, "user told to wait", state.settings.notice == "正在保存，请稍后") &&
            Check(name, "screen unchanged", state.screen == wqn::UiScreen::kSettings));
}

// Same window, different domain: a problem verdict mid-commit must also hold
// the settings page shut.
bool CheckSettingsRowConfirmGateDuringProblemPersist()
{
    wqn::UiState& state = FreshState();
    state.screen = wqn::UiScreen::kSettings;
    state.settings.selected = 0;
    state.settings.dialog = wqn::SettingsDialog::kNone;
    state.problem_app.commit_state = wqn::ProblemVerdictCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kConfirm, wqn::ButtonEventType::kShortPress), 0, &state);

    const char* name = "settings-row Confirm refused while a problem verdict is persisting";
    return Case(
        name,
        Check(name, "no dialog opened", state.settings.dialog == wqn::SettingsDialog::kNone) &&
            Check(name, "user told to wait", state.settings.notice == "正在保存，请稍后"));
}

// The factory-reset dialog is the one path that would destroy data rather than
// merely stall, so it must honour the full §4.2 window and not just the worker
// flag. A regression here reboots the device mid-observation, which is the
// loudest possible failure -- this test cannot run unless the gate holds.
bool CheckFactoryResetGateDuringArmedCommit()
{
    wqn::UiState& state = FreshState();
    state.screen = wqn::UiScreen::kSettings;
    state.settings.dialog = wqn::SettingsDialog::kFactoryReset;
    state.note_app.session.commit_state = wqn::NoteObservationCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kConfirm, wqn::ButtonEventType::kLongPress), 0, &state);

    const char* name = "factory reset refused while a note commit is persisting";
    return Case(
        name,
        Check(
            name,
            "reset refused with a wait notice",
            state.settings.notice == "正在保存，请稍后") &&
            Check(name, "screen unchanged", state.screen == wqn::UiScreen::kSettings));
}

// AGENTS 4.2, the [词]-row path: scoping to a deck runs the synchronous word
// session reset, so it must be refused (and the request consumed so it cannot
// re-fire later) while a word commit is persisting.
//
// A deck id must be exactly 36 characters, or TakeNoteWordDeckOpenRequest
// rejects the request before the gate under test is ever reached -- the request
// is dropped and the refusal path is never exercised. The length is asserted at
// compile time because getting it wrong fails the test in a way that looks like
// a broken gate rather than a broken fixture.
constexpr char kGateTestDeckId[] = "0123456789abcdef0123456789abcdef0123";
static_assert(sizeof(kGateTestDeckId) - 1 == 36, "deck id must be exactly 36 chars");

bool CheckWordRowScopeGateDuringPersist()
{
    wqn::UiState& state = FreshState();
    state.screen = wqn::UiScreen::kNote;
    // Pre-initialised on purpose: HandleUiInput routes kConfirm on the note
    // screen through HandleNoteAppInput, which would otherwise run InitNoteApp
    // and do real storage I/O (note pack index, outbox snapshot) from a boot
    // self-test that runs before the UI exists. kNotebookList is the mode
    // InitNoteApp selects, and an empty pack index makes the list Confirm a
    // no-op that only sets a message.
    state.note_app.initialized = true;
    state.note_app.mode = wqn::NoteAppMode::kNotebookList;
    state.note_app.word_deck_open_requested = true;
    state.note_app.requested_word_deck_id = kGateTestDeckId;
    state.word_app.session.commit_state = wqn::WordObservationCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kConfirm, wqn::ButtonEventType::kShortPress), 0, &state);

    const char* name = "word-row scope switch refused while a word commit is persisting";
    return Case(
        name,
        Check(name, "screen unchanged", state.screen == wqn::UiScreen::kNote) &&
            Check(name, "no deck scope armed", state.word_app.scoped_deck_id.empty()) &&
            Check(name, "request consumed", !state.note_app.word_deck_open_requested) &&
            Check(name, "user told to wait", state.note_app.message == "正在保存，请稍后"));
}

// AGENTS 4.2: top navigation away from a scoped word page. The guard lives
// before the screen switch, so a refusal leaves the screen untouched.
bool CheckTopNavigationGateDuringWordPersist()
{
    wqn::UiState& state = FreshState();
    state.screen = wqn::UiScreen::kWord;
    state.word_app.scoped_deck_id = "deck-1";
    state.word_app.session.commit_state = wqn::WordObservationCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kUp, wqn::ButtonEventType::kLongPress), 0, &state);

    const char* name = "top navigation refused while a scoped word commit is persisting";
    return Case(
        name,
        Check(name, "screen unchanged", state.screen == wqn::UiScreen::kWord) &&
            Check(name, "scope kept", state.word_app.scoped_deck_id == "deck-1") &&
            Check(name, "user told to wait", !state.word_app.message.empty()));
}

// Same contract for a problem verdict reached from the note screen: a long
// UP/DOWN maps straight to top navigation and would bypass the in-layer gate.
bool CheckTopNavigationGateDuringProblemPersist()
{
    wqn::UiState& state = FreshState();
    state.screen = wqn::UiScreen::kNote;
    state.problem_app.active = true;
    state.problem_app.commit_state = wqn::ProblemVerdictCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kDownPower, wqn::ButtonEventType::kLongPress), 0, &state);

    const char* name = "top navigation refused while a problem verdict is persisting";
    return Case(
        name,
        Check(name, "screen unchanged", state.screen == wqn::UiScreen::kNote) &&
            Check(name, "user told to wait", !state.problem_app.message.empty()));
}

}  // namespace

namespace wqn {

bool RunUiGateSelfTest()
{
    // &= and not &&: every case must run so the boot log names every broken
    // gate rather than only the first one.
    bool ok = true;
    ok &= CheckSettingsRowConfirmGateDuringWordPersist();
    ok &= CheckSettingsRowConfirmGateDuringProblemPersist();
    ok &= CheckFactoryResetGateDuringArmedCommit();
    ok &= CheckWordRowScopeGateDuringPersist();
    ok &= CheckTopNavigationGateDuringWordPersist();
    ok &= CheckTopNavigationGateDuringProblemPersist();
    if (ok) {
        ESP_LOGI(kTag, "commit_state gate self-test passed");
    }
    return ok;
}

}  // namespace wqn
