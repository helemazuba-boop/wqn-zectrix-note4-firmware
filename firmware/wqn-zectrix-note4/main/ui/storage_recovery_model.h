#pragma once

#include "button_event.h"
#include "runtime/storage_schema.h"

namespace device_ui_internal {

// Boot-only, single UI owner. Effects carry no storage or hardware calls.
class StorageRecoveryModel {
public:
    enum class Page { kOverview, kDiagnostics, kPreservation, kConfirmReset,
                      kConfirmRestart, kBusy, kRecorded, kUncertain, kError };
    enum class Action { kNone, kInspect, kAuthorizeReset, kRestart };
    explicit StorageRecoveryModel(esp_err_t boot_error) : error_(boot_error) {}

    Page page() const { return page_; }
    unsigned selection() const { return selection_; }
    uint64_t version() const { return version_; }
    uint32_t operation_id() const { return operation_id_; }
    Action operation() const { return operation_; }
    esp_err_t error() const { return error_; }
    const wqn::runtime::StorageRecoverySnapshot& snapshot() const { return snapshot_; }
    bool CanReset() const {
        return snapshot_.error == ESP_OK &&
            snapshot_.reset_policy == wqn::runtime::StorageRecoveryResetPolicy::kAllowed;
    }
    void Presented(uint64_t version, int64_t now_ms) {
        if (version == version_) { presented_ = true; presented_at_ms_ = now_ms; }
    }
    void Begin(Action operation, uint32_t id) {
        if (id == 0 || operation_id_ != 0) return;
        operation_id_ = id;
        operation_ = operation;
        SetPage(Page::kBusy);
    }
    bool Inspected(uint32_t id, const wqn::runtime::StorageRecoverySnapshot& snapshot,
                   esp_err_t error) {
        if (id == 0 || id != operation_id_ || operation_ != Action::kInspect) return false;
        snapshot_ = snapshot;
        snapshot_.error = error;
        error_ = error;
        operation_id_ = 0;
        operation_ = Action::kNone;
        SetPage(first_inspection_ ? Page::kOverview : Page::kDiagnostics);
        first_inspection_ = false;
        return true;
    }
    bool ResetFinished(uint32_t id, const wqn::runtime::StorageRecoveryResetResult& result) {
        if (id == 0 || id != operation_id_ || operation_ != Action::kAuthorizeReset) return false;
        operation_id_ = 0;
        operation_ = Action::kNone;
        error_ = result.error;
        using Outcome = wqn::runtime::StorageRecoveryResetOutcome;
        if (result.outcome == Outcome::kRecorded) SetPage(Page::kRecorded);
        else if (result.outcome == Outcome::kUncertain) SetPage(Page::kUncertain);
        else {
            snapshot_ = {}; // Reinspect before another destructive confirmation.
            SetPage(Page::kError);
        }
        return true;
    }
    void SubmissionFailed(esp_err_t error) {
        if (operation_id_ != 0) return;
        first_inspection_ = false;
        error_ = error;
        snapshot_ = {};
        SetPage(Page::kError);
    }

    Action Input(const wqn::ButtonEvent& event, int64_t now_ms) {
        using wqn::ButtonEventType;
        using wqn::ButtonId;
        // A replayed event, repeat, future timestamp or event produced before
        // this view was presented cannot consent to a destructive action.
        if (event.seq == 0 || (have_seq_ && static_cast<int32_t>(event.seq - last_seq_) <= 0))
            return Action::kNone;
        have_seq_ = true;
        last_seq_ = event.seq;
        if (event.repeat || event.occurred_at_ms <= 0 || event.occurred_at_ms > now_ms) {
            if (!event.repeat) held_at_ms_ = 0;
            return Action::kNone;
        }
        if (page_ == Page::kBusy) return Action::kNone;
        if (page_ == Page::kConfirmReset) {
            if (event.button == ButtonId::kUp && event.type == ButtonEventType::kPress) {
                SetPage(Page::kOverview);
                return Action::kNone;
            }
            if (!presented_ || event.occurred_at_ms <= presented_at_ms_) {
                held_at_ms_ = 0;
                return Action::kNone;
            }
            if (event.button != ButtonId::kConfirm) {
                held_at_ms_ = 0;
                return Action::kNone;
            }
            if (event.type == ButtonEventType::kPress) held_at_ms_ = event.occurred_at_ms;
            if (event.type == ButtonEventType::kRelease) {
                const int64_t held = held_at_ms_;
                held_at_ms_ = 0;
                if (CanReset() && held > presented_at_ms_ &&
                    event.occurred_at_ms >= held && event.occurred_at_ms - held >= 2000 &&
                    event.duration_ms >= 2000) return Action::kAuthorizeReset;
            }
            return Action::kNone;
        }
        if (!presented_ || event.occurred_at_ms <= presented_at_ms_) return Action::kNone;
        if (event.button == ButtonId::kConfirm && event.type == ButtonEventType::kPress)
            held_at_ms_ = event.occurred_at_ms;
        if (event.type != ButtonEventType::kShortPress) return Action::kNone;
        const bool fresh_confirm = event.button == ButtonId::kConfirm &&
            held_at_ms_ > presented_at_ms_ && event.occurred_at_ms >= held_at_ms_ &&
            event.occurred_at_ms - held_at_ms_ <= 1000 && event.duration_ms < 650;
        if (page_ == Page::kRecorded || page_ == Page::kUncertain) {
            held_at_ms_ = 0;
            return fresh_confirm ? Action::kRestart : Action::kNone;
        }
        if (page_ == Page::kConfirmRestart) {
            if (fresh_confirm) {
                held_at_ms_ = 0;
                return Action::kRestart;
            }
            if (event.button == ButtonId::kUp) SetPage(Page::kOverview);
            return Action::kNone;
        }
        if (page_ != Page::kOverview) {
            if (event.button == ButtonId::kUp || event.button == ButtonId::kConfirm)
                SetPage(Page::kOverview);
            return Action::kNone;
        }
        if (event.button == ButtonId::kUp || event.button == ButtonId::kDownPower) {
            selection_ = (selection_ + (event.button == ButtonId::kUp ? 3u : 1u)) % 4;
            SetPage(Page::kOverview);
        } else if (event.button == ButtonId::kConfirm) {
            if (selection_ == 0) return Action::kInspect;
            if (selection_ == 1) SetPage(Page::kConfirmRestart);
            if (selection_ == 2) SetPage(Page::kPreservation);
            if (selection_ == 3 && CanReset()) SetPage(Page::kConfirmReset);
        }
        return Action::kNone;
    }

private:
    void SetPage(Page page) {
        page_ = page;
        ++version_;
        presented_ = false;
        held_at_ms_ = 0;
    }
    Page page_ = Page::kOverview;
    Action operation_ = Action::kNone;
    unsigned selection_ = 0;
    uint64_t version_ = 1;
    uint32_t operation_id_ = 0, last_seq_ = 0;
    bool first_inspection_ = true, presented_ = false, have_seq_ = false;
    int64_t held_at_ms_ = 0, presented_at_ms_ = 0;
    esp_err_t error_;
    wqn::runtime::StorageRecoverySnapshot snapshot_;
};

}  // namespace device_ui_internal
