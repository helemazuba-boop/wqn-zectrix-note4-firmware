#pragma once
#include "esp_err.h"
namespace wqn::runtime {
// Before NVS/schema/mount or business tasks, with safe pins already configured.
// A page-up held continuously for 3 s requests preservation. Normal boots have
// no polling window; deep-sleep resumes ignore this gesture. No GPIO0 chord.
// A sampling/clock error must also block normal storage startup at the caller.
esp_err_t CheckStoragePreservationBoot(bool* requested);
}
