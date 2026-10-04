// M8 gate fixture: the persist worker is the ONE UI-layer file allowed to talk
// to the storage service. Containing the call is what lets the STORAGE-ENTRYPOINT
// whitelist be exercised by a fixture rather than by inspection.
#include "storage_service_fixture.h"
namespace wqn::services { int RunPersistWorkerFixture() { return ExecuteStorageTransaction(nullptr, nullptr); } }
