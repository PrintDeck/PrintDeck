# Clear the retired private test option from persistent development caches.
unset(PRINTDECK_ANYCUBIC_TEST_CONFIG CACHE)

include("${CMAKE_CURRENT_LIST_DIR}/generated/compressed-montserrat/manifest.cmake")

set(PRINTDECK_SRCS
        ${PRINTDECK_COMPRESSED_FONT_SRCS}
        "app_main.cpp"
        "src/core/configuration_backup.cpp"
        "src/core/compressed_resource.cpp"
        "src/core/device_state.cpp"
        "src/core/firmware_channel.cpp"
        "src/core/job_state.cpp"
        "src/core/localization.cpp"
        "src/core/printer_driver.cpp"
        "src/core/reactions.cpp"
        "src/core/settings.cpp"
        "src/core/timezone.cpp"
        "src/core/theme.cpp"
        "src/core/unified_printer_api.cpp"
        "include/font/mdi_40.c"
        "src/platform/embedded_resources.cpp"
        "src/platform/runtime.cpp"
        "src/platform/display_shell.cpp"
        "src/platform/printer_animation_renderer.cpp"
        "src/platform/reaction_asset_service.cpp"
        "src/platform/settings_store.cpp"
        "src/platform/network_service.cpp"
        "src/platform/web_config.cpp"
        "src/platform/web_assets.cpp"
        "src/platform/moonraker_adapter.cpp"
        "src/platform/moonraker_status_parser.cpp"
        "src/platform/prusalink_status_parser.cpp"
        "src/platform/prusalink_auth.cpp"
        "src/platform/prusalink_client.cpp"
        "src/platform/prusalink_http_decoder.cpp"
        "src/platform/prusalink_http_transport.cpp"
        "src/platform/prusalink_service.cpp"
        "src/platform/elegoo_sdcp_parser.cpp"
        "src/platform/elegoo_sdcp_service.cpp"
        "src/platform/elegoo_cc2_parser.cpp"
        "src/platform/elegoo_cc2_service.cpp"
        "src/platform/elegoo_connection_probe.cpp"
    "src/platform/uniformation_connection_probe.cpp"
    "src/platform/uniformation_sdcp_parser.cpp"
        "src/platform/prusalink_discovery.cpp"
        "src/platform/moonraker_connection_probe.cpp"
        "src/platform/moonraker_camera_client.cpp"
        "src/platform/inactive_printer_poller.cpp"
        "src/platform/printer_discovery_service.cpp"
        "src/platform/firmware_update_service.cpp"
        "src/platform/firmware_identity.cpp"
        "src/platform/compressed_font.cpp"
        "src/platform/bambu_lan_adapter.cpp"
        "src/platform/bambu_model.cpp"
        "src/platform/bambu_report_parser.cpp"
        "src/platform/bambu_trust.cpp"
        "src/platform/bambu_a1_preview_client.cpp"
        "src/platform/bambu_a1_camera_client.cpp"
        "src/platform/bambu_compatibility_probe.cpp"
        "src/platform/reset_diagnostics.cpp"
        "src/platform/audio_service_settings.cpp"
        "src/platform/orientation_service.cpp"
)

set(PRINTDECK_BOARD_REQUIRES)
if(PRINTDECK_HW_VARIANT STREQUAL "knomi2")
    list(APPEND PRINTDECK_SRCS
        "src/platform/audio_service_knomi2.cpp"
        "src/platform/board_knomi2.cpp"
        "src/platform/display_shell_lcd_1_54.cpp"
        "src/platform/power_service_knomi2.cpp"
        "src/platform/usb_developer_service_knomi2.cpp"
    )
    list(APPEND PRINTDECK_BOARD_REQUIRES
        espressif__esp_lcd_gc9a01
        espressif__esp_lcd_touch_cst816s
        esp_driver_spi
    )
elseif(PRINTDECK_HW_VARIANT STREQUAL "lcd_1_54")
    list(APPEND PRINTDECK_SRCS
        "src/platform/audio_service.cpp"
        "src/core/audio_sample.cpp"
        "src/platform/board_lcd_1_54.cpp"
        "src/platform/display_shell_lcd_1_54.cpp"
        "src/platform/power_service_lcd_1_54.cpp"
        "src/platform/usb_developer_service.cpp"
    )
    list(APPEND PRINTDECK_BOARD_REQUIRES
        espressif__esp_lcd_touch_cst816s
        esp_adc
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_spi
    )
else()
    list(APPEND PRINTDECK_SRCS
        "src/platform/audio_service.cpp"
        "src/core/audio_sample.cpp"
        "src/platform/board_amoled_1_75.cpp"
        "src/platform/rotating_panel.cpp"
        "src/platform/power_service.cpp"
        "src/platform/usb_developer_service.cpp"
    )
    list(APPEND PRINTDECK_BOARD_REQUIRES
        waveshare__esp32_s3_touch_amoled_1_75
        waveshare__esp_lcd_touch_cst9217
    )
endif()

# Share one speech implementation, model binding and compressed audio bank.
if(NOT PRINTDECK_HW_VARIANT STREQUAL "knomi2")
    list(APPEND PRINTDECK_SRCS
        "src/platform/voice_service.cpp"
        "src/platform/voice_model_store.cpp")
    list(APPEND PRINTDECK_BOARD_REQUIRES espressif__esp-sr)
    include("${CMAKE_CURRENT_LIST_DIR}/audio/voice_en/compressed/manifest.cmake")
endif()

# KNOMI2 has no speaker; keep audio settings but omit playback and its assets.
set(PRINTDECK_COMPRESSED_AUDIO_FILES)
if(NOT PRINTDECK_HW_VARIANT STREQUAL "knomi2")
    set(PRINTDECK_COMPRESSED_AUDIO_ROOT "${CMAKE_CURRENT_LIST_DIR}/audio/compressed")
    include("${PRINTDECK_COMPRESSED_AUDIO_ROOT}/manifest.cmake")
    foreach(asset_name IN LISTS PRINTDECK_AUDIO_RESOURCE_NAMES)
        set(source_variable "PRINTDECK_AUDIO_RESOURCE_${asset_name}_SOURCE")
        set(source_hash_variable "PRINTDECK_AUDIO_RESOURCE_${asset_name}_SOURCE_SHA256")
        set(gzip_variable "PRINTDECK_AUDIO_RESOURCE_${asset_name}_GZIP")
        set(gzip_hash_variable "PRINTDECK_AUDIO_RESOURCE_${asset_name}_GZIP_SHA256")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_LIST_DIR}/${${source_variable}}"
            "${PRINTDECK_COMPRESSED_AUDIO_ROOT}/${${gzip_variable}}")
        file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/${${source_variable}}" source_hash)
        file(SHA256 "${PRINTDECK_COMPRESSED_AUDIO_ROOT}/${${gzip_variable}}" gzip_hash)
        if(NOT source_hash STREQUAL "${${source_hash_variable}}" OR
           NOT gzip_hash STREQUAL "${${gzip_hash_variable}}")
            message(FATAL_ERROR "Compressed audio resource ${asset_name} is stale.")
        endif()
        list(APPEND PRINTDECK_COMPRESSED_AUDIO_FILES "audio/compressed/${${gzip_variable}}")
    endforeach()
    list(REMOVE_DUPLICATES PRINTDECK_COMPRESSED_AUDIO_FILES)
endif()

set(PRINTDECK_COMPRESSED_WEB_ROOT "${CMAKE_CURRENT_LIST_DIR}/web/compressed")
include("${PRINTDECK_COMPRESSED_WEB_ROOT}/manifest.cmake")
set(PRINTDECK_COMPRESSED_WEB_FILES)
foreach(asset_name IN LISTS PRINTDECK_COMPRESSED_WEB_ASSET_NAMES)
    set(source_variable "PRINTDECK_COMPRESSED_WEB_ASSET_${asset_name}_SOURCE")
    set(source_hash_variable "PRINTDECK_COMPRESSED_WEB_ASSET_${asset_name}_SOURCE_SHA256")
    set(gzip_variable "PRINTDECK_COMPRESSED_WEB_ASSET_${asset_name}_GZIP")
    set(gzip_hash_variable "PRINTDECK_COMPRESSED_WEB_ASSET_${asset_name}_GZIP_SHA256")
    set(source_name "${${source_variable}}")
    set(gzip_name "${${gzip_variable}}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/web/${source_name}"
        "${PRINTDECK_COMPRESSED_WEB_ROOT}/${gzip_name}")
    file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/web/${source_name}" source_hash)
    file(SHA256 "${PRINTDECK_COMPRESSED_WEB_ROOT}/${gzip_name}" gzip_hash)
    if(NOT source_hash STREQUAL "${${source_hash_variable}}" OR
       NOT gzip_hash STREQUAL "${${gzip_hash_variable}}")
        message(FATAL_ERROR
            "Compressed Web Config asset ${gzip_name} is stale. "
            "Regenerate it with the compressed Web Config asset workflow.")
    endif()
    list(APPEND PRINTDECK_COMPRESSED_WEB_FILES "web/compressed/${gzip_name}")
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/generated/reaction-catalog/reaction_previews.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/generated/compressed-device-assets/manifest.cmake")

idf_component_register(
    SRCS ${PRINTDECK_SRCS}
    INCLUDE_DIRS "include"
    EMBED_FILES
        ${PRINTDECK_VOICE_ASSETS}
        ${PRINTDECK_COMPRESSED_DEVICE_FILES}
        ${PRINTDECK_COMPRESSED_AUDIO_FILES}
        ${PRINTDECK_COMPRESSED_WEB_FILES}
        ${PRINTDECK_REACTION_PREVIEW_FILES}
    REQUIRES
        app_update
        esp_event
        esp_http_client
        http_parser
        esp_http_server
        esp_https_ota
        esp_netif
        esp_wifi
        esp_timer
        esp_driver_usb_serial_jtag
        esp-tls
        espressif__esp_new_jpeg
        espressif__libpng
        espressif__zlib
        espressif__esp_codec_dev
        espressif__mdns
        XPowersLib
        espressif__esp_io_expander
        mbedtls
        mqtt
        espressif__esp_websocket_client
        espressif__esp_peer
        espressif__esp_h264
        printdeck_h264_snapshot
        nvs_flash
        spi_flash
        lvgl
        lwip
        json
        joltwallet__littlefs
        ${PRINTDECK_BOARD_REQUIRES}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE PRINTDECK_VERSION="${PROJECT_VER}")
idf_build_get_property(build_dir BUILD_DIR)
set(layout_header "${CMAKE_CURRENT_BINARY_DIR}/printdeck_layout.hpp")
add_custom_command(OUTPUT "${layout_header}"
    COMMAND ${CMAKE_COMMAND}
        "-DPARTITION_TABLE=${build_dir}/partition_table/partition-table.bin"
        "-DOUTPUT=${layout_header}" -P "${CMAKE_CURRENT_LIST_DIR}/firmware_layout.cmake"
    DEPENDS partition_table "${build_dir}/partition_table/partition-table.bin"
        "${CMAKE_CURRENT_LIST_DIR}/firmware_layout.cmake")
target_sources(${COMPONENT_LIB} PRIVATE "${layout_header}")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE PRINTDECK_IDENTITY_TARGET="${PRINTDECK_HW_VARIANT}")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,printdeck_firmware_identity")
if(PRINTDECK_HW_VARIANT STREQUAL "knomi2")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC PRINTDECK_BOARD_KNOMI2=1)
elseif(PRINTDECK_HW_VARIANT STREQUAL "lcd_1_54")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC PRINTDECK_BOARD_LCD_1_54=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC PRINTDECK_BOARD_AMOLED_1_75=1)
endif()
if(NOT PRINTDECK_HW_VARIANT STREQUAL "knomi2")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC PRINTDECK_LOCAL_VOICE=1)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_mn_handle_from_name"
        "-Wl,--wrap=get_static_srmodels"
        "-Wl,--wrap=get_model_base_path")
endif()
foreach(font_size 12 14 16 24 32)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lv_font_montserrat_${font_size}")
endforeach()
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wpedantic)
# Configuration and status parsing are not rendering hot paths. Prefer compact
# code here to keep the application within the existing 5 MiB OTA slots.
set_source_files_properties(
    "src/platform/web_config.cpp"
    "src/platform/bambu_report_parser.cpp"
    "src/platform/elegoo_sdcp_parser.cpp"
    "src/platform/moonraker_status_parser.cpp"
    "src/platform/uniformation_sdcp_parser.cpp"
    PROPERTIES COMPILE_OPTIONS "-Os")
