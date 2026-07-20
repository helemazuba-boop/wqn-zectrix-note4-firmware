#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "device_protocol/v3.h"
#include "esp_err.h"

namespace wqn::protocol::word_study_v1 {

#ifndef WQN_WORD_STUDY_SCHEMA_SHA256
#error "word-study-v1 schema hash must be injected by the component build"
#endif

inline constexpr char kSchemaSha256[] = WQN_WORD_STUDY_SCHEMA_SHA256;
inline constexpr uint32_t kPackSchemaVersion = 2;
inline constexpr size_t kMaxDecks = 32;
// Session parsing retains the short-lived 500-item response for rollback
// compatibility. New candidate transport pages are bounded independently.
inline constexpr size_t kMaxSessionItems = 500;
inline constexpr size_t kMaxCandidatePageItems = 100;
inline constexpr size_t kInitialCandidatePageSize = 32;
inline constexpr size_t kCandidatePrefetchPageSize = 64;
inline constexpr size_t kCandidateWindowSize =
    kInitialCandidatePageSize + kCandidatePrefetchPageSize;
inline constexpr size_t kMaxManifestDecks = 100;
inline constexpr size_t kMaxPackEntries = 10000;
inline constexpr size_t kMaxPackBytes = 4U * 1024U * 1024U;
inline constexpr size_t kMaxPackLineBytes = 8191;

enum class Mode : uint8_t {
    kSequential,
    kRandom,
    kDictionary,
};

enum class Purpose : uint8_t {
    kStudy,
    kLookup,
};

enum class Ordering : uint8_t {
    kSequential,
    kGuidedRandomV1,
    kLexicographic,
};

enum class ObservationAction : uint8_t {
    kShown,
    kRevealed,
    kKnown,
    kUnknown,
    kSkipped,
    kLookedUp,
};

enum class CandidateStatus : uint8_t {
    kNew,
    kLearning,
    kReview,
    kMastered,
};

struct Scope {
    std::vector<std::string> deck_ids;
    bool include_mastered = false;
};

struct PackSnapshot {
    std::string deck_id;
    uint64_t content_revision = 0;
    uint64_t pack_revision = 0;
    std::string sha256;
};

struct SessionItem {
    std::string item_id;
    std::string deck_id;
    uint64_t ordinal = 0;
};

struct SessionData {
    std::string session_id;
    Mode mode = Mode::kSequential;
    Purpose purpose = Purpose::kStudy;
    Ordering ordering = Ordering::kSequential;
    std::string candidate_policy_version;
    std::string seed;
    Scope scope;
    int optional_count = 0;  // zero means no explicit bound
    uint64_t next_sequence = 0;
    uint64_t progress_revision = 0;
    std::vector<PackSnapshot> snapshot;
    std::vector<SessionItem> items;
    std::string cursor;
    bool has_more = false;
};

struct CandidatePageRequest {
    v3::RequestMetadata metadata;
    std::string cursor;
    int limit = static_cast<int>(kCandidatePrefetchPageSize);
};

struct CandidatePageData {
    std::string session_id;
    Ordering ordering = Ordering::kSequential;
    std::string candidate_policy_version;
    std::string seed;
    std::vector<PackSnapshot> snapshot;
    uint64_t progress_revision = 0;
    std::string cursor;
    std::string next_cursor;
    std::vector<SessionItem> items;
    bool has_more = false;
};

struct ProgressProjection {
    bool present = false;
    CandidateStatus status = CandidateStatus::kNew;
    std::string due_at;
    uint64_t reviewed_count = 0;
    uint64_t known_count = 0;
    uint64_t unknown_count = 0;
};

struct ObservationData {
    std::string observation_id;
    std::string session_id;
    uint64_t sequence = 0;
    std::string item_id;
    ObservationAction action = ObservationAction::kShown;
    ProgressProjection progress;
    bool replayed = false;
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

struct ManifestDeck {
    std::string deck_id;
    std::string title;
    uint64_t change_sequence = 0;
    uint64_t content_revision = 0;
    bool deleted = false;
    bool has_pack = false;
    ManifestPack pack;
};

struct ManifestData {
    uint64_t cursor = 0;
    bool has_more = false;
    std::vector<ManifestDeck> decks;
};

struct Candidate {
    std::string item_id;
    std::string normalized_word;
    uint32_t deck_order = 0;
    int32_t sort_index = 0;
    CandidateStatus status = CandidateStatus::kNew;
    int64_t due_at_ms = -1;
};

struct CreateSessionRequest {
    v3::RequestMetadata metadata;
    Mode mode = Mode::kSequential;
    Scope scope;
    int optional_count = 0;
    std::string seed;
};

struct ObservationRequest {
    v3::RequestMetadata metadata;
    std::string session_id;
    uint64_t sequence = 0;
    std::string item_id;
    ObservationAction action = ObservationAction::kShown;
    Mode mode = Mode::kSequential;
    std::string occurred_at;
};

const char* ModeName(Mode mode);
const char* CandidatePolicyVersionName(Ordering ordering);
const char* ObservationActionName(ObservationAction action);
uint64_t GuidedRandomHash(const std::string& seed, const std::string& item_id);
void OrderCandidates(
    std::vector<Candidate>* candidates,
    Ordering ordering,
    const std::string& seed,
    int64_t now_ms);

esp_err_t BuildCreateSessionRequest(
    const CreateSessionRequest& request,
    std::string* body);
esp_err_t BuildObservationRequest(
    const ObservationRequest& request,
    std::string* body);
esp_err_t BuildCandidatePageRequest(
    const CandidatePageRequest& request,
    std::string* body);
esp_err_t BuildManifestRequest(
    const v3::RequestMetadata& metadata,
    uint64_t cursor,
    int limit,
    std::string* body);
esp_err_t ParseSessionResponse(
    const std::string& body,
    const std::string& expected_request_id,
    SessionData* data,
    v3::Error* error);
esp_err_t ParseObservationResponse(
    const std::string& body,
    const std::string& expected_request_id,
    ObservationData* data,
    v3::Error* error);
esp_err_t ParseCandidatePageResponse(
    const std::string& body,
    const std::string& expected_request_id,
    CandidatePageData* data,
    v3::Error* error);
esp_err_t ParseManifestResponse(
    const std::string& body,
    const std::string& expected_request_id,
    ManifestData* data,
    v3::Error* error);

}  // namespace wqn::protocol::word_study_v1
