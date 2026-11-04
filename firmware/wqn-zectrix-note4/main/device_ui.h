#pragma once

#include "esp_err.h"

namespace wqn {

esp_err_t StartDeviceUiIfEnabled();

// Boot recovery starts display, input and the maintenance storage owner only.
// Diagnostics/retry/explicit reset run without business services. The UI loop
// returns on initialization failure; otherwise only an explicit reboot exits.
esp_err_t ShowStorageRecoveryUi(esp_err_t storage_error);

}  // namespace wqn
