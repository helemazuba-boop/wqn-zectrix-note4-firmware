// Production replay/checkpoint/load bodies; storage operations are host mocks.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
constexpr esp_err_t ESP_ERR_NOT_FOUND = 0x105;
constexpr esp_err_t ESP_ERR_NO_MEM = 0x101;
constexpr const char* kTag = "replay-fixture";
void FixtureLog(const char*, const char*, ...) {}
#define ESP_LOGI(...) FixtureLog(__VA_ARGS__)
#define ESP_LOGW(...) FixtureLog(__VA_ARGS__)
#define ESP_LOGE(...) FixtureLog(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr, ...) do { \
    const esp_err_t fixture_error = (expr); \
    if (fixture_error != ESP_OK) return fixture_error; \
} while (false)

namespace wqn::protocol::word_study_v1 {
enum class Mode : uint8_t {
    kSequential, kRandom, kDictionary, kReview, kIntake, kShuffle, kMistakes
};
}
using Mode = wqn::protocol::word_study_v1::Mode;
namespace wqn {
enum class WordPresentationPhase : uint8_t { kFront, kBack };
struct PersistedWordSession {
    struct Item { uint64_t ordinal; };
    struct Remote {
        Mode mode = Mode::kReview;
        std::string session_id;
        uint64_t next_sequence = 0;
        std::vector<Item> items;
        std::string cursor;
    } remote;
    uint32_t position = 0;
    WordPresentationPhase phase = WordPresentationPhase::kFront;
    bool paused = false;
    bool active = true;
    uint32_t deck_scope_generation = 7;
};
template <typename T> using WordStorePsramAllocator = std::allocator<T>;
uint32_t scope_generation = 7;
uint32_t GetDeckScopeGeneration() { return scope_generation; }
constexpr size_t kWordObservationOutboxCapacity = 1000;
enum class OutboxSuspendReason : uint8_t { kProtocol = 1 };
const char* OutboxSuspendReasonName(OutboxSuspendReason) { return "fixture"; }
struct DurableWordObservation {
    std::string request_id, session_id;
    uint64_t sequence = 0;
    uint32_t next_position = 0;
    WordPresentationPhase next_phase = WordPresentationPhase::kFront;
    Mode mode = Mode::kReview;
};
}
using Session = wqn::PersistedWordSession;
struct OutboxRecord {
    char request_id[65] = {};
    char session_id[37] = {};
    uint64_t sequence = 0;
    uint32_t next_position = 0;
    uint8_t next_phase = 0;
    uint8_t mode = static_cast<uint8_t>(Mode::kReview);
};
struct OutboxScan {
    std::vector<OutboxRecord> acknowledged, pending, suspended;
    size_t total_records = 0;
    size_t ack_records = 0;
    size_t suspend_records = 0;
    std::vector<uint16_t> suspended_reasons;
    bool partial_tail = false, backup_source = false;
};
constexpr size_t kRuntimeCompactAckThreshold = 32;
@@SESSION_GENERATION@@
@@STEPPED_MAINTENANCE@@
@@MODES@@
const std::string sid = "00000000-0000-0000-0000-000000000001";
Session disk[7];
bool present[7] = {};
OutboxScan journal, cache;
bool cache_loaded = false, cursor_found = false, cursor_paused = false;
esp_err_t read_error = ESP_OK, scan_error = ESP_OK;
esp_err_t save_error = ESP_OK, compact_error = ESP_OK;
int reads = 0, scans = 0, saves = 0, compactions = 0, cursor_reads = 0;
int fail_save_call = 0;
int cursor_writes = 0;
esp_err_t cursor_write_error = ESP_OK;
std::string io_order;
int appends = 0;
esp_err_t append_error = ESP_OK;
int64_t fixture_now = 0;
int64_t esp_timer_get_time() { return ++fixture_now; }
enum class OutboxRecordKind { kObservation, kAck };
wqn::DurableWordObservation ObservationFromRecord(const OutboxRecord& record) {
    return {record.request_id, record.session_id, record.sequence, record.next_position,
            static_cast<wqn::WordPresentationPhase>(record.next_phase),
            static_cast<Mode>(record.mode)};
}
bool SameObservation(const wqn::DurableWordObservation& a,
                     const wqn::DurableWordObservation& b) {
    return a.request_id == b.request_id && a.session_id == b.session_id &&
        a.sequence == b.sequence && a.next_position == b.next_position &&
        a.next_phase == b.next_phase && a.mode == b.mode;
}
esp_err_t BuildObservationRecord(const wqn::DurableWordObservation& observation,
                                OutboxRecordKind, OutboxRecord* record) {
    std::snprintf(record->request_id, sizeof(record->request_id), "%s", observation.request_id.c_str());
    std::snprintf(record->session_id, sizeof(record->session_id), "%s", observation.session_id.c_str());
    record->sequence = observation.sequence;
    record->next_position = observation.next_position;
    record->next_phase = static_cast<uint8_t>(observation.next_phase);
    record->mode = static_cast<uint8_t>(observation.mode);
    return ESP_OK;
}
esp_err_t AppendOutboxRecord(const OutboxRecord&, int64_t* = nullptr, size_t* = nullptr) {
    ++appends;
    return append_error;
}
int forensic_appends = 0;
esp_err_t AppendRejectedOutboxRecord(const OutboxRecord&) { ++forensic_appends; return ESP_OK; }
esp_err_t BuildSuspendRecord(const std::string& request, wqn::OutboxSuspendReason, OutboxRecord* out) {
    std::snprintf(out->request_id, sizeof(out->request_id), "%s", request.c_str()); return ESP_OK;
}

size_t Slot(Mode mode) {
    switch (mode) {
        case Mode::kReview: return 0;
        case Mode::kIntake: return 1;
        case Mode::kSequential: return 2;
        case Mode::kRandom: return 3;
        case Mode::kDictionary: return 4;
        case Mode::kShuffle: return 5;
        case Mode::kMistakes: return 6;
    }
    return 0;
}
esp_err_t LoadSessionRaw(Mode mode, Session* out) {
    ++reads;
    if (read_error != ESP_OK) return read_error;
    if (!present[Slot(mode)]) return ESP_ERR_NOT_FOUND;
    *out = disk[Slot(mode)];
    return ESP_OK;
}
struct SessionPaths { const char* primary; const char* temporary; const char* backup; };
bool GetSessionPaths(Mode, SessionPaths* paths) { *paths = {"mock", "mock.tmp", "mock.bak"}; return true; }
struct SessionHeader {
    uint32_t magic; uint16_t version, reserved; uint32_t payload_size, payload_crc;
};
constexpr uint32_t kSessionMagic = 0;
constexpr uint16_t kSessionSchemaVersion = 4;
Session encoded_session;
bool EncodeSession(const Session& session, std::vector<uint8_t>* output) {
    encoded_session = session; output->assign(1, 0); return true;
}
uint32_t Crc32(const void*, size_t) { return 0; }
esp_err_t AtomicWrite(const char*, const char*, const char*, const void*, size_t, bool) {
    ++saves;
    io_order += 'S';
    if (save_error != ESP_OK) return save_error;
    if (fail_save_call == saves) return ESP_FAIL;
    disk[Slot(encoded_session.remote.mode)] = encoded_session;
    present[Slot(encoded_session.remote.mode)] = true;
    return ESP_OK;
}
@@RAW_SAVE@@
esp_err_t WriteSessionCursor(const Session&) {
    ++cursor_writes;
    return cursor_write_error;
}
esp_err_t EnsureOutboxCache(OutboxScan** out) {
    ++scans;
    if (scan_error != ESP_OK) return scan_error;
    if (!cache_loaded) { cache = journal; cache_loaded = true; }
    *out = &cache;
    return ESP_OK;
}
@@OUTBOX_QUOTA@@
esp_err_t CompactCachedOutbox(OutboxScan* scan) {
    ++compactions;
    io_order += 'C';
    if (compact_error != ESP_OK) return compact_error;
    scan->acknowledged.clear();
    scan->ack_records = 0;
    scan->total_records = scan->pending.size() + 2 * scan->suspended.size();
    scan->partial_tail = false;
    scan->backup_source = false;
    journal = *scan;
    return ESP_OK;
}
bool ReadSessionCursorPaused(Mode, const std::string& session_id, bool* out) {
    ++cursor_reads;
    if (!cursor_found || session_id != sid) return false;
    *out = cursor_paused;
    return true;
}

@@RECONCILE@@
@@CHECKPOINT@@
@@RUNTIME@@
@@LOAD@@
@@SAVE@@
@@COMMIT@@
@@ACK@@
@@TERMINAL@@

namespace wqn::runtime {
enum class SleepBlocker { kStorage };
int active_leases = 0, acquired_leases = 0, released_leases = 0;
bool lease_allowed = true;
struct SleepLease {
    bool held;
    explicit SleepLease(bool acquired) : held(acquired) {
        if (held) { ++active_leases; ++acquired_leases; }
    }
    SleepLease(const SleepLease&) = delete;
    ~SleepLease() { if (held) { --active_leases; ++released_leases; } }
    explicit operator bool() const { return held; }
    static SleepLease TryAcquire(SleepBlocker, const char*, const char*, int) {
        return SleepLease(lease_allowed);
    }
};
}
namespace wqn::services {
esp_err_t ExecuteForegroundStorageTransaction(esp_err_t (*)(void*), void*, const char*);
using StorageTransaction = esp_err_t (*)(void*);
int dispatches = 0, inject_at = 0, fail_dispatch_at = 0;
bool lease_expected = true, lease_continuous = true;
std::function<void()> interleave;
std::vector<std::string> owners;
esp_err_t ExecuteStorageTransactionNamed(StorageTransaction transaction, void* context, const char* owner) {
    ++dispatches; owners.push_back(owner);
    if (lease_expected && runtime::active_leases != 1) lease_continuous = false;
    if (dispatches == inject_at && interleave) interleave();
    if (dispatches == fail_dispatch_at) return ESP_FAIL;
    return transaction(context);
}
}
@@MAINTENANCE_DRIVER@@
namespace wqn::services {
esp_err_t ExecuteForegroundStorageTransaction(esp_err_t (*transaction)(void*), void* context, const char* owner) {
    return ExecuteStorageTransactionNamed(transaction, context, owner);
}
}
@@PUBLIC_MAINTENANCE@@

int passes = 0, failures = 0;
void Check(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    condition ? ++passes : ++failures;
}
void Reset() {
    for (auto& session : disk) session = {};
    std::fill(std::begin(present), std::end(present), false);
    present[0] = present[1] = true;
    disk[0] = {};
    disk[0].remote.session_id = sid;
    disk[0].remote.items = {{10}, {11}, {12}};
    disk[1] = {};
    disk[1].remote.mode = Mode::kIntake;
    disk[1].remote.session_id = "00000000-0000-0000-0000-000000000002";
    disk[1].remote.items = {{10}, {11}, {12}};
    journal = {}; cache = {}; cache_loaded = false;
    g_session_mutation_generation = 0; wqn::scope_generation = 7; fixture_now = 0;
    cursor_found = false; cursor_paused = false;
    read_error = scan_error = save_error = compact_error = ESP_OK;
    reads = scans = saves = compactions = cursor_reads = 0;
    fail_save_call = 0;
    cursor_writes = 0; cursor_write_error = ESP_OK;
    appends = 0;
    forensic_appends = 0;
    append_error = ESP_OK;
    wqn::runtime::active_leases = wqn::runtime::acquired_leases = wqn::runtime::released_leases = 0;
    wqn::runtime::lease_allowed = true;
    wqn::services::dispatches = wqn::services::inject_at = wqn::services::fail_dispatch_at = 0;
    wqn::services::lease_expected = wqn::services::lease_continuous = true;
    wqn::services::interleave = {}; wqn::services::owners.clear();
    io_order.clear();
}
OutboxRecord Record(uint64_t seq = 0, uint32_t next = 11,
                    const std::string& session_id = sid, Mode mode = Mode::kReview) {
    OutboxRecord record = {};
    std::snprintf(record.request_id, sizeof(record.request_id), "request-%llu",
                  static_cast<unsigned long long>(seq));
    std::snprintf(record.session_id, sizeof(record.session_id), "%s", session_id.c_str());
    record.sequence = seq;
    record.next_position = next;
    record.next_phase = static_cast<uint8_t>(wqn::WordPresentationPhase::kBack);
    record.mode = static_cast<uint8_t>(mode);
    return record;
}
esp_err_t Load(Session* out, Mode mode = Mode::kReview) {
    journal.total_records = journal.acknowledged.size() * 2 +
        journal.pending.size() + journal.suspended.size() * 2;
    LoadSessionContext context{mode, out};
    return LoadSessionTransaction(&context);
}

#if FIXTURE_STEPPED_MAINTENANCE
void DueMaintenance() {
    Reset(); cache_loaded = true;
    cache.acknowledged.push_back(Record());
    cache.ack_records = kRuntimeCompactAckThreshold;
    journal = cache;
}
esp_err_t FinishMaintenance(OutboxMaintenanceContext* context) {
    // Test seam models explicit owner boundaries, NOT the FreeRTOS scheduler.
    for (int i = 0; i < 8 && !context->done; ++i) {
        const esp_err_t result = OutboxMaintenanceStepTransaction(context);
        if (result != ESP_OK) return result;
    }
    return context->done ? ESP_OK : ESP_ERR_INVALID_STATE;
}
void TestMaintenanceSteps() {
    Session out;
    DueMaintenance(); OutboxMaintenanceContext context;
    bool one_mode_per_step = true;
    for (size_t i = 0; i < 7; ++i) {
        const int before_reads = reads, before_saves = saves;
        one_mode_per_step = one_mode_per_step &&
            OutboxMaintenanceStepTransaction(&context) == ESP_OK &&
            reads - before_reads <= 1 && saves - before_saves <= 1 && compactions == 0;
    }
    Check(one_mode_per_step && context.next_mode == 7 && saves == 1 &&
          disk[0].remote.next_sequence == 1 && !context.done,
          "each maintenance step checkpoints at most one mode and never reclaims early");
    Check(OutboxMaintenanceStepTransaction(&context) == ESP_OK && context.done &&
          !context.deferred && compactions == 1 && cache.acknowledged.empty(),
          "reclaim is a separate final step after all checkpoint proofs");
    const int old_scans = scans;
    Check(OutboxMaintenanceStepTransaction(&context) == ESP_OK && compactions == 1 && scans == old_scans,
          "completed maintenance context cannot reclaim twice");

    DueMaintenance(); context = {};
    for (int i = 0; i < 4; ++i) OutboxMaintenanceStepTransaction(&context);
    cache.pending.push_back(Record(1, 12));
    Check(FinishMaintenance(&context) == ESP_OK && !context.deferred &&
          journal.pending.size() == 1 && journal.pending.front().sequence == 1 && compactions == 1,
          "reclaim includes new pending observations accepted between checkpoint steps");
    Check(Load(&out) == ESP_OK && out.remote.next_sequence == 2 && saves == 1,
          "new pending progress remains replayable after staged ACK reclamation");

    DueMaintenance(); context = {}; OutboxMaintenanceStepTransaction(&context);
    cache.acknowledged.push_back(Record(1, 12)); ++cache.ack_records;
    Check(OutboxMaintenanceStepTransaction(&context) == ESP_OK && context.done && context.deferred &&
          compactions == 0 && cache.acknowledged.size() == 2,
          "new ACKs between steps invalidate the reclamation proof without dropping records");
    DueMaintenance(); context = {}; OutboxMaintenanceStepTransaction(&context);
    cache.acknowledged.front().next_position = 12;
    Check(OutboxMaintenanceStepTransaction(&context) == ESP_OK && context.deferred && compactions == 0,
          "equal ACK counts cannot hide changed payloads or ABA record replacement");

    DueMaintenance(); context = {}; OutboxMaintenanceStepTransaction(&context);
    SaveSessionRaw(disk[0]);
    Check(OutboxMaintenanceStepTransaction(&context) == ESP_OK && context.deferred && compactions == 0,
          "an interleaved snapshot write invalidates the proof through the production raw-save hook");
    DueMaintenance(); context = {}; OutboxMaintenanceStepTransaction(&context);
    save_error = ESP_FAIL; SaveSessionRaw(disk[0]);
    Check(OutboxMaintenanceStepTransaction(&context) == ESP_OK && context.deferred && compactions == 0,
          "failed snapshot write attempts also invalidate an older checkpoint proof");
    DueMaintenance(); context = {}; OutboxMaintenanceStepTransaction(&context);
    ++wqn::scope_generation;
    Check(OutboxMaintenanceStepTransaction(&context) == ESP_OK && context.deferred && compactions == 0,
          "scope changes between owner steps invalidate the checkpoint proof");

    DueMaintenance(); context = {}; context.deadline_us = 1;
    Check(OutboxMaintenanceStepTransaction(&context) == ESP_OK && context.deferred && scans == 0 &&
          reads == 0 && compactions == 0,
          "expired maintenance deadline skips work before loading the cache");
    DueMaintenance(); context = {}; OutboxMaintenanceStepTransaction(&context);
    context.deadline_us = 2; fixture_now = 2;
    Check(OutboxMaintenanceStepTransaction(&context) == ESP_OK && context.deferred &&
          context.next_mode == 1 && compactions == 0,
          "a deadline expiring while queued prevents the next checkpoint from starting");
    DueMaintenance(); context = {};
    for (int i = 0; i < 7; ++i) OutboxMaintenanceStepTransaction(&context);
    context.deadline_us = 1;
    Check(OutboxMaintenanceStepTransaction(&context) == ESP_OK && context.deferred && compactions == 0 &&
          cache.acknowledged.size() == 1 && disk[0].remote.next_sequence == 1,
          "deadline between checkpoint and reclaim preserves the journal for a fresh pass");
    context = {};
    Check(FinishMaintenance(&context) == ESP_OK && compactions == 1 && saves == 1,
          "fresh maintenance resumes safely without resaving an already-covered snapshot");

    DueMaintenance(); context = {}; save_error = ESP_FAIL;
    Check(FinishMaintenance(&context) == ESP_FAIL && compactions == 0 &&
          cache.acknowledged.size() == 1 && disk[0].remote.next_sequence == 0,
          "checkpoint failure stops staged reclamation and propagates the write error");
    DueMaintenance(); context = {}; compact_error = ESP_FAIL;
    Check(FinishMaintenance(&context) == ESP_FAIL && !context.done && compactions == 1 &&
          cache.acknowledged.size() == 1 && disk[0].remote.next_sequence == 1,
          "reclaim failure reports an error and retains acknowledged progress in cache");
    DueMaintenance(); context = {}; scan_error = ESP_FAIL;
    Check(FinishMaintenance(&context) == ESP_FAIL && !context.started && compactions == 0,
          "cache I/O failure cannot manufacture a checkpoint proof");
    DueMaintenance(); context = {}; --cache.ack_records;
    Check(FinishMaintenance(&context) == ESP_OK && !context.started && reads == 0 && compactions == 0,
          "runtime maintenance preserves the existing 32-ACK trigger");
    context = {}; context.for_sleep = true; cache.ack_records = 1;
    Check(FinishMaintenance(&context) == ESP_OK && context.started && compactions == 1,
          "sleep maintenance still reclaims a single ACK through the same step protocol");
    DueMaintenance(); context = {}; cache.ack_records = 0; cache.acknowledged.clear();
    cache.pending.push_back(Record()); cache.partial_tail = true; context.for_sleep = true;
    Check(FinishMaintenance(&context) == ESP_OK && saves == 1 && compactions == 1 && journal.pending.size() == 1,
          "sleep tail repair checkpoints progress and retains pending payloads without ACKs");
    DueMaintenance(); context = {}; OutboxMaintenanceStepTransaction(&context);
    cache.backup_source = true;
    Check(OutboxMaintenanceStepTransaction(&context) == ESP_OK && context.deferred && compactions == 0,
          "a changed fallback source cannot reuse an older reclamation proof");
    DueMaintenance(); context = {};
    for (int i = 0; i < 4; ++i) OutboxMaintenanceStepTransaction(&context);
    cache.suspended.push_back(Record(4, 11, "parked-session"));
    Check(FinishMaintenance(&context) == ESP_OK && journal.suspended.size() == 1 && compactions == 1,
          "interleaved parked payloads survive rewriting the live cache");
    Check(OutboxMaintenanceStepTransaction(nullptr) == ESP_ERR_INVALID_ARG,
          "null maintenance context is rejected");
}

void PrepareAckBoundary() {
    Reset(); cache_loaded = true; cache.pending.push_back(Record());
    cache.ack_records = kRuntimeCompactAckThreshold - 1;
}
void TestMaintenanceDriver() {
    PrepareAckBoundary(); const std::string request = cache.pending.front().request_id;
    Check(wqn::AcknowledgeWordObservation(request) == ESP_OK && wqn::services::dispatches == 9 &&
          wqn::services::owners.front() == "word-outbox-ack" &&
          std::count(wqn::services::owners.begin(), wqn::services::owners.end(), "word-outbox-maintenance") == 8 &&
          compactions == 1 && saves == 1 && appends == 1,
          "public ACK dispatches a durable ACK plus seven checkpoint boundaries and a separate reclaim");
    Check(wqn::services::lease_continuous && wqn::runtime::active_leases == 0 &&
          wqn::runtime::acquired_leases == 1 && wqn::runtime::released_leases == 1,
          "one caller lease spans all maintenance queue waits and releases on success (host seam)");

    PrepareAckBoundary(); wqn::services::inject_at = 9;
    bool foreground_committed = false;
    wqn::services::interleave = [&]() {
        Session advanced = disk[0]; advanced.remote.next_sequence = 2; advanced.position = 2;
        advanced.phase = wqn::WordPresentationPhase::kBack;
        const auto new_observation = ObservationFromRecord(Record(1, 12));
        CommitContext commit{&new_observation, &advanced};
        foreground_committed = CommitObservationTransaction(&commit) == ESP_OK;
    };
    Check(wqn::AcknowledgeWordObservation(request) == ESP_OK && foreground_committed &&
          appends == 2 && compactions == 1 && journal.pending.size() == 1 &&
          journal.pending.front().sequence == 1 && disk[0].remote.next_sequence == 1,
          "production commit at an injected boundary remains durable in the live rewrite set");

    PrepareAckBoundary(); wqn::services::inject_at = 3;
    wqn::services::interleave = []() { SaveSessionRaw(disk[0]); };
    Check(wqn::AcknowledgeWordObservation(request) == ESP_OK && wqn::services::dispatches == 3 &&
          compactions == 0 && cache.acknowledged.size() == 1 && cache.pending.empty(),
          "invalidated proof defers maintenance without falsely failing the already-durable ACK");

    PrepareAckBoundary(); wqn::runtime::lease_allowed = false;
    Check(wqn::AcknowledgeWordObservation(request) == ESP_ERR_INVALID_STATE && appends == 0 &&
          wqn::services::dispatches == 0 && cache.pending.size() == 1,
          "lease denial rejects before any ACK dispatch or cache mutation");
    PrepareAckBoundary(); append_error = ESP_FAIL;
    Check(wqn::AcknowledgeWordObservation(request) == ESP_FAIL && wqn::services::dispatches == 1 &&
          cache.pending.size() == 1 && cache.acknowledged.empty() &&
          wqn::runtime::active_leases == 0 && wqn::runtime::released_leases == 1,
          "ACK write failure propagates, skips maintenance and releases the lease");
    PrepareAckBoundary(); save_error = ESP_FAIL;
    Check(wqn::AcknowledgeWordObservation(request) == ESP_FAIL && compactions == 0 &&
          cache.acknowledged.size() == 1 && wqn::runtime::active_leases == 0 &&
          wqn::runtime::released_leases == 1,
          "checkpoint I/O failure remains visible through the public ACK API");
    PrepareAckBoundary(); wqn::services::fail_dispatch_at = 2;
    Check(wqn::AcknowledgeWordObservation(request) == ESP_FAIL && cache.acknowledged.size() == 1 &&
          compactions == 0 && wqn::runtime::active_leases == 0,
          "maintenance dispatch failure does not lose the durable ACK and still reports an error");

    DueMaintenance(); wqn::runtime::lease_allowed = false; wqn::services::lease_expected = false;
    Check(wqn::PrepareWordObservationOutboxForSleep(0) == ESP_OK && compactions == 1 &&
          wqn::services::dispatches == 8 && wqn::runtime::acquired_leases == 0,
          "sleep quiesce path uses owner steps directly without trying to acquire a rejected new lease");
    DueMaintenance(); wqn::services::lease_expected = false;
    Check(wqn::PrepareWordObservationOutboxForSleep(1) == ESP_OK && compactions == 0 && reads == 0 &&
          cache.acknowledged.size() == 1 && wqn::services::dispatches == 1,
          "public sleep maintenance checks the absolute deadline inside the dispatched owner step");
}
#endif

void TestOutboxQuota() {
    constexpr size_t limit = 2 * wqn::kWordObservationOutboxCapacity + 2 * kRuntimeCompactAckThreshold;
    Reset(); cache_loaded = true; cache.total_records = limit - 1;
    Session advanced = disk[0]; advanced.position = 1; advanced.remote.next_sequence = 1;
    advanced.phase = wqn::WordPresentationPhase::kBack;
    const auto observation = ObservationFromRecord(Record());
    CommitContext commit{&observation, &advanced};
    Check(CommitObservationTransaction(&commit) == ESP_ERR_INVALID_SIZE && appends == 0 &&
          cache.pending.empty() && compactions == 0,
          "new observation reserves its future terminal marker before any append, without inline reclaim");
    Reset(); cache_loaded = true; cache.total_records = limit - 2;
    Check(CommitObservationTransaction(&commit) == ESP_OK && appends == 1 &&
          cache.total_records == limit - 1 && cache.pending.size() == 1,
          "exact two-slot boundary admits the observation");
    const std::string request = observation.request_id;
    AckContext ack{&request};
    Check(AckObservationTransaction(&ack) == ESP_OK && cache.total_records == limit &&
          cache.pending.empty() && appends == 2,
          "reserved terminal slot remains writable at the exact hard boundary");
    Reset(); cache_loaded = true; cache.total_records = limit - 1;
    cache.pending.push_back(Record());
    Check(wqn::AcknowledgeWordObservation(request) == ESP_OK && compactions == 1 &&
          cache.total_records == 0 && cache.pending.empty() && appends == 1,
          "the final ACK triggers space-pressure reclaim even when it is the last pending item");

    Reset(); cache_loaded = true;
    bool offline_capacity = true;
    for (size_t i = 0; i < wqn::kWordObservationOutboxCapacity; ++i) {
        const auto next = ObservationFromRecord(Record(i));
        advanced.remote.next_sequence = i + 1;
        CommitContext next_commit{&next, &advanced};
        offline_capacity = offline_capacity && CommitObservationTransaction(&next_commit) == ESP_OK;
    }
    Check(offline_capacity && cache.pending.size() == 1000 && appends == 1000 &&
          cache.total_records == 1000 && compactions == 0 && saves == 0,
          "all 1000 offline observations remain admissible without reducing capacity or checkpointing answers");
    auto extra = ObservationFromRecord(Record(1000)); advanced.remote.next_sequence = 1001;
    CommitContext extra_commit{&extra, &advanced};
    Check(CommitObservationTransaction(&extra_commit) == ESP_ERR_NO_MEM && appends == 1000,
          "the existing 1001st-live-observation rejection is unchanged");
    bool terminal_capacity = true;
    while (!cache.pending.empty()) {
        const std::string head = cache.pending.front().request_id;
        AckContext head_ack{&head};
        if (AckObservationTransaction(&head_ack) != ESP_OK) { terminal_capacity = false; break; }
    }
    Check(terminal_capacity && cache.acknowledged.size() == 1000 && cache.total_records == 2000,
          "every admitted offline observation has a reserved ACK slot even without any maintenance");

    Reset(); cache_loaded = true; cache.total_records = limit + 1;
    cache.acknowledged.push_back(Record()); cache.ack_records = 1;
    cache.pending.push_back(Record(1, 12));
    const std::string legacy_request = cache.pending.front().request_id;
    AckContext legacy_ack{&legacy_request};
    Check(AckObservationTransaction(&legacy_ack) == ESP_ERR_INVALID_SIZE && appends == 0 &&
          cache.pending.size() == 1,
          "legacy over-budget ACK is rejected without growing or deleting its pending record");
    QuarantineContext quarantine{&legacy_request};
    Check(QuarantineObservationTransaction(&quarantine) == ESP_ERR_INVALID_SIZE &&
          forensic_appends == 0 && appends == 0,
          "quota rejection happens before quarantine touches the forensic journal");
    Check(wqn::AcknowledgeWordObservation(legacy_request) == ESP_OK && compactions == 1 &&
          saves == 1 && disk[0].remote.next_sequence == 2 && appends == 1 &&
          cache.total_records == 2 && cache.pending.empty(),
          "public ACK heals oversized legacy history via stepped reclaim then retries exactly once");
    Check(wqn::services::lease_continuous && wqn::runtime::acquired_leases == 1 &&
          wqn::runtime::released_leases == 1 && wqn::services::dispatches == 11,
          "legacy reclaim and retry share one lease and bounded owner dispatches");

    Reset(); cache_loaded = true; cache.total_records = limit + 1;
    cache.pending.push_back(Record(1, 12)); cache.acknowledged.push_back(Record());
    wqn::services::inject_at = 3;
    wqn::services::interleave = []() { SaveSessionRaw(disk[0]); };
    Check(wqn::AcknowledgeWordObservation(legacy_request) == ESP_ERR_INVALID_SIZE &&
          compactions == 0 && appends == 0 && cache.pending.size() == 1 &&
          wqn::runtime::active_leases == 0 && wqn::services::dispatches == 3,
          "invalidated forced-reclaim proof preserves the original quota error and pending payload");
    Reset(); cache_loaded = true; cache.total_records = limit + 1;
    cache.pending.push_back(Record(1, 12)); cache.acknowledged.push_back(Record());
    save_error = ESP_FAIL;
    Check(wqn::AcknowledgeWordObservation(legacy_request) == ESP_FAIL && compactions == 0 &&
          appends == 0 && cache.pending.size() == 1,
          "failed legacy checkpoint blocks both reclamation and ACK retry");
    Reset(); cache_loaded = true; cache.total_records = limit + 1;
    cache.pending.push_back(Record(1, 12)); cache.acknowledged.push_back(Record());
    compact_error = ESP_FAIL;
    Check(wqn::AcknowledgeWordObservation(legacy_request) == ESP_FAIL && appends == 0 &&
          cache.pending.size() == 1 && wqn::runtime::active_leases == 0,
          "failed forced compaction stays visible and never consumes the reserved ACK");
    for (bool backup : {false, true}) {
        Reset(); cache_loaded = true; cache.pending.push_back(Record(1, 12));
        cache.acknowledged.push_back(Record()); cache.ack_records = 1; cache.total_records = 3;
        cache.backup_source = backup; cache.partial_tail = !backup;
        Check(wqn::AcknowledgeWordObservation(legacy_request) == ESP_OK && compactions == 1 &&
              saves == 1 && cache.pending.empty() && appends == 1 && cache.total_records == 2,
              "ACK repairs damaged or backup journal before appending its marker");
    }
    Reset(); cache_loaded = true; cache.total_records = limit + 1;
    cache.pending.push_back(Record(1, 12)); cache.acknowledged.push_back(Record());
    Check(wqn::QuarantinePendingWordObservation(legacy_request) == ESP_OK && compactions == 1 &&
          forensic_appends == 1 && appends == 1 && cache.total_records == 2 && cache.pending.empty(),
          "quarantine uses the same fenced legacy-reclaim path and retains forensic-before-ACK order");
    Reset(); cache_loaded = true; cache.pending.push_back(Record(1, 12));
    cache.suspended.push_back(Record()); cache.suspended_reasons.push_back(1);
    cache.partial_tail = true; cache.total_records = 3;
    Check(wqn::SuspendPendingWordObservation(legacy_request, wqn::OutboxSuspendReason::kProtocol) == ESP_OK &&
          compactions == 1 && cache.suspended.size() == 2 && cache.pending.empty() && appends == 1 &&
          !cache.partial_tail && cache.total_records == 4,
          "a later park repairs a damaged journal without losing previously parked data");

#if FIXTURE_OUTBOX_QUOTA
    Reset(); cache_loaded = true; cache.pending.push_back(Record());
    cache.total_records = SIZE_MAX;
    Check(CheckOutboxAppendBudget(cache, true) == ESP_ERR_INVALID_SIZE &&
          CheckOutboxAppendBudget(cache, false) == ESP_ERR_INVALID_SIZE,
          "overflow-sized counters cannot wrap the quota arithmetic");
    Reset(); cache_loaded = true; cache.total_records = limit;
    cache.acknowledged.push_back(Record()); cache.ack_records = 1;
    OutboxMaintenanceContext pressure;
    for (size_t i = 0; i < 8 && !pressure.done; ++i) OutboxMaintenanceStepTransaction(&pressure);
    Check(pressure.done && compactions == 1 && saves == 1 && cache.total_records == 0,
          "space pressure triggers runtime reclaim even below the 32-ACK gate");
    Reset(); cache_loaded = true; cache.total_records = limit; cache.pending.push_back(Record());
    cache.suspended.assign(998, Record());
    Check(CheckOutboxAppendBudget(cache, false) == ESP_ERR_INVALID_SIZE,
          "terminal markers do not bypass an exhausted reserve budget");
#endif
}

int main() {
    Session out;
    Reset(); journal.pending.push_back(Record());
    Check(Load(&out) == ESP_OK && out.position == 1 && out.remote.next_sequence == 1 &&
          out.phase == wqn::WordPresentationPhase::kBack && saves == 0 && compactions == 0 &&
          disk[0].position == 0 && disk[0].remote.next_sequence == 0,
          "ordinary pending replay returns advanced state without rewriting snapshot");
    cache_loaded = false;
    Check(Load(&out) == ESP_OK && out.position == 1 && out.remote.next_sequence == 1 &&
          saves == 0 && disk[0].remote.next_sequence == 0,
          "second simulated boot recovers again from unchanged snapshot and durable journal");
    Reset(); journal.acknowledged.push_back(Record());
    Check(Load(&out) == ESP_OK && out.remote.next_sequence == 1 && saves == 0 &&
          journal.acknowledged.size() == 1,
          "acknowledged progress remains replayable until explicit maintenance");
    Reset(); journal.suspended.push_back(Record());
    Check(Load(&out) == ESP_OK && out.remote.next_sequence == 1 && saves == 0 &&
          journal.suspended.size() == 1,
          "suspended progress is replayed without discarding parked payload");
    Reset(); journal.acknowledged.push_back(Record());
    journal.pending.push_back(Record(1, 12)); journal.suspended.push_back(Record(2, 13));
    Check(Load(&out) == ESP_OK && out.position == 3 && out.remote.next_sequence == 3 && saves == 0,
          "all three durable record classes advance the cursor through the candidate window");
    Reset(); disk[0].remote.next_sequence = 2; disk[0].position = 2;
    journal.pending.push_back(Record());
    Check(Load(&out) == ESP_OK && out.position == 2 && out.remote.next_sequence == 2 && saves == 0,
          "older records cannot rewind a newer checkpoint");
    Reset(); journal.pending.push_back(Record(0, 11, "different-session"));
    Check(Load(&out) == ESP_OK && out.position == 0 && saves == 0,
          "another session cannot advance this snapshot");
    Reset(); journal.pending.push_back(Record(0, 99));
    Check(Load(&out) == ESP_ERR_INVALID_STATE && out.position == 0 &&
          out.remote.next_sequence == 0 && saves == 0 && compactions == 0 && cursor_reads == 0,
          "unrepresentable durable progress is not reported as a successful older-card restore");
    Reset(); read_error = ESP_ERR_NOT_FOUND;
    Check(Load(&out) == ESP_ERR_NOT_FOUND && scans == 0 && saves == 0 && cursor_reads == 0,
          "unused mode remains NOT_FOUND without journal or cursor I/O");
    Reset(); read_error = ESP_FAIL;
    Check(Load(&out) == ESP_FAIL && scans == 0 && saves == 0,
          "snapshot read failure is propagated");
    Reset(); scan_error = ESP_FAIL;
    Check(Load(&out) == ESP_FAIL && saves == 0 && cursor_reads == 0,
          "journal read failure cannot masquerade as recovered progress");
    Reset(); journal.pending.push_back(Record()); save_error = ESP_FAIL;
    Check(Load(&out) == ESP_OK && out.remote.next_sequence == 1 && saves == 0,
          "ordinary replay does not depend on an unnecessary snapshot write succeeding");
    Reset(); journal.pending.push_back(Record()); cursor_found = true; cursor_paused = true;
    Check(Load(&out) == ESP_OK && out.position == 1 && out.paused && saves == 0,
          "NVS paused overlay is still applied after replay");
    Reset(); disk[0].paused = true; cursor_found = true;
    Check(Load(&out) == ESP_OK && !out.paused && saves == 0,
          "NVS resumed overlay can override an older paused snapshot");
    Reset(); disk[0].paused = true;
    Check(Load(&out) == ESP_OK && out.paused && saves == 0,
          "missing cursor retains the snapshot paused value");
    Reset(); journal.acknowledged.push_back(Record()); journal.partial_tail = true;
    Check(Load(&out) == ESP_OK && out.remote.next_sequence == 1 && disk[0].remote.next_sequence == 1 &&
          saves == 1 && compactions == 1 && io_order == "SC" && journal.acknowledged.empty(),
          "torn-tail repair checkpoints once before reclaiming acknowledged progress");
    Reset(); journal.acknowledged.push_back(Record()); journal.backup_source = true;
    Check(Load(&out) == ESP_OK && out.remote.next_sequence == 1 && disk[0].remote.next_sequence == 1 &&
          saves == 1 && compactions == 1 && io_order == "SC",
          "backup journal repair retains checkpoint-before-compaction ordering");
    Reset(); journal.acknowledged.push_back(Record()); journal.partial_tail = true; save_error = ESP_FAIL;
    Check(Load(&out) == ESP_FAIL && compactions == 0 && journal.acknowledged.size() == 1 &&
          disk[0].remote.next_sequence == 0 && cursor_reads == 0,
          "failed checkpoint prevents repair from reclaiming the only durable progress");
    Reset(); journal.acknowledged.push_back(Record()); journal.partial_tail = true; compact_error = ESP_FAIL;
    Check(Load(&out) == ESP_FAIL && disk[0].remote.next_sequence == 1 &&
          journal.acknowledged.size() == 1 && io_order == "SC" && cursor_reads == 0,
          "failed compaction is reported without deleting the replay source");
    Reset(); journal.acknowledged.push_back(Record()); journal.partial_tail = true;
    cursor_found = true; cursor_paused = true;
    Check(Load(&out) == ESP_OK && out.paused && disk[0].remote.next_sequence == 1 && io_order == "SC",
          "mandatory repair still completes before applying the paused overlay");
    Reset(); disk[0].remote.items.clear(); journal.pending.push_back(Record(0, 0));
    Check(Load(&out) == ESP_OK && out.position == 0 && out.remote.next_sequence == 1 && saves == 0,
          "empty candidate window accepts only its zero ordinal without a rewrite");
    Reset(); journal.pending.push_back(Record(0, 11, disk[1].remote.session_id, Mode::kIntake));
    Check(Load(&out, Mode::kIntake) == ESP_OK && out.remote.next_sequence == 1 && saves == 0,
          "Intake also replays normally without a snapshot rewrite");
    Reset(); journal.acknowledged.push_back(Record(0, 11, disk[1].remote.session_id, Mode::kIntake));
    journal.partial_tail = true;
    Check(Load(&out, Mode::kIntake) == ESP_OK && disk[1].remote.next_sequence == 1 &&
          saves == 1 && compactions == 1 && io_order == "SC",
          "repair checkpoints the already-loaded Intake mode before reclamation");
    cache_loaded = false;
    Check(Load(&out, Mode::kIntake) == ESP_OK && out.remote.next_sequence == 1 &&
          journal.acknowledged.empty() && disk[0].remote.next_sequence == 0,
          "Intake progress survives reboot after repair reclaimed the old ACK");
    Reset(); journal.acknowledged.push_back(Record(0, 11, disk[1].remote.session_id, Mode::kIntake));
    journal.partial_tail = true; save_error = ESP_FAIL;
    Check(Load(&out, Mode::kIntake) == ESP_FAIL && compactions == 0 &&
          journal.acknowledged.size() == 1 && disk[1].remote.next_sequence == 0,
          "failed loaded Intake checkpoint prevents repair from reclaiming the journal");
    Reset(); journal.acknowledged.push_back(Record());
    journal.acknowledged.push_back(Record(0, 11, disk[1].remote.session_id, Mode::kIntake));
    journal.partial_tail = true;
    Check(Load(&out, Mode::kIntake) == ESP_OK && saves == 2 && compactions == 1 &&
          disk[0].remote.next_sequence == 1 && disk[1].remote.next_sequence == 1 && io_order == "SSC",
          "repair checkpoints both the loaded mode and other resumable modes before reclaiming ACKs");
    Reset(); journal.acknowledged.push_back(Record());
    journal.acknowledged.push_back(Record(0, 11, disk[1].remote.session_id, Mode::kIntake));
    journal.partial_tail = true; fail_save_call = 2;
    Check(Load(&out, Mode::kIntake) == ESP_FAIL && saves == 2 && compactions == 0 &&
          disk[0].remote.next_sequence == 0 && disk[1].remote.next_sequence == 1 &&
          journal.acknowledged.size() == 2,
          "another mode checkpoint failure retains the entire journal after a partial checkpoint");
    Check(LoadSessionTransaction(nullptr) == ESP_ERR_INVALID_ARG,
          "null transaction context is rejected");
    LoadSessionContext invalid{Mode::kReview, nullptr};
    Check(LoadSessionTransaction(&invalid) == ESP_ERR_INVALID_ARG,
          "null output session is rejected");

    Reset(); journal.acknowledged.push_back(Record(0, 11, disk[1].remote.session_id, Mode::kIntake));
    journal.ack_records = kRuntimeCompactAckThreshold;
    Check(MaybeCompactCachedOutbox(&journal) == ESP_OK && disk[1].remote.next_sequence == 1 &&
          compactions == 1 && journal.acknowledged.empty(),
          "runtime maintenance checkpoints Intake before reclaiming its ACK");
    cache_loaded = false;
    Check(Load(&out, Mode::kIntake) == ESP_OK && out.remote.next_sequence == 1,
          "Intake progress survives reboot after runtime compaction");
    for (const Mode mode : {Mode::kSequential, Mode::kRandom, Mode::kDictionary,
                            Mode::kShuffle, Mode::kMistakes}) {
        Reset(); Session& target = disk[Slot(mode)];
        target = disk[0]; target.remote.mode = mode; present[Slot(mode)] = true;
        journal.acknowledged.push_back(Record(0, 11, sid, mode));
        journal.ack_records = kRuntimeCompactAckThreshold;
        Check(MaybeCompactCachedOutbox(&journal) == ESP_OK && target.remote.next_sequence == 1 &&
              compactions == 1, "every supported mode is checkpointed before reclaim");
    }
    Reset(); journal.acknowledged.push_back(Record(0, 99));
    journal.ack_records = kRuntimeCompactAckThreshold;
    Check(MaybeCompactCachedOutbox(&journal) == ESP_ERR_INVALID_STATE &&
          journal.acknowledged.size() == 1 && compactions == 0 && saves == 0,
          "unrepresentable ACK progress blocks reclamation of its only durable source");
    Reset(); journal.acknowledged.push_back(Record()); journal.ack_records = kRuntimeCompactAckThreshold;
    save_error = ESP_FAIL;
    Check(MaybeCompactCachedOutbox(&journal) == ESP_FAIL && journal.acknowledged.size() == 1 &&
          compactions == 0, "runtime checkpoint failure preserves ACK progress");
    Reset(); journal.acknowledged.push_back(Record()); journal.ack_records = kRuntimeCompactAckThreshold;
    Check(MaybeCompactCachedOutbox(&journal) == ESP_OK && reads == 1 && saves == 1,
          "runtime checkpoint reads only modes represented in the retained journal");
    Reset(); journal.acknowledged.push_back(Record(500, 99, "replaced-old-session"));
    journal.ack_records = kRuntimeCompactAckThreshold;
    Check(MaybeCompactCachedOutbox(&journal) == ESP_OK && compactions == 1 && saves == 0 &&
          disk[0].remote.next_sequence == 0,
          "old SID replaced in this slot neither advances nor blocks the current session");
    Reset(); present[0] = false; journal.acknowledged.push_back(Record());
    journal.ack_records = kRuntimeCompactAckThreshold;
    Check(MaybeCompactCachedOutbox(&journal) == ESP_OK && compactions == 1 && saves == 0 &&
          !present[0], "cleared slot is not resurrected by acknowledged observations");
    Reset(); journal.acknowledged.push_back(Record(0, 99));
    disk[0].remote.next_sequence = 1; disk[0].position = 1;
    journal.ack_records = kRuntimeCompactAckThreshold;
    Check(MaybeCompactCachedOutbox(&journal) == ESP_OK && compactions == 1 && saves == 0,
          "ACK older than durable progress is covered even if its old ordinal was trimmed");
    Reset(); journal.pending.push_back(Record(0, 11, sid, Mode::kIntake));
    Check(Load(&out) == ESP_OK && out.remote.next_sequence == 0 && out.position == 0,
          "same SID in a different mode cannot advance the current slot");

    // This is the parked-page / old retry counterexample, NOT evidence that it
    // occurred on the user's device: advance and checkpoint, reclaim the ACK,
    // then let a previously captured snapshot save after the maintenance.
    Reset(); Session captured = disk[0];
    journal.acknowledged.push_back(Record()); journal.ack_records = kRuntimeCompactAckThreshold;
    const esp_err_t maintenance = MaybeCompactCachedOutbox(&journal);
    const esp_err_t save = SaveSessionTransaction(&captured);
    cache_loaded = false;
    Check(maintenance == ESP_OK && save == ESP_OK && Load(&out) == ESP_OK &&
          out.remote.next_sequence == 1 && out.position == 1,
          "stale snapshot save after ACK reclaim cannot rewind confirmed progress");
    Reset(); captured = disk[0]; journal.pending.push_back(Record());
    Check(SaveSessionTransaction(&captured) == ESP_OK && disk[0].remote.next_sequence == 1 &&
          disk[0].position == 1 && journal.pending.size() == 1,
          "candidate save incorporates retained pending progress without consuming payload");
    Reset(); captured = disk[0]; journal.suspended.push_back(Record());
    Check(SaveSessionTransaction(&captured) == ESP_OK && disk[0].remote.next_sequence == 1 &&
          journal.suspended.size() == 1,
          "candidate save incorporates parked progress without deleting the parked payload");
    Reset(); captured = disk[0]; journal.pending.push_back(Record(0, 99));
    Check(SaveSessionTransaction(&captured) == ESP_ERR_INVALID_STATE && saves == 0 && cursor_writes == 0,
          "candidate without the durable ordinal is rejected before replacing snapshot");
    Reset(); captured = disk[0]; read_error = ESP_FAIL;
    Check(SaveSessionTransaction(&captured) == ESP_FAIL && saves == 0 && cursor_writes == 0,
          "current snapshot I/O error cannot be hidden by overwriting it");
    Reset(); captured = disk[0]; scan_error = ESP_FAIL;
    Check(SaveSessionTransaction(&captured) == ESP_FAIL && saves == 0 && cursor_writes == 0,
          "current journal I/O error prevents a snapshot save with unknown progress");

    Reset(); captured = disk[0]; captured.position = 1; captured.remote.next_sequence = 1;
    captured.phase = wqn::WordPresentationPhase::kBack;
    const auto first_record = Record();
    auto observation = ObservationFromRecord(first_record);
    CommitContext retry{&observation, &captured};
    journal.pending = {first_record, Record(1, 12)};
    Check(CommitObservationTransaction(&retry) == ESP_OK && disk[0].remote.next_sequence == 2 &&
          disk[0].position == 2 && appends == 0 && cursor_writes == 0 && cache.pending.size() == 2,
          "old pending retry preserves newer confirmed progress without another append or cursor write");
    Reset(); disk[0].position = 2; disk[0].remote.next_sequence = 2;
    captured = disk[0]; captured.position = 1; captured.remote.next_sequence = 1;
    captured.phase = wqn::WordPresentationPhase::kBack;
    journal.acknowledged = {first_record};
    Check(CommitObservationTransaction(&retry) == ESP_OK && disk[0].remote.next_sequence == 2 &&
          appends == 0 && cursor_writes == 0,
          "old ACK retry preserves a newer checkpoint without rewriting pause state");
    Reset(); disk[0].position = 2; disk[0].remote.next_sequence = 2;
    journal.suspended = {first_record};
    Check(CommitObservationTransaction(&retry) == ESP_OK && disk[0].remote.next_sequence == 2 &&
          cache.suspended.size() == 1 && appends == 0 && cursor_writes == 0,
          "old parked retry preserves newer checkpoint and retains parked payload");
    Reset();
    Check(CommitObservationTransaction(&retry) == ESP_OK && appends == 1 && saves == 0 &&
          reads == 0 && cursor_writes == 0,
          "normal new answer still appends only and does not open a snapshot or write NVS");
    Reset(); captured.deck_scope_generation = 6;
    Check(CommitObservationTransaction(&retry) == ESP_ERR_INVALID_STATE && appends == 0 &&
          scans == 0 && saves == 0 && cursor_writes == 0,
          "stale-scope answer is rejected before journal append or retry checkpoint");
    captured.deck_scope_generation = 7;
    Reset(); observation.mode = Mode::kIntake;
    Check(CommitObservationTransaction(&retry) == ESP_ERR_INVALID_ARG && appends == 0 && scans == 0,
          "answer mode must match the advanced session before any journal I/O");
    observation.mode = Mode::kReview;
    Reset(); journal.acknowledged = {first_record};
    observation.next_position = 12; captured.position = 2;
    Check(CommitObservationTransaction(&retry) == ESP_ERR_INVALID_STATE && appends == 0 &&
          reads == 0 && saves == 0 && cursor_writes == 0,
          "same request ID with different ACK payload is a conflict, not an idempotent retry");
    observation.next_position = 11;

    Reset(); captured = disk[0]; captured.remote.session_id = "new-session";
    Check(SaveSessionTransaction(&captured) == ESP_OK && disk[0].remote.session_id == "new-session" &&
          cursor_writes == 1, "explicit replacement with a different SID remains supported");
    Reset(); captured = disk[0]; captured.deck_scope_generation = 6;
    Check(SaveSessionTransaction(&captured) == ESP_ERR_INVALID_STATE && reads == 0 && scans == 0 &&
          saves == 0 && cursor_writes == 0,
          "stale scope is rejected before any snapshot or journal read");
    Reset(); captured = disk[0]; save_error = ESP_FAIL;
    Check(SaveSessionTransaction(&captured) == ESP_FAIL && saves == 1 && cursor_writes == 0,
          "snapshot save failure cannot publish a refreshed cursor");
    Reset(); captured = disk[0]; cursor_write_error = ESP_FAIL;
    Check(SaveSessionTransaction(&captured) == ESP_FAIL && saves == 1 && cursor_writes == 1,
          "cursor failure after successful protected snapshot save is still reported");
    Reset(); captured = disk[0]; captured.remote.items = {{12}, {13}};
    captured.remote.cursor = "next-page-cursor";
    captured.remote.next_sequence = 0;
    disk[0].position = 2; disk[0].remote.next_sequence = 2;
    Check(SaveSessionTransaction(&captured) == ESP_OK && disk[0].remote.next_sequence == 2 &&
          disk[0].position == 0 && disk[0].remote.items.front().ordinal == 12 &&
          disk[0].remote.items.size() == 2 && disk[0].remote.cursor == "next-page-cursor",
          "progress merge uses absolute ordinal rather than stale vector position");
    Reset(); captured = disk[0]; captured.position = 0;
    disk[0].position = 1; disk[0].remote.next_sequence = 0;
    disk[0].phase = wqn::WordPresentationPhase::kBack;
    Check(SaveSessionTransaction(&captured) == ESP_OK && disk[0].position == 0 &&
          disk[0].phase == wqn::WordPresentationPhase::kFront,
          "equal sequence control-state changes are not misclassified as an older answer");
    Reset(); captured = disk[0]; captured.remote.items = {{20}, {21}};
    disk[0].position = 2; disk[0].remote.next_sequence = 2;
    Check(SaveSessionTransaction(&captured) == ESP_ERR_INVALID_STATE && saves == 0 && cursor_writes == 0,
          "stale candidate window missing newer checkpoint ordinal is rejected without replacing it");
    Reset(); journal.acknowledged.push_back(Record(0, 99));
    Check(Load(&out) == ESP_ERR_INVALID_STATE && saves == 0 && compactions == 0 && cursor_reads == 0,
          "uncovered ACK cursor is not reported as successfully restored");
    Reset(); journal.suspended.push_back(Record(0, 99));
    Check(Load(&out) == ESP_ERR_INVALID_STATE && saves == 0 && compactions == 0 && cursor_reads == 0,
          "uncovered parked cursor is not reported as successfully restored");
    Reset(); journal.acknowledged.push_back(Record());
    journal.pending.push_back(Record(1, 99)); journal.partial_tail = true;
    Check(Load(&out) == ESP_ERR_INVALID_STATE && saves == 0 && compactions == 0 &&
          journal.acknowledged.size() == 1 && cursor_reads == 0,
          "repair does not publish a partially restored snapshot or reclaim ACKs on uncovered progress");
    const pid_t child = fork();
    if (child == 0) _exit(SaveSessionTransaction(nullptr) == ESP_ERR_INVALID_ARG ? 0 : 1);
    int child_status = 0;
    Check(child > 0 && waitpid(child, &child_status, 0) == child &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "null snapshot-save context is rejected (child isolates baseline crash)");
    Reset(); cache_loaded = true; cache.pending.push_back(Record());
    cache.ack_records = kRuntimeCompactAckThreshold - 1;
    const std::string ack_request = cache.pending.front().request_id;
    AckContext ack_context{&ack_request};
    Check(AckObservationTransaction(&ack_context) == ESP_OK && appends == 1 && saves == 0 &&
          reads == 0 && compactions == 0 && cache.ack_records == kRuntimeCompactAckThreshold &&
          cache.pending.empty() && cache.acknowledged.size() == 1,
          "threshold-crossing ACK transaction only commits its ACK, not whole maintenance");
#if FIXTURE_STEPPED_MAINTENANCE
    TestMaintenanceSteps();
    TestMaintenanceDriver();
    TestOutboxQuota();
#endif
    std::printf("%d PASS / %d FAIL (host storage mocks; NOT power-loss or concurrency HIL)\n",
                passes, failures);
    return failures ? 1 : 0;
}
