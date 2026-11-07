#pragma once

#include "esp_err.h"

namespace device_ui_internal {
// Boot-only UI loop. Returns on input initialization failure; otherwise keeps
// business startup blocked until an explicit whole-device restart.
esp_err_t RunStorageRecoveryUi(esp_err_t boot_error, bool preservation_only = false);
}  // namespace device_ui_internal
