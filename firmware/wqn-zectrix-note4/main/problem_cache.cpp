#include "problem_cache.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "miniz.h"
#include "storage.h"

namespace {

constexpr char kTag[] = "problem_cache";
constexpr char kPartitionLabel[] = "storage";
constexpr char kPrimaryPath[] = "/storage/problems.v1";
constexpr char kTempPath[] = "/storage/problems.tmp";
constexpr char kBackupPath[] = "/storage/problems.bak";

constexpr uint8_t kFileMagic[4] = {'W', 'Q', 'P', 'C'};
constexpr uint8_t kBlockMagic[4] = {'B', 'L', 'K', '1'};
constexpr uint8_t kIndexMagic[4] = {'I', 'N', 'D', 'X'};
constexpr uint16_t kFormatVersion = 1;
constexpr uint8_t kCodecDeflate = 1;
constexpr size_t kHeaderSize = 40;
constexpr size_t kBlockHeaderSize = 20;
constexpr size_t kIndexEntrySize = 16;
constexpr size_t kBlockTargetBytes = 32 * 1024;
constexpr size_t kMaxEncodedRecordBytes = 128 * 1024;
constexpr size_t kMaxCompressedBlockBytes = kMaxEncodedRecordBytes + 64 * 1024;
constexpr size_t kMaxUncompressedCacheBytes = 4 * 1024 * 1024;
constexpr size_t kMaxCacheFileBytes = 4 * 1024 * 1024;
constexpr size_t kSpiffsSafetyReserveBytes = 256 * 1024;
constexpr uint32_t kMaxProblemRecords = 2048;
constexpr int kCompressionFlags =
    static_cast<int>(TDEFL_WRITE_ZLIB_HEADER) |
    static_cast<int>(TDEFL_DEFAULT_MAX_PROBES);

struct CacheHeader {
    uint64_t generation = 0;
    uint32_t record_count = 0;
    uint32_t uncompressed_bytes = 0;
    uint32_t compressed_bytes = 0;
    uint32_t block_count = 0;
    uint32_t index_offset = 0;
};

struct CacheIndexEntry {
    uint64_t id_hash = 0;
    uint32_t block_offset = 0;
    uint32_t record_offset = 0;
};

struct PendingIndexEntry {
    uint64_t id_hash = 0;
    uint32_t record_offset = 0;
};

void PutU16(uint8_t* output, uint16_t value)
{
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8);
}

void PutU32(uint8_t* output, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) {
        output[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

void PutU64(uint8_t* output, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        output[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

uint16_t GetU16(const uint8_t* input)
{
    return static_cast<uint16_t>(input[0]) |
        static_cast<uint16_t>(input[1]) << 8;
}

uint32_t GetU32(const uint8_t* input)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(input[i]) << (i * 8);
    }
    return value;
}

uint64_t GetU64(const uint8_t* input)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(input[i]) << (i * 8);
    }
    return value;
}

uint32_t Crc32(const uint8_t* data, size_t length)
{
    return static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, data, length));
}

bool FileExists(const char* path)
{
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    std::fclose(file);
    return true;
}

esp_err_t RemoveIfPresent(const char* path)
{
    if (std::remove(path) == 0 || errno == ENOENT) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

bool ReadExact(FILE* file, void* output, size_t length)
{
    return length == 0 || std::fread(output, 1, length, file) == length;
}

bool WriteExact(FILE* file, const void* data, size_t length, size_t max_file_bytes)
{
    const long position = std::ftell(file);
    if (position < 0 || static_cast<size_t>(position) > max_file_bytes ||
        length > max_file_bytes - static_cast<size_t>(position)) {
        errno = ENOSPC;
        return false;
    }
    return length == 0 || std::fwrite(data, 1, length, file) == length;
}

void EncodeHeader(const CacheHeader& header, uint8_t output[kHeaderSize])
{
    std::memset(output, 0, kHeaderSize);
    std::memcpy(output, kFileMagic, sizeof(kFileMagic));
    PutU16(output + 4, kFormatVersion);
    output[6] = kCodecDeflate;
    output[7] = 0;
    PutU64(output + 8, header.generation);
    PutU32(output + 16, header.record_count);
    PutU32(output + 20, header.uncompressed_bytes);
    PutU32(output + 24, header.compressed_bytes);
    PutU32(output + 28, header.block_count);
    PutU32(output + 32, header.index_offset);
    PutU32(output + 36, Crc32(output, kHeaderSize - sizeof(uint32_t)));
}

bool DecodeHeader(const uint8_t input[kHeaderSize], CacheHeader* header)
{
    if (header == nullptr || std::memcmp(input, kFileMagic, sizeof(kFileMagic)) != 0 ||
        GetU16(input + 4) != kFormatVersion || input[6] != kCodecDeflate || input[7] != 0 ||
        GetU32(input + 36) != Crc32(input, kHeaderSize - sizeof(uint32_t))) {
        return false;
    }
    header->generation = GetU64(input + 8);
    header->record_count = GetU32(input + 16);
    header->uncompressed_bytes = GetU32(input + 20);
    header->compressed_bytes = GetU32(input + 24);
    header->block_count = GetU32(input + 28);
    header->index_offset = GetU32(input + 32);
    return header->record_count <= kMaxProblemRecords &&
        header->uncompressed_bytes <= kMaxUncompressedCacheBytes &&
        header->index_offset >= kHeaderSize;
}

uint64_t HashProblemId(const std::string& id)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char c : id) {
        hash ^= c;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool HexNibble(char value, uint8_t* nibble)
{
    if (value >= '0' && value <= '9') {
        *nibble = static_cast<uint8_t>(value - '0');
        return true;
    }
    if (value >= 'a' && value <= 'f') {
        *nibble = static_cast<uint8_t>(value - 'a' + 10);
        return true;
    }
    if (value >= 'A' && value <= 'F') {
        *nibble = static_cast<uint8_t>(value - 'A' + 10);
        return true;
    }
    return false;
}

bool ParseUuid(const std::string& text, uint8_t output[16])
{
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
        text[18] != '-' || text[23] != '-') {
        return false;
    }
    size_t output_index = 0;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '-') {
            ++i;
            continue;
        }
        if (i + 1 >= text.size() || output_index >= 16) {
            return false;
        }
        uint8_t high = 0;
        uint8_t low = 0;
        if (!HexNibble(text[i], &high) || !HexNibble(text[i + 1], &low)) {
            return false;
        }
        output[output_index++] = static_cast<uint8_t>((high << 4) | low);
        i += 2;
    }
    return output_index == 16;
}

std::string FormatUuid(const uint8_t input[16])
{
    static constexpr char kHex[] = "0123456789abcdef";
    char output[37] = {};
    size_t cursor = 0;
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            output[cursor++] = '-';
        }
        output[cursor++] = kHex[input[i] >> 4];
        output[cursor++] = kHex[input[i] & 0x0F];
    }
    return std::string(output, cursor);
}

bool ParseDigits(const std::string& value, size_t offset, size_t count, int* output)
{
    if (output == nullptr || offset + count > value.size()) {
        return false;
    }
    int parsed = 0;
    for (size_t i = 0; i < count; ++i) {
        const char c = value[offset + i];
        if (c < '0' || c > '9') {
            return false;
        }
        parsed = parsed * 10 + (c - '0');
    }
    *output = parsed;
    return true;
}

int64_t DaysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const int adjusted_month = static_cast<int>(month) + (month > 2 ? -3 : 9);
    const unsigned day_of_year =
        (153 * static_cast<unsigned>(adjusted_month) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

bool ParseIsoUtc(const std::string& value, int64_t* epoch_seconds)
{
    if (epoch_seconds == nullptr || value.size() < 20 || value[4] != '-' ||
        value[7] != '-' || value[10] != 'T' || value[13] != ':' || value[16] != ':') {
        return false;
    }
    size_t suffix = 19;
    if (value[suffix] == '.') {
        ++suffix;
        while (suffix < value.size() && value[suffix] >= '0' && value[suffix] <= '9') {
            ++suffix;
        }
    }
    if (suffix + 1 != value.size() || value[suffix] != 'Z') {
        return false;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!ParseDigits(value, 0, 4, &year) || !ParseDigits(value, 5, 2, &month) ||
        !ParseDigits(value, 8, 2, &day) || !ParseDigits(value, 11, 2, &hour) ||
        !ParseDigits(value, 14, 2, &minute) || !ParseDigits(value, 17, 2, &second) ||
        month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 60) {
        return false;
    }
    *epoch_seconds = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
        hour * 3600 + minute * 60 + second;
    return true;
}

void CivilFromDays(int64_t days, int* year, unsigned* month, unsigned* day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    int parsed_year = static_cast<int>(year_of_era) + static_cast<int>(era) * 400;
    const unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned month_prime = (5 * day_of_year + 2) / 153;
    const unsigned parsed_day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    const unsigned parsed_month = static_cast<unsigned>(
        static_cast<int>(month_prime) + (month_prime < 10 ? 3 : -9));
    parsed_year += parsed_month <= 2;
    *year = parsed_year;
    *month = parsed_month;
    *day = parsed_day;
}

std::string FormatIsoUtc(int64_t epoch_seconds)
{
    int64_t days = epoch_seconds / 86400;
    int64_t seconds_of_day = epoch_seconds % 86400;
    if (seconds_of_day < 0) {
        seconds_of_day += 86400;
        --days;
    }
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    CivilFromDays(days, &year, &month, &day);
    const int hour = static_cast<int>(seconds_of_day / 3600);
    const int minute = static_cast<int>((seconds_of_day % 3600) / 60);
    const int second = static_cast<int>(seconds_of_day % 60);
    char output[32] = {};
    std::snprintf(
        output,
        sizeof(output),
        "%04d-%02u-%02uT%02d:%02d:%02d.000Z",
        year,
        month,
        day,
        hour,
        minute,
        second);
    return output;
}

void AppendCborHead(std::vector<uint8_t>* output, uint8_t major, uint64_t value)
{
    if (value < 24) {
        output->push_back(static_cast<uint8_t>((major << 5) | value));
    } else if (value <= UINT8_MAX) {
        output->push_back(static_cast<uint8_t>((major << 5) | 24));
        output->push_back(static_cast<uint8_t>(value));
    } else if (value <= UINT16_MAX) {
        output->push_back(static_cast<uint8_t>((major << 5) | 25));
        output->push_back(static_cast<uint8_t>(value >> 8));
        output->push_back(static_cast<uint8_t>(value));
    } else if (value <= UINT32_MAX) {
        output->push_back(static_cast<uint8_t>((major << 5) | 26));
        for (int shift = 24; shift >= 0; shift -= 8) {
            output->push_back(static_cast<uint8_t>(value >> shift));
        }
    } else {
        output->push_back(static_cast<uint8_t>((major << 5) | 27));
        for (int shift = 56; shift >= 0; shift -= 8) {
            output->push_back(static_cast<uint8_t>(value >> shift));
        }
    }
}

void AppendCborUnsigned(std::vector<uint8_t>* output, uint64_t value)
{
    AppendCborHead(output, 0, value);
}

void AppendCborSigned(std::vector<uint8_t>* output, int64_t value)
{
    if (value >= 0) {
        AppendCborHead(output, 0, static_cast<uint64_t>(value));
    } else {
        AppendCborHead(output, 1, static_cast<uint64_t>(-(value + 1)));
    }
}

void AppendCborText(std::vector<uint8_t>* output, const std::string& value)
{
    AppendCborHead(output, 3, value.size());
    output->insert(output->end(), value.begin(), value.end());
}

void AppendCborBytes(std::vector<uint8_t>* output, const uint8_t* value, size_t length)
{
    AppendCborHead(output, 2, length);
    output->insert(output->end(), value, value + length);
}

uint64_t ProblemTypeCode(const std::string& value)
{
    if (value == "mcq") return 1;
    if (value == "short") return 2;
    if (value == "extended") return 3;
    return 0;
}

uint64_t ProblemStatusCode(const std::string& value)
{
    if (value == "wrong") return 1;
    if (value == "needs_review") return 2;
    if (value == "mastered") return 3;
    return 0;
}

std::string ProblemTypeFromCode(uint64_t value)
{
    if (value == 1) return "mcq";
    if (value == 2) return "short";
    if (value == 3) return "extended";
    return "";
}

std::string ProblemStatusFromCode(uint64_t value)
{
    if (value == 1) return "wrong";
    if (value == 2) return "needs_review";
    if (value == 3) return "mastered";
    return "";
}

void AppendCborEnumOrText(
    std::vector<uint8_t>* output,
    const std::string& value,
    uint64_t code)
{
    if (code != 0) {
        AppendCborUnsigned(output, code);
    } else {
        AppendCborText(output, value);
    }
}

esp_err_t EncodeProblem(const wqn::CachedProblem& problem, std::vector<uint8_t>* output)
{
    if (output == nullptr || problem.id.empty() || problem.id.size() > 128 ||
        problem.title.size() > 4096 || problem.type.size() > 32 ||
        problem.status.size() > 32 || problem.content_text.size() > 64 * 1024 ||
        problem.solution_text.size() > 64 * 1024 || problem.updated_at.size() > 64 ||
        problem.asset_count < 0 || problem.solution_asset_count < 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    output->clear();
    output->reserve(std::min(kMaxEncodedRecordBytes, problem.content_text.size() + problem.solution_text.size() + 256));
    AppendCborHead(output, 5, 9);

    AppendCborUnsigned(output, 0);
    uint8_t uuid[16] = {};
    if (ParseUuid(problem.id, uuid)) {
        AppendCborBytes(output, uuid, sizeof(uuid));
    } else {
        AppendCborText(output, problem.id);
    }

    AppendCborUnsigned(output, 1);
    AppendCborText(output, problem.title);
    AppendCborUnsigned(output, 2);
    AppendCborEnumOrText(output, problem.type, ProblemTypeCode(problem.type));
    AppendCborUnsigned(output, 3);
    AppendCborEnumOrText(output, problem.status, ProblemStatusCode(problem.status));
    AppendCborUnsigned(output, 4);
    AppendCborText(output, problem.content_text);
    AppendCborUnsigned(output, 5);
    AppendCborText(output, problem.solution_text);
    AppendCborUnsigned(output, 6);
    AppendCborUnsigned(output, static_cast<uint64_t>(problem.asset_count));
    AppendCborUnsigned(output, 7);
    AppendCborUnsigned(output, static_cast<uint64_t>(problem.solution_asset_count));
    AppendCborUnsigned(output, 8);
    int64_t epoch_seconds = 0;
    if (ParseIsoUtc(problem.updated_at, &epoch_seconds)) {
        AppendCborSigned(output, epoch_seconds);
    } else {
        AppendCborText(output, problem.updated_at);
    }

    return output->size() <= kMaxEncodedRecordBytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

class CborReader {
public:
    CborReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    size_t position() const { return position_; }

    bool ReadHead(uint8_t* major, uint64_t* value)
    {
        if (major == nullptr || value == nullptr || position_ >= size_) {
            return false;
        }
        const uint8_t initial = data_[position_++];
        *major = initial >> 5;
        const uint8_t additional = initial & 0x1F;
        if (additional < 24) {
            *value = additional;
            return true;
        }
        size_t bytes = 0;
        if (additional == 24) bytes = 1;
        else if (additional == 25) bytes = 2;
        else if (additional == 26) bytes = 4;
        else if (additional == 27) bytes = 8;
        else return false;
        if (position_ + bytes > size_) {
            return false;
        }
        uint64_t parsed = 0;
        for (size_t i = 0; i < bytes; ++i) {
            parsed = (parsed << 8) | data_[position_++];
        }
        *value = parsed;
        return true;
    }

    bool ReadUnsigned(uint64_t* value)
    {
        uint8_t major = 0;
        return ReadHead(&major, value) && major == 0;
    }

    bool ReadSigned(int64_t* value)
    {
        uint8_t major = 0;
        uint64_t encoded = 0;
        if (value == nullptr || !ReadHead(&major, &encoded)) {
            return false;
        }
        if (major == 0 && encoded <= static_cast<uint64_t>(INT64_MAX)) {
            *value = static_cast<int64_t>(encoded);
            return true;
        }
        if (major == 1 && encoded <= static_cast<uint64_t>(INT64_MAX)) {
            *value = -1 - static_cast<int64_t>(encoded);
            return true;
        }
        return false;
    }

    bool ReadText(std::string* value)
    {
        uint8_t major = 0;
        uint64_t length = 0;
        if (value == nullptr || !ReadHead(&major, &length) || major != 3 ||
            length > size_ - position_) {
            return false;
        }
        return ReadTextBody(length, value);
    }

    bool ReadBytes(const uint8_t** value, size_t* length)
    {
        uint8_t major = 0;
        uint64_t encoded_length = 0;
        if (value == nullptr || length == nullptr || !ReadHead(&major, &encoded_length) ||
            major != 2 || encoded_length > size_ - position_) {
            return false;
        }
        *length = static_cast<size_t>(encoded_length);
        return ReadBytesBody(*length, value);
    }

    bool ReadTextBody(uint64_t length, std::string* value)
    {
        if (value == nullptr || length > size_ - position_) {
            return false;
        }
        value->assign(
            reinterpret_cast<const char*>(data_ + position_),
            static_cast<size_t>(length));
        position_ += static_cast<size_t>(length);
        return true;
    }

    bool ReadBytesBody(size_t length, const uint8_t** value)
    {
        if (value == nullptr || length > size_ - position_) {
            return false;
        }
        *value = data_ + position_;
        position_ += length;
        return true;
    }

    bool SkipValue()
    {
        uint8_t major = 0;
        uint64_t value = 0;
        if (!ReadHead(&major, &value)) {
            return false;
        }
        if (major == 0 || major == 1 || major == 7) {
            return true;
        }
        if (major == 2 || major == 3) {
            if (value > size_ - position_) return false;
            position_ += static_cast<size_t>(value);
            return true;
        }
        if (major == 4) {
            for (uint64_t i = 0; i < value; ++i) if (!SkipValue()) return false;
            return true;
        }
        if (major == 5) {
            for (uint64_t i = 0; i < value; ++i) {
                if (!SkipValue() || !SkipValue()) return false;
            }
            return true;
        }
        return false;
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t position_ = 0;
};

bool ReadTextOrCode(CborReader* reader, bool status, std::string* value)
{
    if (reader == nullptr || value == nullptr) return false;
    uint8_t major = 0;
    uint64_t encoded = 0;
    if (!reader->ReadHead(&major, &encoded)) return false;
    if (major == 0) {
        *value = status ? ProblemStatusFromCode(encoded) : ProblemTypeFromCode(encoded);
        return !value->empty();
    }
    return major == 3 && encoded <= 32 && reader->ReadTextBody(encoded, value);
}

esp_err_t DecodeProblem(CborReader* reader, wqn::CachedProblem* problem)
{
    if (reader == nullptr || problem == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *problem = {};
    uint8_t major = 0;
    uint64_t field_count = 0;
    if (!reader->ReadHead(&major, &field_count) || major != 5 || field_count > 16) {
        return ESP_ERR_INVALID_STATE;
    }
    for (uint64_t i = 0; i < field_count; ++i) {
        uint64_t key = 0;
        if (!reader->ReadUnsigned(&key)) {
            return ESP_ERR_INVALID_STATE;
        }
        switch (key) {
            case 0: {
                uint8_t id_major = 0;
                uint64_t id_length = 0;
                if (!reader->ReadHead(&id_major, &id_length)) {
                    return ESP_ERR_INVALID_STATE;
                }
                if (id_major == 2 && id_length == 16) {
                    const uint8_t* bytes = nullptr;
                    if (!reader->ReadBytesBody(16, &bytes)) return ESP_ERR_INVALID_STATE;
                    problem->id = FormatUuid(bytes);
                } else if (id_major == 3 && id_length <= 128) {
                    if (!reader->ReadTextBody(id_length, &problem->id)) return ESP_ERR_INVALID_STATE;
                } else {
                    return ESP_ERR_INVALID_STATE;
                }
                break;
            }
            case 1:
                if (!reader->ReadText(&problem->title)) return ESP_ERR_INVALID_STATE;
                break;
            case 2:
                if (!ReadTextOrCode(reader, false, &problem->type)) return ESP_ERR_INVALID_STATE;
                break;
            case 3:
                if (!ReadTextOrCode(reader, true, &problem->status)) return ESP_ERR_INVALID_STATE;
                break;
            case 4:
                if (!reader->ReadText(&problem->content_text)) return ESP_ERR_INVALID_STATE;
                break;
            case 5:
                if (!reader->ReadText(&problem->solution_text)) return ESP_ERR_INVALID_STATE;
                break;
            case 6: {
                uint64_t value = 0;
                if (!reader->ReadUnsigned(&value) || value > INT_MAX) return ESP_ERR_INVALID_STATE;
                problem->asset_count = static_cast<int>(value);
                break;
            }
            case 7: {
                uint64_t value = 0;
                if (!reader->ReadUnsigned(&value) || value > INT_MAX) return ESP_ERR_INVALID_STATE;
                problem->solution_asset_count = static_cast<int>(value);
                break;
            }
            case 8: {
                uint8_t timestamp_major = 0;
                uint64_t timestamp_value = 0;
                if (!reader->ReadHead(&timestamp_major, &timestamp_value)) {
                    return ESP_ERR_INVALID_STATE;
                }
                if (timestamp_major == 0 && timestamp_value <= static_cast<uint64_t>(INT64_MAX)) {
                    problem->updated_at = FormatIsoUtc(static_cast<int64_t>(timestamp_value));
                } else if (timestamp_major == 1 && timestamp_value <= static_cast<uint64_t>(INT64_MAX)) {
                    problem->updated_at = FormatIsoUtc(-1 - static_cast<int64_t>(timestamp_value));
                } else if (timestamp_major == 3 && timestamp_value <= 64) {
                    if (!reader->ReadTextBody(timestamp_value, &problem->updated_at)) {
                        return ESP_ERR_INVALID_STATE;
                    }
                } else {
                    return ESP_ERR_INVALID_STATE;
                }
                break;
            }
            default:
                if (!reader->SkipValue()) return ESP_ERR_INVALID_STATE;
                break;
        }
    }
    return problem->id.empty() || problem->type.empty() || problem->status.empty()
        ? ESP_ERR_INVALID_STATE
        : ESP_OK;
}

struct FileOutputContext {
    FILE* file = nullptr;
    size_t max_file_bytes = 0;
    bool capacity_exceeded = false;
};

mz_bool WriteCompressedOutput(const void* data, int length, void* opaque)
{
    auto* context = static_cast<FileOutputContext*>(opaque);
    if (context == nullptr || context->file == nullptr || length < 0) {
        return MZ_FALSE;
    }
    if (!WriteExact(context->file, data, static_cast<size_t>(length), context->max_file_bytes)) {
        context->capacity_exceeded = errno == ENOSPC;
        return MZ_FALSE;
    }
    return MZ_TRUE;
}

esp_err_t FlushBlock(
    FILE* file,
    size_t max_file_bytes,
    tdefl_compressor* compressor,
    const std::vector<uint8_t>& uncompressed,
    const std::vector<PendingIndexEntry>& pending_index,
    CacheHeader* header,
    std::vector<CacheIndexEntry>* index)
{
    if (file == nullptr || compressor == nullptr || header == nullptr || index == nullptr ||
        uncompressed.empty() || pending_index.empty() ||
        uncompressed.size() > kMaxEncodedRecordBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    const long block_offset_long = std::ftell(file);
    if (block_offset_long < 0 || static_cast<unsigned long>(block_offset_long) > UINT32_MAX) {
        return ESP_FAIL;
    }
    const uint32_t block_offset = static_cast<uint32_t>(block_offset_long);
    uint8_t placeholder[kBlockHeaderSize] = {};
    if (!WriteExact(file, placeholder, sizeof(placeholder), max_file_bytes)) {
        return errno == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    const long payload_offset = std::ftell(file);
    if (payload_offset < 0) return ESP_FAIL;

    FileOutputContext output = {file, max_file_bytes, false};
    if (tdefl_init(compressor, WriteCompressedOutput, &output, kCompressionFlags) != TDEFL_STATUS_OKAY) {
        return ESP_FAIL;
    }
    const tdefl_status status = tdefl_compress_buffer(
        compressor,
        uncompressed.data(),
        uncompressed.size(),
        TDEFL_FINISH);
    if (status != TDEFL_STATUS_DONE) {
        return output.capacity_exceeded ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    const long end_offset = std::ftell(file);
    if (end_offset < payload_offset || static_cast<unsigned long>(end_offset - payload_offset) > UINT32_MAX) {
        return ESP_FAIL;
    }
    const uint32_t compressed_size = static_cast<uint32_t>(end_offset - payload_offset);

    uint8_t block_header[kBlockHeaderSize] = {};
    std::memcpy(block_header, kBlockMagic, sizeof(kBlockMagic));
    PutU32(block_header + 4, static_cast<uint32_t>(uncompressed.size()));
    PutU32(block_header + 8, compressed_size);
    PutU32(block_header + 12, static_cast<uint32_t>(pending_index.size()));
    PutU32(block_header + 16, Crc32(uncompressed.data(), uncompressed.size()));
    if (std::fseek(file, block_offset_long, SEEK_SET) != 0 ||
        !WriteExact(file, block_header, sizeof(block_header), max_file_bytes) ||
        std::fseek(file, end_offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    for (const PendingIndexEntry& pending : pending_index) {
        index->push_back({pending.id_hash, block_offset, pending.record_offset});
    }
    header->uncompressed_bytes += static_cast<uint32_t>(uncompressed.size());
    header->compressed_bytes += compressed_size;
    ++header->block_count;
    return ESP_OK;
}

esp_err_t ReadAndValidateFile(const char* path, std::vector<wqn::CachedProblem>* problems, CacheHeader* parsed_header)
{
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return ESP_FAIL;
    }
    const long file_size_long = std::ftell(file);
    if (file_size_long < static_cast<long>(kHeaderSize) ||
        static_cast<size_t>(file_size_long) > kMaxCacheFileBytes ||
        std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t file_size = static_cast<size_t>(file_size_long);

    uint8_t raw_header[kHeaderSize] = {};
    CacheHeader header;
    if (!ReadExact(file, raw_header, sizeof(raw_header)) || !DecodeHeader(raw_header, &header) ||
        header.index_offset > file_size) {
        std::fclose(file);
        return ESP_ERR_INVALID_CRC;
    }
    if (problems != nullptr) {
        problems->clear();
        problems->reserve(header.record_count);
    }

    uint32_t observed_records = 0;
    uint32_t observed_uncompressed = 0;
    uint32_t observed_compressed = 0;
    for (uint32_t block_index = 0; block_index < header.block_count; ++block_index) {
        const long block_offset = std::ftell(file);
        uint8_t block_header[kBlockHeaderSize] = {};
        if (block_offset < 0 || static_cast<uint32_t>(block_offset) >= header.index_offset ||
            !ReadExact(file, block_header, sizeof(block_header)) ||
            std::memcmp(block_header, kBlockMagic, sizeof(kBlockMagic)) != 0) {
            std::fclose(file);
            return ESP_ERR_INVALID_STATE;
        }
        const uint32_t uncompressed_size = GetU32(block_header + 4);
        const uint32_t compressed_size = GetU32(block_header + 8);
        const uint32_t record_count = GetU32(block_header + 12);
        const uint32_t expected_crc = GetU32(block_header + 16);
        const long compressed_offset = std::ftell(file);
        if (uncompressed_size == 0 || uncompressed_size > kMaxEncodedRecordBytes ||
            compressed_size == 0 || compressed_size > kMaxCompressedBlockBytes ||
            record_count == 0 || record_count > kMaxProblemRecords ||
            compressed_offset < 0 || static_cast<uint64_t>(compressed_offset) + compressed_size > header.index_offset) {
            std::fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }

        std::vector<uint8_t> compressed(compressed_size);
        std::vector<uint8_t> uncompressed(uncompressed_size);
        if (!ReadExact(file, compressed.data(), compressed.size())) {
            std::fclose(file);
            return ESP_FAIL;
        }
        const size_t decoded = tinfl_decompress_mem_to_mem(
            uncompressed.data(),
            uncompressed.size(),
            compressed.data(),
            compressed.size(),
            TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (decoded != uncompressed.size() || Crc32(uncompressed.data(), uncompressed.size()) != expected_crc) {
            std::fclose(file);
            return ESP_ERR_INVALID_CRC;
        }

        CborReader reader(uncompressed.data(), uncompressed.size());
        for (uint32_t record = 0; record < record_count; ++record) {
            wqn::CachedProblem problem;
            const esp_err_t decode_result = DecodeProblem(&reader, &problem);
            if (decode_result != ESP_OK) {
                std::fclose(file);
                return decode_result;
            }
            if (problems != nullptr) {
                problems->push_back(std::move(problem));
            }
        }
        if (reader.position() != uncompressed.size()) {
            std::fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
        observed_records += record_count;
        observed_uncompressed += uncompressed_size;
        observed_compressed += compressed_size;
    }

    if (static_cast<uint32_t>(std::ftell(file)) != header.index_offset ||
        observed_records != header.record_count ||
        observed_uncompressed != header.uncompressed_bytes ||
        observed_compressed != header.compressed_bytes) {
        std::fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t index_size = 8 + static_cast<size_t>(header.record_count) * kIndexEntrySize + 4;
    if (index_size > file_size - header.index_offset || header.index_offset + index_size != file_size) {
        std::fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    std::vector<uint8_t> raw_index(index_size);
    if (!ReadExact(file, raw_index.data(), raw_index.size())) {
        std::fclose(file);
        return ESP_FAIL;
    }
    std::fclose(file);
    if (std::memcmp(raw_index.data(), kIndexMagic, sizeof(kIndexMagic)) != 0 ||
        GetU32(raw_index.data() + 4) != header.record_count ||
        GetU32(raw_index.data() + index_size - 4) != Crc32(raw_index.data(), index_size - 4)) {
        return ESP_ERR_INVALID_CRC;
    }
    for (uint32_t i = 0; i < header.record_count; ++i) {
        const uint8_t* entry = raw_index.data() + 8 + static_cast<size_t>(i) * kIndexEntrySize;
        const uint32_t block_offset = GetU32(entry + 8);
        const uint32_t record_offset = GetU32(entry + 12);
        if (block_offset < kHeaderSize || block_offset >= header.index_offset ||
            record_offset >= kMaxEncodedRecordBytes) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (parsed_header != nullptr) {
        *parsed_header = header;
    }
    return ESP_OK;
}

uint64_t NextGeneration()
{
    CacheHeader header;
    if (ReadAndValidateFile(kPrimaryPath, nullptr, &header) == ESP_OK) {
        return header.generation == UINT64_MAX ? 1 : header.generation + 1;
    }
    return 1;
}

}  // namespace

namespace wqn::problem_cache {

esp_err_t Save(const std::vector<CachedProblem>& problems)
{
    if (problems.size() > kMaxProblemRecords) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(RemoveIfPresent(kTempPath), kTag, "remove stale problem temp");
    ESP_RETURN_ON_ERROR(RemoveIfPresent(kBackupPath), kTag, "remove stale problem backup");

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(
        esp_spiffs_info(kPartitionLabel, &total, &used),
        kTag,
        "read SPIFFS capacity");
    const size_t available = total > used ? total - used : 0;
    if (available <= kSpiffsSafetyReserveBytes + kHeaderSize) {
        ESP_LOGW(kTag, "problem cache storage-full: available=%u reserve=%u",
            static_cast<unsigned>(available),
            static_cast<unsigned>(kSpiffsSafetyReserveBytes));
        return ESP_ERR_NO_MEM;
    }
    const size_t max_file_bytes = std::min(
        kMaxCacheFileBytes,
        available - kSpiffsSafetyReserveBytes);

    FILE* file = std::fopen(kTempPath, "wb+");
    if (file == nullptr) {
        return errno == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    uint8_t empty_header[kHeaderSize] = {};
    esp_err_t result = WriteExact(file, empty_header, sizeof(empty_header), max_file_bytes)
        ? ESP_OK
        : (errno == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL);

    auto* compressor = static_cast<tdefl_compressor*>(heap_caps_calloc(
        1,
        sizeof(tdefl_compressor),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (compressor == nullptr) {
        compressor = static_cast<tdefl_compressor*>(heap_caps_calloc(
            1,
            sizeof(tdefl_compressor),
            MALLOC_CAP_8BIT));
    }
    if (result == ESP_OK && compressor == nullptr) {
        result = ESP_ERR_NO_MEM;
    }

    CacheHeader header;
    header.generation = NextGeneration();
    header.record_count = static_cast<uint32_t>(problems.size());
    std::vector<CacheIndexEntry> index;
    index.reserve(problems.size());
    std::vector<uint8_t> block;
    block.reserve(kBlockTargetBytes);
    std::vector<PendingIndexEntry> pending_index;
    pending_index.reserve(32);
    std::vector<uint8_t> encoded;

    for (const CachedProblem& problem : problems) {
        if (result != ESP_OK) break;
        result = EncodeProblem(problem, &encoded);
        if (result != ESP_OK) break;
        if (!block.empty() && block.size() + encoded.size() > kBlockTargetBytes) {
            result = FlushBlock(
                file, max_file_bytes, compressor, block, pending_index, &header, &index);
            block.clear();
            pending_index.clear();
            if (result != ESP_OK) break;
        }
        if (header.uncompressed_bytes + block.size() + encoded.size() > kMaxUncompressedCacheBytes) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        pending_index.push_back({HashProblemId(problem.id), static_cast<uint32_t>(block.size())});
        block.insert(block.end(), encoded.begin(), encoded.end());
    }
    if (result == ESP_OK && !block.empty()) {
        result = FlushBlock(
            file, max_file_bytes, compressor, block, pending_index, &header, &index);
    }
    heap_caps_free(compressor);

    if (result == ESP_OK && index.size() != problems.size()) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) {
        const long index_offset = std::ftell(file);
        if (index_offset < 0 || static_cast<unsigned long>(index_offset) > UINT32_MAX) {
            result = ESP_FAIL;
        } else {
            header.index_offset = static_cast<uint32_t>(index_offset);
            std::vector<uint8_t> raw_index(8 + index.size() * kIndexEntrySize + 4, 0);
            std::memcpy(raw_index.data(), kIndexMagic, sizeof(kIndexMagic));
            PutU32(raw_index.data() + 4, static_cast<uint32_t>(index.size()));
            for (size_t i = 0; i < index.size(); ++i) {
                uint8_t* entry = raw_index.data() + 8 + i * kIndexEntrySize;
                PutU64(entry, index[i].id_hash);
                PutU32(entry + 8, index[i].block_offset);
                PutU32(entry + 12, index[i].record_offset);
            }
            PutU32(
                raw_index.data() + raw_index.size() - 4,
                Crc32(raw_index.data(), raw_index.size() - 4));
            if (!WriteExact(file, raw_index.data(), raw_index.size(), max_file_bytes)) {
                result = errno == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
            }
        }
    }
    if (result == ESP_OK) {
        uint8_t raw_header[kHeaderSize] = {};
        EncodeHeader(header, raw_header);
        if (std::fseek(file, 0, SEEK_SET) != 0 ||
            !WriteExact(file, raw_header, sizeof(raw_header), max_file_bytes) ||
            std::fflush(file) != 0 || ::fsync(fileno(file)) != 0) {
            result = errno == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
        }
    }
    if (std::fclose(file) != 0 && result == ESP_OK) {
        result = ESP_FAIL;
    }
    if (result != ESP_OK) {
        std::remove(kTempPath);
        ESP_LOGW(kTag, "problem cache write failed: error=%s", esp_err_to_name(result));
        return result;
    }

    ESP_RETURN_ON_ERROR(
        ReadAndValidateFile(kTempPath, nullptr, nullptr),
        kTag,
        "validate problem cache temp");
    const bool had_primary = FileExists(kPrimaryPath);
    if (had_primary && std::rename(kPrimaryPath, kBackupPath) != 0) {
        std::remove(kTempPath);
        return ESP_FAIL;
    }
    if (std::rename(kTempPath, kPrimaryPath) != 0) {
        if (had_primary && std::rename(kBackupPath, kPrimaryPath) != 0) {
            ESP_LOGE(kTag, "problem cache rollback failed; backup remains at %s", kBackupPath);
        }
        std::remove(kTempPath);
        return ESP_FAIL;
    }

    const unsigned ratio = header.uncompressed_bytes == 0
        ? 100
        : static_cast<unsigned>((static_cast<uint64_t>(header.compressed_bytes) * 100) /
            header.uncompressed_bytes);
    ESP_LOGI(
        kTag,
        "problem cache committed: generation=%llu records=%u raw=%u compressed=%u ratio=%u%% blocks=%u",
        static_cast<unsigned long long>(header.generation),
        static_cast<unsigned>(header.record_count),
        static_cast<unsigned>(header.uncompressed_bytes),
        static_cast<unsigned>(header.compressed_bytes),
        ratio,
        static_cast<unsigned>(header.block_count));
    return ESP_OK;
}

esp_err_t Load(std::vector<CachedProblem>* problems)
{
    if (problems == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ReadAndValidateFile(kPrimaryPath, problems, nullptr);
    if (result == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGW(
        kTag,
        "primary problem cache unavailable: %s; trying backup",
        esp_err_to_name(result));
    const esp_err_t backup_result = ReadAndValidateFile(kBackupPath, problems, nullptr);
    if (backup_result == ESP_OK) {
        ESP_LOGW(kTag, "problem cache recovered from backup");
        return ESP_OK;
    }
    problems->clear();
    return result == ESP_ERR_NOT_FOUND && backup_result == ESP_ERR_NOT_FOUND
        ? ESP_ERR_NOT_FOUND
        : result;
}

esp_err_t Clear()
{
    esp_err_t result = RemoveIfPresent(kTempPath);
    const esp_err_t primary_result = RemoveIfPresent(kPrimaryPath);
    const esp_err_t backup_result = RemoveIfPresent(kBackupPath);
    if (result == ESP_OK) result = primary_result;
    if (result == ESP_OK) result = backup_result;
    return result;
}

bool Exists()
{
    return FileExists(kPrimaryPath) || FileExists(kBackupPath);
}

}  // namespace wqn::problem_cache
