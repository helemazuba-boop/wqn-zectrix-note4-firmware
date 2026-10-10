// M8 gate fixture: a cloud lane. Lanes are legitimate owners of durable session
// writes (persist_worker.h's contract), so they must stay on the whitelist when
// the feature-layer rule lands.
namespace wqn { void SavePersistedWordSessionFixture(); }
namespace wqn { int WordCloudLaneFixture() { SavePersistedWordSessionFixture(); return 0; } }
