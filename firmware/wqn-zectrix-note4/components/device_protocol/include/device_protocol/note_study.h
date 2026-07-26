#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "device_protocol/v3.h"
#include "esp_err.h"

namespace wqn::protocol::note_study_v1 {

#ifndef WQN_NOTE_STUDY_SCHEMA_SHA256
#error "note-study-v1 schema hash must be injected by the component build"
#endif

inline constexpr char kSchemaSha256[] = WQN_NOTE_STUDY_SCHEMA_SHA256;
inline constexpr uint32_t kPackSchemaVersion = 1;
inline constexpr size_t kMaxNotebooks = 32;
// Candidate snapshots are bounded at the contract boundary.
inline constexpr size_t kMaxSessionItems = 500;
inline constexpr size_t kMaxCandidatePageItems = 100;
inline constexpr size_t kInitialCandidatePageSize = 32;
inline constexpr size_t kCandidatePrefetchPageSize = 64;
inline constexpr size_t kCandidateWindowSize =
    kInitialCandidatePageSize + kCandidatePrefetchPageSize;
inline constexpr size_t kMaxManifestNotebooks = 100;
inline constexpr size_t kMaxPackEntries = 5000;
inline constexpr size_t kMaxPackBytes = 4U * 1024U * 1024U;
// A note record carries the full plain_text_v1 body (<=16 KiB UTF-8) plus a
// JSON-escaped title, so a single JSONL line is far larger than word study's.
inline constexpr size_t kMaxPackLineBytes = 65535;

enum class Mode : uint8_t {
    kSequential,
    kRecent,
};

enum class Purpose : uint8_t {
    kBrowse,
};

enum class Ordering : uint8_t {
    kSequentialNoteV1,
    kLeastRecentlyViewedV1,
};

enum class ObservationAction : uint8_t {
    kOpened,
    kReadCompleted,
    kSkipped,
    kSessionPaused,
};

struct Scope {
    std::vector<std::string> notebook_ids;
    bool include_archived = false;
};

struct PackSnapshot {
    std::string notebook_id;
    uint64_t content_revision = 0;
    uint64_t pack_revision = 0;
    std::string sha256;
};

struct SessionItem {
    std::string item_id;
    std::string notebook_id;
    uint64_t ordinal = 0;
    // Last-viewed pin as of session creation; empty string means never viewed.
    std::string last_opened_at;
};

struct SessionData {
    std::string session_id;
    Mode mode = Mode::kSequential;
    Purpose purpose = Purpose::kBrowse;
    Ordering ordering = Ordering::kSequentialNoteV1;
    std::string candidate_policy_version;
    std::string seed;
    Scope scope;
    int optional_count = 500;
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
    Ordering ordering = Ordering::kSequentialNoteV1;
    std::string candidate_policy_version;
    std::string seed;
    std::vector<PackSnapshot> snapshot;
    uint64_t progress_revision = 0;
    std::string cursor;
    std::string next_cursor;
    std::vector<SessionItem> items;
    bool has_more = false;
};

// Read-state projection only: never mastery / known-unknown / schedule.
struct ProgressProjection {
    bool present = false;
    std::string last_opened_at;
    std::string last_completed_at;
    uint64_t completed_count = 0;
};

struct ObservationData {
    std::string observation_id;
    std::string session_id;
    uint64_t sequence = 0;
    std::string item_id;
    ObservationAction action = ObservationAction::kOpened;
    ProgressProjection progress;
    bool projection_applied = false;
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

struct ManifestNotebook {
    std::string notebook_id;
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
    std::vector<ManifestNotebook> notebooks;
};

struct CreateSessionRequest {
    v3::RequestMetadata metadata;
    Mode mode = Mode::kSequential;
    Scope scope;
    int optional_count = 500;
    std::string seed;
};

struct ObservationRequest {
    v3::RequestMetadata metadata;
    std::string session_id;
    uint64_t sequence = 0;
    std::string item_id;
    ObservationAction action = ObservationAction::kOpened;
    Mode mode = Mode::kSequential;
    std::string occurred_at;
};

const char* ModeName(Mode mode);
const char* CandidatePolicyVersionName(Ordering ordering);
const char* ObservationActionName(ObservationAction action);

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

}  // namespace wqn::protocol::note_study_v1
