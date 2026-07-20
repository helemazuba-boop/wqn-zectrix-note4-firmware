#pragma once

#include "esp_err.h"

namespace wqn::internal {

// Pure helpers kept separate from the ESP HTTP event plumbing so the boot
// contract self-test can lock down error propagation without opening a socket.
const char* AiStreamHttpErrorCode(int http_status);
esp_err_t FinalizeAiStreamResult(bool fatal_http_status, esp_err_t transport_result);

}  // namespace wqn::internal
