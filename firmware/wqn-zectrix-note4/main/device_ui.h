#pragma once

#include "esp_err.h"

namespace wqn {

esp_err_t StartDeviceUiIfEnabled();

// M7 boot-recovery surface. Starts only the display pipeline, presents a
// terminal storage failure, and powers the panel down after it is committed.
// No input, cloud, audio, storage, or power-coordinator task is started.
esp_err_t ShowStorageRecoveryUi(esp_err_t storage_error);

}  // namespace wqn
