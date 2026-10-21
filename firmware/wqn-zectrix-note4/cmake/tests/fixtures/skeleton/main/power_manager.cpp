// M8 gate fixture: the only file allowed to contain the deep-sleep entry.
namespace wqn { void PowerCoordinatorFixture() { esp_deep_sleep_start(); } }
