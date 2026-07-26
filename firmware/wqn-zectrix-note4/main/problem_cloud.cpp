// Problem-pack content sync: manifest paging, pack download, merge/persist,
// and index rebuild. Mirrors note_cloud.cpp (the repaired form: offset-relist
// cursor, verified-sha download skip, sticky stale-index self-heal); the
// device-facing cloud task drives this from the kProblem executor.

#include "problem_cloud.h"

#include <utility>
#include <vector>

#include "esp_log.h"
#include "problem_pack.h"
#include "services/sync_service.h"
#include "storage.h"
#include "wqn_api.h"

namespace wqn {

namespace {

constexpr char kTag[] = "problem_cloud";
// A single sync drains a bounded number of manifest pages so a runaway server
// cursor cannot spin the task forever; the caller re-runs to continue.
constexpr size_t kMaxManifestPagesPerSync = 32;

const WqnProblemPackManifestSet* FindManifestSet(
    const WqnProblemPackManifest& manifest, const std::string& problem_set_id)
{
    for (const WqnProblemPackManifestSet& item : manifest.problem_sets) {
        if (item.problem_set_id == problem_set_id) {
            return &item;
        }
    }
    return nullptr;
}

// Content equality of two manifests (cursor/has_more excluded: they are paging
// state, not content). Both sides are sorted by problem_set_id.
bool SameManifestSets(
    const WqnProblemPackManifest& a, const WqnProblemPackManifest& b)
{
    if (a.problem_sets.size() != b.problem_sets.size()) {
        return false;
    }
    for (size_t i = 0; i < a.problem_sets.size(); ++i) {
        const WqnProblemPackManifestSet& x = a.problem_sets[i];
        const WqnProblemPackManifestSet& y = b.problem_sets[i];
        if (x.problem_set_id != y.problem_set_id || x.name != y.name ||
            x.is_smart != y.is_smart ||
            x.has_pack != y.has_pack || x.pack_id != y.pack_id ||
            x.pack_revision != y.pack_revision ||
            x.schema_version != y.schema_version ||
            x.entry_count != y.entry_count || x.byte_size != y.byte_size ||
            x.sha256 != y.sha256 || x.download_url != y.download_url) {
            return false;
        }
    }
    return true;
}

}  // namespace

esp_err_t SyncProblemPacks(const std::string& token, ProblemPackSyncResult* out)
{
    // Sticky repair marker: set once pack files on disk may differ from the
    // index the app is holding (a download landed, or a required rebuild did
    // not complete). Without it, a sync that failed AFTER downloading packs
    // leaves every later round in "nothing changed; rebuild skipped" while the
    // stale in-memory index keeps dereferencing old offsets into the new files
    // (see note_cloud.cpp for the HIL history).
    static bool s_index_stale = false;

    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = ProblemPackSyncResult{};
    if (token.empty()) {
        out->auth_required = true;
        out->result = ESP_ERR_INVALID_STATE;
        return out->result;
    }

    WqnProblemPackManifest local_manifest;
    bool had_local_manifest = true;
    bool manifest_content_changed = false;
    out->result = LoadProblemPackManifest(&local_manifest);
    if (out->result == ESP_ERR_NOT_FOUND) {
        had_local_manifest = false;
        out->result = ResetProblemPackStorageCache();
        if (out->result == ESP_OK) {
            local_manifest = {};
        }
    } else if (out->result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "local problem pack manifest is incompatible; reset cache: %s",
            esp_err_to_name(out->result));
        had_local_manifest = false;
        out->result = ResetProblemPackStorageCache();
        if (out->result == ESP_OK) {
            local_manifest = {};
        }
    }

    // The server's problem manifest cursor is a SET LIST OFFSET: the manifest
    // relists every set with its current pack sha (same semantics as notes,
    // see note_cloud.cpp for why persisting the offset broke sync). Relist
    // from 0 on every sync; the per-set sha comparison below keeps unchanged
    // packs download-free.
    local_manifest.cursor = 0;

    size_t page_count = 0;
    bool has_more = out->result == ESP_OK;
    while (out->result == ESP_OK && has_more &&
           page_count < kMaxManifestPagesPerSync) {
        ++page_count;
        WqnProblemPackManifest delta;
        const auto metadata = services::MakeDeviceRequestMetadata();
        out->result = FetchProblemStudyManifest(
            token, metadata, local_manifest.cursor, &delta);
        if (out->result != ESP_OK) {
            break;
        }
        if (delta.has_more && delta.cursor <= local_manifest.cursor) {
            out->result = ESP_ERR_INVALID_RESPONSE;
            out->message = "错题游标未推进";
            break;
        }
        // A relisted page alone is not a content change; only an applied
        // tombstone or an actual pack download below flips the flag, so an
        // all-unchanged sync still skips the expensive index rebuild.
        for (const WqnProblemPackManifestSet& item : delta.problem_sets) {
            if (item.deleted) {
                manifest_content_changed = true;
            }
        }

        // Evaluate each pack's download decision exactly once (the needs-
        // download check hashes the whole local file when the verified-sha
        // shortcut does not apply).
        std::vector<uint8_t> needs_download(delta.problem_sets.size(), 0);
        size_t total_needed = 0;
        for (size_t i = 0; i < delta.problem_sets.size(); ++i) {
            const WqnProblemPackManifestSet& item = delta.problem_sets[i];
            // Under relist-every-sync, the recorded sha lets unchanged packs
            // skip the full-file re-hash (see ProblemPackNeedsDownload).
            const WqnProblemPackManifestSet* known =
                FindManifestSet(local_manifest, item.problem_set_id);
            const std::string* verified_sha =
                known != nullptr && known->has_pack ? &known->sha256 : nullptr;
            if (!item.deleted && ProblemPackNeedsDownload(item, verified_sha)) {
                needs_download[i] = 1;
                total_needed += item.byte_size;
            }
        }
        if (total_needed > 0) {
            StorageCapacitySnapshot storage;
            if (ReadStorageCapacitySnapshot(&storage) && storage.spiffs_valid) {
                const size_t available =
                    storage.spiffs_total_bytes > storage.spiffs_used_bytes
                        ? storage.spiffs_total_bytes - storage.spiffs_used_bytes
                        : 0;
                if (available < total_needed) {
                    ESP_LOGW(kTag, "SPIFFS space insufficient: need=%u avail=%u",
                             static_cast<unsigned>(total_needed),
                             static_cast<unsigned>(available));
                    out->result = ESP_ERR_NO_MEM;
                    out->message = "存储空间不足";
                }
            }
        }

        for (size_t i = 0; i < delta.problem_sets.size() && out->result == ESP_OK; ++i) {
            if (!needs_download[i]) {
                continue;
            }
            out->result = DownloadProblemPackToStorage(
                token, services::MakeDeviceRequestMetadata(), delta.problem_sets[i]);
            if (out->result == ESP_OK) {
                manifest_content_changed = true;
                s_index_stale = true;
            }
        }
        if (out->result != ESP_OK) {
            break;
        }

        const bool page_changed =
            !delta.problem_sets.empty() || delta.cursor != local_manifest.cursor;
        if (page_changed) {
            WqnProblemPackManifest merged;
            out->result = MergeProblemPackManifestDelta(delta, &merged);
            if (out->result == ESP_OK &&
                (!SameManifestSets(merged, local_manifest) ||
                 !had_local_manifest)) {
                // Persist only real content changes: relisting produces an
                // identical merge every idle cycle, and an unconditional save
                // would be a recurring multi-second storage transaction.
                out->result = SaveProblemPackManifest(merged);
            }
            if (out->result == ESP_OK) {
                local_manifest = std::move(merged);
            }
        }
        has_more = delta.has_more;
    }
    if (out->result == ESP_OK && has_more) {
        out->result = ESP_ERR_INVALID_SIZE;
        out->message = "错题变更过多，请重试";
    }

    if (out->result == ESP_OK &&
        (manifest_content_changed || !had_local_manifest || s_index_stale)) {
        out->result = LoadProblemPackIndex(&out->index);
        out->message = out->index.status_message;
        out->index_ready = out->result == ESP_OK;
        out->content_changed = true;
        if (out->result == ESP_OK) {
            s_index_stale = false;
        } else {
            s_index_stale = true;
        }
    } else if (out->result == ESP_OK) {
        out->message = "错题无变更";
        ESP_LOGI(kTag, "problem pack manifest unchanged; index rebuild skipped");
    }
    if (out->result != ESP_OK) {
        // Name the failing stage: the UI only surfaces a generic message.
        ESP_LOGW(
            kTag,
            "problem pack sync failed: %s stage_message=%s index_stale=%d",
            esp_err_to_name(out->result),
            out->message.empty() ? "-" : out->message.c_str(),
            s_index_stale ? 1 : 0);
    }
    return out->result;
}

}  // namespace wqn
