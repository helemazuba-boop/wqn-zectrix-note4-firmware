// Problem-pack content sync: manifest paging, pack download, merge/persist,
// and index rebuild. Mirrors note_cloud.cpp (the repaired form: offset-relist
// cursor, verified-sha download skip, sticky stale-index self-heal); the
// device-facing cloud task drives this from the kProblem executor.

#include "problem_cloud.h"

#include <algorithm>
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
constexpr size_t kMaxManifestProblemSets = 200;
constexpr size_t kMaxSnapshotRestarts = 2;

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

esp_err_t FetchStableProblemManifest(
    const std::string& token,
    WqnProblemPackManifest* manifest,
    std::string* message)
{
    if (manifest == nullptr || message == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t restart = 0; restart <= kMaxSnapshotRestarts; ++restart) {
        WqnProblemPackManifest candidate;
        uint64_t cursor = 0;
        bool has_more = true;
        bool snapshot_expired = false;
        size_t page_count = 0;
        while (has_more && page_count < kMaxManifestPagesPerSync) {
            ++page_count;
            WqnProblemPackManifest page;
            protocol::v3::Error error;
            const esp_err_t result = FetchProblemStudyManifest(
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
                *message = "错题清单拉取失败";
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
                *message = "错题游标未推进";
                return ESP_ERR_INVALID_RESPONSE;
            }
            for (WqnProblemPackManifestSet& item : page.problem_sets) {
                if (item.deleted) {
                    continue;
                }
                const auto duplicate = std::find_if(
                    candidate.problem_sets.begin(),
                    candidate.problem_sets.end(),
                    [&](const WqnProblemPackManifestSet& existing) {
                        return existing.problem_set_id == item.problem_set_id;
                    });
                if (duplicate != candidate.problem_sets.end()) {
                    *message = "错题清单包含重复项";
                    return ESP_ERR_INVALID_RESPONSE;
                }
                candidate.problem_sets.push_back(std::move(item));
                if (candidate.problem_sets.size() > kMaxManifestProblemSets) {
                    *message = "错题集数量超过设备上限";
                    return ESP_ERR_INVALID_SIZE;
                }
            }
            candidate.revision = std::max(candidate.revision, page.revision);
            cursor = page.cursor;
            has_more = page.has_more;
        }
        if (snapshot_expired) {
            ESP_LOGW(kTag, "problem manifest snapshot expired; attempt=%u/%u",
                     static_cast<unsigned>(restart + 1),
                     static_cast<unsigned>(kMaxSnapshotRestarts + 1));
            continue;
        }
        if (has_more) {
            *message = "错题变更过多，请重试";
            return ESP_ERR_INVALID_SIZE;
        }
        candidate.cursor = cursor;
        candidate.has_more = false;
        std::sort(
            candidate.problem_sets.begin(),
            candidate.problem_sets.end(),
            [](const auto& left, const auto& right) {
                return left.problem_set_id < right.problem_set_id;
            });
        *manifest = std::move(candidate);
        return ESP_OK;
    }
    *message = "错题快照持续变化，请稍后重试";
    return ESP_ERR_INVALID_STATE;
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

    // [snapshot-fix] Install nothing until the complete offset-relisted
    // snapshot is stable. A 410 restarts from cursor zero and cannot leave a
    // mixed local generation behind.
    WqnProblemPackManifest remote_manifest;
    if (out->result == ESP_OK) {
        out->result = FetchStableProblemManifest(
            token, &remote_manifest, &out->message);
    }
    std::vector<uint8_t> needs_download(remote_manifest.problem_sets.size(), 0);
    size_t total_needed = 0;
    bool pack_downloaded = false;
    if (out->result == ESP_OK) {
        for (size_t i = 0; i < remote_manifest.problem_sets.size(); ++i) {
            const WqnProblemPackManifestSet& item = remote_manifest.problem_sets[i];
            const WqnProblemPackManifestSet* known =
                FindManifestSet(local_manifest, item.problem_set_id);
            const std::string* verified_sha =
                known != nullptr && known->has_pack ? &known->sha256 : nullptr;
            if (ProblemPackNeedsDownload(item, verified_sha)) {
                needs_download[i] = 1;
                total_needed += item.byte_size;
            }
        }
        if (total_needed > 0) {
            out->result = EnsurePackDownloadCapacity(total_needed);
            if (out->result == ESP_ERR_NO_MEM) {
                out->message = "存储空间不足，已保留现有错题";
            }
        }
    }
    for (size_t i = 0;
         i < remote_manifest.problem_sets.size() && out->result == ESP_OK;
         ++i) {
        if (!needs_download[i]) {
            continue;
        }
        out->result = DownloadProblemPackToStorage(
            token,
            services::MakeDeviceRequestMetadata(),
            remote_manifest.problem_sets[i]);
        if (out->result == ESP_OK) {
            pack_downloaded = true;
            s_index_stale = true;
        } else {
            out->message = "错题包下载失败，已保留旧清单";
        }
    }
    if (out->result == ESP_OK) {
        manifest_content_changed =
            !had_local_manifest ||
            !SameManifestSets(remote_manifest, local_manifest);
        if (manifest_content_changed) {
            out->result = SaveProblemPackManifest(remote_manifest);
            if (out->result == ESP_OK) {
                local_manifest = remote_manifest;
                s_index_stale = true;
            } else {
                out->message = "错题清单保存失败";
            }
        }
        manifest_content_changed = manifest_content_changed || pack_downloaded;
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
    } else {
        out->snapshot_id = remote_manifest.snapshot_id;
    }
    return out->result;
}

}  // namespace wqn
