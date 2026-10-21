// M8 gate fixture: on the NVS-write allowlist, so it may hold the primitives.
#include "nvs.h"
namespace wqn { int StorageFixture(nvs_handle_t h) { nvs_set_u32(h, "k", 1); return nvs_commit(h); } }
