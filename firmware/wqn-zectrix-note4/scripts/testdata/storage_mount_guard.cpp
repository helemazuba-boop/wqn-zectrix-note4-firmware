#include <cstdio>
#include <cstring>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NO_MEM = 0x101, ESP_ERR_INVALID_STATE = 0x103,
                    ESP_ERR_NOT_FOUND = 0x105;
constexpr unsigned CONFIG_SPIFFS_PAGE_SIZE = 256;
constexpr const char* kTag = "storage", *kStorageBasePath = "/storage",
                    *kStoragePartitionLabel = "storage";
struct esp_vfs_spiffs_conf_t {
    const char* base_path = nullptr;
    const char* partition_label = nullptr;
    size_t max_files = 0;
    bool format_if_mount_failed = false;
};
struct Scenario {
    esp_err_t registration = ESP_OK;
    const char* mounted_label = "storage";
    esp_err_t stats = ESP_OK;
} scenario;
int checks = 0, failures = 0, registrations = 0, probes = 0, infos = 0;
const char* probed_label = nullptr;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL mount-guard: %s\n", message); }
}
void HostLog(const char*, const char*, ...) __attribute__((format(printf, 2, 3)));
void HostLog(const char*, const char*, ...) {}
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr, ...) do { const auto e = (expr); if (e != ESP_OK) return e; } while (false)
const char* esp_err_to_name(esp_err_t) { return "fixture"; }
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t* config)
{
    ++registrations;
    Check(std::strcmp(config->base_path, "/storage") == 0, "keep the storage path");
    Check(std::strcmp(config->partition_label, "storage") == 0, "mount the intended partition");
    Check(config->max_files == 8, "keep the configured file budget");
    Check(!config->format_if_mount_failed, "never format to recover a failed mount");
    return scenario.registration;
}
bool esp_spiffs_mounted(const char* label)
{
    ++probes;
    probed_label = label;
    return scenario.mounted_label != nullptr && std::strcmp(label, scenario.mounted_label) == 0;
}
esp_err_t esp_spiffs_info(const char* label, size_t* total, size_t* used)
{
    ++infos;
    Check(std::strcmp(label, "storage") == 0, "stats use the intended partition");
    *total = 512; *used = 128;
    return scenario.stats;
}

// PRODUCTION_FUNCTION

void Run(Scenario input, esp_err_t expected, int expected_probes, int expected_infos)
{
    scenario = input;
    registrations = probes = infos = 0;
    probed_label = nullptr;
    const esp_err_t result = InitStoragePartition();
    Check(result == expected, "only a proven existing mount may suppress invalid-state");
    Check(registrations == 1, "one registration attempt per startup");
    Check(probes == expected_probes, "ambiguous invalid-state requires a mount probe");
    Check(infos == expected_infos, "a rejected mount cannot enter the success stats path");
    if (probes != 0)
        Check(probed_label != nullptr && std::strcmp(probed_label, "storage") == 0,
              "another mounted partition cannot prove storage is mounted");
}
int main()
{
    Run({ESP_OK, "storage", ESP_OK}, ESP_OK, 0, 1);
    Run({ESP_ERR_INVALID_STATE, "storage", ESP_OK}, ESP_OK, 1, 0);
    // IDF v5.5.6 also uses INVALID_STATE for no free mount slots and encrypted
    // SPIFFS partitions. These API outcomes do not prove an existing mount.
    Run({ESP_ERR_INVALID_STATE, nullptr, ESP_OK}, ESP_ERR_INVALID_STATE, 1, 0);
    Run({ESP_ERR_INVALID_STATE, "another", ESP_OK}, ESP_ERR_INVALID_STATE, 1, 0);
    Run({ESP_ERR_NOT_FOUND, nullptr, ESP_OK}, ESP_ERR_NOT_FOUND, 0, 0);
    Run({ESP_ERR_NO_MEM, nullptr, ESP_OK}, ESP_ERR_NO_MEM, 0, 0);
    Run({ESP_FAIL, nullptr, ESP_OK}, ESP_FAIL, 0, 0);
    // A failed optional space report does not undo a successful SDK mount.
    Run({ESP_OK, "storage", ESP_FAIL}, ESP_OK, 0, 1);
    std::printf("Storage mount guard: %d checks, %d failures (production function; host SPIFFS API; NOT HIL)\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
