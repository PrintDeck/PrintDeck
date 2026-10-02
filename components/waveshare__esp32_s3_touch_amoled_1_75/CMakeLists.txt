if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.5")
    set(REQ esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_sdmmc)
    set(PRIV_REQ esp_driver_ledc)
else()
    set(REQ driver)
    set(PRIV_REQ "")
endif()

if(${IDF_VERSION_MAJOR} LESS 6)
    list(APPEND PRIV_REQ usb)
endif()

idf_component_register(
    SRCS "esp32_s3_touch_amoled_1_75.c" ${SRC_VER}
    INCLUDE_DIRS "include" "include/bsp"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES ${REQ} esp_lcd
    PRIV_REQUIRES ${PRIV_REQ} esp_timer spiffs esp_psram fatfs
)

# Preserve the reviewed panel/rotation path while selecting the C board wiring.
idf_build_get_property(printdeck_variant PRINTDECK_HW_VARIANT)
if(printdeck_variant STREQUAL "amoled_1_75c")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC PRINTDECK_BOARD_AMOLED_1_75C=1)
endif()
