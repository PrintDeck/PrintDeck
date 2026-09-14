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
if(NOT resource_hash STREQUAL "f7475c33991ed3cb2ee60ac1c1f47a75411a6d07895e7a516c8be386a1bbdea9")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../fonts/NotoSansSC-PrintDeck.ttf" resource_hash)
if(NOT resource_hash STREQUAL "9caaf412249e10801451dbc0ff2e1a326f6d4f13e24444762d70ff8a469d8ab6")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../fonts/Unscii-PrintDeck.ttf" resource_hash)
if(NOT resource_hash STREQUAL "d72de9dbac8b4a2b1a70ef5ea9efe828fcad946c3329a20c51a73b94281065c2")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printer_brand_logos_small.c" resource_hash)
if(NOT resource_hash STREQUAL "e9a699d15a0de920b53371b3ce54e3496a67af48636a3ccb191d9513d644633e")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printer_brand_logos.c" resource_hash)
if(NOT resource_hash STREQUAL "cbbc8bace6a59f01db89e139b7cfa4f2b3abe253eebb01b4f46b3d1174b480b9")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printdeck_boot_logo.c" resource_hash)
if(NOT resource_hash STREQUAL "a2886cb08bccf7d4dc2ec960f7f07c726a9b6afea39f96d40bc5a2fd8a87f462")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_latin_ttf.gz" resource_hash)
if(NOT resource_hash STREQUAL "d91975cfead139f989ae4a4d483aff8916bcec7eeb96ee55a2aa95ea3160a970")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_latin_ttf.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_cjk_ttf.gz" resource_hash)
if(NOT resource_hash STREQUAL "8298e6b9a6c15d1ab63779ab98f31e3d8572ff196efa5fa509320c610df2d462")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_cjk_ttf.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_terminal_ttf.gz" resource_hash)
if(NOT resource_hash STREQUAL "5f7007916c847dc719861414368e5fb6751689be1ebdfbdde06e57fb5bcdce5c")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_terminal_ttf.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_small_logos.bin.gz" resource_hash)
if(NOT resource_hash STREQUAL "7215c472c8e6d31c955d26deef91f7be11151349854dd27bf50b072883d19324")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_small_logos.bin.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_large_logos.bin.gz" resource_hash)
if(NOT resource_hash STREQUAL "6ab50c004f4bf0011dd1997d60cb2f09d1bf40bbafad27721e8f73651c39785a")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
if(PRINTDECK_HW_VARIANT STREQUAL "amoled_1_75")
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_large_logos.bin.gz")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/resource_sizes.hpp" resource_hash)
if(NOT resource_hash STREQUAL "54a06240e8c554b91a21629586d5a008e8f7083097b95bd44365611fb8cd8a02")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
