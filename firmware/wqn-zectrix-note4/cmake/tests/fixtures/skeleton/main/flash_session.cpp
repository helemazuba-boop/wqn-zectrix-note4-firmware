// M8 gate fixture: minimal stand-in for the real flash_session.cpp. The architecture gate
// reads every file it enumerates, so this tree must contain one stub per entry.
namespace wqn { void flash_session_fixture_marker() {} }
