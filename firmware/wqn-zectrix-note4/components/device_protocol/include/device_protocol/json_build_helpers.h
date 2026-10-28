#pragma once

#include "cJSON.h"

namespace wqn::protocol {

// cJSON takes ownership only on successful attachment. Keep the unattached
// string local so an allocation/attachment failure cannot orphan it.
inline bool AppendJsonString(cJSON* array, const char* value)
{
    cJSON* item = cJSON_CreateString(value);
    if (item == nullptr) return false;
    if (!cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

}  // namespace wqn::protocol
