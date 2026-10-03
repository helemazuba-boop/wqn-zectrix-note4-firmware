// Note-pack content sync: manifest paging, pack download, merge/persist, and
// index rebuild. Extracted from the word-pack sync path (ui/word_cloud.cpp) with
// the UI runtime coupling removed so the note domain owns a pure, reusable
// content sync. The device-facing cloud task drives this in a later milestone.

#include "note_cloud.h"

#include <algorithm>
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
constexpr size_t kMaxManifestNotebooks = 200;
constexpr size_t kMaxSnapshotRestarts = 2;

const WqnNotePackManifestNotebook* FindManifestNotebook(
    const WqnNotePackManifest& manifest, const std::string& notebook_id)
{
    for (const WqnNotePackManifestNotebook& item : manifest.notebooks) {
        if (item.notebook_id == notebook_id) {
            return &item;
        }
    }
    return nullptr;
}

// Content equality of two manifests (cursor/has_more excluded: they are paging
// state, not content). Both sides are sorted by notebook_id.
bool SameManifestNotebooks(
    const WqnNotePackManifest& a, const WqnNotePackManifest& b)
{
    if (a.notebooks.size() != b.notebooks.size()) {
        return false;
    }
    for (size_t i = 0; i < a.notebooks.size(); ++i) {
        const WqnNotePackManifestNotebook& x = a.notebooks[i];
        const WqnNotePackManifestNotebook& y = b.notebooks[i];
        if (x.notebook_id != y.notebook_id || x.title != y.title ||
            x.change_sequence != y.change_sequence ||
            x.content_revision != y.content_revision ||
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

esp_err_t FetchStableNoteManifest(
    const std::string& token,
    WqnNotePackManifest* manifest,
    std::string* message)
{
    if (manifest == nullptr || message == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t restart = 0; restart <= kMaxSnapshotRestarts; ++restart) {
        WqnNotePackManifest candidate;
        uint64_t cursor = 0;
        bool has_more = true;
        bool snapshot_expired = false;
        size_t page_count = 0;
        while (has_more && page_count < kMaxManifestPagesPerSync) {
            ++page_count;
            WqnNotePackManifest page;
            protocol::v3::Error error;
            const esp_err_t result = FetchNoteStudyManifest(
                token,
                services::MakeDeviceRequestMetadata(),
                cursor,
                &page,
                candidate.snapshot_id,
                &error);
            if (result != ESP_OK) {
                if (error.code == "SNAPSHOT_EXPIRED" ||
                    error.code == "snapshot_expired") {
                    snapshot_expired = true;
                    break;
                }
                *message = "笔记清单拉取失败";
                return result;
            }
            if (!candidate.snapshot_id.empty() &&
                candidate.snapshot_id != page.snapshot_id) {
                snapshot_expired = true;
                break;
            }
            if (candidate.snapshot_id.empty()) {
                candidate.snapshot_id = page.snapshot_id;
            }
            if (page.has_more && page.cursor <= cursor) {
                *message = "笔记游标未推进";
                return ESP_ERR_INVALID_RESPONSE;
            }
            for (WqnNotePackManifestNotebook& item : page.notebooks) {
                if (item.deleted) {
                    continue;
                }
                const auto duplicate = std::find_if(
                    candidate.notebooks.begin(),
                    candidate.notebooks.end(),
                    [&](const WqnNotePackManifestNotebook& existing) {
                        return existing.notebook_id == item.notebook_id;
                    });
                if (duplicate != candidate.notebooks.end()) {
                    *message = "笔记清单包含重复项";
                    return ESP_ERR_INVALID_RESPONSE;
                }
                candidate.notebooks.push_back(std::move(item));
                if (candidate.notebooks.size() > kMaxManifestNotebooks) {
                    *message = "笔记数量超过设备上限";
                    return ESP_ERR_INVALID_SIZE;
                }
            }
            candidate.revision = std::max(candidate.revision, page.revision);
            cursor = page.cursor;
            has_more = page.has_more;
        }
        if (snapshot_expired) {
            ESP_LOGW(kTag, "note manifest snapshot expired; attempt=%u/%u",
                     static_cast<unsigned>(restart + 1),
                     static_cast<unsigned>(kMaxSnapshotRestarts + 1));
            continue;
        }
        if (has_more) {
            *message = "笔记变更过多，请重试";
            return ESP_ERR_INVALID_SIZE;
        }
        candidate.cursor = cursor;
        candidate.has_more = false;
        std::sort(
            candidate.notebooks.begin(),
            candidate.notebooks.end(),
            [](const auto& left, const auto& right) {
                return left.notebook_id < right.notebook_id;
            });
        *manifest = std::move(candidate);
        return ESP_OK;
    }
    *message = "笔记快照持续变化，请稍后重试";
    return ESP_ERR_INVALID_STATE;
}

}  // namespace

esp_err_t SyncNotePacks(const std::string& token, NotePackSyncResult* out)
{
    // Sticky repair marker: set once pack files on disk may differ from the
    // index the app is holding (a download landed, or a required rebuild did
    // not complete). Without it, a sync that failed AFTER downloading packs
    // left every later round in "nothing changed; rebuild skipped" while the
    // stale in-memory index kept dereferencing old offsets into the new files
    // (HIL: packs re-downloaded fine, yet every note open failed silently).
    static bool s_index_stale = false;

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

    // [snapshot-fix] Fetch and validate the complete offset-relisted snapshot
    // before touching pack files or the persisted manifest. A 410 can now
    // restart from cursor zero without leaving a mixed local generation.
    WqnNotePackManifest remote_manifest;
    if (out->result == ESP_OK) {
        out->result = FetchStableNoteManifest(
            token, &remote_manifest, &out->message);
    }
    std::vector<uint8_t> needs_download(remote_manifest.notebooks.size(), 0);
    size_t total_needed = 0;
    bool pack_downloaded = false;
    if (out->result == ESP_OK) {
        for (size_t i = 0; i < remote_manifest.notebooks.size(); ++i) {
            const WqnNotePackManifestNotebook& item = remote_manifest.notebooks[i];
            const WqnNotePackManifestNotebook* known =
                FindManifestNotebook(local_manifest, item.notebook_id);
            const std::string* verified_sha =
                known != nullptr && known->has_pack ? &known->sha256 : nullptr;
            if (NotePackNeedsDownload(item, verified_sha)) {
                needs_download[i] = 1;
                total_needed += item.byte_size;
            }
        }
        if (total_needed > 0) {
            out->result = EnsurePackDownloadCapacity(total_needed);
            if (out->result == ESP_ERR_NO_MEM) {
                out->message = "存储空间不足，已保留现有笔记";
            }
        }
    }
    for (size_t i = 0;
         i < remote_manifest.notebooks.size() && out->result == ESP_OK;
         ++i) {
        if (!needs_download[i]) {
            continue;
        }
        out->result = DownloadNotePackToStorage(
            token,
            services::MakeDeviceRequestMetadata(),
            remote_manifest.notebooks[i]);
        if (out->result == ESP_OK) {
            pack_downloaded = true;
            s_index_stale = true;
        } else {
            out->message = "笔记包下载失败，已保留旧清单";
        }
    }
    if (out->result == ESP_OK) {
        manifest_content_changed =
            !had_local_manifest ||
            !SameManifestNotebooks(remote_manifest, local_manifest);
        if (manifest_content_changed) {
            out->result = SaveNotePackManifest(remote_manifest);
            if (out->result == ESP_OK) {
                local_manifest = remote_manifest;
                s_index_stale = true;
            } else {
                out->message = "笔记清单保存失败";
            }
        }
        manifest_content_changed = manifest_content_changed || pack_downloaded;
    }

    if (out->result == ESP_OK &&
        (manifest_content_changed || !had_local_manifest || s_index_stale)) {
        out->result = LoadNotePackIndex(&out->index);
        out->message = out->index.status_message;
        out->index_ready = out->result == ESP_OK;
        out->content_changed = true;
        if (out->result == ESP_OK) {
            s_index_stale = false;
        } else {
            s_index_stale = true;
        }
    } else if (out->result == ESP_OK) {
        out->message = "笔记无变更";
        ESP_LOGI(kTag, "note pack manifest unchanged; index rebuild skipped");
    }
    if (out->result != ESP_OK) {
        // Name the failing stage: the UI only surfaces a generic message, and
        // HIL had to reverse-engineer the break point from server logs.
        ESP_LOGW(
            kTag,
            "note pack sync failed: %s stage_message=%s index_stale=%d",
            esp_err_to_name(out->result),
            out->message.empty() ? "-" : out->message.c_str(),
            s_index_stale ? 1 : 0);
    } else {
        out->snapshot_id = remote_manifest.snapshot_id;
    }
    return out->result;
}

esp_err_t SyncSingleNotebookPack(
    const std::string& token,
    const std::string& notebook_id,
    NotePackSyncResult* out,
    WqnTransferProgressSink progress)
{
    if (out == nullptr || notebook_id.size() != 36) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NotePackSyncResult{};
    if (token.empty()) {
        out->auth_required = true;
        out->result = ESP_ERR_INVALID_STATE;
        return out->result;
    }

    // Reuse the same stable-snapshot walk as full sync. Stopping as soon as
    // the target appears can accept a row from a generation that expires on a
    // later page, so validate the complete listing before touching storage.
    WqnNotePackManifest remote_manifest;
    out->result = FetchStableNoteManifest(
        token, &remote_manifest, &out->message);
    if (out->result != ESP_OK) {
        ESP_LOGW(kTag, "single notebook sync manifest walk failed: %s",
                 esp_err_to_name(out->result));
        return out->result;
    }
    const WqnNotePackManifestNotebook* remote_target =
        FindManifestNotebook(remote_manifest, notebook_id);
    if (remote_target == nullptr || !remote_target->has_pack) {
        // The cloud genuinely has no pack for this notebook (deleted, archived
        // or never packed): nothing to download, and no local state to touch.
        out->result = ESP_ERR_NOT_FOUND;
        out->message = "云端暂无该笔记本内容";
        return out->result;
    }
    const WqnNotePackManifestNotebook target = *remote_target;

    WqnNotePackManifest local_manifest;
    const esp_err_t manifest_result = LoadNotePackManifest(&local_manifest);
    if (manifest_result != ESP_OK && manifest_result != ESP_ERR_NOT_FOUND) {
        // Broken local manifest is full-sync territory; do not half-repair it
        // here (ResetNotePackStorageCache would drop every other pack).
        out->result = manifest_result;
        out->message = "笔记清单损坏";
        return out->result;
    }
    const WqnNotePackManifestNotebook* known =
        FindManifestNotebook(local_manifest, notebook_id);
    const std::string* verified_sha =
        known != nullptr && known->has_pack ? &known->sha256 : nullptr;
    if (NotePackNeedsDownload(target, verified_sha)) {
        out->result = EnsurePackDownloadCapacity(target.byte_size);
        if (out->result != ESP_OK) {
            out->message = out->result == ESP_ERR_NO_MEM
                ? "存储空间不足，已保留现有笔记"
                : "存储空间检查失败";
            return out->result;
        }
        out->result = DownloadNotePackToStorage(
            token, services::MakeDeviceRequestMetadata(), target, progress);
        if (out->result != ESP_OK) {
            ESP_LOGW(kTag, "single notebook pack download failed: %s id=%.8s",
                     esp_err_to_name(out->result), notebook_id.c_str());
            return out->result;
        }
    }

    // Merge ONLY the target row. A full-delta merge would advance other rows
    // to shas whose pack files are still the old ones on disk, and the next
    // index rebuild would fail their verification wholesale.
    WqnNotePackManifest single_delta;
    single_delta.cursor = local_manifest.cursor;
    single_delta.has_more = local_manifest.has_more;
    single_delta.notebooks.push_back(target);
    WqnNotePackManifest merged;
    out->result = MergeNotePackManifestDelta(single_delta, &merged);
    if (out->result == ESP_OK) {
        out->result = SaveNotePackManifest(merged);
    }
    if (out->result != ESP_OK) {
        ESP_LOGW(kTag, "single notebook manifest merge failed: %s",
                 esp_err_to_name(out->result));
        return out->result;
    }

    out->result = LoadNotePackIndex(&out->index);
    out->message = out->index.status_message;
    out->index_ready = out->result == ESP_OK;
    out->content_changed = true;
    if (out->result == ESP_OK) {
        out->snapshot_id = remote_manifest.snapshot_id;
    }
    ESP_LOGI(kTag, "single notebook pack synced: id=%.8s index_ready=%d",
             notebook_id.c_str(), out->index_ready ? 1 : 0);
    return out->result;
}

}  // namespace wqn
