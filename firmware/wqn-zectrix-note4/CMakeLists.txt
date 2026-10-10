cmake_minimum_required(VERSION 3.16)

# [release] The version comes from the release tooling (tools/release/release.py),
# which derives it from a content fingerprint rather than a commit or tag.
# WQN_RELEASE_VERSION is exported by the deploy/release entry points; a build
# without it is a local dev build. Read from the environment (not a cache
# variable) so a stale cache entry can never label a dev build as a release.
if(DEFINED ENV{WQN_RELEASE_VERSION} AND NOT "$ENV{WQN_RELEASE_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{WQN_RELEASE_VERSION}" CACHE STRING "Firmware version" FORCE)
else()
    set(PROJECT_VER "0.1.0-dev" CACHE STRING "Firmware version" FORCE)
endif()

# [power-fix] Layer the ai-local profile on top of the default sdkconfig
# values. The ai-local.defaults file is the place where we collect the
# overrides that are specific to the in-house AI build (WiFi STA + AI
# modules + power-management config). Without listing it here idf.py
# would only pick up sdkconfig.defaults and silently drop the PM /
# deep-sleep overrides that turn the device from a 100mA heater into
# a proper e-paper note.
list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults" "sdkconfig.ai-local.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wqn-zectrix-note4)

# [ui-stack] Check compiled entry frames too: lexical scoping of a 4 KiB
# UiFrame did not prevent the 1009.4 overflow. Disabled UI builds have no
# DeviceUiTask symbol, so only the enabled S3 profile runs this gate.
if(CONFIG_WQN_EPD_UI_ENABLE AND IDF_TARGET STREQUAL "esp32s3")
    add_custom_target(
        wqn_ui_stack_check ALL
        COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/scripts/check_ui_stack.py
            $<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf> --objdump ${CMAKE_OBJDUMP}
        DEPENDS ${CMAKE_PROJECT_NAME}.elf
        COMMENT "Verifying compiled UI stack frame budgets (not HIL)"
        VERBATIM)
endif()

add_custom_target(
    wqn_architecture_check ALL
    COMMAND
        ${CMAKE_COMMAND}
        -DWQN_PROJECT_DIR=${CMAKE_CURRENT_LIST_DIR}
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/verify_architecture.cmake
    COMMENT "Verifying M8 resource ownership and layer boundaries"
    VERBATIM)

# Fixture tests for the architecture gate itself. Every rule above is
# load-bearing and until now had no test of its own, and the gate is the only
# automated protection against a new single-owner or layering violation.
#
# Deliberately not part of ALL: a fresh clone should not be surprised by a
# second cmake invocation on every build. Run it with the target below, or with
# ctest. The same `cmake -P` script is the entry point if CI ever appears.
enable_testing()
add_test(NAME m8_gate_fixtures
    COMMAND ${CMAKE_COMMAND}
        -DQN_REPO_DIR=${CMAKE_CURRENT_LIST_DIR}
        -DQN_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/tests/run_gate_fixture.cmake)
add_custom_target(
    wqn_m8_gate_tests
    COMMAND ${CMAKE_COMMAND}
        -DQN_REPO_DIR=${CMAKE_CURRENT_LIST_DIR}
        -DQN_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/tests/run_gate_fixture.cmake
    COMMENT "Running M8 gate fixture tests"
    VERBATIM)
