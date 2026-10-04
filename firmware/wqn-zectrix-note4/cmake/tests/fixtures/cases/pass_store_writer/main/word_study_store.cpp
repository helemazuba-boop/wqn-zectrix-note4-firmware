// M8 gate fixture: a store-layer file doing exactly what the real ones do.
#include <cstdio>
namespace wqn { int StoreWriterFixture(const char* p) {
    FILE* f = std::fopen(p, "wb");
    if (f != nullptr) { std::fclose(f); }
    std::rename("a", "b");
    return 0;
} }
