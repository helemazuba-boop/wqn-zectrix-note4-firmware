// Note-pack content sync: manifest paging, pack download, merge/persist, and
// index rebuild. Extracted from the word-pack sync path (ui/word_cloud.cpp) with
// the UI runtime coupling removed so the note domain owns a pure, reusable
// content sync. The device-facing cloud task drives this in a later milestone.

#include "note_cloud.h"

#include <utility>

#include "esp_log.h"
#include "note_pack.h"
#include "services/sync_service.h"
#include "storage.h"
#include "wqn_api.h"

namespace wqn {

namespace {

constexpr char kTag[] = "note_cloud";
// A single sync drains a bounded number of manifest pages so a runaway server
// cursor cannot spin the task forever; the caller re-runs to continue.
constexpr size_t kMaxManifestPagesPerSync = 32;

}  // namespace

esp_err_t SyncNotePacks(const std::string& token, NotePackSyncResult* out)
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NotePackSyncResult{};
    if (token.empty()) {
        out->auth_required = true;
        out->result = ESP_ERR_INVALID_STATE;
        return out->result;
    }

    WqnNotePackManifest local_manifest;
    bool had_local_manifest = true;
    bool manifest_content_changed = false;
    out->result = LoadNotePackManifest(&local_manifest);
    if (out->result == ESP_ERR_NOT_FOUND) {
        had_local_manifest = false;
        out->result = ResetNotePackStorageCache();
        if (out->result == ESP_OK) {
            local_manifest = {};
        }
    } else if (out->result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "local note pack manifest is incompatible; reset cache: %s",
            esp_err_to_name(out->result));
        had_local_manifest = false;
        out->result = ResetNotePackStorageCache();
        if (out->result == ESP_OK) {
            local_manifest = {};
        }
    }

    // The server's note manifest cursor is a NOTEBOOK LIST OFFSET: the
    // manifest relists every notebook with its current pack sha, unlike the
    // word manifest whose cursor is a change_seq feed. Persisting the offset
    // made every later sync resume listing past the end (cursor == notebook
    // count -> changes=0 forever), so web-side attaches/edits never reached a
    // device that had synced once (HIL: cursor=3 changes=0 while the cloud
    // note_change_log sat at seq 5). Relist from 0 on every sync; the
    // per-notebook sha comparison below keeps unchanged packs download-free.
    local_manifest.cursor = 0;

    size_t page_count = 0;
    bool has_more = out->result == ESP_OK;
    while (out->result == ESP_OK && has_more &&
           page_count < kMaxManifestPagesPerSync) {
        ++page_count;
        WqnNotePackManifest delta;
        const auto metadata = services::MakeDeviceRequestMetadata();
        out->result = FetchNoteStudyManifest(
            token, metadata, local_manifest.cursor, &delta);
        if (out->result != ESP_OK) {
            break;
        }
        if (delta.has_more && delta.cursor <= local_manifest.cursor) {
            out->result = ESP_ERR_INVALID_RESPONSE;
            out->message = "笔记游标未推进";
            break;
        }
        // A relisted page alone is not a content change; only an applied
        // tombstone or an actual pack download below flips the flag, so an
        // all-unchanged sync still skips the expensive index rebuild.
        for (const WqnNotePackManifestNotebook& item : delta.notebooks) {
            if (item.deleted) {
                manifest_content_changed = true;
            }
        }

        // Evaluate each pack's download decision exactly once. NotePackNeedsDownload
        // hashes the entire local file, so calling it for both the capacity estimate
        // and the download loop re-hashed every unchanged pack twice on every sync.
        std::vector<uint8_t> needs_download(delta.notebooks.size(), 0);
        size_t total_needed = 0;
        for (size_t i = 0; i < delta.notebooks.size(); ++i) {
            const WqnNotePackManifestNotebook& item = delta.notebooks[i];
            if (!item.deleted && NotePackNeedsDownload(item)) {
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

        for (size_t i = 0; i < delta.notebooks.size() && out->result == ESP_OK; ++i) {
            if (!needs_download[i]) {
                continue;
            }
            out->result = DownloadNotePackToStorage(
                token, services::MakeDeviceRequestMetadata(), delta.notebooks[i]);
            if (out->result == ESP_OK) {
                manifest_content_changed = true;
            }
        }
        if (out->result != ESP_OK) {
            break;
        }

        const bool page_changed =
            !delta.notebooks.empty() || delta.cursor != local_manifest.cursor;
        if (page_changed) {
            WqnNotePackManifest merged;
            out->result = MergeNotePackManifestDelta(delta, &merged);
            if (out->result == ESP_OK) {
                out->result = SaveNotePackManifest(merged);
            }
            if (out->result == ESP_OK) {
                local_manifest = std::move(merged);
            }
        }
        has_more = delta.has_more;
    }
    if (out->result == ESP_OK && has_more) {
        out->result = ESP_ERR_INVALID_SIZE;
        out->message = "笔记变更过多，请重试";
    }

    if (out->result == ESP_OK &&
        (manifest_content_changed || !had_local_manifest)) {
        out->result = LoadNotePackIndex(&out->index);
        out->message = out->index.status_message;
        out->index_ready = out->result == ESP_OK;
        out->content_changed = true;
    } else if (out->result == ESP_OK) {
        out->message = "笔记无变更";
        ESP_LOGI(kTag, "note pack manifest unchanged; index rebuild skipped");
    }
    return out->result;
}

}  // namespace wqn
