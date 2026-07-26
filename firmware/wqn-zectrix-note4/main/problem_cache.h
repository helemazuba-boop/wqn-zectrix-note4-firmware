#pragma once

#include <vector>

#include "esp_err.h"

namespace wqn {

struct CachedProblem;

namespace problem_cache {

// WQPC v1 is the durable, compressed SPIFFS representation of the problem
// content cache. These raw functions do not acquire a SleepLease or dispatch
// through StorageService; callers must execute mutations inside a storage
// transaction.
esp_err_t Save(const std::vector<CachedProblem>& problems);
esp_err_t Load(std::vector<CachedProblem>* problems);
esp_err_t Clear();
bool Exists();

}  // namespace problem_cache
}  // namespace wqn
