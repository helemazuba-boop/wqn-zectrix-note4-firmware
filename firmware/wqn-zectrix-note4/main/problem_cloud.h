#pragma once

#include <string>

#include "esp_err.h"
#include "problem_pack.h"

namespace wqn {

// Outcome of one problem-pack content sync. `index` is populated only when the
// sync rebuilt it (content_changed); otherwise the caller keeps its current
// index.
struct ProblemPackSyncResult {
    esp_err_t result = ESP_OK;
    bool content_changed = false;
    bool index_ready = false;
    bool auth_required = false;
    ProblemPackIndex index;
    std::string message;
};

// Runs one problem-pack content sync: relists the problem-study manifest from
// offset 0, downloads changed/new packs (sha-driven), merges + durably
// persists the manifest, and rebuilds the problem catalog when content changed.
// Content only; self-assessment observations flow through the outbox path.
//
// The caller supplies a validated bearer token and is expected to hold the
// problem-cloud sleep lease for the call. Storage writes take their own
// storage leases internally.
esp_err_t SyncProblemPacks(const std::string& token, ProblemPackSyncResult* out);

}  // namespace wqn
