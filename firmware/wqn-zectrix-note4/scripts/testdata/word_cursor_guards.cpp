// Function-level host fixtures only: NVS is mocked, libc files are temporary.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_NVS_INVALID_STATE = 0x110b;
constexpr const char* kTag = "cursor-fixture";
const char* esp_err_to_name(esp_err_t) { return "injected-error"; }
void FixtureLog(const char*, const char*, ...) {}
#define ESP_LOGW(...) FixtureLog(__VA_ARGS__)

namespace wqn::protocol::word_study_v1 {
enum class Mode { kReview, kInvalid };
}
using Mode = wqn::protocol::word_study_v1::Mode;
struct SessionPaths { const char* primary; const char* temporary; const char* backup; };
std::string session_names[3], cursor_names[3];
SessionPaths SessionFiles() { return {session_names[0].c_str(), session_names[1].c_str(), session_names[2].c_str()}; }
SessionPaths CursorFiles() { return {cursor_names[0].c_str(), cursor_names[1].c_str(), cursor_names[2].c_str()}; }
bool GetSessionPaths(Mode mode, SessionPaths* out) {
    if (mode != Mode::kReview) return false;
    *out = SessionFiles();
    return true;
}
bool GetSessionCursorPaths(Mode mode, SessionPaths* out) {
    if (mode != Mode::kReview) return false;
    *out = CursorFiles();
    return true;
}
const char* GetSessionCursorNvsKey(Mode mode) { return mode == Mode::kReview ? "fixture" : nullptr; }
@@SESSION_GENERATION@@
// ROM mock matches esp_rom_crc.h and patches/esp_rom_crc.c: invert both ends.
// The production Crc32 wrapper below is extracted verbatim, not mocked.
uint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t* bytes, uint32_t size) {
    uint32_t crc = ~seed;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
}

@@CRC@@

@@RECORD@@

struct FakeNvs {
    SessionCursorRecord record = {};
    bool found = false;
    int saves = 0, clears = 0;
    esp_err_t load_error = ESP_OK, save_error = ESP_OK, clear_error = ESP_OK;
} fake;
namespace wqn {
uint32_t scope_generation = 42;
uint32_t GetDeckScopeGeneration() { return scope_generation; }
struct PersistedWordSession {
    struct Remote { Mode mode = Mode::kReview; std::string session_id; } remote;
    uint32_t deck_scope_generation = 42;
    bool paused = false;
};
esp_err_t LoadWordSessionCursorNvs(const char*, void* out, size_t size, bool* found) {
    *found = fake.found;
    if (fake.found) std::memcpy(out, &fake.record, size);
    return fake.load_error;
}
esp_err_t SaveWordSessionCursorNvs(const char*, const void* record, size_t size) {
    ++fake.saves;
    if (fake.save_error == ESP_OK) { std::memcpy(&fake.record, record, size); fake.found = true; }
    return fake.save_error;
}
esp_err_t ClearWordSessionCursorNvs(const char*) { ++fake.clears; return fake.clear_error; }
}

@@WRITE@@
@@READ@@
@@CLEAR@@

int passes = 0, failures = 0;
void Check(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    condition ? ++passes : ++failures;
}
void Reset() {
    fake = {};
    for (const auto& name : session_names) std::remove(name.c_str());
    for (const auto& name : cursor_names) std::remove(name.c_str());
}
const std::string sid = "00000000-0000-0000-0000-000000000001";
void Seal(SessionCursorRecord* record) { record->crc = Crc32(record, sizeof(*record) - sizeof(record->crc)); }
SessionCursorRecord Record() {
    SessionCursorRecord record = {};
    record.magic = kSessionCursorMagic;
    record.version = kSessionCursorVersion;
    record.paused = 1;
    std::snprintf(record.session_id, sizeof(record.session_id), "%s", sid.c_str());
    Seal(&record);
    return record;
}
void Legacy(const SessionCursorRecord& record, size_t bytes = sizeof(SessionCursorRecord), bool trailing = false) {
    FILE* file = std::fopen(cursor_names[0].c_str(), "wb");
    if (!file) std::abort();
    if (std::fwrite(&record, 1, bytes, file) != bytes) std::abort();
    if (trailing && std::fputc('x', file) == EOF) std::abort();
    if (std::fclose(file) != 0) std::abort();
}
void Rejected(const SessionCursorRecord& record, const char* name,
              size_t bytes = sizeof(SessionCursorRecord), bool trailing = false) {
    Reset(); Legacy(record, bytes, trailing);
    bool paused = false;
    const bool accepted = ReadSessionCursorPaused(Mode::kReview, sid, &paused);
    Check(!accepted && !paused && fake.saves == 0 && !fake.found, name);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    // SDK oracle: components/esp_rom/test_apps/linux_rom_apis/main/rom_test.cpp.
    const uint8_t sdk_crc_input[] = {0x01, 0x21, 0x09, 0xff, 0x63, 0x65, 0x72, 0x74,
        0x69, 0x66, 0x69, 0x63, 0x61, 0x74, 0x65, 0, 0, 0, 0, 0, 0xe5, 0, 0xff, 0xff,
        0x06, 0xba, 0xe7, 0xa1};
    Check(esp_rom_crc32_le(UINT32_MAX, sdk_crc_input, sizeof(sdk_crc_input)) == UINT32_C(0xd4dc5010),
          "ROM CRC convention agrees with SDK golden vector");
    Check(Crc32("123456789", 9) == UINT32_C(0x2dfd2d88),
          "production CRC wrapper uses zero raw seed, not default zlib convention");
    for (int i = 0; i < 3; ++i) {
        session_names[i] = std::string(argv[1]) + "/session-" + std::to_string(i);
        cursor_names[i] = std::string(argv[1]) + "/cursor-" + std::to_string(i);
    }
    auto record = Record();
    Reset(); Legacy(record); bool paused = false;
    Check(ReadSessionCursorPaused(Mode::kReview, sid, &paused) && paused && fake.saves == 1,
          "valid legacy cursor is migrated once, after validation");
    paused = false;
    Check(ReadSessionCursorPaused(Mode::kReview, sid, &paused) && paused && fake.saves == 1,
          "second read uses NVS without another migration");
    record = Record(); record.magic ^= 1; Seal(&record); Rejected(record, "bad magic never reaches NVS");
    record = Record(); ++record.version; Seal(&record); Rejected(record, "unknown version never reaches NVS");
    record = Record(); record.crc ^= 1; Rejected(record, "bad CRC never reaches NVS");
    record = Record(); record.session_id[35] = '2'; Seal(&record); Rejected(record, "stale session never reaches NVS");
    record = Record(); record.session_id[36] = 'x'; Seal(&record); Rejected(record, "unterminated session id is not repaired into validity");
    record = Record(); record.paused = 2; Seal(&record); Rejected(record, "invalid paused flag never reaches NVS");
    Rejected(Record(), "short legacy record is rejected", sizeof(SessionCursorRecord) - 1);
    Rejected(Record(), "legacy trailing bytes are rejected", sizeof(SessionCursorRecord), true);

    Reset(); Legacy(Record()); fake.save_error = ESP_FAIL; paused = false;
    Check(ReadSessionCursorPaused(Mode::kReview, sid, &paused) && paused && fake.saves == 1 && !fake.found,
          "migration write failure preserves the valid in-memory paused value");
    Reset(); Legacy(Record()); fake.load_error = ESP_FAIL; paused = false;
    Check(!ReadSessionCursorPaused(Mode::kReview, sid, &paused) && fake.saves == 0,
          "NVS read error does not overwrite the stored value from legacy");
    Reset(); Legacy(Record()); fake.found = true; fake.record = Record(); fake.record.crc ^= 1; paused = false;
    Check(!ReadSessionCursorPaused(Mode::kReview, sid, &paused) && fake.saves == 0,
          "invalid existing NVS cursor is not overwritten by a legacy fallback");
    Reset(); paused = false;
    Check(!ReadSessionCursorPaused(Mode::kReview, sid, &paused) && fake.saves == 0,
          "empty mode does not create an NVS cursor");

    for (esp_err_t error : {ESP_OK, ESP_FAIL, ESP_ERR_NVS_INVALID_STATE}) {
        Reset(); Legacy(Record()); fake.clear_error = error;
        ClearSessionContext context = {Mode::kReview};
        const uint64_t previous_generation = g_session_mutation_generation;
        const auto actual = ClearSessionTransaction(&context);
        Check(actual == error && fake.clears == 1, error == ESP_OK ? "clear success propagates" : "clear failure propagates");
        Check(g_session_mutation_generation == previous_generation + 1,
              "successful or NVS-failed clear invalidates an older maintenance proof");
    }
    // A nonempty directory makes real libc remove fail independently of UID
    // or permission overrides. Everything stays in the unique fixture dir.
    // This checks error propagation, not SPIFFS fault injection or atomicity.
    const char* legacy_errors[] = {
        "legacy primary erase failure propagates before NVS erase",
        "legacy temporary erase failure propagates before NVS erase",
        "legacy backup erase failure propagates before NVS erase",
    };
    for (int slot = 0; slot < 3; ++slot) {
        Reset();
        if (::mkdir(cursor_names[slot].c_str(), 0700) != 0) std::abort();
        const std::string sentinel = cursor_names[slot] + "/keep";
        FILE* file = std::fopen(sentinel.c_str(), "wb");
        if (file == nullptr || std::fclose(file) != 0) std::abort();
        ClearSessionContext context = {Mode::kReview};
        const uint64_t previous_generation = g_session_mutation_generation;
        const auto actual = ClearSessionTransaction(&context);
        Check(actual == ESP_FAIL && fake.clears == 0, legacy_errors[slot]);
        Check(g_session_mutation_generation == previous_generation + 1,
              "partial legacy-clear failure still invalidates an older maintenance proof");
        if (std::remove(sentinel.c_str()) != 0 ||
            std::remove(cursor_names[slot].c_str()) != 0) std::abort();
    }
    Reset(); ClearSessionContext invalid = {Mode::kInvalid};
    const uint64_t before_invalid = g_session_mutation_generation;
    Check(ClearSessionTransaction(&invalid) == ESP_ERR_INVALID_ARG && fake.clears == 0,
          "invalid clear mode performs no NVS erase");
    Check(g_session_mutation_generation == before_invalid,
          "invalid clear context does not manufacture a snapshot mutation");
    Reset(); wqn::PersistedWordSession session;
    session.remote.session_id = sid; session.paused = true;
    Check(WriteSessionCursor(session) == ESP_OK && fake.saves == 1 && fake.record.paused == 1 &&
          fake.record.crc == Crc32(&fake.record, sizeof(fake.record) - sizeof(fake.record.crc)),
          "valid scoped write emits a valid 52-byte cursor");
    Reset(); ++session.deck_scope_generation;
    Check(WriteSessionCursor(session) == ESP_ERR_INVALID_STATE && fake.saves == 0,
          "stale scope performs no NVS write");
    Reset(); session.deck_scope_generation = wqn::scope_generation; session.remote.mode = Mode::kInvalid;
    Check(WriteSessionCursor(session) == ESP_ERR_INVALID_ARG && fake.saves == 0,
          "invalid write mode performs no NVS write");
    Reset(); session.remote.mode = Mode::kReview; fake.save_error = ESP_FAIL;
    Check(WriteSessionCursor(session) == ESP_FAIL && fake.saves == 1,
          "cursor write failure propagates");
    Reset();
    std::printf("%d PASS / %d FAIL (host I/O mocks; NOT physical power-loss or migration HIL)\n", passes, failures);
    return failures ? 1 : 0;
}
