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
if(NOT resource_hash STREQUAL "d8e8882f6aab3ddf60739841fd9a4605deee8fd72ec42d81fa9a949097d23eca")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../fonts/Unscii-PrintDeck.ttf" resource_hash)
if(NOT resource_hash STREQUAL "d72de9dbac8b4a2b1a70ef5ea9efe828fcad946c3329a20c51a73b94281065c2")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printer_brand_logos_small.c" resource_hash)
if(NOT resource_hash STREQUAL "4514bb68600f0b4b778e3a3a42f62a32c71f21de5c6c55835705d75468356f4d")
    message(FATAL_ERROR "Compressed device asset source is stale; regenerate compressed device assets.")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../include/img/printer_brand_logos.c" resource_hash)
if(NOT resource_hash STREQUAL "710fc0f36241eb8d56d7aa91ab7c46773900d80b9a316e769a0455521fd30902")
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
if(NOT resource_hash STREQUAL "ffa9443d93c8a43c893996589dfa1e481b7da7d6feba4febdbf8315bcee8bad5")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_cjk_ttf.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_terminal_ttf.gz" resource_hash)
if(NOT resource_hash STREQUAL "5f7007916c847dc719861414368e5fb6751689be1ebdfbdde06e57fb5bcdce5c")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_terminal_ttf.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_small_logos.bin.gz" resource_hash)
if(NOT resource_hash STREQUAL "81e81ca8a543ba173ba263a0312caa65fd16f2d41d7ade520f264ea913dfea4b")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_small_logos.bin.gz")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/device_large_logos.bin.gz" resource_hash)
if(NOT resource_hash STREQUAL "2beeb00c7f57fc1362cfd9475c2bf6f13b6997ef19ac96fc0d96d221400c0329")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
if(PRINTDECK_HW_VARIANT STREQUAL "amoled_1_75")
list(APPEND PRINTDECK_COMPRESSED_DEVICE_FILES "generated/compressed-device-assets/device_large_logos.bin.gz")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/resource_sizes.hpp" resource_hash)
if(NOT resource_hash STREQUAL "435b9af316c32b63c5c616b763bc4a36acb0c8e49ad5d74b83929a8297aa3f89")
    message(FATAL_ERROR "Compressed device asset output is stale; regenerate compressed device assets.")
endif()
