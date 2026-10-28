// Production generic HttpRequest with controlled SDK reads and time.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "network_response_limits.h"

constexpr int HTTP_METHOD_GET = 0;
constexpr int ESP_ERR_INVALID_SIZE = 0x104;
constexpr char kTag[] = "buffered-test";
@@TIMEOUT@@
using wqn::network_limits::kMaxBufferedJsonBytes;
int checks = 0, failures = 0, reads = 0, closes = 0, cleanups = 0;
int64_t clock_us = 0, packet_us = 0, advertised = -1;
size_t supplied = 0, total_bytes = 0, chunk_bytes = 511;
int response_status = 200, read_failure_at = -1;
bool initialized = true;
std::string normal_json;
std::string* output = nullptr;
size_t largest_body_seen = 0;
struct HostClient {} client;

void Check(bool ok, const char* message)
{
    ++checks;
    if (!ok) {
        if (failures < 10) std::fprintf(stderr, "FAIL %s\n", message);
        ++failures;
    }
}
void HostLog(const char*, const char*, ...) {}
void FeedTaskWatchdogIfSubscribed() {}
int64_t esp_timer_get_time() { return clock_us; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*) { return initialized ? &client : nullptr; }
esp_err_t esp_http_client_set_header(HostClient*, const char*, const char*) { return ESP_OK; }
esp_err_t esp_http_client_open(HostClient*, int) { return ESP_OK; }
int esp_http_client_write(HostClient*, const char*, int size) { return size; }
int64_t esp_http_client_fetch_headers(HostClient*) { return advertised; }
int esp_http_client_get_status_code(HostClient*) { return response_status; }
int esp_http_client_read(HostClient*, char* data, int size)
{
    largest_body_seen = std::max(largest_body_seen, output->size());
    ++reads;
    clock_us += packet_us;
    if (reads == read_failure_at) return -1;
    const size_t count = std::min({static_cast<size_t>(size), chunk_bytes, total_bytes - supplied});
    if (normal_json.empty()) std::memset(data, 'x', count);
    else std::memcpy(data, normal_json.data() + supplied, count);
    supplied += count;
    return static_cast<int>(count);
}
esp_err_t esp_http_client_close(HostClient*) {
    largest_body_seen = std::max(largest_body_seen, output->size());
    ++closes; return ESP_OK;
}
esp_err_t esp_http_client_cleanup(HostClient*) { ++cleanups; return ESP_OK; }

@@HTTP@@

void Reset(size_t bytes, int64_t declared = -1, size_t chunk = 511)
{
    reads = closes = cleanups = 0;
    supplied = 0;
    largest_body_seen = 0;
    total_bytes = bytes;
    chunk_bytes = chunk;
    advertised = declared;
    clock_us = packet_us = 0;
    response_status = 200;
    read_failure_at = -1;
    initialized = true;
    normal_json.clear();
}

int main(int argc, char** argv)
{
    std::string body;
    output = &body;
    int status = 0;
    for (size_t chunk : {size_t(1), size_t(17), size_t(511)}) {
        for (size_t bytes : {size_t(0), kMaxBufferedJsonBytes - 1, kMaxBufferedJsonBytes}) {
            Reset(bytes, static_cast<int64_t>(bytes), chunk);
            Check(HttpRequest("GET", "https://synthetic.invalid", nullptr, nullptr, &status, &body) == ESP_OK,
                  "body within limit succeeds");
            Check(body.size() == bytes && status == 200, "within-limit body is complete");
            Check(closes == 1 && cleanups == 1, "healthy client closed and cleaned once");
        }
        for (int64_t declared : {int64_t(-1), int64_t(0), int64_t(8)}) {
            Reset(kMaxBufferedJsonBytes + 1, declared, chunk);
            Check(HttpRequest("GET", "https://synthetic.invalid", nullptr, nullptr, &status, &body) == ESP_ERR_INVALID_SIZE,
                  "unknown, chunked or lying length cannot bypass cumulative cap");
            Check(body.empty() && body.capacity() == std::string().capacity(), "oversize failure releases partial body allocation");
            Check(closes == 1 && cleanups == 1, "oversize client closed and cleaned once");
            Check(largest_body_seen <= kMaxBufferedJsonBytes, "body never grows past cap before cleanup");
            Check(supplied <= kMaxBufferedJsonBytes + chunk && supplied < total_bytes + chunk,
                  "oversize stops at first violating chunk");
        }
    }
    for (int64_t declared : {static_cast<int64_t>(kMaxBufferedJsonBytes + 1),
                             int64_t(1) << 32, std::numeric_limits<int64_t>::max()}) {
        Reset(0, declared);
        Check(HttpRequest("GET", "https://synthetic.invalid", nullptr, nullptr, &status, &body) == ESP_ERR_INVALID_SIZE,
              "oversize advertised length rejected without narrowing");
        Check(reads == 0 && body.empty(), "advertised oversize does not read or allocate body");
        Check(closes == 1 && cleanups == 1, "advertised rejection releases client");
    }
    for (int http_status : {200, 400, 401, 500}) {
        Reset(0, -1, 17);
        response_status = http_status;
        normal_json = "{\"ok\":false,\"message\":\"synthetic\"}";
        total_bytes = normal_json.size();
        Check(HttpRequest("POST", "https://synthetic.invalid", nullptr, nullptr, &status, &body) == ESP_OK &&
              body == normal_json && status == http_status, "normal success/error response preserved for caller");
    }
    Reset(kMaxBufferedJsonBytes, -1);
    packet_us = 1000000;
    Check(HttpRequest("GET", "https://synthetic.invalid", nullptr, nullptr, &status, &body, nullptr, nullptr, 100) == ESP_ERR_TIMEOUT,
          "existing whole-read deadline remains active");
    Check(body.empty() && closes == 1 && cleanups == 1, "deadline failure discards incomplete body and closes client");
    Reset(1000, -1);
    read_failure_at = 2;
    Check(HttpRequest("GET", "https://synthetic.invalid", nullptr, nullptr, &status, &body) == ESP_FAIL && body.empty(),
          "read failure also discards incomplete body");
    Check(closes == 1 && cleanups == 1, "read failure closes client");
    Reset(0, -1);
    initialized = false;
    body.assign(100000, 's');
    Check(HttpRequest("GET", "https://synthetic.invalid", nullptr, nullptr, &status, &body) == ESP_ERR_NO_MEM,
          "initialization failure propagates");
    Check(body.empty() && body.capacity() == std::string().capacity(), "new request releases previous output allocation");
    for (int index = 1; index < argc; ++index) {
        std::ifstream input(argv[index], std::ios::binary);
        const std::string expected((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        Check(input.good(), "generated manifest fixture is readable");
        Check(expected.size() > 16*1024 && expected.size() <= kMaxBufferedJsonBytes,
              "full-size manifest page fits independent buffered JSON budget");
        Reset(expected.size(), -1, 511);
        normal_json = expected;
        Check(HttpRequest("POST", "https://synthetic.invalid", nullptr, nullptr, &status, &body) == ESP_OK && body == expected,
              "large legitimate JSON page is returned byte-identically");
    }
    std::printf("Buffered HTTP: %d checks, %d failures (production function; host SDK I/O; NOT HIL)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
