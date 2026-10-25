#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utime.h>
#include <vector>

#include "runtime/storage_temp_file_lease.h"
#include "voice_diagnostics.h"

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::abort(); } } while (false)
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 258,
    ESP_ERR_INVALID_STATE = 259, ESP_ERR_NO_MEM = 257, ESP_ERR_NOT_FINISHED = 264;
static time_t now_sec = 100000;
static int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" time_t __wrap_time(time_t* out) { if (out) *out = now_sec; return now_sec; }
static void HostLog(const char*, const char*, ...) __attribute__((format(printf, 2, 3)));
static void HostLog(const char*, const char*, ...) {}
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expression, ...) do { int code = (expression); if (code != ESP_OK) return code; } while (false)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(expression) ((void)(expression))
static const char* esp_err_to_name(int error) { return error == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
static std::string root;
static const char* kStorageBasePath = nullptr;
static constexpr char kStoragePartitionLabel[] = "storage", kTag[] = "test_storage";
static size_t base_used = 800, capacity_total = 1024, gc_freed = 0, last_gc_target = 0;
static unsigned info_calls = 0, fail_info_at = 0, gc_calls = 0;
static int gc_result = ESP_ERR_NOT_FINISHED;
static int esp_spiffs_info(const char* label, size_t* total, size_t* used)
{
    CHECK(std::string(label) == "storage");
    if (++info_calls == fail_info_at) return ESP_FAIL;
    *total = capacity_total;
    *used = base_used;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.is_regular_file()) *used += entry.file_size();
    return ESP_OK;
}
static int esp_spiffs_gc(const char* label, size_t target)
{
    CHECK(std::string(label) == "storage");
    ++gc_calls; last_gc_target = target;
    base_used -= std::min(base_used, gc_freed);
    return gc_result;
}

@@CAPACITY@@

static std::string Path(const char* name) { return root + "/" + name; }
static bool Exists(const char* name) { return std::filesystem::exists(Path(name)); }
static void Age(const std::string& path, time_t when)
{
    utimbuf times{when, when}; CHECK(::utime(path.c_str(), &times) == 0);
}
static void File(const char* name, size_t size, time_t when)
{
    std::ofstream stream(Path(name), std::ios::binary); stream << std::string(size, 'x'); stream.close();
    CHECK(stream.good()); Age(Path(name), when);
}
static void Reset()
{
    CHECK(!wqn::runtime::HasActiveStorageTempFileLease());
    std::filesystem::remove_all(root); std::filesystem::create_directories(root);
    now_sec = 100000; base_used = 800; capacity_total = 1024;
    gc_freed = last_gc_target = 0; info_calls = fail_info_at = gc_calls = 0;
    gc_result = ESP_ERR_NOT_FINISHED;
}
static int Capacity(size_t required = 1024, size_t reserve = 0)
{
    PackCapacityRequest request{required, reserve};
    return EnsurePackDownloadCapacityTransaction(&request);
}

namespace wqn {
namespace protocol {
namespace v3 { struct RequestMetadata {}; }
namespace word_study_v1 { constexpr unsigned kPackSchemaVersion = 1; }
}
struct Manifest {
    bool has_pack = true;
    std::string pack_id = std::string(36, 'p'), sha256 = std::string(64, 'a');
    unsigned schema_version = 1, entry_count = 1;
    size_t byte_size = 64;
};
using WqnWordPackManifestItem = Manifest;
using WqnNotePackManifestNotebook = Manifest;
using WqnProblemPackManifestSet = Manifest;
using WqnHttpChunkSink = esp_err_t (*)(void*, const uint8_t*, size_t);
using WqnTransferProgressSink = void*;
namespace runtime {
static bool sleep_admitted = true;
enum class SleepBlocker { kStorage };
class SleepLease {
    bool held_;
    explicit SleepLease(bool held) : held_(held) {}
public:
    static SleepLease TryAcquire(SleepBlocker, const char*, const char*, int) { return SleepLease(sleep_admitted); }
    explicit operator bool() const { return held_; }
};
}
namespace services {
int ExecuteStorageTransactionNamed(int (*transaction)(void*), void* context, const char*)
{ return transaction(context); }
}
static constexpr unsigned kPackSchemaVersion = 1, kMaxPackEntries = 1024, kMaxIndexEntries = 1024;
static constexpr size_t kMaxPackBytes = 1024 * 1024;
struct Hash {};
static void mbedtls_sha256_init(Hash*) {}
static void mbedtls_sha256_free(Hash*) {}
enum class Operation { kBegin, kAppend, kCommit, kAbort };
using WordPackStreamOperation = Operation;
using NotePackStreamOperation = Operation;
using ProblemPackStreamOperation = Operation;
@@WORD_PROFILE@@
struct Context {
    const Manifest *item = nullptr, *notebook = nullptr, *set = nullptr;
    Hash sha;
    Operation operation = Operation::kBegin;
    FILE* file = nullptr;
    const uint8_t* chunk = nullptr;
    size_t chunk_size = 0;
    std::string temporary;
    WordPackAppendProfile profile;
    size_t bytes_written = 0;
};
using WordPackStreamContext = Context;
using NotePackStreamContext = Context;
using ProblemPackStreamContext = Context;
enum class Failure { kNone, kBegin, kNetwork, kAppend, kCommit, kCancel };
static Failure failure;
static bool needs_download = true;
static unsigned stream_events = 0, aborts = 0;
static int HostStream(Context* context, const char* name)
{
    ++stream_events;
    CHECK(runtime::HasActiveStorageTempFileLease());
    switch (context->operation) {
    case Operation::kBegin:
        if (failure == Failure::kBegin) return ESP_FAIL;
        context->temporary = Path(name);
        context->file = std::fopen(context->temporary.c_str(), "wb");
        CHECK(context->file != nullptr);
        CHECK(std::fputs("initial", context->file) >= 0 && std::fflush(context->file) == 0);
        // Match SPIFFS: mtime captured at open, independent of a later SNTP step.
        Age(context->temporary, now_sec);
        break;
    case Operation::kAppend:
        if (failure == Failure::kAppend) return ESP_FAIL;
        CHECK(context->file != nullptr);
        CHECK(std::fwrite(context->chunk, 1, context->chunk_size, context->file) == context->chunk_size);
        break;
    case Operation::kCommit:
        CHECK(context->file != nullptr);
        CHECK(std::fclose(context->file) == 0); context->file = nullptr;
        if (failure == Failure::kCommit) return ESP_FAIL;
        CHECK(std::rename(context->temporary.c_str(), Path("final.pack").c_str()) == 0);
        break;
    case Operation::kAbort:
        ++aborts;
        if (context->file) { CHECK(std::fclose(context->file) == 0); context->file = nullptr; }
        if (!context->temporary.empty()) std::remove(context->temporary.c_str());
        break;
    }
    return ESP_OK;
}
static int WordPackStreamTransaction(void* p) { return HostStream(static_cast<Context*>(p), "wp_live.tmp"); }
static int NotePackStreamTransaction(void* p) { return HostStream(static_cast<Context*>(p), "np_live.tmp"); }
static int ProblemPackStreamTransaction(void* p) { return HostStream(static_cast<Context*>(p), "pp_live.tmp"); }
static bool WordPackNeedsDownload(const Manifest&) { return needs_download; }
static bool NotePackNeedsDownload(const Manifest&) { return needs_download; }
static bool ProblemPackNeedsDownload(const Manifest&) { return needs_download; }
static int Http(WqnHttpChunkSink sink, void* opaque)
{
    auto* context = static_cast<Context*>(opaque);
    now_sec += 600;  // Wall-clock correction; no monotonic transfer time elapses.
    CHECK(Capacity() == ESP_ERR_NO_MEM);
    CHECK(std::filesystem::exists(context->temporary));
    if (failure == Failure::kNetwork || failure == Failure::kCancel) return ESP_FAIL;
    const uint8_t bytes[] = {'c', 'h', 'u', 'n', 'k'};
    return sink(opaque, bytes, sizeof(bytes));
}
static int DownloadWordPackStream(const std::string&, const protocol::v3::RequestMetadata&,
                                  const Manifest&, WqnHttpChunkSink sink, void* p) { return Http(sink, p); }
static int DownloadNotePackStream(const std::string&, const protocol::v3::RequestMetadata&,
                                  const Manifest&, WqnHttpChunkSink sink, void* p, WqnTransferProgressSink) { return Http(sink, p); }
static int DownloadProblemPackStream(const std::string&, const protocol::v3::RequestMetadata&,
                                     const Manifest&, WqnHttpChunkSink sink, void* p) { return Http(sink, p); }

@@WORD@@
@@NOTE@@
@@PROBLEM@@

static void Lifecycles()
{
    for (unsigned domain = 0; domain < 3; ++domain) {
        for (Failure point : {Failure::kNone, Failure::kBegin, Failure::kNetwork,
                              Failure::kAppend, Failure::kCommit, Failure::kCancel}) {
            Reset(); failure = point; stream_events = aborts = 0;
            Manifest manifest; protocol::v3::RequestMetadata metadata;
            int result = domain == 0 ? DownloadWordPackToStorage("token", metadata, manifest) :
                         domain == 1 ? DownloadNotePackToStorage("token", metadata, manifest, nullptr) :
                                       DownloadProblemPackToStorage("token", metadata, manifest);
            CHECK(result == (point == Failure::kNone ? ESP_OK : ESP_FAIL));
            CHECK(!runtime::HasActiveStorageTempFileLease());
            CHECK(aborts == (point == Failure::kNone ? 0u : 1u));
            CHECK(Exists("final.pack") == (point == Failure::kNone));
            if (point == Failure::kNone) {
                std::ifstream stream(Path("final.pack")); std::string body;
                stream >> body; CHECK(body == "initialchunk");
            }
        }
        for (unsigned skip = 0; skip < 3; ++skip) {
            Reset(); Manifest manifest; protocol::v3::RequestMetadata metadata;
            needs_download = skip != 0; runtime::sleep_admitted = skip != 1;
            if (skip == 2) manifest.byte_size = 0;
            stream_events = 0;
            const int result = domain == 0 ? DownloadWordPackToStorage("", metadata, manifest) :
                               domain == 1 ? DownloadNotePackToStorage("", metadata, manifest, nullptr) :
                                             DownloadProblemPackToStorage("", metadata, manifest);
            CHECK(result == (skip == 0 ? ESP_OK : skip == 1 ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG));
            CHECK(stream_events == 0 && !runtime::HasActiveStorageTempFileLease());
            needs_download = runtime::sleep_admitted = true;
        }
    }
}
}  // namespace wqn

static void Reclaim()
{
    using wqn::runtime::StorageTempFileLease;
    Reset(); File("wp_orphan.tmp", 40, now_sec - 600); File("np_active.tmp", 32, now_sec - 600);
    File("ni_cache.wqni", 64, now_sec - 600); File("nses.v1", 16, now_sec - 600);
    {
        StorageTempFileLease outer;
        { StorageTempFileLease inner; CHECK(Capacity(128) == ESP_OK); }
        CHECK(wqn::runtime::HasActiveStorageTempFileLease());
        CHECK(Exists("wp_orphan.tmp") && Exists("np_active.tmp"));
        CHECK(!Exists("ni_cache.wqni") && Exists("nses.v1"));
        CHECK(Capacity() == ESP_ERR_NO_MEM && Exists("np_active.tmp"));
    }
    CHECK(Capacity() == ESP_ERR_NO_MEM);
    CHECK(!Exists("wp_orphan.tmp") && !Exists("np_active.tmp") && Exists("nses.v1"));
    Reset(); File("fresh.tmp", 8, now_sec - 299); File("future.tmp", 8, now_sec + 1);
    File("unclocked.tmp", 8, 0); File("old.tmp", 8, now_sec - 300);
    CHECK(Capacity() == ESP_ERR_NO_MEM);
    CHECK(Exists("fresh.tmp") && Exists("future.tmp") && Exists("unclocked.tmp") && !Exists("old.tmp"));
    Reset(); File("old.tmp", 8, now_sec - 600);
    CHECK(Capacity(1) == ESP_OK && gc_calls == 0 && Exists("old.tmp"));
    CHECK(EnsurePackDownloadCapacityTransaction(nullptr) == ESP_ERR_INVALID_ARG);
    CHECK(Capacity(SIZE_MAX, 1) == ESP_ERR_INVALID_ARG);
    Reset(); fail_info_at = 1; CHECK(Capacity() == ESP_FAIL && gc_calls == 0);
    Reset(); capacity_total = 4 * 1024 * 1024; base_used = 2 * 1024 * 1024;
    gc_freed = 1024 * 1024; gc_result = ESP_OK;
    CHECK(Capacity(3 * 1024 * 1024) == ESP_OK && last_gc_target == 1024 * 1024);
    Reset(); File("np_live.tmp", 32, now_sec - 600); File("ni_cache.wqni", 16, now_sec - 600);
    { StorageTempFileLease lease; fail_info_at = 2;
      CHECK(Capacity() == ESP_FAIL && Exists("np_live.tmp")); }
    CHECK(!wqn::runtime::HasActiveStorageTempFileLease());
}

static void ConcurrentLeases()
{
    std::atomic<unsigned> ready{0}; std::atomic<bool> release{false};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 4; ++i) workers.emplace_back([&] {
        { wqn::runtime::StorageTempFileLease lease;
          ready.fetch_add(1, std::memory_order_release);
          while (!release.load(std::memory_order_acquire)) std::this_thread::yield(); }
        for (unsigned n = 0; n < 5000; ++n) { wqn::runtime::StorageTempFileLease lease; }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ready.load(std::memory_order_acquire) < 4 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK(ready.load() == 4 && wqn::runtime::HasActiveStorageTempFileLease());
    release.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    CHECK(!wqn::runtime::HasActiveStorageTempFileLease());
}

int main(int argc, char** argv)
{
    static_assert(!std::is_copy_constructible_v<wqn::runtime::StorageTempFileLease>);
    static_assert(!std::is_move_constructible_v<wqn::runtime::StorageTempFileLease>);
    CHECK(argc == 2); root = argv[1]; kStorageBasePath = root.c_str();
    wqn::Lifecycles(); Reclaim(); ConcurrentLeases();
    std::printf("Storage capacity + three production download lifecycles: %u checks PASS; host ports, not SPIFFS/clock/SMP HIL\n", checks);
}
