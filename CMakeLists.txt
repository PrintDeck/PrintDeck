cmake_minimum_required(VERSION 3.24)

set(IDF_TARGET esp32s3)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep generated configuration scoped to its build directory. ESP-IDF otherwise
# reuses a project-root sdkconfig, which can silently override newly committed
# release defaults depending on which machine performs the build.
if(NOT DEFINED SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()

# Keep hardware selection explicit and build-directory-local.  The 1.75-inch
# AMOLED remains the default so existing build and flash commands
# continue to produce the exact same target unless a caller opts in to another
# board with -DPRINTDECK_HW_VARIANT=<target>.
set(PRINTDECK_HW_VARIANT "amoled_1_75" CACHE STRING "PrintDeck hardware variant")
set_property(CACHE PRINTDECK_HW_VARIANT PROPERTY STRINGS amoled_1_75 lcd_1_54 knomi2)
if(NOT PRINTDECK_HW_VARIANT STREQUAL "amoled_1_75" AND
   NOT PRINTDECK_HW_VARIANT STREQUAL "lcd_1_54" AND
   NOT PRINTDECK_HW_VARIANT STREQUAL "knomi2")
    message(FATAL_ERROR
        "Unsupported PRINTDECK_HW_VARIANT='${PRINTDECK_HW_VARIANT}'. "
        "Choose amoled_1_75, lcd_1_54 or knomi2.")
endif()
message(STATUS "PrintDeck hardware variant: ${PRINTDECK_HW_VARIANT}")

# KNOMI2 exposes its ESP32-S3 console through an external WCH UART bridge.
# Its native USB pins drive the display, so this target needs a console override
# without changing the USB Serial/JTAG defaults of the two Waveshare boards.
if(NOT DEFINED SDKCONFIG_DEFAULTS OR SDKCONFIG_DEFAULTS STREQUAL "")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()
if(PRINTDECK_HW_VARIANT STREQUAL "knomi2")
    list(FIND SDKCONFIG_DEFAULTS "sdkconfig.knomi2.defaults" KNOMI2_DEFAULTS_INDEX)
    if(KNOMI2_DEFAULTS_INDEX EQUAL -1)
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.knomi2.defaults")
    endif()
endif()

# Each hardware target has an independent release stream. Never align one
# target to the other target's version merely because both share this source.
set(PRINTDECK_AMOLED_1_75_VERSION "1.0.2")
set(PRINTDECK_LCD_1_54_VERSION "1.0.2")
set(PRINTDECK_KNOMI2_VERSION "1.0.2")
if(PRINTDECK_HW_VARIANT STREQUAL "knomi2")
    set(PROJECT_VER "${PRINTDECK_KNOMI2_VERSION}")
    set(PRINTDECK_REACTION_SEED_DIR "main/reaction_seed/knomi2")
elseif(PRINTDECK_HW_VARIANT STREQUAL "lcd_1_54")
    set(PROJECT_VER "${PRINTDECK_LCD_1_54_VERSION}")
    set(PRINTDECK_REACTION_SEED_DIR "main/reaction_seed/lcd_1_54")
else()
    set(PROJECT_VER "${PRINTDECK_AMOLED_1_75_VERSION}")
    set(PRINTDECK_REACTION_SEED_DIR "main/reaction_seed/amoled_1_75")
endif()
message(STATUS "PrintDeck ${PRINTDECK_HW_VARIANT} version: ${PROJECT_VER}")
message(STATUS "PrintDeck reaction seed: ${PRINTDECK_REACTION_SEED_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(printdeck)

# Factory/full flashes receive one validated default reaction set. OTA keeps
# the existing LittleFS partition, so user overrides and the selected set are
# not replaced by an application update.
littlefs_create_partition_image(assets ${PRINTDECK_REACTION_SEED_DIR} FLASH_IN_PROJECT)

if(NOT CONFIG_MQTT_TASK_STACK_ON_EXTERNAL_MEMORY)
    message(FATAL_ERROR
        "PrintDeck requires CONFIG_MQTT_TASK_STACK_ON_EXTERNAL_MEMORY=y. "
        "Discard the stale sdkconfig and configure with the committed defaults.")
endif()
