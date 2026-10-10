cmake_minimum_required(VERSION 3.16)

# Fixture tests for the M8 architecture gate itself.
#
# The gate is the only automated protection this firmware has against a new
# single-owner or layering violation, and until this file existed it had no test
# of its own: every rule below was load-bearing and unverified. Each case builds
# a synthetic tree from the skeleton plus an overlay, runs the REAL gate against
# it via `cmake -P`, and asserts both the exit status and the rule's own reason
# text -- so a regression in message quality fails a fixture here.
#
# Usage (also the CI entry point if CI ever appears):
#   cmake -DQN_REPO_DIR=<repo> -DQN_BUILD_DIR=<build> -P cmake/tests/run_gate_fixture.cmake
#
# Notes proven while writing this:
#  * message(FATAL_ERROR) lands on stderr, so both streams must be captured.
#  * multi-line FATAL_ERROR text will not MATCH a substring until newlines are
#    normalised, so flatten before comparing.
#  * file(COPY) is copy_if_different and never removes a stale file, so the
#    working tree is torn down before each case.

if(NOT DEFINED QN_REPO_DIR OR NOT DEFINED QN_BUILD_DIR)
    message(FATAL_ERROR "QN_REPO_DIR and QN_BUILD_DIR are required")
endif()

set(QN_FIXTURE_ROOT "${QN_REPO_DIR}/cmake/tests/fixtures")
set(QN_SKELETON "${QN_FIXTURE_ROOT}/skeleton")
set(QN_CASES_DIR "${QN_FIXTURE_ROOT}/cases")
set(QN_TREE_ROOT "${QN_BUILD_DIR}/m8_fixtures")
set(QN_GATE "${QN_REPO_DIR}/cmake/verify_architecture.cmake")

if(NOT EXISTS "${QN_GATE}")
    message(FATAL_ERROR "architecture gate not found at ${QN_GATE}")
endif()

file(MAKE_DIRECTORY "${QN_TREE_ROOT}")

file(GLOB QN_CASE_ENTRIES LIST_DIRECTORIES true "${QN_CASES_DIR}/*")
set(QN_CASES "")
foreach(QN_ENTRY IN LISTS QN_CASE_ENTRIES)
    if(IS_DIRECTORY "${QN_ENTRY}")
        list(APPEND QN_CASES "${QN_ENTRY}")
    endif()
endforeach()
list(SORT QN_CASES)
list(LENGTH QN_CASES QN_CASE_COUNT)
if(QN_CASE_COUNT EQUAL 0)
    message(FATAL_ERROR "no gate fixture cases found under ${QN_CASES_DIR}")
endif()

set(QN_FAILURES "")
set(QN_PASSED 0)

foreach(QN_CASE_DIR IN LISTS QN_CASES)
    get_filename_component(QN_CASE_NAME "${QN_CASE_DIR}" NAME)
    set(QN_TREE "${QN_TREE_ROOT}/${QN_CASE_NAME}")

    # 1. Fresh skeleton. file(COPY) is copy_if_different and leaves stale files
    #    behind, and a leftover file from the previous case is exactly the kind
    #    of thing that would make a case pass for the wrong reason.
    file(REMOVE_RECURSE "${QN_TREE}")
    file(COPY "${QN_SKELETON}/" DESTINATION "${QN_TREE}")

    # 2. Overlay this case's files, preserving their relative path so a case can
    #    add a new file the skeleton does not have. Recurse: overlays may live at
    #    the same depth as the real file they replace. Directories are excluded
    #    so only files are copied.
    file(GLOB_RECURSE QN_OVERLAYS LIST_DIRECTORIES false "${QN_CASE_DIR}/*")
    foreach(QN_OVERLAY IN LISTS QN_OVERLAYS)
        get_filename_component(QN_OVERLAY_NAME "${QN_OVERLAY}" NAME)
        if(NOT QN_OVERLAY_NAME STREQUAL "_case.cmake")
            file(RELATIVE_PATH QN_REL "${QN_CASE_DIR}" "${QN_OVERLAY}")
            get_filename_component(QN_DEST_DIR "${QN_TREE}/${QN_REL}" DIRECTORY)
            file(MAKE_DIRECTORY "${QN_DEST_DIR}")
            file(COPY_FILE "${QN_OVERLAY}" "${QN_TREE}/${QN_REL}")
        endif()
    endforeach()

    # 3. Expectations, reset per case: include() shares this scope, so a value
    #    set by an earlier case would otherwise leak into the next one.
    set(WQN_EXPECT "pass")
    set(WQN_EXPECT_ERROR "")
    include("${QN_CASE_DIR}/_case.cmake")
    if(NOT WQN_EXPECT MATCHES "^(pass|fail)$")
        message(FATAL_ERROR
            "case ${QN_CASE_NAME}: WQN_EXPECT must be pass or fail, got '${WQN_EXPECT}'")
    endif()

    # 4. Run the real gate against the synthetic tree.
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DWQN_PROJECT_DIR=${QN_TREE}"
                -P "${QN_GATE}"
        OUTPUT_VARIABLE QN_GATE_OUT
        ERROR_VARIABLE QN_GATE_ERR
        RESULT_VARIABLE QN_GATE_RC)

    # 5. Flatten: a multi-line FATAL_ERROR message never MATCHES a substring.
    string(REGEX REPLACE "[\r\n\t ]+" " " QN_FLAT "${QN_GATE_OUT} ${QN_GATE_ERR}")

    set(QN_OK TRUE)
    set(QN_WHY "")
    if(WQN_EXPECT STREQUAL "pass")
        if(NOT QN_GATE_RC EQUAL 0)
            set(QN_OK FALSE)
            set(QN_WHY "expected rc=0, got ${QN_GATE_RC}: ${QN_FLAT}")
        endif()
    else()
        if(QN_GATE_RC EQUAL 0)
            set(QN_OK FALSE)
            set(QN_WHY "expected the gate to fail, but it passed")
        elseif(NOT QN_FLAT MATCHES "${WQN_EXPECT_ERROR}")
            set(QN_OK FALSE)
            set(QN_WHY "failed for the wrong reason; wanted '${WQN_EXPECT_ERROR}', got: ${QN_FLAT}")
        endif()
    endif()

    if(QN_OK)
        math(EXPR QN_PASSED "${QN_PASSED} + 1")
        message(STATUS "m8 gate fixture ${QN_CASE_NAME}: ok")
    else()
        list(APPEND QN_FAILURES "${QN_CASE_NAME}: ${QN_WHY}")
        message(STATUS "m8 gate fixture ${QN_CASE_NAME}: FAILED")
    endif()
endforeach()

list(LENGTH QN_FAILURES QN_FAILED)
message(STATUS "m8 gate fixtures: ${QN_PASSED} passed, ${QN_FAILED} failed")
if(QN_FAILURES)
    string(REPLACE ";" "\n  " QN_FAILURE_TEXT "${QN_FAILURES}")
    message(FATAL_ERROR "m8 gate fixture failures:\n  ${QN_FAILURE_TEXT}")
endif()
