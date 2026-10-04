// M8 gate fixture: a screen writing a cache file straight to SPIFFS.
#include <cstdio>
namespace wqn { void RenderHomePageFixture() { FILE* f = std::fopen("/cache/x", "wb"); (void)f; } }
