// M8 gate fixture: a feature-layer file using an NVS primitive directly. The
// handle arrives from a service interface, so the include rule is NOT what
// should trip here -- the primitive itself is the violation. The typedef keeps
// the fixture self-contained: nothing here is compiled, the gate reads text.
typedef void* nvs_handle_t;
namespace wqn { int WordAppNvsFixture(nvs_handle_t h) { return nvs_set_u32(h, "k", 1); } }
