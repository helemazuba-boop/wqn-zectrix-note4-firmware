#pragma once

#include <string>

#include "esp_err.h"

namespace wqn {

esp_err_t InitStorage();
esp_err_t LoadAccessToken(std::string* token);
esp_err_t SaveAccessToken(const std::string& token);
esp_err_t ClearAccessToken();
std::string MaskTokenForLog(const std::string& token);

}  // namespace wqn
