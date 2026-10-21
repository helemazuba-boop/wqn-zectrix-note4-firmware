// M8 gate fixture: a screen removing a file straight from SPIFFS.
#include <unistd.h>
namespace wqn { void RenderHomePageFixture() { unlink("/cache/x"); } }
