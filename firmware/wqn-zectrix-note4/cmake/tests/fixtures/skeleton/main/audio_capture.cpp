// M8 gate fixture: minimal stand-in for the real audio_capture.cpp. The architecture gate
// reads every file it enumerates, so this tree must contain one stub per entry.
namespace wqn { void audio_capture_fixture_marker() {} }
