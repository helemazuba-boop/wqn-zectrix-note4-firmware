#pragma once

#include <cstddef>
#include <string>

#include "device_protocol/problem_study.h"
#include "esp_err.h"

namespace wqn {

// A durable self-assessment verdict (correct/hesitant/wrong/skip). Unlike the
// note domain there is no session and no sequence: every record is a
// standalone idempotent observation keyed by request_id, applied server-side
// by record_problem_review_v1.
struct DurableProblemObservation {
    std::string request_id;
    std::string problem_id;
    protocol::problem_study_v1::ReviewAction action =
        protocol::problem_study_v1::ReviewAction::kSkip;
    std::string occurred_at;
};

struct ProblemOutboxSnapshot {
    size_t pending_count = 0;
    size_t capacity = 0;
};

inline constexpr size_t kProblemObservationOutboxCapacity = 200;

// Appends one verdict to the durable upload queue (idempotent on request_id).
esp_err_t CommitProblemObservation(const DurableProblemObservation& observation);
esp_err_t PeekPendingProblemObservation(DurableProblemObservation* observation);
esp_err_t AcknowledgeProblemObservation(const std::string& request_id);
// Moves one permanently rejected observation to a bounded forensic journal,
// then removes it from the upload queue so a terminal server error cannot
// wedge the durable head.
esp_err_t QuarantinePendingProblemObservation(const std::string& request_id);
esp_err_t ReadProblemOutboxSnapshot(ProblemOutboxSnapshot* snapshot);

}  // namespace wqn
