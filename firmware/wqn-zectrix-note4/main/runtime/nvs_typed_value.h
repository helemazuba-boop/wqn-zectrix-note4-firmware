#pragma once

#include "nvs.h"

namespace wqn::runtime {

// Retains the caller/stack requirements of the existing storage/bootstrap read;
// this probe does not acquire ownership or make two SDK calls atomic.
// ESP-IDF's typed lookup can fold TYPE_MISMATCH into NOT_FOUND. Establish actual
// presence/type first; a later typed-read NOT_FOUND remains an error, not new
// permission to use defaults, migrate legacy data or overwrite unknown bytes.
inline esp_err_t ReadNvsTypedPresence(
    nvs_handle_t handle, const char* key, nvs_type_t expected, bool* found)
{
    if (key == nullptr || found == nullptr) return ESP_ERR_INVALID_ARG;
    *found = false;
    nvs_type_t observed = NVS_TYPE_ANY;
    const esp_err_t result = nvs_find_key(handle, key, &observed);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    if (observed != expected) return ESP_ERR_NVS_TYPE_MISMATCH;
    *found = true;
    return ESP_OK;
}

}  // namespace wqn::runtime
