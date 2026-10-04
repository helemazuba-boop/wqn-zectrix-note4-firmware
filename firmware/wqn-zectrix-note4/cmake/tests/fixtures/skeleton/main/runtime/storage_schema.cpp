// M8 gate fixture: the second file on the NVS-write allowlist.
#include "nvs.h"
namespace wqn { int SchemaFixture(nvs_handle_t h) { return nvs_erase_key(h, "k"); } }
