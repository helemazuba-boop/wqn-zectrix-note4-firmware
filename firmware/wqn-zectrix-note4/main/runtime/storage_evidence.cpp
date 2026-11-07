#include "runtime/storage_evidence.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "config.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "runtime/pack_stream_path_lease.h"
#include "runtime/storage_temp_file_lease.h"
#include "services/storage_service.h"
#include "storage.h"

namespace wqn::runtime {
namespace {
constexpr size_t kChunk = 384, kName = 64;
constexpr const char* kKeys[] = {"cur_seq", "cur_rnd", "cur_dic", "cur_rev",
    "cur_int", "cur_shf", "cur_mis", "word_deck_gen", "word_deck_chg",
    "word_deck", "word_seq_idx"};
constexpr size_t kNvsCount = sizeof(kKeys) / sizeof(kKeys[0]);
constexpr size_t kMaxFiles = kStorageEvidenceMaxEntities - kNvsCount;
enum class Kind : uint8_t { kBegin = 1, kEntity = 2, kData = 3, kEntityEnd = 4, kFinish = 5 };

class Hash {
public:
    Hash() { mbedtls_sha256_init(&value_); good_ = mbedtls_sha256_starts(&value_, 0) == 0; }
    ~Hash() { mbedtls_sha256_free(&value_); }
    bool Add(const uint8_t* data, size_t size) {
        good_ = good_ && mbedtls_sha256_update(&value_, data, size) == 0;
        return good_;
    }
    bool Finish(std::array<uint8_t, 32>* out) {
        std::array<uint8_t, 32> internal{};
        if (!good_ || mbedtls_sha256_finish(&value_, internal.data()) != 0) return false;
        *out = internal; return true;
    }
private:
    mbedtls_sha256_context value_;
    bool good_ = false;
};

struct Bytes {
    std::array<uint8_t, 512> value{};
    size_t size = 0;
    void U(uint64_t number, size_t count) {
        for (size_t i = 0; i < count; ++i) value[size++] = static_cast<uint8_t>(number >> (8 * i));
    }
    void Raw(const void* data, size_t count) {
        if (count != 0) std::memcpy(value.data() + size, data, count);
        size += count;
    }
};

class Writer {
public:
    Writer(const StorageEvidenceSink& sink, StorageEvidenceSummary* summary)
        : sink_(sink), summary_(summary) {}
    esp_err_t Poll() {
        if (error != ESP_OK) return error;
        if (sink_.cancelled != nullptr && sink_.cancelled(sink_.cancel_context))
            return error = ESP_ERR_INVALID_STATE;
        const int64_t now = esp_timer_get_time();
        if (now >= sink_.deadline_us) return error = ESP_ERR_TIMEOUT;
        if (now - last_yield_us_ >= 20000) {
            vTaskDelay(1);
            last_yield_us_ = esp_timer_get_time();
            if (sink_.cancelled != nullptr && sink_.cancelled(sink_.cancel_context))
                return error = ESP_ERR_INVALID_STATE;
            if (last_yield_us_ >= sink_.deadline_us) return error = ESP_ERR_TIMEOUT;
        }
        return ESP_OK;
    }
    bool Send(Kind kind, const Bytes& body) {
        error = Poll();
        if (error != ESP_OK) return false;
        if (body.size > 512 || sequence_ == UINT32_MAX) { error = ESP_ERR_INVALID_SIZE; return false; }
        std::array<uint8_t, kStorageEvidenceMaxFrameBytes> frame{};
        std::memcpy(frame.data(), "WQE1", 4);
        frame[4] = static_cast<uint8_t>(kind);
        frame[5] = body.size & 255;
        frame[6] = body.size >> 8;
        for (size_t i = 0; i < 4; ++i) frame[7 + i] = static_cast<uint8_t>(sequence_ >> (8 * i));
        std::memcpy(frame.data() + 11, body.value.data(), body.size);
        if (kind != Kind::kFinish && !hash_.Add(frame.data(), 11 + body.size)) {
            error = ESP_FAIL; return false;
        }
        error = sink_.emit(frame.data(), 11 + body.size, sink_.context);
        if (error != ESP_OK) return false;
        ++sequence_; ++summary_->frames;
        return true;
    }
    bool Data(uint16_t id, uint64_t offset, const uint8_t* data, size_t size) {
        if (size > kChunk || size > kStorageEvidenceMaxTotalBytes - summary_->data_bytes) {
            error = ESP_ERR_INVALID_SIZE; return false;
        }
        Bytes payload; payload.U(id, 2); payload.U(offset, 8); payload.Raw(data, size);
        if (!Send(Kind::kData, payload)) return false;
        summary_->data_bytes += size;
        return true;
    }
    bool Digest(std::array<uint8_t, 32>* out) { return hash_.Finish(out); }
    esp_err_t error = ESP_OK;
private:
    const StorageEvidenceSink& sink_;
    StorageEvidenceSummary* summary_;
    Hash hash_;
    uint32_t sequence_ = 0;
    int64_t last_yield_us_ = 0;
};

struct Entry {
    char name[kName]{};
    uint8_t presence = 0, type = 0;  // 0 absent, 1 present, 2 unreadable/unknown.
    esp_err_t error = ESP_OK;
    uint64_t size = 0, captured = 0;
    std::array<uint8_t, 32> hash{};
};
class Inventory {
public:
    Inventory() {
        rows = static_cast<Entry*>(heap_caps_calloc(kMaxFiles, sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    ~Inventory() { heap_caps_free(rows); }
    bool Add(const char* name) {
        if (std::strlen(name) >= kName) return false;
        for (size_t i = 0; i < size; ++i) if (std::strcmp(name, rows[i].name) == 0) return true;
        if (size == kMaxFiles) return false;
        // calloc storage holds trivial Entry objects; placement construction
        // establishes the C++ object lifetime before field access.
        new (&rows[size]) Entry{};
        std::strcpy(rows[size++].name, name); return true;
    }
    Entry* rows = nullptr;
    size_t size = 0;
};

bool FixedName(const char* name) {
    const char* journals[] = {"wout", "nout"};
    const char* sessions[] = {"wsq", "wsr", "wsd", "wsv", "wsi", "wsh", "wsm", "nses", "wrej", "nrej"};
    char buffer[kName];
    for (const char* stem : journals) for (const char* ext : {"v1", "rec", "bak", "tmp"}) {
        std::snprintf(buffer, sizeof(buffer), "%s.%s", stem, ext);
        if (std::strcmp(name, buffer) == 0) return true;
    }
    for (const char* stem : sessions) for (const char* ext : {"v1", "tmp", "bak"}) {
        std::snprintf(buffer, sizeof(buffer), "%s.%s", stem, ext);
        if (std::strcmp(name, buffer) == 0) return true;
    }
    for (size_t i = 0; i < 7; ++i) for (const char* ext : {"cur", "ctp", "cbk"}) {
        std::snprintf(buffer, sizeof(buffer), "%s.%s", sessions[i], ext);
        if (std::strcmp(name, buffer) == 0) return true;
    }
    return false;
}
bool Selected(const char* name) {
    return FixedName(name) || std::strncmp(name, "wl", 2) == 0 ||
        std::strncmp(name, "we", 2) == 0 ||
        std::strncmp(name, "wp_", 3) == 0 || std::strncmp(name, "np_", 3) == 0;
}
void PathFor(const char* name, char* path, size_t capacity) {
    std::snprintf(path, capacity, "/storage/%s", name);
}
esp_err_t List(Inventory* out, Writer* control) {
    if (out->rows == nullptr) return ESP_ERR_NO_MEM;
    if (control->Poll() != ESP_OK) return control->error;
    DIR* directory = opendir("/storage");
    if (directory == nullptr) return ESP_FAIL;
    esp_err_t result = ESP_OK;
    while (true) {
        if ((result = control->Poll()) != ESP_OK) break;
        errno = 0;
        const auto* item = readdir(directory);
        if (item == nullptr) { if (errno != 0) result = ESP_FAIL; break; }
        const char* name = item->d_name;
        if (!Selected(name)) continue;
        if (std::strchr(name, '/') || std::strchr(name, '\\') || !out->Add(name)) {
            result = ESP_ERR_INVALID_SIZE; break;
        }
    }
    if (closedir(directory) != 0) result = ESP_FAIL;
    if (result != ESP_OK) return result;
    // Record genuine absence for all fixed forensic names, without globbing
    // unrelated storage or inspecting credential keys.
    const char* stems[] = {"wout", "nout", "wsq", "wsr", "wsd", "wsv", "wsi", "wsh", "wsm", "nses", "wrej", "nrej"};
    char name[kName];
    for (const char* stem : stems) for (const char* ext : {"v1", "tmp", "bak", "rec", "cur", "ctp", "cbk"}) {
        std::snprintf(name, sizeof(name), "%s.%s", stem, ext);
        if (FixedName(name) && !out->Add(name)) return ESP_ERR_INVALID_SIZE;
    }
    std::sort(out->rows, out->rows + out->size,
        [](const Entry& a, const Entry& b) { return std::strcmp(a.name, b.name) < 0; });
    for (size_t i = 0; i < out->size; ++i) {
        if ((result = control->Poll()) != ESP_OK) return result;
        Entry& row = out->rows[i]; char path[128]; PathFor(row.name, path, sizeof(path));
        struct stat info{};
#ifdef ESP_PLATFORM
        // ESP-IDF's flat SPIFFS VFS has stat, but no symbolic links/lstat.
        const int inspected = stat(path, &info);
#else
        const int inspected = lstat(path, &info);
#endif
        if (inspected != 0) {
            if (errno != ENOENT) { row.presence = 2; row.error = ESP_FAIL; }
        } else {
            row.presence = 1;
            if (!S_ISREG(info.st_mode) || info.st_size < 0) row.error = ESP_ERR_INVALID_STATE;
            else {
                row.size = static_cast<uint64_t>(info.st_size);
                if (row.size > kStorageEvidenceMaxFileBytes) row.error = ESP_ERR_INVALID_SIZE;
            }
        }
    }
    return ESP_OK;
}

esp_err_t ReadFile(Entry* row, Writer* writer, uint16_t id, bool publish = true) {
    Hash hash;
    if (row->presence == 0 || row->error != ESP_OK) {
        if (!hash.Finish(&row->hash)) return ESP_FAIL;
        return row->error;
    }
    char path[128]; PathFor(row->name, path, sizeof(path));
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(path, flags);
    if (fd < 0) { (void)hash.Finish(&row->hash); return ESP_FAIL; }
    struct stat info{};
    esp_err_t result = ESP_OK;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) != row->size) result = ESP_ERR_INVALID_STATE;
    std::array<uint8_t, kChunk> data{};
    while (result == ESP_OK) {
        if (writer != nullptr && (result = writer->Poll()) != ESP_OK) break;
        const ssize_t length = read(fd, data.data(), data.size());
        if (length < 0) { result = ESP_FAIL; break; }
        if (length == 0) break;
        if (static_cast<uint64_t>(length) > row->size - row->captured) { result = ESP_ERR_INVALID_STATE; break; }
        if (!hash.Add(data.data(), length)) { result = ESP_FAIL; break; }
        if (writer != nullptr && publish && !writer->Data(id, row->captured, data.data(), length)) { result = writer->error; break; }
        row->captured += length;
    }
    if (close(fd) != 0) result = ESP_FAIL;
    if (result == ESP_OK && row->captured != row->size) result = ESP_ERR_INVALID_STATE;
    if (!hash.Finish(&row->hash)) result = ESP_FAIL;
    return result;
}

struct NvsBuffer {
    uint8_t* data = nullptr;
    ~NvsBuffer() { heap_caps_free(data); }
    bool Allocate(size_t size) {
        data = static_cast<uint8_t*>(heap_caps_malloc(size == 0 ? 1 : size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return data != nullptr;
    }
};
template <typename T>
esp_err_t Scalar(nvs_handle_t handle, const char* key,
    esp_err_t (*get)(nvs_handle_t, const char*, T*), NvsBuffer* bytes, Entry* row) {
    row->size = sizeof(T);
    T value{}; const esp_err_t result = get(handle, key, &value);
    if (result != ESP_OK) return result;
    if (!bytes->Allocate(row->size)) return ESP_ERR_NO_MEM;
    const uint64_t bits = static_cast<std::make_unsigned_t<T>>(value);
    for (size_t i = 0; i < sizeof(T); ++i) bytes->data[i] = static_cast<uint8_t>(bits >> (8 * i));
    return ESP_OK;
}
esp_err_t ReadNvs(nvs_handle_t handle, esp_err_t namespace_status, const char* key,
    NvsBuffer* bytes, Entry* row) {
    std::strcpy(row->name, key);
    if (namespace_status == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (namespace_status != ESP_OK) { row->presence = 2; return namespace_status; }
    nvs_type_t type{};
    esp_err_t result = nvs_find_key(handle, key, &type);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) { row->presence = 2; return result; }
    row->presence = 1; row->type = static_cast<uint8_t>(type);
    switch (type) {
        case NVS_TYPE_U8: return Scalar(handle, key, nvs_get_u8, bytes, row);
        case NVS_TYPE_I8: return Scalar(handle, key, nvs_get_i8, bytes, row);
        case NVS_TYPE_U16: return Scalar(handle, key, nvs_get_u16, bytes, row);
        case NVS_TYPE_I16: return Scalar(handle, key, nvs_get_i16, bytes, row);
        case NVS_TYPE_U32: return Scalar(handle, key, nvs_get_u32, bytes, row);
        case NVS_TYPE_I32: return Scalar(handle, key, nvs_get_i32, bytes, row);
        case NVS_TYPE_U64: return Scalar(handle, key, nvs_get_u64, bytes, row);
        case NVS_TYPE_I64: return Scalar(handle, key, nvs_get_i64, bytes, row);
        case NVS_TYPE_STR: case NVS_TYPE_BLOB: {
            size_t measured = 0;
            result = type == NVS_TYPE_STR ? nvs_get_str(handle, key, nullptr, &measured)
                : nvs_get_blob(handle, key, nullptr, &measured);
            if (result != ESP_OK) return result;
            row->size = measured;
            if (measured > kStorageEvidenceMaxNvsBytes) return ESP_ERR_INVALID_SIZE;
            if (!bytes->Allocate(measured)) return ESP_ERR_NO_MEM;
            size_t actual = measured;
            result = type == NVS_TYPE_STR ? nvs_get_str(handle, key, reinterpret_cast<char*>(bytes->data), &actual)
                : nvs_get_blob(handle, key, bytes->data, &actual);
            if (result != ESP_OK) return result;
            return actual == measured ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }
        default: return ESP_ERR_NOT_SUPPORTED;
    }
}
bool EntityBegin(Writer* writer, const Entry& row, uint16_t id, uint8_t domain) {
    Bytes body; body.U(id, 2); body.U(domain, 1); body.U(row.presence, 1);
    body.U(row.type, 1); body.U(static_cast<uint32_t>(row.error), 4); body.U(row.size, 8);
    body.U(std::strlen(row.name), 1); body.Raw(row.name, std::strlen(row.name));
    return writer->Send(Kind::kEntity, body);
}
bool EntityEnd(Writer* writer, const Entry& row, uint16_t id) {
    Bytes body; body.U(id, 2); body.U(row.captured, 8); body.Raw(row.hash.data(), row.hash.size());
    body.U(static_cast<uint32_t>(row.error), 4); return writer->Send(Kind::kEntityEnd, body);
}
bool Same(const Entry& a, const Entry& b) {
    return std::strcmp(a.name, b.name) == 0 && a.presence == b.presence &&
        a.type == b.type && a.error == ESP_OK && b.error == ESP_OK &&
        a.size == b.size && a.captured == b.captured && a.hash == b.hash;
}
struct NvsHandle { nvs_handle_t value = 0; ~NvsHandle() { if (value != 0) nvs_close(value); } };
template <size_t N> bool Nonzero(const std::array<uint8_t, N>& value) {
    return std::any_of(value.begin(), value.end(), [](uint8_t c) { return c != 0; });
}
}  // namespace

esp_err_t CaptureStorageEvidenceOnOwner(const StorageEvidenceIdentity& identity,
    const StorageEvidenceSink& sink, StorageEvidenceSummary* summary) {
    if (summary == nullptr) return ESP_ERR_INVALID_ARG;
    *summary = {};
    auto fail = [summary](esp_err_t error) { summary->error = error; return error; };
    StorageEvidenceBootContext boot;
    if (ReadStorageEvidenceBootContextOnOwner(&boot) != ESP_OK)
        return fail(ESP_ERR_INVALID_STATE);
    if (boot.preservation_only) return fail(ESP_ERR_INVALID_STATE);
    if (!esp_spiffs_mounted("storage") || HasActiveStorageTempFileLease() || HasActivePackStreamPath())
        return fail(ESP_ERR_INVALID_STATE);
    if (sink.emit == nullptr || !Nonzero(identity.capture_id) || !Nonzero(identity.mac) ||
        !Nonzero(identity.firmware_elf_sha256) || sink.deadline_us <= 0) return fail(ESP_ERR_INVALID_ARG);
    Writer writer(sink, summary);
    if (writer.Poll() != ESP_OK) return fail(writer.Poll());
    Inventory before, after;
    esp_err_t result = List(&before, &writer);
    if (result != ESP_OK) return fail(result);
    if (after.rows == nullptr) return fail(ESP_ERR_NO_MEM);
    if (writer.Poll() != ESP_OK) return fail(writer.error);
    NvsHandle nvs;
    const esp_err_t namespace_status = nvs_open(WQN_NVS_NAMESPACE, NVS_READONLY, &nvs.value);
    const uint32_t cached_generation = wqn::GetDeckScopeGeneration();
    const bool scope_ready = wqn::IsWordDeckScopeReady();
    Bytes begin;
    begin.Raw(identity.capture_id.data(), 16); begin.Raw(identity.mac.data(), 6);
    begin.Raw(identity.firmware_elf_sha256.data(), 32);
    begin.U(static_cast<uint32_t>(boot.error), 4); begin.U(boot.observed_generation, 4);
    begin.U(cached_generation, 4); begin.U(scope_ready, 1);
    begin.U(static_cast<uint32_t>(namespace_status == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : namespace_status), 4);
    begin.U(namespace_status == ESP_OK, 1);
    if (!writer.Send(Kind::kBegin, begin)) return fail(writer.error);
    esp_err_t first_error = ESP_OK;
    auto remember = [&first_error](esp_err_t error) { if (first_error == ESP_OK && error != ESP_OK) first_error = error; };
    for (size_t i = 0; i < before.size; ++i) {
        Entry& row = before.rows[i]; const uint16_t id = ++summary->entities;
        if (!EntityBegin(&writer, row, id, 0)) return fail(writer.error);
        row.error = ReadFile(&row, &writer, id); remember(row.error);
        if (writer.error != ESP_OK) return fail(writer.error);
        if (!EntityEnd(&writer, row, id)) return fail(writer.error);
    }
    std::array<Entry, kNvsCount> saved_nvs{};
    for (size_t i = 0; i < kNvsCount; ++i) {
        if (writer.Poll() != ESP_OK) return fail(writer.Poll());
        Entry& row = saved_nvs[i]; NvsBuffer bytes;
        row.error = ReadNvs(nvs.value, namespace_status, kKeys[i], &bytes, &row);
        remember(row.error); const uint16_t id = ++summary->entities;
        if (!EntityBegin(&writer, row, id, 1)) return fail(writer.error);
        Hash hash;
        if (row.error == ESP_OK && row.presence == 1) {
            for (size_t offset = 0; offset < row.size;) {
                const size_t count = std::min<size_t>(kChunk, row.size - offset);
                if (!hash.Add(bytes.data + offset, count)) return fail(ESP_FAIL);
                if (!writer.Data(id, offset, bytes.data + offset, count)) return fail(writer.error);
                offset += count; row.captured = offset;
            }
        }
        if (!hash.Finish(&row.hash)) return fail(ESP_FAIL);
        if (!EntityEnd(&writer, row, id)) return fail(writer.error);
    }
    // Re-read full source bytes, not just length/mtime or our own emitted hash.
    bool unchanged = true;
    result = List(&after, &writer);
    if (result != ESP_OK || before.size != after.size) unchanged = false;
    if (unchanged) for (size_t i = 0; i < before.size; ++i) {
        if (writer.Poll() != ESP_OK) return fail(writer.Poll());
        after.rows[i].error = ReadFile(&after.rows[i], &writer, 0, false);
        remember(after.rows[i].error);
        if (!Same(before.rows[i], after.rows[i])) unchanged = false;
    }
    NvsHandle second;
    if (writer.Poll() != ESP_OK) return fail(writer.error);
    const esp_err_t second_status = nvs_open(WQN_NVS_NAMESPACE, NVS_READONLY, &second.value);
    if (namespace_status != second_status) unchanged = false;
    for (size_t i = 0; i < kNvsCount; ++i) {
        if (writer.Poll() != ESP_OK) return fail(writer.Poll());
        Entry row; NvsBuffer bytes;
        row.error = ReadNvs(second.value, second_status, kKeys[i], &bytes, &row);
        remember(row.error);
        Hash hash;
        if (row.error == ESP_OK && row.presence == 1) {
            if (!hash.Add(bytes.data, row.size)) return fail(ESP_FAIL);
            row.captured = row.size;
        }
        if (!hash.Finish(&row.hash)) return fail(ESP_FAIL);
        if (!Same(saved_nvs[i], row)) unchanged = false;
    }
    if (!unchanged) remember(ESP_ERR_INVALID_STATE);
    if (cached_generation != wqn::GetDeckScopeGeneration() || scope_ready != wqn::IsWordDeckScopeReady()) {
        unchanged = false;
        remember(ESP_ERR_INVALID_STATE);
    }
    if (!writer.Digest(&summary->transcript_sha256)) return fail(ESP_FAIL);
    Bytes finish; finish.U(summary->entities, 2); finish.U(summary->data_bytes, 8);
    finish.U(static_cast<uint32_t>(first_error), 4);
    finish.Raw(summary->transcript_sha256.data(), 32);
    finish.U(unchanged, 1); finish.U(first_error == ESP_OK && unchanged, 1);
    if (!writer.Send(Kind::kFinish, finish)) return fail(writer.error);
    summary->capture_complete = first_error == ESP_OK && unchanged;
    summary->error = first_error;
    return first_error;
}
}  // namespace wqn::runtime
