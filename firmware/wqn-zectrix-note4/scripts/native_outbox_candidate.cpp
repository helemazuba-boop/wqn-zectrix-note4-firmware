// Built by outbox_candidate.py from pinned production codec/scanner/replay code.
// Inputs and outputs are private host files. No device/storage service is linked.
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
@@HEADER@@
extern "C" uint32_t esp_rom_crc32_le(uint32_t, const uint8_t*, uint32_t);
void Log(const char*, const char*, ...) {}
#define ESP_LOGW(...) Log(__VA_ARGS__)
constexpr char kTag[] = "native-candidate";
@@DECLARATIONS@@
namespace native_candidate {
@@CONSTANTS@@
@@RECORDS@@
@@FUNCTIONS@@
}
using namespace native_candidate;
using Session = wqn::Persisted@@DOMAIN@@Session;

int Fail(const char* reason) {
    std::printf("{\"error\":\"%s\"}\n", reason); return 3;
}
bool Read(const char* path, std::vector<uint8_t>* bytes) {
    FILE* file = std::fopen(path, "rb"); if (!file) return false;
    uint8_t block[8192]; size_t count;
    while ((count = std::fread(block, 1, sizeof(block), file)) != 0) {
        bytes->insert(bytes->end(), block, block + count);
        if (bytes->size() > 4U * 1024U * 1024U) { std::fclose(file); return false; }
    }
    const bool ok = std::ferror(file) == 0;
    return std::fclose(file) == 0 && ok;
}
bool Put(const char* path, const std::vector<uint8_t>& bytes) {
    FILE* file = std::fopen(path, "wbx"); if (!file) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() && std::fflush(file) == 0;
    return std::fclose(file) == 0 && ok;
}
std::vector<uint8_t> SessionBytes(const Session& session) {
    std::vector<uint8_t> payload;
    if (!EncodeSession(session, &payload)) return {};
    SessionHeader header{};
    header.magic = kSessionMagic; header.version = kSessionSchemaVersion;
    header.payload_size = payload.size(); header.payload_crc = Crc32(payload.data(), payload.size());
    std::vector<uint8_t> bytes(sizeof(header) + payload.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), payload.data(), payload.size());
    return bytes;
}
void Replay(const OutboxScan& scan, Session* session) {
    bool changed = false;
#if WORD
    ReconcileSession(scan.acknowledged, session, &changed);
    ReconcileSession(scan.pending, session, &changed);
    ReconcileSession(scan.suspended, session, &changed);
#else
    ReconcileSessionFromOutbox(scan, session, &changed);
#endif
}
void Counts(const OutboxScan& scan) {
    std::printf("{\"pending\":%zu,\"acknowledged\":%zu,\"suspended\":%zu,\"records\":%zu}",
        scan.pending.size(), scan.acknowledged.size(), scan.suspended.size(), scan.total_records);
}
int main(int argc, char** argv) {
    if (argc != 6) return Fail("NATIVE_ARGUMENTS");
    Session original;
    if (LoadSessionFile(argv[1], &original@@LOAD_SCOPE@@) != ESP_OK) return Fail("NATIVE_SESSION_INVALID");
    std::vector<uint8_t> original_session;
    if (!Read(argv[1], &original_session) || SessionBytes(original) != original_session)
        return Fail("NATIVE_SESSION_ROUNDTRIP_NOT_EXACT");
    OutboxScan source;
    if (ScanOutboxFile(argv[2], &source) != ESP_OK || source.partial_tail) return Fail("NATIVE_SOURCE_INVALID");
    std::vector<uint8_t> query;
    if (!Read(argv[3], &query)) return Fail("NATIVE_REQUEST_LIST");
    std::vector<std::string> ids;
    std::string current;
    for (uint8_t ch : query) {
        if (ch == '\n') {
            if (current.size() < 16 || current.size() > 64 ||
                std::find(ids.begin(), ids.end(), current) != ids.end() || ids.size() >= 8192)
                return Fail("NATIVE_REQUEST_LIST");
            ids.push_back(current); current.clear();
        } else {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') || current.size() >= 64)
                return Fail("NATIVE_REQUEST_LIST");
            current.push_back(static_cast<char>(ch));
        }
    }
    if (!current.empty() || ids.empty()) return Fail("NATIVE_REQUEST_LIST");
    std::vector<uint8_t> journal;
    if (!Read(argv[2], &journal)) return Fail("NATIVE_SOURCE_READ");
    const size_t original_size = journal.size();
    size_t matching = 0;
    for (const auto& id : ids) {
        const auto found = std::find_if(source.pending.begin(), source.pending.end(),
            [&](const auto& record) { return id == record.request_id; });
        if (found == source.pending.end()) return Fail("NATIVE_PROPOSAL_NOT_PENDING");
        if (original.remote.session_id == found->session_id &&
            static_cast<uint8_t>(original.remote.mode) == found->mode) ++matching;
        wqn::Durable@@DOMAIN@@Observation observation;
        observation.request_id = id;
        OutboxRecord marker{};
        if (BuildObservationRecord(observation, OutboxRecordKind::kAck, &marker) != ESP_OK)
            return Fail("NATIVE_ACK_ENCODE");
        const auto* raw = reinterpret_cast<const uint8_t*>(&marker);
        journal.insert(journal.end(), raw, raw + sizeof(marker));
    }
    if (!matching) return Fail("NATIVE_SESSION_HAS_NO_PROPOSALS");
    if (!Put(argv[5], journal)) return Fail("NATIVE_OUTPUT_WRITE");
    OutboxScan candidate;
    if (ScanOutboxFile(argv[5], &candidate) != ESP_OK || candidate.partial_tail ||
        source.pending.size() != candidate.pending.size() + ids.size() ||
        candidate.acknowledged.size() != source.acknowledged.size() + ids.size() ||
        candidate.suspended.size() != source.suspended.size() ||
        candidate.suspended_reasons != source.suspended_reasons) return Fail("NATIVE_DISPOSITION_CHANGED");
    auto expected = original;
    auto result = original;
    Replay(source, &expected);
    Replay(candidate, &result);
    const auto bytes = SessionBytes(result);
    if (bytes.empty() || bytes != SessionBytes(expected)) return Fail("NATIVE_REPLAY_CHANGED_BY_ACK_PARTITION");
#if WORD
    if (!SessionCoversRecords(candidate.acknowledged, result)) return Fail("NATIVE_CHECKPOINT_INCOMPLETE");
#else
    for (const auto& record : candidate.acknowledged) {
        if (result.remote.session_id == record.session_id && record.sequence >= result.remote.next_sequence)
            return Fail("NATIVE_CHECKPOINT_INCOMPLETE");
    }
#endif
    if (!Put(argv[4], bytes)) return Fail("NATIVE_OUTPUT_WRITE");
    Session reloaded;
    if (LoadSessionFile(argv[4], &reloaded@@LOAD_SCOPE@@) != ESP_OK || SessionBytes(reloaded) != bytes)
        return Fail("NATIVE_CANDIDATE_RELOAD_FAILED");
    std::printf("{\"session_id\":\"%s\",\"mode\":%u,\"proposals\":%zu,\"matching_proposals\":%zu,",
        original.remote.session_id.c_str(), static_cast<unsigned>(original.remote.mode), ids.size(), matching);
    std::printf("\"source\":"); Counts(source); std::printf(",\"candidate\":"); Counts(candidate);
    std::printf(",\"checkpoint_next_sequence\":%llu,\"position\":%u,\"prefix_bytes\":%zu,"
        "\"replay_partition_invariant\":true,\"native_reload_verified\":true}\n",
        static_cast<unsigned long long>(result.remote.next_sequence), result.position, original_size);
}
