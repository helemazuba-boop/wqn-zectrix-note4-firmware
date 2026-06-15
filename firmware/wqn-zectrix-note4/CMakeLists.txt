cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.0")

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
