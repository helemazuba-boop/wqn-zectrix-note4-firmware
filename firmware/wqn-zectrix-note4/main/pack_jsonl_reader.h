#pragma once

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "esp_err.h"

namespace wqn {

// Pack files are binary, seekable streams (index offsets already require
// ftell/fseek). fgets can read embedded zero bytes; strlen cannot count them.
// Use the actual consumed span before accepting framing or constructing text.
inline esp_err_t ReadBoundedPackTextLine(
    FILE* file, std::vector<char>* buffer, std::string* line,
    size_t max_content_bytes, bool allow_eof_termination)
{
    if (file == nullptr || buffer == nullptr || line == nullptr ||
        buffer->size() < 2 || buffer->size() > std::numeric_limits<int>::max()) {
        return ESP_ERR_INVALID_ARG;
    }
    line->clear();
    const long start = std::ftell(file);
    if (start < 0) return ESP_FAIL;
    if (std::fgets(buffer->data(), static_cast<int>(buffer->size()), file) == nullptr) {
        return std::ferror(file) == 0 && std::feof(file) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    const long end = std::ftell(file);
    if (std::ferror(file) != 0 || end < start) return ESP_FAIL;
    const auto length = static_cast<size_t>(end - start);
    if (length == 0 || length >= buffer->size()) return ESP_FAIL;
    if (std::memchr(buffer->data(), '\0', length) != nullptr) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const bool has_lf = (*buffer)[length - 1] == '\n';
    if (!has_lf && (!allow_eof_termination || std::feof(file) == 0)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t content_length = has_lf ? length - 1 : length;
    if (content_length > 0 && (*buffer)[content_length - 1] == '\r') --content_length;
    if (content_length > max_content_bytes) return ESP_ERR_INVALID_SIZE;
    line->assign(buffer->data(), content_length);
    return ESP_OK;
}

}  // namespace wqn
