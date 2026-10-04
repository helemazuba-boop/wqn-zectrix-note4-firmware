# A declared writable Load whose annotation was removed -- for instance by a
# refactor that "cleaned up" the comment block. The gate must notice that the
# read/write split has not actually landed.
set(WQN_EXPECT fail)
set(WQN_EXPECT_ERROR "rule LOAD-REPAIR")
