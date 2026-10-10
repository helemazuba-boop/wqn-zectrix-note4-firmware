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

# [storage-single-writer] The only files allowed to contain a SPIFFS mutation
# primitive. Each is reached through a StorageService transaction.
set(spiffs_writer_files
    main/storage.cpp
    main/word_study_store.cpp
    main/note_store.cpp
    main/problem_store.cpp
    main/word_pack.cpp
    main/note_pack.cpp
    main/problem_pack.cpp)

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

    # [storage-single-writer] SPIFFS mutation primitives are confined to the
    # seven files that own durable pack/store I/O; every one of them is reached
    # through a StorageService transaction. The journal's raw rename sequence
    # was the one path that bypassed the owner task, and this is what keeps the
    # next one from being added silently. Balanced against the NVS rule above,
    # which already had a file allowlist.
    #
    # NOT enforced here: which task performs the write. A caller-level check
    # belongs to the STORAGE-ENTRYPOINT rule and to the storage service's own
    # caller accounting, not to a text scan.
    #
    # Pattern notes, all verified against the tree:
    #  * fopen: any mode containing w, a or + is a writing mode ("rb" is not).
    #  * std::remove is constrained to a path-looking argument, because the
    #    <algorithm> erase-remove idiom shares the name; the writer files always
    #    pass a *Path variable or a .c_str() path, the idiom never does.
    #  * bare rename/unlink are constrained the same way.
    if(NOT source IN_LIST spiffs_writer_files)
        if(contents MATCHES "fopen[ \t\r\n]*\\([^;]*\"[^\"]*[wa+][^\"]*\"")
            message(FATAL_ERROR
                "M8 architecture gate: ${source}: rule SPIFFS-WRITER: fopen with a writing mode -- route durable writes through StorageService")
        endif()
        if(contents MATCHES "std::rename[ \t\r\n]*\\(")
            message(FATAL_ERROR
                "M8 architecture gate: ${source}: rule SPIFFS-WRITER: std::rename -- route file replacement through StorageService")
        endif()
        if(contents MATCHES "unlink[ \t\r\n]*\\(")
            message(FATAL_ERROR
                "M8 architecture gate: ${source}: rule SPIFFS-WRITER: unlink -- route file removal through StorageService")
        endif()
        if(contents MATCHES "std::remove[ \t\r\n]*\\([^;]*([Pp]ath|c_str)")
            message(FATAL_ERROR
                "M8 architecture gate: ${source}: rule SPIFFS-WRITER: std::remove on a path -- route file removal through StorageService")
        endif()
        if(contents MATCHES "(^|[^:_[:alnum:]])rename[ \t\r\n]*\\([^;]*([Pp]ath|c_str)")
            message(FATAL_ERROR
                "M8 architecture gate: ${source}: rule SPIFFS-WRITER: rename on a path -- route file replacement through StorageService")
        endif()
    endif()
endforeach()

# [load-repair] Functions named Load* that nevertheless write. They are not
# accidents and not laziness: each one is a recovery or repair step that has to
# run on the same task that owns the read, because the write it performs is the
# healing of the thing it just read (promote the backup, re-derive the cursor
# from the durable outbox, drop a corrupt cache entry). Splitting them is future
# work; until then they must stay annotated so nobody mistakes one for a pure
# read -- and so the build can tell the day the split actually lands.
#
# Format: <relative path>|<function name>, one per declared writable Load. The
# annotation requirement is per FILE and counted, so removing the annotation on
# the second Load in a file is caught, not just the first.
set(declared_load_repairs
    main/word_study_store.cpp|LoadSessionSlotRaw
    main/word_study_store.cpp|LoadSessionTransaction
    main/note_store.cpp|LoadSessionRaw
    main/note_store.cpp|LoadSessionTransaction
    main/note_pack.cpp|LoadNoteImageTransaction
    main/note_pack.cpp|LoadCachedNoteImage)

set(load_repair_files "")
foreach(entry IN LISTS declared_load_repairs)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 repair_file)
    if(NOT repair_file IN_LIST load_repair_files)
        list(APPEND load_repair_files "${repair_file}")
    endif()
endforeach()

foreach(repair_file IN LISTS load_repair_files)
    set(repair_required 0)
    foreach(entry IN LISTS declared_load_repairs)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 declared_file)
        if(declared_file STREQUAL repair_file)
            math(EXPR repair_required "${repair_required} + 1")
        endif()
    endforeach()

    wqn_read("${repair_file}" contents)

    # Every declared function must still be defined here -- a rename, or a split
    # into a pure read plus a separate repair, has to update this list rather
    # than leave a stale entry pointing at nothing.
    foreach(entry IN LISTS declared_load_repairs)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 declared_file)
        list(GET parts 1 declared_function)
        if(declared_file STREQUAL repair_file)
            string(FIND "${contents}" "esp_err_t ${declared_function}(" definition_at)
            if(definition_at EQUAL -1)
                message(FATAL_ERROR
                    "M8 architecture gate: ${repair_file}: rule LOAD-REPAIR: ${declared_function} is declared as a writable Load but is no longer defined -- update declared_load_repairs when the read/write split lands")
            endif()
        endif()
    endforeach()

    string(REGEX MATCHALL "\\[load-repair\\]" repair_markers "${contents}")
    list(LENGTH repair_markers repair_marker_count)
    if(repair_marker_count LESS repair_required)
        message(FATAL_ERROR
            "M8 architecture gate: ${repair_file}: rule LOAD-REPAIR: ${repair_required} writable Load(s) but ${repair_marker_count} // [load-repair] annotation(s) -- name the write each one performs")
    endif()
endforeach()

set(removed_problem_prototype_patterns
    "CachedProblem"
    "PendingReviewResult"
    "ReviewChoice"
    "FetchProblems"
    "FetchProblemIndex"
    "UploadReviewComplete"
    "SyncDueProblemIds"
    "SyncDueProblemsAndLog"
    "WQN_DEBUG_PROBLEM_IDS"
    "[/]problem-index"
    "[/]review-complete"
    "problems[?]ids="
    "UiScreen::k(Library|Problem|Solution|ReviewQueue|ReviewScore|ReviewQueued)")
foreach(source IN LISTS firmware_sources)
    wqn_read("${source}" contents)
    foreach(pattern IN LISTS removed_problem_prototype_patterns)
        if(contents MATCHES "${pattern}")
            message(FATAL_ERROR
                "M8 architecture gate: ${source}: removed problem prototype matched ${pattern}")
        endif()
    endforeach()
endforeach()

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
    main/problem_cache.cpp
    main/problem_cache.h
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
