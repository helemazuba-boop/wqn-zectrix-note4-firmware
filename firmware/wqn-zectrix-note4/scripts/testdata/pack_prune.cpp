#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include "wqn_api.h"
#include "cJSON.h"
#include "device_protocol/json_depth_guard.h"

static unsigned checks = 0, backup_opens = 0, open_handles = 0, json_allocs = 0;
static unsigned live_json = 0, json_failures = 0, write_opens = 0, encoder_fault_cases = 0;
static bool encode_mode = false, fail_once = false;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); std::abort(); } } while (false)
static std::string directory, primary, temporary, backup;
static const char *kStorageRoot, *kManifestPath, *kManifestTempPath, *kManifestBackupPath;
static constexpr char kTag[] = "prune_test";
static void HostLog(const char*, const char*, ...) {}
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
enum class Fault { kNone, kOpen, kRead, kStat };
static Fault fault = Fault::kNone;
static FILE* backup_file = nullptr;
static bool count_json = false;
static int fail_json_at = -1;
static std::vector<std::string> removals;
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" int __real_fclose(FILE*);
extern "C" size_t __real_fread(void*, size_t, size_t, FILE*);
extern "C" int __real_ferror(FILE*);
extern "C" int __real_stat(const char*, struct stat*);
extern "C" int __real_remove(const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode)
{
    const bool is_backup = path == backup && mode[0] == 'r';
    if (encode_mode) count_json = false;  // Encoding must finish before any file is opened.
    if (mode[0] != 'r') ++write_opens;
    if (is_backup) {
        ++backup_opens;
        count_json = !encode_mode;
        if (fault == Fault::kOpen) { errno = EIO; return nullptr; }
    }
    FILE* file = __real_fopen(path, mode);
    if (file) ++open_handles;
    if (is_backup) backup_file = file;
    return file;
}
extern "C" int __wrap_fclose(FILE* file)
{
    CHECK(open_handles > 0);
    --open_handles;
    if (file == backup_file) backup_file = nullptr;
    return __real_fclose(file);
}
extern "C" size_t __wrap_fread(void* data, size_t size, size_t count, FILE* file)
{ return __real_fread(data, size, count, file); }
extern "C" int __wrap_ferror(FILE* file)
{ return file == backup_file && fault == Fault::kRead ? 1 : __real_ferror(file); }
extern "C" int __wrap_stat(const char* path, struct stat* status)
{
    if (path == backup && fault == Fault::kStat) { errno = EIO; return -1; }
    return __real_stat(path, status);
}
extern "C" int __wrap_remove(const char* path)
{
    removals.emplace_back(path);
    return __real_remove(path);
}
static void* JsonAlloc(size_t bytes)
{
    if (count_json) {
        const unsigned index = json_allocs++;
        if (fail_json_at >= 0 && (index == static_cast<unsigned>(fail_json_at) ||
            (!fail_once && index > static_cast<unsigned>(fail_json_at)))) {
            ++json_failures;
            return nullptr;
        }
    }
    void* pointer = std::malloc(bytes);
    if (pointer) ++live_json;
    return pointer;
}
static void JsonFree(void* pointer)
{
    if (pointer) {
        if (live_json == 0) std::abort();
        --live_json;
        std::free(pointer);
    }
}

// Persisted Word sessions are a controlled port; the prune/hash matching is real.
static std::string pinned_sha;
namespace wqn {
struct PersistedWordSession {
    bool active = false;
    struct { std::vector<protocol::word_study_v1::PackSnapshot> snapshot; } remote;
};
static int LoadPersistedWordSession(protocol::word_study_v1::Mode, PersistedWordSession* session)
{
    if (pinned_sha.empty()) return ESP_ERR_NOT_FOUND;
    session->active = true;
    protocol::word_study_v1::PackSnapshot snapshot; snapshot.sha256 = pinned_sha;
    session->remote.snapshot.push_back(snapshot);
    return ESP_OK;
}
}

@@PRODUCTION@@

static bool Exists(const std::string& path) { return std::filesystem::is_regular_file(path); }
static void Write(const std::string& path, const std::string& data)
{ std::ofstream file(path, std::ios::binary); file << data; CHECK(file.good()); }
static std::string Read(const std::string& path)
{ std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}}; }
static std::string id(unsigned value)
{
    char text[37] = {};
    std::snprintf(text, sizeof(text), "%08x-0000-0000-0000-000000000001", value);
    return text;
}
static wqn::WqnNotePackManifestNotebook Note(unsigned value)
{
    wqn::WqnNotePackManifestNotebook item;
    item.notebook_id = id(value); item.title = "Notebook";
    item.has_pack = true; item.pack_id = id(value);
    item.sha256.assign(64, static_cast<char>('a' + value % 6));
    item.schema_version = 1; item.entry_count = 1; item.byte_size = 16;
    item.download_url = "/fixture";
    return item;
}
static wqn::WqnProblemPackManifestSet Problem(unsigned value)
{
    wqn::WqnProblemPackManifestSet item;
    item.problem_set_id = id(value); item.name = "Problem set";
    item.has_pack = true; item.pack_id = id(value);
    item.sha256.assign(64, static_cast<char>('a' + value % 6));
    item.schema_version = 1; item.entry_count = 1; item.byte_size = 16;
    item.download_url = "/fixture";
    return item;
}
static wqn::WqnWordPackManifestItem Word(unsigned value)
{
    wqn::WqnWordPackManifestItem item;
    item.pack_id = id(value); item.deck_id = id(value); item.title = "Word deck";
    item.sha256.assign(64, static_cast<char>('a' + value % 6));
    item.schema_version = wqn::protocol::word_study_v1::kPackSchemaVersion;
    item.entry_count = 1; item.byte_size = 16; item.format = "jsonl";
    item.compression = "none"; item.download_url = "/fixture";
    return item;
}
static const char* Prefix(unsigned domain) { return domain == 2 ? "/wp_" : domain ? "/pp_" : "/np_"; }
static const char* Rows(unsigned domain) { return domain == 2 ? "packs" : domain ? "problem_sets" : "notebooks"; }
static std::string Pack(unsigned domain, unsigned value)
{
    return directory + Prefix(domain) + (domain == 2 ? wqn::SafePackStem(Word(value)) :
        domain ? wqn::SafeProblemPackStem(Problem(value)) : wqn::SafeNotePackStem(Note(value))) +
        (domain == 2 ? ".wqwp" : ".wqnp");
}
static int Save(unsigned domain, unsigned value)
{
    if (domain == 2) {
        wqn::WqnWordPackManifest manifest; manifest.packs.push_back(Word(value));
        return word_fixture::SaveWordPackManifestRaw(manifest);
    }
    if (domain) {
        wqn::WqnProblemPackManifest manifest; manifest.problem_sets.push_back(Problem(value));
        return problem_fixture::SaveProblemPackManifestRaw(manifest);
    }
    wqn::WqnNotePackManifest manifest; manifest.notebooks.push_back(Note(value));
    return note_fixture::SaveNotePackManifestRaw(manifest);
}
static void Prune(unsigned domain)
{
    if (domain == 2) {
        wqn::WqnWordPackManifest manifest; manifest.packs.push_back(Word(2));
        word_fixture::PruneUnreferencedPackFiles(manifest);
    } else if (domain) {
        wqn::WqnProblemPackManifest manifest; manifest.problem_sets.push_back(Problem(2));
        problem_fixture::PruneUnreferencedProblemPackFiles(manifest);
    } else {
        wqn::WqnNotePackManifest manifest; manifest.notebooks.push_back(Note(2));
        note_fixture::PruneUnreferencedNotePackFiles(manifest);
    }
}
static std::string PreparedPayload(unsigned domain)
{
    fault = Fault::kNone; fail_json_at = -1; count_json = false;
    CHECK(Save(domain, 1) == ESP_OK);
    return Read(primary);
}
static void Reset(const std::string& base, unsigned domain)
{
    CHECK(open_handles == 0);
    CHECK(live_json == 0);
    directory = base; std::filesystem::remove_all(directory); std::filesystem::create_directories(directory);
    const std::string prefix = std::string(Prefix(domain)) + "manifest";
    primary = directory + prefix + ".json"; temporary = directory + prefix + ".tmp";
    backup = directory + prefix + ".bak";
    kStorageRoot = directory.c_str(); kManifestPath = primary.c_str();
    kManifestTempPath = temporary.c_str(); kManifestBackupPath = backup.c_str();
    backup_file = nullptr; backup_opens = 0; json_allocs = 0;
    fault = Fault::kNone; fail_json_at = -1; count_json = false; removals.clear(); pinned_sha.clear();
    encode_mode = false; fail_once = false; json_failures = 0; write_opens = 0;
}
static void SeedPacks(unsigned domain)
{
    for (unsigned value : {1, 2, 3}) Write(Pack(domain, value), "offline-pack-" + std::to_string(value));
    Write(directory + "/unrelated.txt", "untouched");
    Write(directory + Prefix(domain) + "active.tmp", "active-temp");
    Write(directory + (domain ? "/np_other.wqnp" : "/pp_other.wqnp"), "other-domain");
}
static void CheckSurvivors(unsigned domain, bool deferred)
{
    CHECK(Exists(Pack(domain, 1))); CHECK(Exists(Pack(domain, 2)));
    CHECK(Exists(Pack(domain, 3)) == deferred);
    CHECK(Read(Pack(domain, 1)) == "offline-pack-1");
    CHECK(Exists(directory + "/unrelated.txt"));
    CHECK(Exists(directory + Prefix(domain) + "active.tmp"));
    CHECK(Exists(directory + (domain ? "/np_other.wqnp" : "/pp_other.wqnp")));
    CHECK(open_handles == 0);
}
static void TestDomain(const std::string& base, unsigned domain)
{
    Reset(base, domain); const std::string valid = PreparedPayload(domain);
    // The actual publication rotates the old primary, then prune hits a transient
    // backup read/parse failure. New primary and previous offline packs must survive.
    for (Fault injected : {Fault::kOpen, Fault::kRead, Fault::kStat, Fault::kNone}) {
        Reset(base, domain); Write(primary, valid); SeedPacks(domain); fault = injected;
        CHECK(Save(domain, 2) == ESP_OK);
        count_json = false; fault = Fault::kNone;
        CHECK(Read(backup) == valid); CHECK(!Exists(temporary));
        CheckSurvivors(domain, injected == Fault::kOpen || injected == Fault::kRead);
        // With I/O restored, the backup is a usable fallback and cleanup can retry.
        const std::string new_primary = Read(primary); CHECK(!new_primary.empty());
        Write(primary, "{");
        if (domain == 2) {
            wqn::WqnWordPackManifest parsed;
            CHECK(word_fixture::ParsePackManifestFile(&parsed) == ESP_OK);
            CHECK(parsed.packs.size() == 1 && parsed.packs[0].pack_id == id(1));
        } else if (domain) {
            wqn::WqnProblemPackManifest parsed;
            CHECK(problem_fixture::ParseStoredProblemManifest(&parsed) == ESP_OK);
            CHECK(parsed.problem_sets.size() == 1 && parsed.problem_sets[0].pack_id == id(1));
        } else {
            wqn::WqnNotePackManifest parsed;
            CHECK(note_fixture::ParseStoredNoteManifest(&parsed) == ESP_OK);
            CHECK(parsed.notebooks.size() == 1 && parsed.notebooks[0].pack_id == id(1));
        }
        Write(primary, new_primary); Prune(domain); CheckSurvivors(domain, false);
    }
    // A syntactically valid but incomplete backup must not produce a partial set
    // of retained files; late invalid rows must be rejected before any deletion.
    std::vector<std::string> invalid = {"{", "{}", "{\"data\":{\"cursor\":0,\"has_more\":false}}", valid};
    invalid.back().replace(invalid.back().find("\"schema_version\":"), 18, "\"schema_version\":9");
    cJSON* doc = cJSON_Parse(valid.c_str()); CHECK(doc != nullptr);
    cJSON* rows = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(doc, "data"), Rows(domain));
    cJSON_AddItemToArray(rows, cJSON_CreateObject());
    char* rendered = cJSON_PrintUnformatted(doc); CHECK(rendered != nullptr);
    invalid.emplace_back(rendered); cJSON_free(rendered); cJSON_Delete(doc);
    for (const auto& bad : invalid) {
        Reset(base, domain); Write(backup, bad); SeedPacks(domain); Prune(domain);
        CHECK(removals.empty()); CheckSurvivors(domain, true); CHECK(Read(backup) == bad);
    }
    // A valid empty backup is known, so it permits deletion rather than deferral.
    Reset(base, domain);
    doc = cJSON_Parse(valid.c_str()); CHECK(doc != nullptr);
    cJSON_ReplaceItemInObject(cJSON_GetObjectItemCaseSensitive(doc, "data"),
                             Rows(domain), cJSON_CreateArray());
    rendered = cJSON_PrintUnformatted(doc); CHECK(rendered != nullptr);
    Write(backup, rendered); cJSON_free(rendered); cJSON_Delete(doc);
    SeedPacks(domain); Prune(domain);
    CHECK(!Exists(Pack(domain, 1))); CHECK(Exists(Pack(domain, 2))); CHECK(!Exists(Pack(domain, 3)));
    CHECK(Exists(backup)); CHECK(open_handles == 0);
    Reset(base, domain); Write(backup, valid); SeedPacks(domain); Prune(domain);
    const unsigned allocations = json_allocs; CHECK(allocations > 0);
    // Fail each cJSON allocation while reading a valid rollback manifest.
    for (unsigned index = 0; index < allocations; ++index) {
        Reset(base, domain); Write(primary, valid); SeedPacks(domain); fail_json_at = static_cast<int>(index);
        CHECK(Save(domain, 2) == ESP_OK); count_json = false; fail_json_at = -1;
        CHECK(json_allocs > index); CheckSurvivors(domain, true);
        CHECK(Read(backup) == valid); CHECK(!Exists(temporary));
    }
    // First install has no backup. It still prunes genuinely stale final packs.
    Reset(base, domain); SeedPacks(domain); CHECK(Save(domain, 2) == ESP_OK);
    CHECK(!Exists(Pack(domain, 1))); CHECK(Exists(Pack(domain, 2))); CHECK(!Exists(Pack(domain, 3)));
    CHECK(!Exists(backup)); CHECK(!Exists(temporary)); CHECK(open_handles == 0);
    if (domain == 2) {
        Reset(base, domain); Write(backup, valid); SeedPacks(domain); pinned_sha = Word(3).sha256;
        Prune(domain); CheckSurvivors(domain, true);
        pinned_sha.clear(); Prune(domain); CheckSurvivors(domain, false);
    }
}
static int SaveShape(unsigned domain, unsigned shape)
{
    if (domain == 2) {
        wqn::WqnWordPackManifest manifest;
        manifest.server_time = "2026-10-25T00:00:00Z"; manifest.cursor = 42; manifest.has_more = true;
        if (shape) for (unsigned value : {3, 4}) manifest.packs.push_back(Word(value));
        return word_fixture::SaveWordPackManifestRaw(manifest);
    }
    if (domain == 1) {
        wqn::WqnProblemPackManifest manifest; manifest.cursor = 42; manifest.has_more = true;
        if (shape) for (unsigned value : {3, 4}) {
            auto item = Problem(value); item.has_pack = shape != 2;
            item.name = "quoted \"name\"\n中文"; manifest.problem_sets.push_back(item);
        }
        return problem_fixture::SaveProblemPackManifestRaw(manifest);
    }
    wqn::WqnNotePackManifest manifest; manifest.cursor = 42; manifest.has_more = true;
    if (shape) for (unsigned value : {3, 4}) {
        auto item = Note(value); item.has_pack = shape != 2;
        item.title = "quoted \"title\"\n中文"; manifest.notebooks.push_back(item);
    }
    return note_fixture::SaveNotePackManifestRaw(manifest);
}
static void CheckLoadedShape(unsigned domain, unsigned shape)
{
    const size_t size = shape ? 2 : 0;
    if (domain == 2) {
        wqn::WqnWordPackManifest parsed;
        CHECK(word_fixture::ParsePackManifestFile(&parsed) == ESP_OK);
        CHECK(parsed.cursor == 42 && parsed.has_more && parsed.packs.size() == size);
        if (shape) CHECK(parsed.packs.back().sha256 == Word(4).sha256);
    } else if (domain) {
        wqn::WqnProblemPackManifest parsed;
        CHECK(problem_fixture::ParseStoredProblemManifest(&parsed) == ESP_OK);
        CHECK(parsed.cursor == 42 && parsed.has_more && parsed.problem_sets.size() == size);
        if (shape) CHECK(parsed.problem_sets.back().has_pack == (shape != 2));
    } else {
        wqn::WqnNotePackManifest parsed;
        CHECK(note_fixture::ParseStoredNoteManifest(&parsed) == ESP_OK);
        CHECK(parsed.cursor == 42 && parsed.has_more && parsed.notebooks.size() == size);
        if (shape) CHECK(parsed.notebooks.back().has_pack == (shape != 2));
    }
}
static void TestEncoding(const std::string& base, unsigned domain)
{
    Reset(base, domain); const std::string older = PreparedPayload(domain);
    CHECK(Save(domain, 2) == ESP_OK); const std::string previous = Read(primary);
    for (unsigned shape = 0; shape < (domain == 2 ? 2U : 3U); ++shape) {
        Reset(base, domain); encode_mode = true; count_json = true;
        CHECK(SaveShape(domain, shape) == ESP_OK);
        const unsigned allocations = json_allocs; const std::string expected = Read(primary);
        CHECK(allocations > 0); CHECK(live_json == 0); CheckLoadedShape(domain, shape);
        for (bool installed : {false, true}) for (unsigned index = 0; index < allocations; ++index) {
            Reset(base, domain);
            if (installed) { Write(primary, previous); Write(backup, older); }
            SeedPacks(domain); removals.clear(); write_opens = 0;
            encode_mode = true; count_json = true; fail_once = true;
            fail_json_at = static_cast<int>(index);
            const int result = SaveShape(domain, shape);
            count_json = false; fail_json_at = -1;
            if (result != ESP_ERR_NO_MEM) std::fprintf(stderr,
                "encoder fault domain=%u shape=%u installed=%d allocation=%u result=%d\n",
                domain, shape, installed, index, result);
            CHECK(json_failures == 1); CHECK(result == ESP_ERR_NO_MEM);
            CHECK(live_json == 0); CHECK(open_handles == 0); CHECK(write_opens == 0);
            CHECK(removals.empty()); CHECK(!Exists(temporary));
            CHECK(Exists(primary) == installed); CHECK(Exists(backup) == installed);
            if (installed) { CHECK(Read(primary) == previous); CHECK(Read(backup) == older); }
            CheckSurvivors(domain, true);
            CHECK(SaveShape(domain, shape) == ESP_OK);
            CHECK(Read(primary) == expected); CheckLoadedShape(domain, shape);
            if (installed) CHECK(Read(backup) == previous); else CHECK(!Exists(backup));
            CHECK(!Exists(temporary)); CHECK(live_json == 0); ++encoder_fault_cases;
        }
        std::printf("Encoder domain=%u shape=%u: %u single-allocation sites, first install + replacement PASS\n",
                    domain, shape, allocations);
    }
}
int main(int argc, char** argv)
{
    CHECK(argc == 3); cJSON_Hooks hooks{JsonAlloc, JsonFree}; cJSON_InitHooks(&hooks);
    const char* domains[] = {"note", "problem", "word"};
    for (unsigned domain : {0, 1, 2}) {
        if (std::strcmp(argv[2], "all") == 0 || std::strcmp(argv[2], domains[domain]) == 0) {
            TestDomain(argv[1], domain);
            TestEncoding(argv[1], domain);
        }
    }
    CHECK(live_json == 0);
    std::printf("Production Word/Note/Problem publish + rollback pruning: %u checks, %u single-allocation encoder faults PASS; libc/cJSON/session ports, not SPIFFS HIL\n", checks, encoder_fault_cases);
}
