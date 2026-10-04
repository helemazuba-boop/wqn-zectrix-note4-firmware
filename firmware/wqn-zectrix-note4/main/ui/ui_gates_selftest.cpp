#include "ui/ui_gates_selftest.h"

#include "button_input.h"
#include "esp_log.h"
#include "note_app.h"
#include "problem_app.h"
#include "ui_internal.h"
#include "ui_model.h"
#include "word_app.h"

namespace {

constexpr char kTag[] = "ui_gates";

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

wqn::UiState MakeState()
{
    wqn::UiState state;
    // A fresh UiState is a valid, idle device: every commit_state is kIdle and
    // nothing is armed.
    return state;
}

bool Case(const char* name, bool passed)
{
    if (!passed) {
        ESP_LOGE(kTag, "gate self-test failed: %s", name);
    }
    return passed;
}

// AGENTS 4.2: the settings-page Confirm must refuse while a word observation is
// mid-commit, and must not open the dialog it would otherwise open.
bool CheckSettingsRowConfirmGateDuringWordPersist()
{
    wqn::UiState state = MakeState();
    state.screen = wqn::UiScreen::kSettings;
    state.settings.selected = 0;
    state.settings.dialog = wqn::SettingsDialog::kNone;
    state.word_app.session.commit_state = wqn::WordObservationCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kConfirm, wqn::ButtonEventType::kShortPress), 0, &state);

    return Case("settings-row Confirm refused while a word commit is persisting",
                state.settings.dialog == wqn::SettingsDialog::kNone &&
                    state.settings.notice == "正在保存，请稍后" &&
                    state.screen == wqn::UiScreen::kSettings);
}

// Same window, different domain: a problem verdict mid-commit must also hold
// the settings page shut.
bool CheckSettingsRowConfirmGateDuringProblemPersist()
{
    wqn::UiState state = MakeState();
    state.screen = wqn::UiScreen::kSettings;
    state.settings.selected = 0;
    state.settings.dialog = wqn::SettingsDialog::kNone;
    state.problem_app.commit_state = wqn::ProblemVerdictCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kConfirm, wqn::ButtonEventType::kShortPress), 0, &state);

    return Case("settings-row Confirm refused while a problem verdict is persisting",
                state.settings.dialog == wqn::SettingsDialog::kNone &&
                    state.settings.notice == "正在保存，请稍后");
}

// The factory-reset dialog is the one path that would destroy data rather than
// merely stall, so it must honour the full §4.2 window and not just the worker
// flag. A regression here reboots the device mid-observation, which is the
// loudest possible failure -- this test cannot run unless the gate holds.
bool CheckFactoryResetGateDuringArmedCommit()
{
    wqn::UiState state = MakeState();
    state.screen = wqn::UiScreen::kSettings;
    state.settings.dialog = wqn::SettingsDialog::kFactoryReset;
    state.note_app.session.commit_state = wqn::NoteObservationCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kConfirm, wqn::ButtonEventType::kLongPress), 0, &state);

    return Case("factory reset refused while a note commit is persisting",
                state.settings.notice == "正在保存，请稍后" &&
                    state.screen == wqn::UiScreen::kSettings);
}

// AGENTS 4.2, the [词]-row path: scoping to a deck runs the synchronous word
// session reset, so it must be refused (and the request consumed so it cannot
// re-fire later) while a word commit is persisting. A deck id must be exactly
// 36 characters, or TakeNoteWordDeckOpenRequest rejects the request before the
// gate under test is ever reached.
bool CheckWordRowScopeGateDuringPersist()
{
    wqn::UiState state = MakeState();
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
    state.note_app.requested_word_deck_id =
        "0123456789abcdef0123456789abcdef012345";
    state.word_app.session.commit_state = wqn::WordObservationCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kConfirm, wqn::ButtonEventType::kShortPress), 0, &state);

    return Case("word-row scope switch refused while a word commit is persisting",
                state.screen == wqn::UiScreen::kNote &&
                    state.word_app.scoped_deck_id.empty() &&
                    !state.note_app.word_deck_open_requested &&
                    state.note_app.message == "正在保存，请稍后");
}

// AGENTS 4.2: top navigation away from a scoped word page. The guard lives
// before the screen switch, so a refusal leaves the screen untouched.
bool CheckTopNavigationGateDuringWordPersist()
{
    wqn::UiState state = MakeState();
    state.screen = wqn::UiScreen::kWord;
    state.word_app.scoped_deck_id = "deck-1";
    state.word_app.session.commit_state = wqn::WordObservationCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kUp, wqn::ButtonEventType::kLongPress), 0, &state);

    return Case("top navigation refused while a scoped word commit is persisting",
                state.screen == wqn::UiScreen::kWord &&
                    state.word_app.scoped_deck_id == "deck-1" &&
                    !state.word_app.message.empty());
}

// Same contract for a problem verdict reached from the note screen: a long
// UP/DOWN maps straight to top navigation and would bypass the in-layer gate.
bool CheckTopNavigationGateDuringProblemPersist()
{
    wqn::UiState state = MakeState();
    state.screen = wqn::UiScreen::kNote;
    state.problem_app.active = true;
    state.problem_app.commit_state = wqn::ProblemVerdictCommitState::kPersisting;

    device_ui_internal::ApplyButtonEvent(
        Press(wqn::ButtonId::kDownPower, wqn::ButtonEventType::kLongPress), 0, &state);

    return Case("top navigation refused while a problem verdict is persisting",
                state.screen == wqn::UiScreen::kNote &&
                    !state.problem_app.message.empty());
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
