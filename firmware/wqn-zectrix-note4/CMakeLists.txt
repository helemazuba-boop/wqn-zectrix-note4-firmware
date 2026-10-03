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

add_custom_target(
    wqn_architecture_check ALL
    COMMAND
        ${CMAKE_COMMAND}
        -DWQN_PROJECT_DIR=${CMAKE_CURRENT_LIST_DIR}
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/verify_architecture.cmake
    COMMENT "Verifying M8 resource ownership and layer boundaries"
    VERBATIM)
