// Host fixture for the REAL word_study_store.cpp descriptor codec + file +
// StorageService seam. The Python driver compiles the FULL production
// translation units (word_study_store.cpp + word_local_session.cpp) with stub
// ESP/IDF headers, and link-time --wrap redirects /storage/* to a temp dir.
// StorageService transactions run inline (the seam), so save/load/prune/collect
// exercise real production code end to end on real libc files.
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <map>

#include "word_local_session.h"
#include "word_study_store.h"
#include "outbox_suspend_reason.h"
#include "storage.h"
#include "services/storage_service.h"
#include "runtime/sleep_coordinator.h"

// ---- stubbed external services (declared in generated stub headers) -------
namespace {
uint32_t g_scope_generation = 7;
std::string g_storage_root;
bool g_fail_fopen = false;
bool g_fail_opendir = false;
std::map<std::string, std::vector<uint8_t>> g_cursor_nvs;
}

uint32_t wqn::GetDeckScopeGeneration() { return g_scope_generation; }
esp_err_t wqn::SaveWordSessionCursorNvs(const char* key, const void* bytes, size_t size)
{
    const auto* data=static_cast<const uint8_t*>(bytes);
    g_cursor_nvs[key]=std::vector<uint8_t>(data,data+size); return ESP_OK;
}
esp_err_t wqn::LoadWordSessionCursorNvs(const char* key, void* bytes, size_t size, bool* found)
{
    if (found) *found = false;
    const auto value=g_cursor_nvs.find(key);
    if(value!=g_cursor_nvs.end() && value->second.size()==size) {
        std::memcpy(bytes,value->second.data(),size);
        if(found) *found=true;
    }
    return ESP_OK;
}
esp_err_t wqn::ClearWordSessionCursorNvs(const char* key) { g_cursor_nvs.erase(key); return ESP_OK; }

namespace wqn::services {
esp_err_t ExecuteStorageTransactionNamed(StorageTransaction tx, void* ctx, const char*)
{
    return tx ? tx(ctx) : ESP_ERR_INVALID_ARG;
}
esp_err_t ExecuteForegroundStorageTransaction(StorageTransaction tx, void* ctx, const char*)
{
    return tx ? tx(ctx) : ESP_ERR_INVALID_ARG;
}
esp_err_t ExecuteOwnedStorageTransactionUntil(
    StorageTransaction tx, void* ctx, StorageContextCleanup cleanup,
    const char*, int64_t)
{
    esp_err_t r = tx ? tx(ctx) : ESP_ERR_INVALID_ARG;
    if (cleanup) cleanup(ctx);
    return r;
}
bool IsStorageServiceTask() { return true; }
}

namespace wqn::runtime {
SleepLease SleepLease::TryAcquire(SleepBlocker, const char*, const char*, int)
{
    SleepLease lease; lease.ok_ = true; return lease;
}
SleepLease SleepLease::TryAcquireStoragePreparation(
    uint32_t, const char*, const char*, int)
{
    SleepLease lease; lease.ok_ = true; return lease;
}
}

// ---- /storage path redirection via --wrap ---------------------------------
static std::string Redirect(const char* path)
{
    const std::string p(path ? path : "");
    if (p.rfind("/storage", 0) == 0) {
        return g_storage_root + p.substr(8);  // drop "/storage"
    }
    return p;
}

extern "C" {
FILE* __real_fopen(const char*, const char*);
int __real_rename(const char*, const char*);
int __real_remove(const char*);
int __real_stat(const char*, struct stat*);
DIR* __real_opendir(const char*);

FILE* __wrap_fopen(const char* path, const char* mode)
{
    if (g_fail_fopen) { errno = EIO; return nullptr; }
    const std::string p = Redirect(path);
    return __real_fopen(p.c_str(), mode);
}
int __wrap_rename(const char* a, const char* b)
{
    const std::string pa = Redirect(a);
    const std::string pb = Redirect(b);
    return __real_rename(pa.c_str(), pb.c_str());
}
int __wrap_remove(const char* p)
{
    const std::string rp = Redirect(p);
    return __real_remove(rp.c_str());
}
int __wrap_stat(const char* p, struct stat* st)
{
    const std::string rp = Redirect(p);
    return __real_stat(rp.c_str(), st);
}
DIR* __wrap_opendir(const char* p)
{
    if (g_fail_opendir) { errno = EIO; return nullptr; }
    const std::string rp = Redirect(p);
    return __real_opendir(rp.c_str());
}
}

// ---- test helpers ---------------------------------------------------------
static int passes = 0, failures = 0;
static void Check(bool ok, const char* name)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    ok ? ++passes : ++failures;
}

static std::string Uuid(uint32_t n)
{
    char buf[37];
    std::snprintf(buf, sizeof(buf), "%08x-1111-4222-8333-%012x", n, n);
    return std::string(buf);
}

static std::string Sha(char fill)
{
    return std::string(64, fill);
}

// Build a valid descriptor with N items across M decks in traversal order.
static wqn::LocalWordSessionDescriptor MakeDescriptor(
    uint32_t session_n, uint32_t start, uint32_t total,
    size_t count, size_t decks, char sha_fill, uint32_t gen)
{
    wqn::LocalWordSessionDescriptor d;
    d.total_count = total;
    wqn::PersistedWordSession& s = d.session;
    s.active = true; s.paused = false; s.position = 0;
    s.start_index = start; s.phase = wqn::WordPresentationPhase::kFront;
    s.deck_scope_generation = gen;
    s.remote.session_id = Uuid(session_n);
    s.remote.mode = wqn::protocol::word_study_v1::Mode::kSequential;
    s.remote.purpose = wqn::protocol::word_study_v1::Purpose::kStudy;
    s.remote.ordering = wqn::protocol::word_study_v1::Ordering::kSequential;
    s.remote.seed = "seed-A";
    s.remote.include_mastered = true;
    s.remote.optional_count = 500;
    s.remote.next_sequence = 0; s.remote.progress_revision = 0;
    s.remote.has_more = false; s.remote.cursor.clear();
    for (size_t k = 0; k < decks; ++k) {
        wqn::StoredWordDeckId id;
        std::snprintf(id.value, sizeof(id.value), "%s", Uuid(1000 + k).c_str());
        s.remote.deck_ids.push_back(id);
        wqn::StoredWordPackSnapshot snap;
        std::snprintf(snap.deck_id, sizeof(snap.deck_id), "%s", id.value);
        snap.content_revision = 3; snap.pack_revision = 9;
        std::snprintf(snap.sha256, sizeof(snap.sha256), "%s", Sha(sha_fill).c_str());
        s.remote.snapshot.push_back(snap);
    }
    for (size_t i = 0; i < count; ++i) {
        wqn::StoredWordSessionItem item;
        std::snprintf(item.item_id, sizeof(item.item_id), "%s",
                      Uuid(50000 + session_n * 1000 + i).c_str());
        std::snprintf(item.deck_id, sizeof(item.deck_id), "%s",
                      s.remote.deck_ids[i % decks].value);
        item.ordinal = i;
        s.remote.items.push_back(item);
    }
    return d;
}

static bool PathExists(const std::string& rel)
{
    struct stat st = {};
    return stat((g_storage_root + rel).c_str(), &st) == 0;
}

static void WriteFile(const std::string& rel, const std::vector<uint8_t>& bytes)
{
    FILE* f = std::fopen((g_storage_root + rel).c_str(), "wb");
    if (f) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
}

static std::vector<uint8_t> ReadFile(const std::string& rel)
{
    std::vector<uint8_t> out;
    FILE* f = std::fopen((g_storage_root + rel).c_str(), "rb");
    if (!f) return out;
    uint8_t buf[512]; size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return out;
}

// Token of a session id (first 24 hex after stripping hyphens) -- matches the
// production DeriveLocalDescriptorPaths naming.
static std::string Token(const std::string& uuid)
{
    std::string t;
    for (char c : uuid) {
        if (c == '-') continue;
        if (t.size() == 24) break;
        t.push_back(c);
    }
    return t;
}

// Host seam exported from the REAL word_study_store.cpp translation unit
// (guarded by WORD_LOCAL_DESCRIPTOR_HOST_SEAM). These let the fixture mint
// genuine v1 200B records with the production BuildObservationRecord /
// BuildSuspendRecord + RecordCrc, so ScanOutboxFile validates real bytes.
extern "C" {
esp_err_t WqnHostBuildObservationRecord(
    const wqn::DurableWordObservation* observation, void* record);
esp_err_t WqnHostBuildSuspendRecord(
    const char* request_id, wqn::OutboxSuspendReason reason, void* record);
size_t WqnHostOutboxRecordSize();
void WqnHostResetWordOutboxCache();
}

// Append one genuine production v1 record (or a paired observation+suspend
// pair) referencing session_id to /storage/wout.v1. Suspend MUST pair by the
// SAME request_id and carry a legal reason, otherwise ScanOutboxFile treats
// it as an orphan diagnostic instead of a suspended durable fact.
static void WriteOutboxRecord(const std::string& session_id, bool suspended)
{
    const size_t record_size = WqnHostOutboxRecordSize();
    if (record_size != 200) return;  // v1 record shape is fixed
    std::vector<uint8_t> obs(record_size, 0);
    wqn::DurableWordObservation o;
    o.request_id = std::string(suspended ? "suspend-" : "observe-") + session_id;
    o.session_id = session_id;
    o.sequence = 1;
    o.item_id = Uuid(777);
    o.action = wqn::protocol::word_study_v1::ObservationAction::kShown;
    o.mode = wqn::protocol::word_study_v1::Mode::kSequential;
    o.occurred_at = "2026-01-01T00:00:00Z";
    o.next_position = 0;
    o.next_phase = wqn::WordPresentationPhase::kFront;
    if (WqnHostBuildObservationRecord(&o, obs.data()) != ESP_OK) return;
    std::vector<uint8_t> data(obs);
    if (suspended) {
        std::vector<uint8_t> park(record_size, 0);
        if (WqnHostBuildSuspendRecord(
                o.request_id.c_str(),
                wqn::OutboxSuspendReason::kIdempotencyConflict,
                park.data()) != ESP_OK) {
            return;
        }
        data.insert(data.end(), park.begin(), park.end());
    }
    FILE* f = std::fopen((g_storage_root + "/wout.v1").c_str(), "ab");
    if (f) {
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
}

static void CleanStorage()
{
    WqnHostResetWordOutboxCache();
    g_cursor_nvs.clear();
    DIR* dir = opendir(g_storage_root.c_str());
    if (!dir) return;
    struct dirent* e = nullptr;
    while ((e = readdir(dir)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::remove((g_storage_root + "/" + e->d_name).c_str());
    }
    closedir(dir);
}

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    g_storage_root = argv[1];
    CleanStorage();
    g_scope_generation = 7;

    // 1) save -> load round trip, idempotent retry, full semantics.
    {
        CleanStorage();
        auto d = MakeDescriptor(1, 0, 3507, 500, 3, 'a', 7);
        Check(wqn::SaveLocalWordSessionDescriptor(d) == ESP_OK,
              "save new descriptor under current scope generation");
        Check(PathExists("/wl" + Token(d.session.remote.session_id) + ".v1"),
              "descriptor primary file uses short 29-char basename");
        wqn::LocalWordSessionDescriptor loaded;
        Check(wqn::LoadLocalWordSessionDescriptor(d.session.remote.session_id,
                                                  &loaded) == ESP_OK,
              "load saved descriptor");
        Check(loaded.total_count == 3507 &&
              loaded.session.start_index == 0 &&
              loaded.session.remote.items.size() == 500 &&
              loaded.session.remote.items[499].ordinal == 499 &&
              loaded.session.remote.session_id == d.session.remote.session_id &&
              loaded.session.remote.snapshot.size() == 3,
              "loaded total_count/500/dense ordinal/uuid/pin match");
        // idempotent retry
        Check(wqn::SaveLocalWordSessionDescriptor(d) == ESP_OK,
              "identical retry is idempotent (no overwrite error)");
    }

    // 2) same UUID different content -> explicitly rejected, immutable.
    {
        CleanStorage();
        auto d = MakeDescriptor(2, 0, 3507, 500, 3, 'a', 7);
        Check(wqn::SaveLocalWordSessionDescriptor(d) == ESP_OK, "save baseline");
        auto mutated = MakeDescriptor(2, 0, 3507, 499, 3, 'a', 7);  // different items
        Check(wqn::SaveLocalWordSessionDescriptor(mutated) != ESP_OK,
              "same UUID, different content is rejected (immutable)");
        wqn::LocalWordSessionDescriptor loaded;
        Check(wqn::LoadLocalWordSessionDescriptor(d.session.remote.session_id,
                                                  &loaded) == ESP_OK &&
              loaded.session.remote.items.size() == 500,
              "original immutable descriptor is NOT overwritten");
    }

    // 3) stale deck-scope generation -> save rejected; load still allowed.
    {
        CleanStorage();
        auto d = MakeDescriptor(3, 500, 3507, 500, 2, 'a', 6);  // gen 6 != 7
        Check(wqn::SaveLocalWordSessionDescriptor(d) == ESP_ERR_INVALID_STATE,
              "NEW save rejects stale deck-scope generation");
        g_scope_generation = 6;
        Check(wqn::SaveLocalWordSessionDescriptor(d) == ESP_OK,
              "save accepted when generation matches");
        g_scope_generation = 7;  // now stale scope
        wqn::LocalWordSessionDescriptor loaded;
        Check(wqn::LoadLocalWordSessionDescriptor(d.session.remote.session_id,
                                                  &loaded) == ESP_OK,
              "LOAD does NOT filter old-scope descriptor (upload facts survive)");
    }

    // 4) invalid UUID / path injection rejected by save + load.
    {
        CleanStorage();
        auto d = MakeDescriptor(4, 0, 3507, 500, 2, 'a', 7);
        d.session.remote.session_id = "../etc/evil-0000-0000-000000000000";
        Check(wqn::SaveLocalWordSessionDescriptor(d) == ESP_ERR_INVALID_ARG,
              "path-injection session UUID rejected on save");
        Check(wqn::LoadLocalWordSessionDescriptor("../../etc/passwd", nullptr) !=
              ESP_OK, "path-injection UUID rejected on load");
        wqn::LocalWordSessionDescriptor out;
        Check(wqn::LoadLocalWordSessionDescriptor("not-a-uuid", &out) ==
              ESP_ERR_INVALID_ARG, "invalid UUID rejected on load");
    }

    // 5) CRC wrong / truncated / bad length -> explicit error, NOT missing.
    {
        CleanStorage();
        auto d = MakeDescriptor(5, 1000, 3507, 500, 2, 'a', 7);
        Check(wqn::SaveLocalWordSessionDescriptor(d) == ESP_OK, "save for corruption tests");
        const std::string rel = "/wl" + Token(d.session.remote.session_id) + ".v1";
        auto good = ReadFile(rel);
        // wrong CRC (flip a payload byte)
        auto bad_crc = good;
        bad_crc[bad_crc.size() / 2] ^= 0x5a;
        WriteFile(rel, bad_crc);
        wqn::LocalWordSessionDescriptor out;
        Check(wqn::LoadLocalWordSessionDescriptor(d.session.remote.session_id,
                                                  &out) != ESP_OK &&
              wqn::LoadLocalWordSessionDescriptor(d.session.remote.session_id,
                                                  &out) != ESP_ERR_NOT_FOUND,
              "wrong CRC is an error, never NOT_FOUND");
        // truncated
        auto truncated = good;
        truncated.resize(truncated.size() / 2);
        WriteFile(rel, truncated);
        Check(wqn::LoadLocalWordSessionDescriptor(d.session.remote.session_id,
                                                  &out) != ESP_OK &&
              wqn::LoadLocalWordSessionDescriptor(d.session.remote.session_id,
                                                  &out) != ESP_ERR_NOT_FOUND,
              "truncated file is an error, never NOT_FOUND");
        WriteFile(rel, good);
    }

    // 6) missing primary + valid backup recovers; leftover .tmp is NOT used.
    {
        CleanStorage();
        auto d = MakeDescriptor(6, 2000, 3507, 500, 2, 'a', 7);
        Check(wqn::SaveLocalWordSessionDescriptor(d) == ESP_OK, "save for recovery tests");
        const std::string stem = "wl" + Token(d.session.remote.session_id);
        auto bytes = ReadFile("/" + stem + ".v1");
        // Move primary aside to backup, leave primary absent.
        WriteFile("/" + stem + ".bak", bytes);
        std::remove((g_storage_root + "/" + stem + ".v1").c_str());
        wqn::LocalWordSessionDescriptor out;
        Check(wqn::LoadLocalWordSessionDescriptor(d.session.remote.session_id,
                                                  &out) == ESP_OK &&
              out.session.remote.items.size() == 500,
              "missing primary + intact backup recovers");
        // Now remove backup too, leave ONLY a leftover .tmp -> NOT recovered.
        std::remove((g_storage_root + "/" + stem + ".bak").c_str());
        WriteFile("/" + stem + ".tmp", bytes);
        wqn::LocalWordSessionDescriptor out2;
        Check(wqn::LoadLocalWordSessionDescriptor(d.session.remote.session_id,
                                                  &out2) != ESP_OK,
              "leftover .tmp is NOT authoritative for recovery");
    }

    // 7) unknown I/O failure is a real error, NOT missing.
    {
        CleanStorage();
        auto d = MakeDescriptor(7, 0, 3507, 500, 2, 'a', 7);
        g_fail_fopen = true;
        Check(wqn::SaveLocalWordSessionDescriptor(d) != ESP_OK,
              "fopen failure on save propagates");
        g_fail_fopen = false;
        // A load under a transient fopen failure must not be NOT_FOUND.
        g_fail_fopen = true;
        wqn::LocalWordSessionDescriptor out;
        Check(wqn::LoadLocalWordSessionDescriptor(d.session.remote.session_id,
                                                  &out) != ESP_ERR_NOT_FOUND,
              "fopen failure on load is not reinterpreted as NOT_FOUND");
        g_fail_fopen = false;
    }

    // 8) path token collision with a DIFFERENT full UUID is rejected.
    {
        CleanStorage();
        // Two UUIDs sharing first 24 hex but differing in the tail.
        auto a = MakeDescriptor(8, 0, 3507, 500, 1, 'a', 7);
        Check(wqn::SaveLocalWordSessionDescriptor(a) == ESP_OK, "save first UUID");
        auto b = a;
        // Keep the same 24-hex prefix, change the last 8 hex of the UUID.
        std::string id = a.session.remote.session_id;
        id[35] = (id[35] == 'f') ? '0' : 'f';
        b.session.remote.session_id = id;  // different full UUID, same token
        b.session.remote.items.resize(499);
        Check(wqn::SaveLocalWordSessionDescriptor(b) != ESP_OK,
              "24-hex token collision with different full UUID rejected");
    }

    // 9) GC: keep active + outbox-referenced; remove an unproven inactive one.
    {
        CleanStorage();
        auto active = MakeDescriptor(9, 0, 3507, 500, 2, 'a', 7);
        Check(wqn::SaveLocalWordSessionDescriptor(active) == ESP_OK, "save active");
        // An inactive descriptor with no outbox fact and no resumable slot.
        auto inactive = MakeDescriptor(10, 500, 3507, 500, 2, 'a', 7);
        Check(wqn::SaveLocalWordSessionDescriptor(inactive) == ESP_OK, "save inactive");
        const std::string inactive_rel = "/wl" + Token(inactive.session.remote.session_id) + ".v1";
        Check(PathExists(inactive_rel), "inactive descriptor present before pruning");
        // No outbox file, no session slots -> GC may remove the inactive one.
        Check(wqn::PruneLocalWordSessionDescriptors(
                  active.session.remote.session_id) == ESP_OK,
              "prune with readable proof sources succeeds");
        Check(PathExists("/wl" + Token(active.session.remote.session_id) + ".v1"),
              "active descriptor retained by GC");
        Check(!PathExists(inactive_rel),
              "unproven inactive descriptor removed by GC");
    }

    // 10) GC refuses to clean when outbox proof is unreadable.
    {
        CleanStorage();
        auto d = MakeDescriptor(11, 0, 3507, 500, 1, 'a', 7);
        Check(wqn::SaveLocalWordSessionDescriptor(d) == ESP_OK, "save for GC proof test");
        g_fail_opendir = true;
        Check(wqn::PruneLocalWordSessionDescriptors("") != ESP_OK,
              "unreadable descriptor dir makes GC refuse to clean");
        g_fail_opendir = false;
        Check(PathExists("/wl" + Token(d.session.remote.session_id) + ".v1"),
              "descriptor preserved when GC refuses");
    }

    // 11) GC keeps every outbox-referenced descriptor (pending + suspended).
    {
        CleanStorage();
        auto d = MakeDescriptor(12, 0, 3507, 500, 1, 'a', 7);
        Check(wqn::SaveLocalWordSessionDescriptor(d) == ESP_OK, "save referenced descriptor");
        // Build a real v1 200B outbox record referencing this session id and
        // write it to /storage/wout.v1. The production ScanOutboxFile reads it.
        // (Layout mirrors OutboxRecord: magic,version,kind,action,mode,phase,
        //  reserved,sequence,next_position,request_id[65],session_id[37],
        //  item_id[37],occurred_at[33],crc.) We build via a small helper below.
        WriteOutboxRecord(d.session.remote.session_id, /*suspended=*/false);
        WriteOutboxRecord(d.session.remote.session_id, /*suspended=*/true);
        Check(wqn::PruneLocalWordSessionDescriptors("") == ESP_OK,
              "prune over outbox-referenced descriptor succeeds");
        Check(PathExists("/wl" + Token(d.session.remote.session_id) + ".v1"),
              "pending+suspended referenced descriptor retained by GC");
        std::remove((g_storage_root + "/wout.v1").c_str());
    }

    // 12) CollectRetainedLocalSnapshots returns pinned sha256 of all present.
    {
        CleanStorage();
        auto d1 = MakeDescriptor(13, 0, 3507, 500, 2, 'a', 7);
        auto d2 = MakeDescriptor(14, 500, 3507, 500, 2, 'b', 7);
        Check(wqn::SaveLocalWordSessionDescriptor(d1) == ESP_OK, "save d1");
        Check(wqn::SaveLocalWordSessionDescriptor(d2) == ESP_OK, "save d2");
        wqn::LocalDescriptorRetention retention;
        Check(wqn::CollectRetainedLocalSnapshots(&retention) == ESP_OK,
              "collect retained snapshots succeeds");
        Check(retention.pinned_sha256.size() == 4 &&
              std::find(retention.pinned_sha256.begin(),
                        retention.pinned_sha256.end(), Sha('a')) !=
                  retention.pinned_sha256.end() &&
              std::find(retention.pinned_sha256.begin(),
                        retention.pinned_sha256.end(), Sha('b')) !=
                  retention.pinned_sha256.end(),
              "collect returns every pinned sha256 (both descriptors)");
    }

    // 13) 32-descriptor capacity backpressure (explicit, facts preserved).
    {
        CleanStorage();
        for (uint32_t i = 0; i < 32; ++i) {
            // start_index must stay within total_count; only the UUID/items
            // differ here. Using i*500 would exceed 3507 for i>=8 and be
            // correctly rejected by descriptor semantics, not by the cap.
            auto d = MakeDescriptor(100 + i, 0, 3507, 4, 1, 'a', 7);
            Check(wqn::SaveLocalWordSessionDescriptor(d) == ESP_OK,
                  "fill 32 descriptors");
            WriteOutboxRecord(d.session.remote.session_id, false);
        }
        auto overflow = MakeDescriptor(200, 0, 3507, 4, 1, 'a', 7);
        Check(wqn::SaveLocalWordSessionDescriptor(overflow) != ESP_OK,
              "33rd descriptor backpressured at 32-file cap");
        // All 32 original facts are still loadable (no silent eviction).
        uint32_t still = 0;
        for (uint32_t i = 0; i < 32; ++i) {
            wqn::LocalWordSessionDescriptor out;
            if (wqn::LoadLocalWordSessionDescriptor(Uuid(100 + i), &out) == ESP_OK) {
                ++still;
            }
        }
        Check(still == 32, "all 32 original descriptor facts preserved under backpressure");
    }

    // Recovery families participate in pack retention even without a primary.
    {
        CleanStorage();
        auto d=MakeDescriptor(301,0,3507,4,1,'a',7);
        Check(wqn::SaveLocalWordSessionDescriptor(d)==ESP_OK,"save backup-only retention fixture");
        const std::string stem="/wl"+Token(d.session.remote.session_id);
        WriteFile(stem+".bak",ReadFile(stem+".v1"));
        std::remove((g_storage_root+stem+".v1").c_str());
        WriteOutboxRecord(d.session.remote.session_id,false);
        wqn::LocalDescriptorRetention pins;
        Check(wqn::CollectRetainedLocalSnapshots(&pins)==ESP_OK && pins.pinned_sha256.size()==1 &&
              pins.pinned_sha256[0]==Sha('a'),"backup-only descriptor retains its pinned pack");
        Check(wqn::PruneLocalWordSessionDescriptors("")==ESP_OK && PathExists(stem+".bak"),
              "backup-only referenced descriptor survives GC");
    }
    // A completed current slot is the authoritative between-segment cursor.
    {
        CleanStorage();
        auto d=MakeDescriptor(302,500,3507,4,1,'a',7);
        Check(wqn::SaveLocalWordSessionDescriptor(d)==ESP_OK,"save completed-slot descriptor");
        wqn::PersistedWordSession slot;
        const auto initial=wqn::BuildLocalWordInitialSession(d,32,&slot);
        slot.position=4;slot.active=false;slot.remote.next_sequence=8;
        Check(initial==ESP_OK && wqn::SavePersistedWordSession(slot)==ESP_OK,"publish completed current slot");
        Check(wqn::PruneLocalWordSessionDescriptors("")==ESP_OK &&
              PathExists("/wl"+Token(d.session.remote.session_id)+".v1"),
              "GC retains completed slot descriptor even after every old fact is ACKed");
        wqn::PersistedWordSession restored;
        Check(wqn::LoadPersistedWordSession(slot.remote.mode,&restored)==ESP_OK &&
              !restored.active && restored.start_index==500,"completed hand-off cursor survives reload");
        Check(wqn::ClearPersistedWordSessionIfMatches(slot.remote.mode,Uuid(999),7)==ESP_ERR_INVALID_STATE,
              "stale server-invalid SID cannot clear replacement slot");
        Check(wqn::ClearPersistedWordSessionIfMatches(slot.remote.mode,slot.remote.session_id,6)==ESP_ERR_INVALID_STATE,
              "stale scope generation cannot clear current slot");
        Check(wqn::LoadPersistedWordSession(slot.remote.mode,&restored)==ESP_OK,
              "rejected stale clear preserves durable session");
        Check(wqn::ClearPersistedWordSessionIfMatches(slot.remote.mode,slot.remote.session_id,7)==ESP_OK &&
              wqn::LoadPersistedWordSession(slot.remote.mode,&restored)==ESP_ERR_NOT_FOUND,
              "matching server-invalid clear retires only its own slot");
    }
    // A plausible but corrupt header never authorizes deleting a descriptor.
    {
        CleanStorage();
        auto d=MakeDescriptor(303,0,3507,4,1,'a',7);
        Check(wqn::SaveLocalWordSessionDescriptor(d)==ESP_OK,"save corrupt-family fixture");
        const std::string rel="/wl"+Token(d.session.remote.session_id)+".v1";
        auto bytes=ReadFile(rel); bytes.back()^=0x40; WriteFile(rel,bytes);
        Check(wqn::PruneLocalWordSessionDescriptors("")==ESP_OK && PathExists(rel),
              "corrupt descriptor remains opaque during GC");
        wqn::LocalDescriptorRetention pins;
        Check(wqn::CollectRetainedLocalSnapshots(&pins)!=ESP_OK,
              "unreadable descriptor prevents unsafe pinned-pack pruning");
    }

    std::printf("%d PASS / %d FAIL (real descriptor codec/files/seam; NOT HIL)\n",
                passes, failures);
    return failures ? 1 : 0;
}
