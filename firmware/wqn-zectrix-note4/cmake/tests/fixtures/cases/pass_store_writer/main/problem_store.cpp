// M8 gate fixture: a store-layer file doing exactly what the real ones do. The
// seven SPIFFS-WRITER owners are legal to hold these primitives; this file is
// one of them and is NOT a declared writable Load, so it carries no annotation.
#include <cstdio>
namespace wqn { int StoreWriterFixture(const char* p) {
    FILE* f = std::fopen(p, "wb");
    if (f != nullptr) { std::fclose(f); }
    std::rename("a", "b");
    if (std::remove("/tmp/x") != 0 && errno != ENOENT) { return 1; }
    return 0;
} }
