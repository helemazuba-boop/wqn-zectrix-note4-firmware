// Production codecs/replay are inserted verbatim by the Python parity driver.
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
constexpr char kTag[] = "checkpoint-codec";
@@DECLARATIONS@@
namespace checkpoint_native {
@@CONSTANTS@@
@@RECORDS@@
@@CODEC@@
@@DESCRIPTOR@@
}
using namespace checkpoint_native;

using Session = wqn::Persisted@@DOMAIN@@Session;
constexpr char kSessionId[] = "11111111-1111-1111-1111-111111111111";
constexpr char kScope[] = "33333333-3333-3333-3333-333333333333";

void Put(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file || std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size() ||
        std::fclose(file) != 0) std::abort();
}

std::vector<uint8_t> SessionBytes(const Session& session) {
    std::vector<uint8_t> payload;
    if (!EncodeSession(session, &payload)) std::abort();
    SessionHeader header{};
    header.magic = kSessionMagic; header.version = kSessionSchemaVersion;
    header.payload_size = payload.size(); header.payload_crc = Crc32(payload.data(), payload.size());
    std::vector<uint8_t> bytes(sizeof(header) + payload.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), payload.data(), payload.size());
    return bytes;
}

Session GoodSession() {
    Session session;
    session.active = true; session.remote.session_id = kSessionId;
    session.remote.seed = "checkpoint_native_seed"; session.remote.next_sequence = 7;
#if WORD
    wqn::StoredWordDeckId deck; std::snprintf(deck.value, sizeof(deck.value), "%s", kScope);
    session.remote.deck_ids.push_back(deck);
    session.deck_scope_generation = 7;
    wqn::StoredWordPackSnapshot pin;
#else
    session.remote.notebook_id = kScope;
    wqn::StoredNotePackSnapshot pin;
#endif
    @@PIN_ID@@
    pin.content_revision = 1; pin.pack_revision = 2;
    std::memset(pin.sha256, 'a', 64); session.remote.snapshot.push_back(pin);
    for (unsigned i = 0; i < (WORD ? 2U : 18U); ++i) {
        wqn::Stored@@DOMAIN@@SessionItem item;
        if (i == 0) std::snprintf(item.item_id, sizeof(item.item_id),
            "22222222-2222-2222-2222-222222222222");
        else std::snprintf(item.item_id, sizeof(item.item_id), "00000000-0000-0000-0000-%012u", i);
        @@ITEM_SCOPE@@
        item.ordinal = WORD ? 16 + i : i;
        session.remote.items.push_back(item);
    }
    return session;
}

int main(int argc, char** argv) {
#if !WORD
    if (argc == 2 && std::strcmp(argv[1], "replay-sets") == 0) {
        auto session = GoodSession();
        OutboxRecord old{}, newest{}, parked{};
        for (auto* record : {&old, &newest, &parked}) {
            std::snprintf(record->session_id, sizeof(record->session_id), "%s", kSessionId);
            std::snprintf(record->item_id, sizeof(record->item_id), "%s", session.remote.items[0].item_id);
            record->next_position = 17;
        }
        old.sequence = 7; parked.sequence = 8; newest.sequence = 9;
        std::snprintf(old.occurred_at, sizeof(old.occurred_at), "2026-11-05T00:00:01Z");
        std::snprintf(parked.occurred_at, sizeof(parked.occurred_at), "2026-11-05T00:00:02Z");
        std::snprintf(newest.occurred_at, sizeof(newest.occurred_at), "2026-11-05T00:00:03Z");
        OutboxScan scan;
        scan.acknowledged.push_back(old); scan.pending.push_back(newest); scan.suspended.push_back(parked);
        bool changed = false;
        ReconcileSessionFromOutbox(scan, &session, &changed);
        const std::string first_pin = session.remote.items[0].last_opened_at;
        changed = false;
        ReconcileSessionFromOutbox(scan, &session, &changed);
        const bool second_changed = changed;
        std::snprintf(scan.pending[0].occurred_at, sizeof(scan.pending[0].occurred_at), "2026-11-05T00:00:00Z");
        auto reversed_clock = GoodSession();
        ReconcileSessionFromOutbox(scan, &reversed_clock, &changed);
        std::printf("{\"last_opened_at\":\"%s\",\"next_sequence\":%llu,\"second_changed\":%s,\"reversed_clock_pin\":\"%s\"}\n",
            first_pin.c_str(), static_cast<unsigned long long>(session.remote.next_sequence),
            second_changed ? "true" : "false", reversed_clock.remote.items[0].last_opened_at);
        return 0;
    }
#endif
    if (argc >= 3) {
        Session session;
        int error = LoadSessionFile(argv[2], &session@@LOAD_SCOPE@@);
#if WORD
        if (std::strcmp(argv[1], "descriptor") == 0 || std::strcmp(argv[1], "cursor") == 0) {
            std::vector<uint8_t> raw; FILE* file = std::fopen(argv[2], "rb");
            if (!file) return 2;
            uint8_t buf[512]; size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), file)) != 0) raw.insert(raw.end(), buf, buf+n);
            if (std::fclose(file) != 0) return 2;
            if (std::strcmp(argv[1], "cursor") == 0) {
                g_cursor_bytes = raw;
                bool paused = false;
                bool valid = ReadSessionCursorPaused(wqn::protocol::word_study_v1::Mode::kSequential,
                    argc == 4 ? argv[3] : kSessionId, &paused);
                std::printf("{\"native_valid\":%s,\"paused\":%s}\n",valid?"true":"false",paused?"true":"false");
                return 0;
            }
            wqn::LocalWordSessionDescriptor descriptor;
            error = DecodeLocalDescriptor(raw, argc == 4 ? argv[3] : kSessionId, &descriptor);
        }
#endif
        std::printf("{\"native_error\":%d}\n", error); return 0;
    }
    if (argc != 2) return 2;
    const std::string directory = argv[1];
    auto good = GoodSession();
    Put(directory + "/checkpoint-session.bin", SessionBytes(good));
#if WORD
    SessionCursorRecord cursor{};
    cursor.magic = kSessionCursorMagic; cursor.version = kSessionCursorVersion; cursor.paused = 1;
    std::snprintf(cursor.session_id, sizeof(cursor.session_id), "%s", kSessionId);
    cursor.crc = Crc32(&cursor, sizeof(cursor) - sizeof(cursor.crc));
    Put(directory + "/checkpoint-cursor.bin", {reinterpret_cast<uint8_t*>(&cursor),
        reinterpret_cast<uint8_t*>(&cursor) + sizeof(cursor)});
    auto local = good;
    local.start_index = 1000; local.position = 0; local.remote.include_mastered = true;
    local.remote.next_sequence = 0; local.remote.optional_count = 2;
    for (size_t i = 0; i < local.remote.items.size(); ++i) local.remote.items[i].ordinal = i;
    wqn::LocalWordSessionDescriptor descriptor; descriptor.session = local; descriptor.total_count = 1002;
    if (!LocalDescriptorSemanticsValid(descriptor)) std::abort();
    std::vector<uint8_t> bytes;
    if (EncodeLocalDescriptor(descriptor, &bytes) != ESP_OK) std::abort();
    Put(directory + "/checkpoint-descriptor.bin", bytes);
    local.remote.cursor = "2";
    Put(directory + "/checkpoint-local-session.bin", SessionBytes(local));
#endif
    OutboxRecord record{};
    record.sequence = 9; record.next_position = 17;
    record.mode = 0;
    std::snprintf(record.session_id, sizeof(record.session_id), "%s", kSessionId);
    std::snprintf(record.item_id, sizeof(record.item_id), "%s", good.remote.items[0].item_id);
    std::snprintf(record.occurred_at, sizeof(record.occurred_at), "2026-11-05T00:00:00Z");
#if WORD
    record.next_phase = 1;
#endif
    std::vector<OutboxRecord, wqn::@@DOMAIN@@StorePsramAllocator<OutboxRecord>> records{record};
    bool changed = false;
#if WORD
    ReconcileSession(records, &good, &changed);
#else
    OutboxScan scan; scan.pending = records;
    ReconcileSessionFromOutbox(scan, &good, &changed);
#endif
    std::printf("{\"position\":%u,\"next_sequence\":%llu,\"changed\":%s",
        good.position, static_cast<unsigned long long>(good.remote.next_sequence), changed ? "true" : "false");
#if !WORD
    std::printf(",\"last_opened_at\":\"%s\"", good.remote.items[0].last_opened_at);
#endif
    std::printf("}\n");
}
