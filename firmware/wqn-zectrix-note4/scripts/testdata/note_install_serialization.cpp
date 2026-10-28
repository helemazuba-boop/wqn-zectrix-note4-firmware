#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <utility>

#include "note_cloud.h"

namespace {
using Manifest = wqn::WqnNotePackManifest;
using Row = wqn::WqnNotePackManifestNotebook;
enum class Stage { kNone, kFetch, kLoad, kNeedsDownload, kCapacity, kDownload, kMerge, kSave, kIndex };
Manifest stored, remote;
std::map<std::string, bool> files;
std::function<void()> interleave;
Stage trigger = Stage::kNone, fail_stage = Stage::kNone;
size_t calls = 0, checks = 0, failures = 0;
bool missing = false;
esp_err_t fetch_error = ESP_FAIL;

void Check(bool ok, const char* message)
{
    ++checks;
    if (!ok) {
        if (failures < 10) std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void Boundary(Stage stage)
{
    ++calls;
    if (stage == trigger && interleave) {
        auto callback = std::move(interleave);
        interleave = {};
        callback();
    }
}

Row Notebook(char id, uint64_t revision)
{
    Row row;
    row.notebook_id = std::string(36, id);
    row.pack_id = row.notebook_id;
    row.title = std::string("notebook-") + id;
    row.content_revision = row.pack_revision = row.change_sequence = revision;
    row.has_pack = true;
    row.schema_version = 1;
    row.entry_count = 1;
    row.byte_size = 200;
    row.sha256 = std::string(63, id) + std::to_string(revision);
    return row;
}

void Reset()
{
    stored = {};
    stored.notebooks = {Notebook('a', 0)};
    remote = {};
    remote.notebooks = {Notebook('a', 1), Notebook('b', 1)};
    remote.snapshot_id = std::string(64, 'e');
    files.clear();
    files[stored.notebooks[0].sha256] = true;
    interleave = {};
    trigger = fail_stage = Stage::kNone;
    missing = false;
    fetch_error = ESP_FAIL;
    calls = 0;
}

esp_err_t Install(bool full, wqn::NotePackSyncResult* out)
{
    return full ? wqn::SyncNotePacks("synthetic-token", out) :
        wqn::SyncSingleNotebookPack("synthetic-token", Notebook('a', 0).notebook_id, out);
}

void CompetingCall(bool full)
{
    const size_t before = calls;
    wqn::NotePackSyncResult out;
    Check(Install(full, &out) == ESP_ERR_INVALID_STATE, "competing installer is immediately rejected");
    Check(calls == before, "rejected installer touched no network/storage boundary");
    Check(!out.index_ready && !out.content_changed && !out.auth_required && out.snapshot_id.empty(),
          "busy result publishes no index, success snapshot or auth failure");
    Check(!out.message.empty(), "busy result explains retry");
}
} // namespace

namespace wqn {
static constexpr size_t kMaxStoredNotebooks = 200;
@@MERGE@@
namespace services { protocol::v3::RequestMetadata MakeDeviceRequestMetadata() { return {}; } }

esp_err_t FetchNoteStudyManifest(const std::string&, const protocol::v3::RequestMetadata&,
                               uint64_t, Manifest* out, const std::string&, protocol::v3::Error*)
{
    Boundary(Stage::kFetch);
    if (fail_stage == Stage::kFetch) return fetch_error;
    *out = remote;
    return ESP_OK;
}
esp_err_t LoadNotePackManifest(Manifest* out)
{
    Boundary(Stage::kLoad);
    if (fail_stage == Stage::kLoad) return ESP_FAIL;
    *out = stored;
    return missing ? ESP_ERR_NOT_FOUND : ESP_OK;
}
esp_err_t SaveNotePackManifest(const Manifest& value)
{
    Boundary(Stage::kSave);
    if (fail_stage == Stage::kSave) return ESP_FAIL;
    stored = value;
    missing = false;
    return ESP_OK;
}
esp_err_t ResetNotePackStorageCache()
{
    stored = {};
    files.clear();
    missing = false;
    return ESP_OK;
}
esp_err_t EnsurePackDownloadCapacity(size_t)
{
    Boundary(Stage::kCapacity);
    return fail_stage == Stage::kCapacity ? ESP_ERR_NO_MEM : ESP_OK;
}
bool NotePackNeedsDownload(const Row& row, const std::string*)
{
    Boundary(Stage::kNeedsDownload);
    return files.count(row.sha256) == 0;
}
esp_err_t DownloadNotePackToStorage(const std::string&, const protocol::v3::RequestMetadata&,
                                  const Row& row, WqnTransferProgressSink)
{
    Boundary(Stage::kDownload);
    if (fail_stage == Stage::kDownload) return ESP_FAIL;
    files[row.sha256] = true;
    return ESP_OK;
}
esp_err_t LoadNotePackIndex(NotePackIndex* index)
{
    Boundary(Stage::kIndex);
    if (fail_stage == Stage::kIndex) return ESP_FAIL;
    index->mounted = true;
    return ESP_OK;
}
} // namespace wqn

int main()
{
    // Both lane directions, same-lane reentry and every external boundary.
    for (bool owner_full : {true, false}) {
        for (bool contender_full : {true, false}) {
            for (Stage stage : {Stage::kFetch, Stage::kLoad, Stage::kNeedsDownload,
                                Stage::kCapacity, Stage::kDownload, Stage::kSave, Stage::kIndex}) {
                Reset();
                trigger = stage;
                interleave = [=] { CompetingCall(contender_full); };
                wqn::NotePackSyncResult out;
                Check(Install(owner_full, &out) == ESP_OK, "owner completes");
                Check(!interleave, "scheduled interleave was reached");
                Check(stored.notebooks.front().content_revision == 1, "owner's manifest persists");
                remote.notebooks.front() = Notebook('a', 2);
                Check(Install(contender_full, &out) == ESP_OK, "contender retries after owner returns");
                Check(stored.notebooks.front().content_revision == 2, "retry fetches a fresh snapshot");
            }
        }
    }
    // Every error after admission releases the gate; missing/deleted/rebuilt
    // catalogs also remain possible once the prior operation has completed.
    for (bool full : {true, false}) {
        for (Stage stage : {Stage::kFetch, Stage::kCapacity, Stage::kDownload, Stage::kSave, Stage::kIndex}) {
            Reset();
            fail_stage = stage;
            wqn::NotePackSyncResult out;
            Check(Install(full, &out) != ESP_OK, "injected owner failure is propagated");
            if (stage == Stage::kSave) Check(stored.notebooks.front().content_revision == 0, "failed save preserves old manifest");
            fail_stage = Stage::kNone;
            Check(Install(!full, &out) == ESP_OK, "other lane retries after failure");
        }
    }
    Reset();
    remote.notebooks.clear();
    wqn::NotePackSyncResult out;
    Check(Install(false, &out) == ESP_ERR_NOT_FOUND, "target absent returns not found");
    Check(Install(true, &out) == ESP_OK && stored.notebooks.empty(), "full snapshot removes deleted notebooks");
    remote.notebooks = {Notebook('a', 3)};
    Check(Install(false, &out) == ESP_OK && stored.notebooks[0].content_revision == 3, "recreated notebook installs after deletion");
    Reset();
    missing = true;
    Check(Install(true, &out) == ESP_OK, "first installation without local manifest succeeds");
    Check(wqn::SyncNotePacks("", &out) == ESP_ERR_INVALID_STATE && out.auth_required, "invalid token does not acquire the gate");
    Check(wqn::SyncNotePacks("synthetic-token", nullptr) == ESP_ERR_INVALID_ARG, "null output rejected");
    Check(Install(false, &out) == ESP_OK, "invalid arguments cannot leave gate claimed");
    Reset();
    fail_stage = Stage::kFetch;
    fetch_error = ESP_ERR_INVALID_SIZE;
    Check(Install(true, &out) == ESP_ERR_INVALID_SIZE && !out.index_ready && !out.content_changed,
          "oversize remote response cannot publish a partial index");
    Check(stored.notebooks[0].content_revision == 0 && files.count(stored.notebooks[0].sha256) == 1,
          "remote size rejection preserves installed manifest and pack");
    std::printf("%zu checks, %zu failures (real Note orchestration; host network/storage ports; NOT HIL)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
