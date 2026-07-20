cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED WQN_PROJECT_DIR)
    message(FATAL_ERROR "WQN_PROJECT_DIR is required")
endif()

function(wqn_read relative_path output)
    set(path "${WQN_PROJECT_DIR}/${relative_path}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "M8 architecture gate: missing ${relative_path}")
    endif()
    file(READ "${path}" contents)
    set(${output} "${contents}" PARENT_SCOPE)
endfunction()

function(wqn_reject relative_path pattern reason)
    wqn_read("${relative_path}" contents)
    if(contents MATCHES "${pattern}")
        message(FATAL_ERROR
            "M8 architecture gate: ${relative_path}: ${reason}")
    endif()
endfunction()

set(feature_sources
    main/ai_session.cpp
    main/ai_session.h
    main/audio_capture.cpp
    main/audio_capture.h
    main/audio_player.cpp
    main/audio_player.h
    main/audio_selftest.cpp
    main/audio_selftest.h
    main/audio_volume.cpp
    main/audio_volume.h
    main/device_ui.cpp
    main/device_ui.h
    main/flash_session.cpp
    main/flash_session.h
    main/time_app.cpp
    main/time_app.h
    main/ui_model.cpp
    main/ui_model.h
    main/word_app.cpp
    main/word_app.h)
file(GLOB ui_sources
    RELATIVE "${WQN_PROJECT_DIR}"
    "${WQN_PROJECT_DIR}/main/ui/*.cpp"
    "${WQN_PROJECT_DIR}/main/ui/*.h")
list(APPEND feature_sources ${ui_sources})

set(forbidden_feature_include
    "#[ \t]*include[ \t]*[<\"](driver/|esp_adc/|esp_wifi\\.h|esp_sleep\\.h|nvs\\.h|nvs_flash\\.h|esp_spiffs\\.h|board_zectrix_note4\\.h)")
foreach(source IN LISTS feature_sources)
    wqn_reject(
        "${source}"
        "${forbidden_feature_include}"
        "feature code must depend on service interfaces, not ESP-IDF drivers or Note4 HAL")
endforeach()

file(GLOB_RECURSE firmware_sources
    RELATIVE "${WQN_PROJECT_DIR}"
    "${WQN_PROJECT_DIR}/main/*.c"
    "${WQN_PROJECT_DIR}/main/*.cpp"
    "${WQN_PROJECT_DIR}/main/*.h"
    "${WQN_PROJECT_DIR}/components/*.c"
    "${WQN_PROJECT_DIR}/components/*.cpp"
    "${WQN_PROJECT_DIR}/components/*.h")

set(deep_sleep_call_count 0)
foreach(source IN LISTS firmware_sources)
    wqn_read("${source}" contents)

    if(source MATCHES "^components/(platform_note4|power_runtime|display_service|device_protocol)/" AND
       contents MATCHES "device_ui_internal|#[ \t]*include[ \t]*[<\"](storage|power_manager|device_ui|services/|ui/)")
        message(FATAL_ERROR
            "M8 architecture gate: ${source}: extracted component depends back on main/features")
    endif()

    string(REGEX MATCHALL "esp_deep_sleep_start[ \t\r\n]*\\(" calls "${contents}")
    list(LENGTH calls source_call_count)
    if(source_call_count GREATER 0 AND NOT source STREQUAL "main/power_manager.cpp")
        message(FATAL_ERROR
            "M8 architecture gate: ${source}: only PowerCoordinator may enter deep sleep")
    endif()
    math(EXPR deep_sleep_call_count "${deep_sleep_call_count} + ${source_call_count}")
endforeach()
if(NOT deep_sleep_call_count EQUAL 1)
    message(FATAL_ERROR
        "M8 architecture gate: expected exactly one deep-sleep call, found ${deep_sleep_call_count}")
endif()

foreach(source IN LISTS firmware_sources)
    wqn_read("${source}" contents)
    if(contents MATCHES "esp_wifi_[A-Za-z0-9_]+[ \t\r\n]*\\(" AND
       NOT source STREQUAL "main/wifi_manager.cpp" AND
       NOT source STREQUAL "components/wqn_wifi_provision/wifi_provision_portal.cpp")
        message(FATAL_ERROR
            "M8 architecture gate: ${source}: Wi-Fi driver access bypasses ConnectivityService adapter")
    endif()

    if((contents MATCHES "#[ \t]*include[ \t]*[<\"]driver/spi_master\\.h" OR
        contents MATCHES "GPIO_NUM_6([^0-9]|$)") AND
       NOT source MATCHES "^components/display_service/" AND
       NOT source STREQUAL "components/platform_note4/board_zectrix_note4.cpp")
        message(FATAL_ERROR
            "M8 architecture gate: ${source}: EPD SPI/GPIO6 access bypasses DisplayService")
    endif()

    if((contents MATCHES "#[ \t]*include[ \t]*[<\"]driver/i2s" OR
        contents MATCHES "GPIO_NUM_(42|46)([^0-9]|$)") AND
       NOT source STREQUAL "main/services/audio_service.cpp" AND
       NOT source STREQUAL "components/platform_note4/board_zectrix_note4.cpp")
        message(FATAL_ERROR
            "M8 architecture gate: ${source}: codec/I2S/amplifier access bypasses AudioService")
    endif()

    if(contents MATCHES "nvs_(set|erase|commit)[A-Za-z0-9_]*[ \t\r\n]*\\(" AND
       NOT source STREQUAL "main/storage.cpp" AND
       NOT source STREQUAL "main/runtime/storage_schema.cpp")
        message(FATAL_ERROR
            "M8 architecture gate: ${source}: NVS write bypasses StorageService/schema bootstrap")
    endif()
endforeach()

wqn_reject(
    "main/storage.cpp"
    "SaveBlobToNvs[ \t\r\n]*\\([ \t\r\n]*kProblemsKey"
    "problem content cache must use the bounded WQPC SPIFFS transaction, never NVS")

set(removed_word_client_patterns
    "WqnWord(Sync|Review)"
    "(Fetch|Parse|Submit)Word(Sync|Review)"
    "[/]words[/](sync|review)"
    "daily_target"
    "review_indices"
    "pending_submit"
    "random_review"
    "wsess[.]"
    "WordAppMode::k(ReviewFront|ReviewBack|DictionaryDetail|LookupResult)")
foreach(source IN LISTS firmware_sources)
    wqn_read("${source}" contents)
    foreach(pattern IN LISTS removed_word_client_patterns)
        if(contents MATCHES "${pattern}")
            message(FATAL_ERROR
                "M8 architecture gate: ${source}: removed legacy word path matched ${pattern}")
        endif()
    endforeach()
endforeach()

set(removed_legacy_paths
    main/epd_display.cpp
    main/epd_display.h
    main/online_sync.h
    main/audio_sleep.cpp
    main/audio_sleep.h
    main/board_zectrix_note4.cpp
    main/board_zectrix_note4.h
    main/device_protocol/v3.cpp
    main/runtime/sleep_coordinator.cpp)
foreach(relative_path IN LISTS removed_legacy_paths)
    if(EXISTS "${WQN_PROJECT_DIR}/${relative_path}")
        message(FATAL_ERROR
            "M8 architecture gate: legacy implementation returned at ${relative_path}")
    endif()
endforeach()

message(STATUS "M8 architecture ownership gate passed")
