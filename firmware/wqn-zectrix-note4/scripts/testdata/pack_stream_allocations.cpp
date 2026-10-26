#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include "runtime/storage_temp_file_lease.h"
#include "runtime/pack_stream_path_lease.h"
#include "voice_diagnostics.h"

static int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}


static bool deny_cpp = false, inside_transaction = false;
static unsigned callback_allocations = 0, denied_allocations = 0, checks = 0, path_calls = 0;
void* operator new(std::size_t bytes)
{
    if (inside_transaction) ++callback_allocations;
    if (deny_cpp) { ++denied_allocations; throw std::bad_alloc(); }
    if (void* pointer = std::malloc(bytes ? bytes : 1)) return pointer;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); std::abort(); } } while (false)
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 258, ESP_ERR_INVALID_STATE = 259,
    ESP_ERR_INVALID_SIZE = 260, ESP_ERR_INVALID_CRC = 261;
enum class Failure { kNone, kNetwork, kOpen, kStart, kAppend, kDigest, kFinish, kFlush, kSync, kClose, kRename };
static Failure failure = Failure::kNone;
static bool deny_after_begin = false;
static unsigned opens = 0, closes = 0, abort_calls = 0, begin_calls = 0;
static FILE* handles[16] = {};
static std::string root;
static constexpr char payload[] = "0123456789abcdef";
static constexpr char kTag[] = "pack_test";
static int current_phase = -1;
static unsigned digest_seed = 0;
static unsigned nesting_mode = 0, nested_calls = 0;
static bool inside_nested_download = false;
static int nested_result = ESP_OK;
static bool probe_resets = false;
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" int __real_fclose(FILE*);
extern "C" int __real_fflush(FILE*);
extern "C" int __real_fsync(int);
extern "C" int __real_rename(const char*, const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode)
{
    if (failure == Failure::kOpen) { errno = EIO; return nullptr; }
    FILE* file = __real_fopen(path, mode);
    CHECK(file != nullptr);
    for (auto& handle : handles) if (!handle) { handle = file; ++opens; return file; }
    CHECK(false); return nullptr;
}
extern "C" int __wrap_fclose(FILE* file)
{
    bool found = false;
    for (auto& handle : handles) if (handle == file) { handle = nullptr; found = true; break; }
    CHECK(found); ++closes;
    const int result = __real_fclose(file);
    return failure == Failure::kClose && current_phase == 2 ? EOF : result;
}
extern "C" int __wrap_fflush(FILE* file)
{ return failure == Failure::kFlush ? EOF : __real_fflush(file); }
extern "C" int __wrap_fsync(int descriptor)
{ return failure == Failure::kSync ? -1 : __real_fsync(descriptor); }
extern "C" int __wrap_rename(const char* from, const char* to)
{ return failure == Failure::kRename ? -1 : __real_rename(from, to); }
static void HostLog(const char*, const char*, ...) {}
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(expression) ((void)(expression))
struct mbedtls_sha256_context { bool started = false; };
static void mbedtls_sha256_init(mbedtls_sha256_context* hash) { hash->started = false; }
static void mbedtls_sha256_free(mbedtls_sha256_context*) {}
static int mbedtls_sha256_starts(mbedtls_sha256_context* hash, int)
{ hash->started = failure != Failure::kStart; return hash->started ? 0 : -1; }
static int mbedtls_sha256_update(mbedtls_sha256_context* hash, const uint8_t*, size_t)
{ CHECK(hash->started); return failure == Failure::kAppend ? -1 : 0; }
static uint8_t DigestByte(unsigned index) { return static_cast<uint8_t>(index * 17 + digest_seed); }
static int mbedtls_sha256_finish(mbedtls_sha256_context* hash, unsigned char* output)
{
    CHECK(hash->started); hash->started = false;
    for (unsigned i = 0; i < 32; ++i) output[i] = DigestByte(i);
    if (failure == Failure::kDigest) output[31] ^= 1;
    return failure == Failure::kFinish ? -1 : 0;
}
static std::string ExpectedHash()
{
    char output[65] = {};
    for (unsigned i = 0; i < 32; ++i) std::snprintf(output + 2 * i, 3, "%02x", DigestByte(i));
    return output;
}

namespace wqn {
namespace protocol {
namespace v3 { struct RequestMetadata {}; }
namespace word_study_v1 { constexpr unsigned kPackSchemaVersion = 1; }
}
namespace runtime {
enum class SleepBlocker { kStorage };
struct SleepLease {
    static SleepLease TryAcquire(SleepBlocker, const char*, const char*, int) { return {}; }
    explicit operator bool() const { return true; }
};
}
struct Manifest {
    bool has_pack = true;
    std::string pack_id = "00112233-4455-6677-8899-aabbccddeeff", sha256 = ExpectedHash();
    unsigned schema_version = 1, entry_count = 1;
    size_t byte_size = sizeof(payload) - 1;
};
using WqnWordPackManifestItem = Manifest;
using WqnNotePackManifestNotebook = Manifest;
using WqnProblemPackManifestSet = Manifest;
using WqnHttpChunkSink = esp_err_t (*)(void*, const uint8_t*, size_t);
using WqnTransferProgressSink = void*;
static constexpr unsigned kPackSchemaVersion = 1, kMaxPackEntries = 1024, kMaxIndexEntries = 1024;
static constexpr size_t kMaxPackBytes = 1024 * 1024;
static const char* kStorageRoot = nullptr;
static std::string manifest_path, manifest_temp_path, manifest_backup_path;
static const char *kManifestPath = nullptr, *kManifestTempPath = nullptr, *kManifestBackupPath = nullptr;
@@FILE_EXISTS@@
static bool WordPackNeedsDownload(const Manifest&) { return true; }
static bool NotePackNeedsDownload(const Manifest&) { return true; }
static bool ProblemPackNeedsDownload(const Manifest&) { return true; }
namespace services {
esp_err_t ExecuteStorageTransactionNamed(esp_err_t (*transaction)(void*), void*, const char*);
}
static int Download(unsigned, const Manifest&, WqnHttpChunkSink, void*);
static int DownloadWordPackStream(const std::string&, const protocol::v3::RequestMetadata&, const Manifest& item,
                                  WqnHttpChunkSink sink, void* context) { return Download(0, item, sink, context); }
static int DownloadNotePackStream(const std::string&, const protocol::v3::RequestMetadata&, const Manifest& item,
                                  WqnHttpChunkSink sink, void* context, WqnTransferProgressSink) { return Download(1, item, sink, context); }
static int DownloadProblemPackStream(const std::string&, const protocol::v3::RequestMetadata&, const Manifest& item,
                                     WqnHttpChunkSink sink, void* context) { return Download(2, item, sink, context); }

@@PRODUCTION@@

static int Download(unsigned domain, const Manifest& item, WqnHttpChunkSink sink, void* context)
{
    deny_cpp = deny_after_begin;
    if (probe_resets) {
        for (auto reset : {ResetWordPackStorageCacheRaw, ResetNotePackStorageCacheRaw, ResetProblemPackStorageCacheRaw})
            CHECK(reset(nullptr) == ESP_ERR_INVALID_STATE);
        CHECK(::access(kManifestPath, F_OK) == 0 && ::access(kManifestTempPath, F_OK) == 0 &&
              ::access(kManifestBackupPath, F_OK) == 0);
    }
    if (nesting_mode && !inside_nested_download) {
        inside_nested_download = true;
        Manifest nested = item;
        if (nesting_mode == 2) nested.pack_id = "ffeedd33-4455-6677-8899-aabbccddeeff";
        protocol::v3::RequestMetadata metadata;
        nested_result = domain == 0 ? DownloadWordPackToStorage("token", metadata, nested) :
                        domain == 1 ? DownloadNotePackToStorage("token", metadata, nested, nullptr) :
                                      DownloadProblemPackToStorage("token", metadata, nested);
        ++nested_calls;
        inside_nested_download = false;
        CHECK(nested_result == (nesting_mode == 1 ? ESP_ERR_INVALID_STATE : ESP_OK));
        CHECK(opens == closes + 1); // The original stream still owns its FILE.
    }
    if (failure == Failure::kNetwork) return ESP_FAIL;
    for (const auto& chunk : {std::pair<size_t, size_t>{0, 3}, {3, 5}, {8, sizeof(payload) - 1 - 8}}) {
        const int result = sink(context, reinterpret_cast<const uint8_t*>(payload) + chunk.first, chunk.second);
        if (result != ESP_OK) return result;
    }
    return ESP_OK;
}

namespace services {
esp_err_t ExecuteStorageTransactionNamed(esp_err_t (*transaction)(void*), void* context, const char*)
{
    CHECK(runtime::HasActiveStorageTempFileLease());
    int phase = transaction == WordPackStreamTransaction ? static_cast<int>(static_cast<WordPackStreamContext*>(context)->operation) :
                transaction == NotePackStreamTransaction ? static_cast<int>(static_cast<NotePackStreamContext*>(context)->operation) :
                static_cast<int>(static_cast<ProblemPackStreamContext*>(context)->operation);
    current_phase = phase;
    if (phase == 0) ++begin_calls;
    if (phase == 3) ++abort_calls;
    struct TransactionScope {
        TransactionScope() { CHECK(!inside_transaction); inside_transaction = true; }
        ~TransactionScope() { inside_transaction = false; }
    } scope;
    return transaction(context);
}
}
static void Reset()
{
    deny_cpp = inside_transaction = false;
    CHECK(!runtime::HasActiveStorageTempFileLease() && !runtime::HasActivePackStreamPath());
    for (FILE* handle : handles) CHECK(handle == nullptr);
    std::filesystem::remove_all(root); std::filesystem::create_directories(root);
    opens = closes = abort_calls = begin_calls = path_calls = callback_allocations = denied_allocations = 0;
    failure = Failure::kNone; deny_after_begin = false;
    nesting_mode = nested_calls = 0; inside_nested_download = false;
    probe_resets = false;
}
static void Case(unsigned domain, Failure point, bool deny, bool measure = false, bool check_reset = false)
{
    Reset(); failure = point; deny_after_begin = deny;
    probe_resets = check_reset;
    if (probe_resets) {
        for (const char* path : {kManifestPath, kManifestTempPath, kManifestBackupPath}) {
            std::ofstream file(path); file << "existing manifest";
        }
    }
    Manifest item;
    const std::string final = domain == 0 ? PackPathForItem(item) : domain == 1 ? PackPathForNotebook(item) : PackPathForSet(item);
    const std::string temporary = domain == 0 ? TempPackPathForItem(item) : domain == 1 ? TempPackPathForNotebook(item) : TempPackPathForSet(item);
    path_calls = 0;
    protocol::v3::RequestMetadata metadata;
    int result = ESP_OK; bool escaped = false;
    try {
        result = domain == 0 ? DownloadWordPackToStorage("token", metadata, item) :
                 domain == 1 ? DownloadNotePackToStorage("token", metadata, item, nullptr) :
                               DownloadProblemPackToStorage("token", metadata, item);
    } catch (const std::bad_alloc&) { escaped = true; }
    deny_cpp = false;
    CHECK(!escaped);  // Cleanup must not throw on a new path/digest allocation.
    CHECK(opens == closes);
    CHECK(!runtime::HasActiveStorageTempFileLease() && !runtime::HasActivePackStreamPath());
    const int expected = point == Failure::kNone ? ESP_OK : point == Failure::kDigest ? ESP_ERR_INVALID_CRC : ESP_FAIL;
    CHECK(result == expected);
    CHECK(abort_calls == (point == Failure::kNone ? 0u : 1u));
    CHECK(!FileExists(temporary));
    CHECK(FileExists(final) == (point == Failure::kNone));
    if (point == Failure::kNone) {
        std::ifstream stream(final); std::string body; stream >> body;
        CHECK(body == payload);
    }
    if (measure) {
        std::printf("domain=%u path_builders=%u callback_cpp_allocations=%u\n", domain, path_calls, callback_allocations);
    } else {
        CHECK(path_calls == 2 && callback_allocations == 0 && denied_allocations == 0);
    }
    if (probe_resets) {
        const int cleared = domain == 0 ? ResetWordPackStorageCacheRaw(nullptr) :
                            domain == 1 ? ResetNotePackStorageCacheRaw(nullptr) : ResetProblemPackStorageCacheRaw(nullptr);
        CHECK(cleared == ESP_OK && !FileExists(final));
        CHECK(::access(kManifestPath, F_OK) != 0 && ::access(kManifestTempPath, F_OK) != 0 &&
              ::access(kManifestBackupPath, F_OK) != 0);
    }
}
static void ConcurrentDownloads(unsigned domain, unsigned mode)
{
    Reset(); nesting_mode = mode;
    Manifest item; protocol::v3::RequestMetadata metadata;
    const int result = domain == 0 ? DownloadWordPackToStorage("token", metadata, item) :
                       domain == 1 ? DownloadNotePackToStorage("token", metadata, item, nullptr) :
                                     DownloadProblemPackToStorage("token", metadata, item);
    CHECK(result == ESP_OK && nested_calls == 1 && opens == closes && abort_calls == 0);
    CHECK(begin_calls == (mode == 1 ? 1u : 2u));
    CHECK(callback_allocations == 0 && !runtime::HasActiveStorageTempFileLease());
    const std::string final = domain == 0 ? PackPathForItem(item) : domain == 1 ? PackPathForNotebook(item) : PackPathForSet(item);
    std::ifstream stream(final); std::string body; stream >> body; CHECK(body == payload);
    nesting_mode = 0;
    // A failed duplicate must not release the owner's slot; after the owner
    // completes, the same path can be acquired again without leaking a slot.
    CHECK((domain == 0 ? DownloadWordPackToStorage("token", metadata, item) :
           domain == 1 ? DownloadNotePackToStorage("token", metadata, item, nullptr) :
                         DownloadProblemPackToStorage("token", metadata, item)) == ESP_OK);
    CHECK(opens == closes);
}
}

int main(int argc, char** argv)
{
    CHECK(argc == 3);
    root = argv[1]; wqn::kStorageRoot = root.c_str();
    wqn::manifest_path = root + "/fixture_manifest.json";
    wqn::manifest_temp_path = root + "/fixture_manifest.tmp";
    wqn::manifest_backup_path = root + "/fixture_manifest.bak";
    wqn::kManifestPath = wqn::manifest_path.c_str();
    wqn::kManifestTempPath = wqn::manifest_temp_path.c_str();
    wqn::kManifestBackupPath = wqn::manifest_backup_path.c_str();
    const bool measure = std::atoi(argv[2]) != 0;
    if (measure) {
        for (unsigned domain = 0; domain < 3; ++domain) wqn::Case(domain, Failure::kNone, false, true);
    } else {
        for (unsigned domain = 0; domain < 3; ++domain) {
            wqn::ConcurrentDownloads(domain, 1);
            wqn::ConcurrentDownloads(domain, 2);
            wqn::Case(domain, Failure::kNone, true, false, true);
            wqn::Case(domain, Failure::kNetwork, true);
            wqn::Case(domain, Failure::kNone, true);
            for (Failure point : {Failure::kOpen, Failure::kStart, Failure::kAppend, Failure::kDigest,
                                  Failure::kFinish, Failure::kFlush, Failure::kSync, Failure::kClose, Failure::kRename})
                wqn::Case(domain, point, true);
            for (digest_seed = 0; digest_seed < 16; ++digest_seed) wqn::Case(domain, Failure::kNone, true);
        }
    }
    std::printf("Production pack stream allocation/lifecycle: %u checks PASS; libc/RTOS/SHA ports, not ESP32 HIL\n", checks);
}
