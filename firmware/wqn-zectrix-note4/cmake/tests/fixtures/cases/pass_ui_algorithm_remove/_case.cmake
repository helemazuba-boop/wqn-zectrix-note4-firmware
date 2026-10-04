# The <algorithm> erase-remove idiom shares std::remove's name. The SPIFFS-WRITER
# pattern is constrained to a path-looking argument so this stays legal; a future
# edit that loosens it turns this fixture red.
set(WQN_EXPECT pass)
