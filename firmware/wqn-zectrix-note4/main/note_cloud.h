#pragma once

#include <string>

#include "esp_err.h"
#include "note_pack.h"

namespace wqn {

// Outcome of one note-pack content sync. `index` is populated only when the sync
// rebuilt it (content_changed); otherwise the caller keeps its current index.
struct NotePackSyncResult {
    esp_err_t result = ESP_OK;
    bool content_changed = false;
    bool index_ready = false;
    bool auth_required = false;
    NotePackIndex index;
    std::string message;
};

// Runs one note-pack content sync: pages the note-study manifest from the given
// cursor, downloads changed/new packs, merges + durably persists the manifest,
// and rebuilds the note index when content changed. Content only; sessions,
// candidates and read observations flow through separate paths.
//
// The caller supplies a validated bearer token and is expected to hold the
// note-cloud sleep lease for the call. Storage writes take their own storage
// leases internally.
esp_err_t SyncNotePacks(const std::string& token, NotePackSyncResult* out);

// Targeted variant for a user who is actively waiting: opening a note whose
// pack is not on disk. Pages the manifest only until `notebook_id` is found,
// downloads that single pack, merges ONLY that notebook's manifest row (other
// rows must keep their on-disk sha or the next index rebuild would fail their
// verification), and rebuilds the index. Full-catalog convergence stays with
// SyncNotePacks on its background cadence.
esp_err_t SyncSingleNotebookPack(
    const std::string& token,
    const std::string& notebook_id,
    NotePackSyncResult* out,
    WqnTransferProgressSink progress = nullptr);

}  // namespace wqn
