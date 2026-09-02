set(PRINTDECK_SRCS
        "app_main.cpp"
        "src/core/configuration_backup.cpp"
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
        "include/img/printer_brand_logos.c"
        "include/img/printer_brand_logos_small.c"
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
        "src/platform/moonraker_connection_probe.cpp"
        "src/platform/moonraker_camera_client.cpp"
        "src/platform/inactive_printer_poller.cpp"
        "src/platform/printer_discovery_service.cpp"
        "src/platform/firmware_update_service.cpp"
        "src/platform/bambu_lan_adapter.cpp"
        "src/platform/bambu_model.cpp"
        "src/platform/bambu_report_parser.cpp"
        "src/platform/bambu_trust.cpp"
        "src/platform/bambu_a1_preview_client.cpp"
        "src/platform/bambu_a1_camera_client.cpp"
        "src/platform/bambu_compatibility_probe.cpp"
        "src/platform/reset_diagnostics.cpp"
        "src/platform/audio_service.cpp"
        "src/platform/orientation_service.cpp"
)

set(PRINTDECK_BOARD_REQUIRES)
if(PRINTDECK_HW_VARIANT STREQUAL "knomi2")
    list(APPEND PRINTDECK_SRCS
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
        "include/img/printdeck_boot_logo.c"
        "src/platform/board_amoled_1_75.cpp"
        "src/platform/rotating_panel.cpp"
        "src/platform/power_service.cpp"
        "src/platform/usb_developer_service.cpp"
    )
    list(APPEND PRINTDECK_BOARD_REQUIRES
        waveshare__esp32_s3_touch_amoled_1_75
    )
endif()

set(PRINTDECK_VOICE_AUDIO_FILES)
set(PRINTDECK_VOICE_LANGUAGES en pl es fr de zh_cn)
set(PRINTDECK_VOICE_EVENTS
    startup
    print_started
    progress_25
    progress_50
    progress_75
    print_paused
    print_finished
    print_error
    hms_alert
    filament_attention
    shutdown
    restarting
)
foreach(language IN LISTS PRINTDECK_VOICE_LANGUAGES)
    foreach(event IN LISTS PRINTDECK_VOICE_EVENTS)
        list(APPEND PRINTDECK_VOICE_AUDIO_FILES
            "audio/voice/voice_${language}_${event}.adpcm")
    endforeach()
endforeach()

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

idf_component_register(
    SRCS ${PRINTDECK_SRCS}
    INCLUDE_DIRS "include"
    EMBED_FILES
        "fonts/DejaVuSans-PrintDeck.ttf"
        "fonts/NotoSansSC-PrintDeck.ttf"
        "fonts/Unscii-PrintDeck.ttf"
        "audio/kenney/modern_startup.adpcm"
        "audio/kenney/modern_navigation.adpcm"
        "audio/kenney/modern_orientation.adpcm"
        "audio/kenney/modern_print_started.adpcm"
        "audio/kenney/modern_print_paused.adpcm"
        "audio/kenney/modern_print_finished.adpcm"
        "audio/kenney/modern_print_error.adpcm"
        "audio/kenney/modern_hms_alert.adpcm"
        "audio/kenney/modern_filament_attention.adpcm"
        "audio/kenney/modern_shutdown_countdown.adpcm"
        "audio/kenney/modern_shutdown.adpcm"
        "audio/kenney/modern_test.adpcm"
        "audio/kenney/arcade/arcade_startup.adpcm"
        "audio/kenney/arcade/arcade_navigation.adpcm"
        "audio/kenney/arcade/arcade_orientation.adpcm"
        "audio/kenney/arcade/arcade_print_started.adpcm"
        "audio/kenney/arcade/arcade_progress_25.adpcm"
        "audio/kenney/arcade/arcade_progress_50.adpcm"
        "audio/kenney/arcade/arcade_progress_75.adpcm"
        "audio/kenney/arcade/arcade_print_paused.adpcm"
        "audio/kenney/arcade/arcade_print_finished.adpcm"
        "audio/kenney/arcade/arcade_print_error.adpcm"
        "audio/kenney/arcade/arcade_hms_alert.adpcm"
        "audio/kenney/arcade/arcade_filament_attention.adpcm"
        "audio/kenney/arcade/arcade_shutdown_countdown.adpcm"
        "audio/kenney/arcade/arcade_shutdown.adpcm"
        "audio/kenney/arcade/arcade_test.adpcm"
        "audio/kenney/scifi/scifi_startup.adpcm"
        "audio/kenney/scifi/scifi_navigation.adpcm"
        "audio/kenney/scifi/scifi_orientation.adpcm"
        "audio/kenney/scifi/scifi_print_started.adpcm"
        "audio/kenney/scifi/scifi_progress_25.adpcm"
        "audio/kenney/scifi/scifi_progress_50.adpcm"
        "audio/kenney/scifi/scifi_progress_75.adpcm"
        "audio/kenney/scifi/scifi_print_paused.adpcm"
        "audio/kenney/scifi/scifi_print_finished.adpcm"
        "audio/kenney/scifi/scifi_print_error.adpcm"
        "audio/kenney/scifi/scifi_hms_alert.adpcm"
        "audio/kenney/scifi/scifi_filament_attention.adpcm"
        "audio/kenney/scifi/scifi_shutdown_countdown.adpcm"
        "audio/kenney/scifi/scifi_shutdown.adpcm"
        "audio/kenney/scifi/scifi_test.adpcm"
        "audio/kenney/clean/clean_navigation.adpcm"
        "audio/kenney/clean/clean_orientation.adpcm"
        "audio/kenney/clean/clean_shutdown_countdown.adpcm"
        "audio/kenney/clean/clean_test.adpcm"
        ${PRINTDECK_VOICE_AUDIO_FILES}
        ${PRINTDECK_COMPRESSED_WEB_FILES}
        "web/reaction-previews/alloy_iris_green.webp"
        "web/reaction-previews/alloy_iris_blue.webp"
        "web/reaction-previews/alloy_iris_brown.webp"
        "web/reaction-previews/alloy_iris_amber.webp"
        "web/reaction-previews/alloy_iris_gray.webp"
        "web/reaction-previews/alloy_iris_hazel.webp"
        "web/reaction-previews/alloy_iris_red.webp"
        "web/reaction-previews/alloy_iris_violet.webp"
        "web/reaction-previews/alloy_iris_cyan.webp"
    REQUIRES
        app_update
        esp_event
        esp_http_client
        esp_http_server
        esp_https_ota
        esp_netif
        esp_wifi
        esp_timer
        esp_driver_usb_serial_jtag
        esp-tls
        espressif__esp_new_jpeg
        espressif__libpng
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
if(PRINTDECK_HW_VARIANT STREQUAL "knomi2")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC PRINTDECK_BOARD_KNOMI2=1)
elseif(PRINTDECK_HW_VARIANT STREQUAL "lcd_1_54")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC PRINTDECK_BOARD_LCD_1_54=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC PRINTDECK_BOARD_AMOLED_1_75=1)
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wpedantic)
