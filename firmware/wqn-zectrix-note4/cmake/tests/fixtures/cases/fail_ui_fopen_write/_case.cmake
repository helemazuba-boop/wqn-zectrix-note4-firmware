# A feature-layer file opening a file for writing. Before the SPIFFS-WRITER rule
# this exited 0; it is recorded here as the documented blind spot the rule closes.
set(WQN_EXPECT fail)
set(WQN_EXPECT_ERROR "rule SPIFFS-WRITER: fopen with a writing mode")
