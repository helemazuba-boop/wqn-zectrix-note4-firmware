#include <cstdio>
#include "ui/storage_recovery_model.h"
using namespace wqn;
using namespace wqn::runtime;
using Model = device_ui_internal::StorageRecoveryModel;
using Action = Model::Action;
using Page = Model::Page;
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL recovery-model:%d: %s\n", __LINE__, #x); } } while (false)
struct Harness {
    Model model{ESP_FAIL};
    uint32_t sequence = 0;
    int64_t now = 100;
    Action Send(ButtonId key, ButtonEventType type, int64_t duration = 0, bool repeat = false) {
        ++now;
        return model.Input({key, type, duration, repeat, now, ++sequence}, now);
    }
    void Present() { model.Presented(model.version(), ++now); }
    void Ready(StorageRecoveryResetPolicy policy = StorageRecoveryResetPolicy::kAllowed) {
        model.Begin(Action::kInspect, 1);
        StorageRecoverySnapshot snapshot; snapshot.error = ESP_OK; snapshot.reset_policy = policy;
        CHECK(!model.Inspected(2, snapshot, ESP_OK));
        CHECK(model.Inspected(1, snapshot, ESP_OK));
        CHECK(model.page() == Page::kOverview);
        Present();
    }
    void Select(unsigned target) {
        while (model.selection() != target) {
            CHECK(Send(ButtonId::kDownPower, ButtonEventType::kShortPress) == Action::kNone);
            Present();
        }
    }
    void ConfirmView(bool present = true) {
        Select(3);
        CHECK(Send(ButtonId::kConfirm, ButtonEventType::kShortPress) == Action::kNone);
        CHECK(model.page() == Page::kConfirmReset);
        if (present) Present();
    }
    Action Hold(int64_t duration, bool repeat = false) {
        CHECK(Send(ButtonId::kConfirm, ButtonEventType::kPress) == Action::kNone);
        now += duration - 1;
        return Send(ButtonId::kConfirm, ButtonEventType::kRelease, duration, repeat);
    }
};
int main() {
    for (int64_t duration : {0, 199, 649, 1999, 2000, 2500, 10000}) {
        Harness h; h.Ready(); h.ConfirmView();
        CHECK(h.Hold(duration) == (duration >= 2000 ? Action::kAuthorizeReset : Action::kNone));
    }
    { Harness h; h.Ready(); h.ConfirmView(false); CHECK(h.Hold(2500) == Action::kNone);
      h.Present(); CHECK(h.Hold(2500) == Action::kAuthorizeReset); }
    { Harness h; h.Ready(); h.ConfirmView();
      CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kRelease, 2500) == Action::kNone);
      CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kPress) == Action::kNone);
      h.now += 2500;
      CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kLongPress, 2500, true) == Action::kNone);
      CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kRelease, 2500) == Action::kAuthorizeReset);
      CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kRelease, 2500) == Action::kNone); }
    for (int invalid = 0; invalid < 5; ++invalid) {
        Harness h; h.Ready(); h.ConfirmView();
        ButtonEvent event{ButtonId::kConfirm, ButtonEventType::kPress, 0, false, ++h.now, ++h.sequence};
        if (invalid == 0) event.seq = 0;
        if (invalid == 1) --event.seq;
        if (invalid == 2) event.occurred_at_ms = 1;
        if (invalid == 3) event.occurred_at_ms = h.now + 1;
        if (invalid == 4) event.repeat = true;
        CHECK(h.model.Input(event, h.now) == Action::kNone);
        h.now += 2500;
        CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kRelease, 2500) == Action::kNone);
    }
    { Harness h; h.Ready(); h.ConfirmView(); h.Send(ButtonId::kConfirm, ButtonEventType::kPress);
      h.Send(ButtonId::kUp, ButtonEventType::kPress); CHECK(h.model.page() == Page::kOverview);
      h.Present(); h.ConfirmView();
      CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kRelease, 3000) == Action::kNone); }
    for (auto policy : {StorageRecoveryResetPolicy::kUnavailable,
                       StorageRecoveryResetPolicy::kNewerFirmwareRequired,
                       StorageRecoveryResetPolicy::kAlreadyPending}) {
        Harness h; h.Ready(policy); h.Select(3);
        CHECK(!h.model.CanReset()); h.Send(ButtonId::kConfirm, ButtonEventType::kShortPress);
        CHECK(h.model.page() == Page::kOverview && h.Hold(3000) != Action::kAuthorizeReset);
    }
    for (auto outcome : {StorageRecoveryResetOutcome::kRecorded, StorageRecoveryResetOutcome::kUncertain,
                         StorageRecoveryResetOutcome::kRejected}) {
        Harness h; h.Ready(); h.ConfirmView(); CHECK(h.Hold(2500) == Action::kAuthorizeReset);
        h.model.Begin(Action::kAuthorizeReset, 3);
        h.Present(); CHECK(h.Send(ButtonId::kUp, ButtonEventType::kShortPress) == Action::kNone);
        CHECK(h.Hold(3000) == Action::kNone);
        CHECK(h.model.operation_id() == 3 && h.model.page() == Page::kBusy);
        StorageRecoveryResetResult result{outcome == StorageRecoveryResetOutcome::kRecorded ? ESP_OK : ESP_FAIL, outcome};
        CHECK(!h.model.ResetFinished(2, result));
        CHECK(h.model.ResetFinished(3, result));
        CHECK(h.model.operation_id() == 0);
        CHECK(h.model.page() == (outcome == StorageRecoveryResetOutcome::kRecorded ? Page::kRecorded :
            outcome == StorageRecoveryResetOutcome::kUncertain ? Page::kUncertain : Page::kError));
        CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kShortPress) == Action::kNone);
        h.Present(); CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kShortPress) != Action::kRestart);
        if (outcome != StorageRecoveryResetOutcome::kRejected) {
            CHECK(h.Send(ButtonId::kUp, ButtonEventType::kShortPress) == Action::kNone);
            h.Send(ButtonId::kConfirm, ButtonEventType::kPress); h.now += 2000;
            CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kShortPress) == Action::kNone);
            h.Send(ButtonId::kConfirm, ButtonEventType::kPress);
            CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kShortPress) == Action::kRestart);
        } else CHECK(!h.model.CanReset());
    }
    { Harness h; h.Ready(); h.Select(1); h.Send(ButtonId::kConfirm, ButtonEventType::kShortPress);
      CHECK(h.model.page() == Page::kConfirmRestart);
      CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kShortPress) == Action::kNone);
      h.Present(); CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kShortPress) == Action::kNone);
      h.Send(ButtonId::kConfirm, ButtonEventType::kPress);
      CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kShortPress) == Action::kRestart); }
    { Harness h; h.Ready(); h.ConfirmView(); h.model.SubmissionFailed(ESP_FAIL);
      CHECK(h.model.page() == Page::kError && !h.model.CanReset()); }
    { Harness h; h.model.SubmissionFailed(ESP_ERR_NO_MEM); h.Present();
      h.Send(ButtonId::kConfirm, ButtonEventType::kShortPress); h.Present();
      CHECK(h.Send(ButtonId::kConfirm, ButtonEventType::kShortPress) == Action::kInspect);
      h.model.Begin(Action::kInspect, 4); StorageRecoverySnapshot snapshot; snapshot.error = ESP_OK;
      CHECK(h.model.Inspected(4, snapshot, ESP_OK)); CHECK(h.model.page() == Page::kDiagnostics); }
    std::printf("Storage recovery model: %u checks, %u failures (NOT HIL)\n", checks, failures);
    return failures ? 1 : 0;
}
