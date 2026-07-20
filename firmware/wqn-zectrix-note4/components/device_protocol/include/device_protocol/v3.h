#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace wqn::protocol::v3 {

inline constexpr char kProtocolHeader[] = "3";
#ifndef WQN_DEVICE_CONTROL_SCHEMA_SHA256
#error "device-control-v3 schema hash must be injected by the component build"
#endif
inline constexpr char kSchemaSha256[] = WQN_DEVICE_CONTROL_SCHEMA_SHA256;

struct RequestMetadata {
    std::string request_id;
    std::string boot_id;
    std::string firmware_version;
    uint64_t config_revision = 0;
    uint64_t sync_cursor = 0;
    int limit = 20;
};

struct Error {
    std::string code;
    bool retryable = false;
    uint32_t retry_after_ms = 0;
};

struct ClaimStartData {
    std::string claim_id;
    std::string display_code;
    uint64_t expires_at_ms = 0;
    uint32_t poll_interval_ms = 0;
};

enum class ClaimStatus : uint8_t {
    kPending,
    kApproved,
    kExpired,
};

struct SealedCredential {
    std::string server_public_key;
    std::string salt;
    std::string iv;
    std::string ciphertext;
};

struct ClaimPollData {
    ClaimStatus status = ClaimStatus::kPending;
    uint32_t poll_interval_ms = 0;
    SealedCredential sealed_credential;
};

struct BootstrapData {
    std::string device_id;
    uint64_t config_revision = 0;
    uint64_t sync_cursor = 0;
};

struct SyncData {
    uint64_t config_revision = 0;
    uint64_t sync_cursor = 0;
    uint32_t auto_sync_interval_minutes = 0;
    int todo_count = 0;
    int word_due_count = 0;
    std::vector<std::string> due_problem_ids;
};

esp_err_t BuildBootstrapRequest(const RequestMetadata& metadata, std::string* body);
esp_err_t BuildSyncRequest(const RequestMetadata& metadata, std::string* body);
esp_err_t BuildClaimStartRequest(
    const RequestMetadata& metadata,
    const std::string& hardware_id,
    const std::string& device_public_key,
    std::string* body);
esp_err_t BuildClaimPollRequest(
    const RequestMetadata& metadata,
    const std::string& claim_id,
    std::string* body);
esp_err_t ParseClaimStartResponse(
    const std::string& body,
    const std::string& expected_request_id,
    ClaimStartData* data,
    Error* error);
esp_err_t ParseClaimPollResponse(
    const std::string& body,
    const std::string& expected_request_id,
    ClaimPollData* data,
    Error* error);
esp_err_t ParseBootstrapResponse(
    const std::string& body,
    const std::string& expected_request_id,
    BootstrapData* data,
    Error* error);
esp_err_t ParseSyncResponse(
    const std::string& body,
    const std::string& expected_request_id,
    SyncData* data,
    Error* error);

}  // namespace wqn::protocol::v3
