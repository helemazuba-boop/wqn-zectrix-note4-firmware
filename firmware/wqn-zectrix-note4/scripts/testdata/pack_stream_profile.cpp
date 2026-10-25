// Actual transaction/sink with libc files and deterministic time/queue/SHA ports.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>
#include "voice_diagnostics.h"

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 258,
    ESP_ERR_INVALID_SIZE = 260, ESP_ERR_INVALID_CRC = 265;
constexpr size_t kMaxPackBytes = 1024 * 1024;
constexpr char kTag[] = "test_pack";
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::abort(); } } while (false)
static int64_t now_us = 0;
static int64_t esp_timer_get_time() { return now_us; }
static std::vector<std::string> logs;
static void HostLog(const char*, const char*, ...) __attribute__((format(printf, 2, 3)));
static void HostLog(const char*, const char* format, ...) {
    char text[1536]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args); va_end(args);
    logs.emplace_back(text); now_us += 300;
}
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
struct WqnWordPackManifestItem { size_t byte_size = 8; std::string sha256 = std::string(64, '0'), pack_id = "pack-a"; };
struct mbedtls_sha256_context {};
static bool sha_fail = false, short_write = false, owner_reject = false;
static int mbedtls_sha256_starts(mbedtls_sha256_context*, int) { return 0; }
static int mbedtls_sha256_update(mbedtls_sha256_context*, const uint8_t*, size_t) {
    now_us += 700; return sha_fail ? -1 : 0;
}
static int mbedtls_sha256_finish(mbedtls_sha256_context*, unsigned char* digest) {
    std::memset(digest, 0, 32); return 0;
}
static std::string directory;
static std::string TempPackPathForItem(const WqnWordPackManifestItem&) { return directory + "/pack.tmp"; }
static std::string PackPathForItem(const WqnWordPackManifestItem&) { return directory + "/pack.jsonl"; }
static bool FileExists(const std::string& path) { return std::filesystem::exists(path); }
extern "C" size_t __real_fwrite(const void*, size_t, size_t, FILE*);
extern "C" size_t __wrap_fwrite(const void* bytes, size_t size, size_t count, FILE* file) {
    now_us += 11000;
    return __real_fwrite(bytes, size, short_write && count ? count - 1 : count, file);
}
namespace services {
static int ExecuteStorageTransactionNamed(int (*transaction)(void*), void* context, const char*) {
    now_us += 5000; // queue wait is deliberately distinct from the callback cost
    const int result = owner_reject ? ESP_FAIL : transaction(context);
    now_us += 1000; return result;
}
}
@@TYPES@@
@@PRODUCTION@@

static void Reset(WordPackStreamContext& context, WqnWordPackManifestItem& item) {
    context = WordPackStreamContext{}; context.item = &item;
    sha_fail = short_write = owner_reject = false;
    context.operation = WordPackStreamOperation::kBegin;
    CHECK(WordPackStreamTransaction(&context) == ESP_OK);
    now_us = 0; logs.clear();
}
static void Abort(WordPackStreamContext& context) {
    context.operation = WordPackStreamOperation::kAbort;
    CHECK(WordPackStreamTransaction(&context) == ESP_OK);
    CHECK(context.file == nullptr && !FileExists(TempPackPathForItem(*context.item)));
}
int main(int argc, char** argv) {
    CHECK(argc == 2); directory = argv[1];
    WqnWordPackManifestItem item;
    WordPackStreamContext context;
    const uint8_t bytes[] = {'a','b','c','d'};
    Reset(context, item);
    CHECK(AppendWordPackStream(&context, bytes, 4) == ESP_OK);
    CHECK(AppendWordPackStream(&context, bytes, 4) == ESP_OK);
    CHECK(context.profile.append_calls == 2 && context.profile.requested_bytes == 8);
    CHECK(context.profile.fwrite_bytes == 8 && context.bytes_written == 8);
    CHECK(context.profile.write_us == 22000 && context.profile.sha_us == 1400);
    CHECK(context.profile.log_us == 600 && context.profile.append_wall_us == 36000);
    CHECK(context.profile.short_writes == 0 && context.profile.sha_failures == 0);
    context.operation = WordPackStreamOperation::kCommit;
    CHECK(WordPackStreamTransaction(&context) == ESP_OK);
    CHECK(context.file == nullptr && FileExists(PackPathForItem(item)));
    CHECK(std::filesystem::file_size(PackPathForItem(item)) == 8);
    LogWordPackStreamProfile(context, ESP_OK);
    CHECK(logs.back().find("append_calls=2 requested_bytes=8 fwrite_bytes=8 accepted_bytes=8") != std::string::npos);
    CHECK(logs.back().find("append_wall_us=36000 fwrite_us=22000 sha_us=1400 append_log_us=600") != std::string::npos);
    // A short libc write and a rejected owner transaction are not successful
    // plaintext commits; their time and actual write count remain observable.
    Reset(context, item); short_write = true;
    CHECK(AppendWordPackStream(&context, bytes, 4) == ESP_FAIL);
    CHECK(context.profile.short_writes == 1 && context.profile.fwrite_bytes == 3);
    CHECK(context.bytes_written == 0 && context.profile.sha_us == 0 && context.profile.log_us == 0);
    CHECK(context.profile.append_wall_us == 17000); Abort(context);
    Reset(context, item); sha_fail = true;
    CHECK(AppendWordPackStream(&context, bytes, 4) == ESP_FAIL);
    CHECK(context.profile.sha_failures == 1 && context.profile.fwrite_bytes == 4);
    CHECK(context.bytes_written == 0 && context.profile.sha_us == 700 && context.profile.log_us == 0);
    CHECK(context.profile.append_wall_us == 17700); Abort(context);
    Reset(context, item); owner_reject = true;
    CHECK(AppendWordPackStream(&context, bytes, 4) == ESP_FAIL);
    CHECK(context.profile.append_calls == 1 && context.profile.write_us == 0);
    CHECK(context.profile.fwrite_bytes == 0 && context.profile.append_wall_us == 6000);
    owner_reject = false; Abort(context);
    Reset(context, item);
    CHECK(AppendWordPackStream(&context, bytes, 0) == ESP_ERR_INVALID_ARG);
    CHECK(context.profile.append_calls == 0); Abort(context);
    Reset(context, item); item.byte_size = 3;
    CHECK(AppendWordPackStream(&context, bytes, 4) == ESP_ERR_INVALID_SIZE);
    CHECK(context.profile.append_calls == 1 && context.profile.requested_bytes == 4);
    CHECK(context.profile.fwrite_bytes == 0 && context.profile.write_us == 0);
    CHECK(context.profile.short_writes == 0 && context.profile.append_wall_us == 6000);
    Abort(context); item.byte_size = 8;
    Reset(context, item);
    CHECK(AppendWordPackStream(&context, bytes, 4) == ESP_OK);
    CHECK(AppendWordPackStream(&context, bytes, 4) == ESP_OK);
    item.sha256.assign(64, '1'); context.operation = WordPackStreamOperation::kCommit;
    CHECK(WordPackStreamTransaction(&context) == ESP_ERR_INVALID_CRC);
    CHECK(context.file == nullptr && !FileExists(TempPackPathForItem(item)));
    CHECK(FileExists(PackPathForItem(item))); // prior valid pack is retained
    std::printf("Word pack append profiling: %u checks passed (controlled host costs; not SPIFFS HIL)\n", checks);
}
