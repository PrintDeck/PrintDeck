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
if(NOT resource_hash STREQUAL "0744d65a57d006775a4af763e6ab59f49fef1bb4c2ef013c810eb2954da34db1")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../fonts/Unscii-PrintDeck.ttf" resource_hash)
if(NOT resource_hash STREQUAL "d72de9dbac8b4a2b1a70ef5ea9efe828fcad946c3329a20c51a73b94281065c2")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printer_brand_logos_small.c" resource_hash)
if(NOT resource_hash STREQUAL "0bd0627ad0587ff9aa1901e989983e2866af7c096fb1b66c89c79b42afd27c99")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printer_brand_logos.c" resource_hash)
if(NOT resource_hash STREQUAL "ac3e48aff7701b5e23f459a809f3e4d251ba3e17eceae41b9fa322e902bdd4c7")
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
if(NOT resource_hash STREQUAL "b0512545b27657c46e30ceda2889417b9a8e06dd184abb33ababd2143a6ce169")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_cjk_ttf.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_terminal_ttf.gz" resource_hash)
if(NOT resource_hash STREQUAL "5f7007916c847dc719861414368e5fb6751689be1ebdfbdde06e57fb5bcdce5c")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_terminal_ttf.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_small_logos.bin.gz" resource_hash)
if(NOT resource_hash STREQUAL "fe35d8d922161f2c53c1668add862b310d381959d5e23a66d614e1abb81d11d6")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_small_logos.bin.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_large_logos.bin.gz" resource_hash)
if(NOT resource_hash STREQUAL "89222030e8f080ce21e16cfa10423ff51ef74b4b4edd225bd8964cdd201d6ba2")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
if(PRINTDECK_HW_VARIANT STREQUAL "amoled_1_75")
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_large_logos.bin.gz")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/resource_sizes.hpp" resource_hash)
if(NOT resource_hash STREQUAL "ac364fdb102ad5c0c50edb123b3319177ff72a770a981ede7355e54b9befd7d8")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
