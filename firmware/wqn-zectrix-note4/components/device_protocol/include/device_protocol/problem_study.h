#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "device_protocol/v3.h"
#include "esp_err.h"

namespace wqn::protocol::problem_study_v1 {

#ifndef WQN_PROBLEM_STUDY_SCHEMA_SHA256
#error "problem-study-v1 schema hash must be injected by the component build"
#endif

inline constexpr char kSchemaSha256[] = WQN_PROBLEM_STUDY_SCHEMA_SHA256;
inline constexpr uint32_t kPackSchemaVersion = 1;
inline constexpr size_t kMaxManifestSets = 100;
inline constexpr size_t kMaxPackEntries = 500;
inline constexpr size_t kMaxPackBytes = 4U * 1024U * 1024U;
// A problem row carries the shell body plus up to 10 typed parts (each with
// its own content_text/answer_text), so lines size like note-study's.
inline constexpr size_t kMaxPackLineBytes = 65535;
inline constexpr size_t kMaxProblemParts = 10;
inline constexpr size_t kMaxImagesPerRow = 8;

// Device verdicts, mapped server-side by record_problem_review_v1:
// correct -> mastered + SM-2 advance, hesitant -> needs_review + small step,
// wrong -> wrong + interval reset, skip -> observation only.
enum class ReviewAction : uint8_t {
    kCorrect,
    kHesitant,
    kWrong,
    kSkip,
};

enum class ProblemStatus : uint8_t {
    kWrong,
    kNeedsReview,
    kMastered,
};

struct ManifestPack {
    std::string pack_id;
    uint64_t pack_revision = 0;
    uint32_t schema_version = 0;
    uint32_t entry_count = 0;
    uint32_t byte_size = 0;
    std::string sha256;
    std::string download_url;
};

struct ManifestSet {
    std::string problem_set_id;
    std::string name;
    bool is_smart = false;
    bool deleted = false;
    bool has_pack = false;
    ManifestPack pack;
};

struct ManifestData {
    uint64_t cursor = 0;
    bool has_more = false;
    std::vector<ManifestSet> problem_sets;
};

struct ObservationRequest {
    v3::RequestMetadata metadata;
    std::string problem_id;
    ReviewAction action = ReviewAction::kSkip;
    std::string occurred_at;
};

// SM-2 projection echoed by the RPC; absent (present=false) for skip.
struct ScheduleProjection {
    bool present = false;
    std::string next_review_at;
    uint64_t interval_days = 0;
    double ease_factor = 0.0;
    uint64_t repetition_number = 0;
};

struct ObservationData {
    std::string observation_id;
    std::string problem_id;
    ReviewAction action = ReviewAction::kSkip;
    ProblemStatus status = ProblemStatus::kWrong;
    ScheduleProjection schedule;
    bool projection_applied = false;
    bool replayed = false;
};

const char* ReviewActionName(ReviewAction action);
const char* ProblemStatusName(ProblemStatus status);
bool ParseProblemStatus(const std::string& value, ProblemStatus* status);

esp_err_t BuildManifestRequest(
    const v3::RequestMetadata& metadata,
    uint64_t cursor,
    int limit,
    std::string* body);
esp_err_t BuildObservationRequest(
    const ObservationRequest& request,
    std::string* body);
esp_err_t ParseManifestResponse(
    const std::string& body,
    const std::string& expected_request_id,
    ManifestData* data,
    v3::Error* error);
esp_err_t ParseObservationResponse(
    const std::string& body,
    const std::string& expected_request_id,
    ObservationData* data,
    v3::Error* error);

}  // namespace wqn::protocol::problem_study_v1
