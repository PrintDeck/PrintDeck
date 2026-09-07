# Compressed shipping resources and source freshness checks.
set(PRINTDECK_COMPRESSED_DEVICE_FILES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../../fonts/DejaVuSans-PrintDeck.ttf"
    "${CMAKE_CURRENT_LIST_DIR}/../../fonts/NotoSansSC-PrintDeck.ttf"
    "${CMAKE_CURRENT_LIST_DIR}/../../fonts/Unscii-PrintDeck.ttf"
    "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printer_brand_logos_small.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printer_brand_logos.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printdeck_boot_logo.c"
    "${CMAKE_CURRENT_LIST_DIR}/device_latin_ttf.gz"
    "${CMAKE_CURRENT_LIST_DIR}/device_cjk_ttf.gz"
    "${CMAKE_CURRENT_LIST_DIR}/device_terminal_ttf.gz"
    "${CMAKE_CURRENT_LIST_DIR}/device_small_logos.bin.gz"
    "${CMAKE_CURRENT_LIST_DIR}/device_large_logos.bin.gz"
    "${CMAKE_CURRENT_LIST_DIR}/resource_sizes.hpp"
)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../fonts/DejaVuSans-PrintDeck.ttf" resource_hash)
if(NOT resource_hash STREQUAL "5a548e0a3cf6970ebb6d340d5e9e2ff07a537f52852da3406c4e21dda930a790")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../fonts/NotoSansSC-PrintDeck.ttf" resource_hash)
if(NOT resource_hash STREQUAL "cef11523edae2bbb0d508d8de53700ddad487f3a5c6fd3bea202e426bd613b06")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../fonts/Unscii-PrintDeck.ttf" resource_hash)
if(NOT resource_hash STREQUAL "d72de9dbac8b4a2b1a70ef5ea9efe828fcad946c3329a20c51a73b94281065c2")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printer_brand_logos_small.c" resource_hash)
if(NOT resource_hash STREQUAL "b9f8e19e87ed5c635d70cc5705e1f41b4fec29d96e60b6205957c3d02237c41c")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printer_brand_logos.c" resource_hash)
if(NOT resource_hash STREQUAL "c19c7cc2c18ef6462c4f1bc8052a42b2d975fac3baf1fccc9bdd91999119d58a")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printdeck_boot_logo.c" resource_hash)
if(NOT resource_hash STREQUAL "a2886cb08bccf7d4dc2ec960f7f07c726a9b6afea39f96d40bc5a2fd8a87f462")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_latin_ttf.gz" resource_hash)
if(NOT resource_hash STREQUAL "17f417badca592cdf81d718fb5984239aecc09062e252af1589fa3836dd08773")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_latin_ttf.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_cjk_ttf.gz" resource_hash)
if(NOT resource_hash STREQUAL "885ad1c5b164e5391395820e11fad13ca422d06950565aec28b6ec81664f165e")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_cjk_ttf.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_terminal_ttf.gz" resource_hash)
if(NOT resource_hash STREQUAL "5f7007916c847dc719861414368e5fb6751689be1ebdfbdde06e57fb5bcdce5c")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_terminal_ttf.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_small_logos.bin.gz" resource_hash)
if(NOT resource_hash STREQUAL "d85e03c79982e42933f552ee3c0c09f57540f4ba7059164f8a207614e2a6603d")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_small_logos.bin.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_large_logos.bin.gz" resource_hash)
if(NOT resource_hash STREQUAL "4547b2e86bd8bc3fae0f00e0c2297a241601d647aafb58e6b4302d796df0e3cf")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
if(PRINTDECK_HW_VARIANT STREQUAL "amoled_1_75")
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_large_logos.bin.gz")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/resource_sizes.hpp" resource_hash)
if(NOT resource_hash STREQUAL "f508dfe1a2539eee98830ff2912431efd275e06e9c86e82e3277ad5439580853")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
