file(GLOB OPENH264_COMMON_SOURCES
    "third_party/openh264/codec/common/src/*.cpp")
file(GLOB OPENH264_DECODER_SOURCES
    "third_party/openh264/codec/decoder/src/*.cpp")

idf_component_register(
    SRCS
        ${OPENH264_COMMON_SOURCES}
        ${OPENH264_DECODER_SOURCES}
    INCLUDE_DIRS
        "third_party/openh264/codec/api/wels"
        "third_party/openh264/codec/common/inc"
        "third_party/openh264/codec/decoder/inc"
    REQUIRES
        esp_mm
)

# This is intentionally not a general video decoder.  PrintDeck connects to a
# camera, consumes one self-contained IDR access unit, scales it to the display
# and tears the decoder down.  These changes remove DPB/inter-frame storage and
# keep the stock K2's 1080p Main/CABAC keyframe inside the board's 8 MB PSRAM.
target_compile_definitions(${COMPONENT_LIB} PRIVATE WELS_IDR_ONLY ESP_PLATFORM)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-format
)
