#!/bin/sh
# Tier L1 of doc/1005-opencode-bidi-gap-plan.md: the host matrix for
# main/agent_round_policy.h.
#
# It needs nothing but a C++17 compiler -- the policy header pulls in no
# ESP-IDF, which is the whole reason it is a separate file. Run it after any
# change to the header or to the frame branches in opencode_session.cpp.
#
#   sh scripts/run_agent_round_policy_test.sh
#
# Exit 0 means every check passed; the failure lines name the assertion and the
# line in the test file.

set -e

here=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/agent_round_policy_test

CXX=${CXX:-c++}
"$CXX" -std=c++17 -Wall -Wextra -I"$here/main" -o "$out" \
    "$here/test/agent_round_policy_test.cpp"

"$out"
rm -f "$out"
