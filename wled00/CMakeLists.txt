# wled00/CMakeLists.txt — IDF component registration for WLED pure-IDF build.
# Active when building with framework = espidf (pioarduino).

cmake_minimum_required(VERSION 3.16)

# ─── Collect source files ─────────────────────────────────────────────────────
FILE(GLOB_RECURSE _all_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.S"
)

# Remove files/directories that are Arduino-only or not applicable to IDF build.
list(FILTER _all_sources EXCLUDE REGEX ".*wled00\\.ino$")
list(FILTER _all_sources EXCLUDE REGEX ".*/dmx[^/]*(\\.[cCh]|\\.[cC][pP][pP])$")
list(FILTER _all_sources EXCLUDE REGEX ".*/async-mqtt-client/.*")
list(FILTER _all_sources EXCLUDE REGEX ".*/methods/Esp32_i2s.*")
list(FILTER _all_sources EXCLUDE REGEX ".*/espalexa/.*")
list(FILTER _all_sources EXCLUDE REGEX ".*/DS1307RTC.*")
list(FILTER _all_sources EXCLUDE REGEX ".*/dependencies/dmx/.*")
# ESPAsyncE131.cpp compiles in IDF builds — AsyncUDP is stubbed in ESPAsyncE131.h
list(FILTER _all_sources EXCLUDE REGEX ".*/ESPAsyncWebServer/.*")
list(FILTER _all_sources EXCLUDE REGEX ".*/AsyncTCP/.*")
list(FILTER _all_sources EXCLUDE REGEX ".*/SPIFFSEditor/.*")

# ─── Register component ───────────────────────────────────────────────────────
idf_component_register(
    SRCS          ${_all_sources}
    INCLUDE_DIRS  "."
                  "idf_shims"
    PRIV_REQUIRES esp_psram
                  joltwallet__littlefs
                  espressif__esp_hosted
)

# ─── Compile definitions (equivalent to platformio.ini build_flags -D) ────────
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    WLED_IDF_BUILD
    CONFIG_IDF_TARGET_ESP32P4=1
    ESP32=1
    ARDUINO=10819
    WLED_MAX_USERMODS=25
    WLEDMM_FASTPATH
    WLEDMM_REMAP_AT_OUTPUT
    "WLEDMM_DISPLAY_MODE=P4_1280x720_60HZ"
    WLEDMM_DISPLAY_BUTTONS=32
    WLEDMM_DISPLAY_BUTTONS_COLS=8
    REALTIME_OUTPUT_TIMER
    WLED_USE_ETHERNET
    WLED_USE_ETHERNET_ONLY
    BOARD_HAS_PSRAM
    WLED_USE_PSRAM
    MAX_LEDS=99999
    MAX_LEDS_PER_BUS=99999
    MAX_LED_MEMORY=64000000
    "DEFAULT_LED_TYPE=TYPE_NET_ARTNET_RGB"
    LEDPIN=192
    E131_MAX_UNIVERSE_COUNT=50
    ARTNET_TIMER
    FASTLED_NO_FASTLED
    "WLED_RELEASE_NAME=ESP32-P4_IDF"
    WLED_WATCHDOG_TIMEOUT=100
    USERMOD_AUDIOREACTIVE
    USERMOD_AUTO_PLAYLIST
    # USERMOD_PIONEER_PROLINK
    # USERMOD_ARTNETMAP
    UM_AUDIOREACTIVE_USE_ESPDSP_FFT
    SR_DMTYPE=9
    SR_ENABLE_DEFAULT
    WLED_DISABLE_ESPNOW
    WLED_DISABLE_IMPROV_WIFISCAN
    WLED_DISABLE_MQTT
    WLED_DISABLE_ALEXA
    WLED_DISABLE_LOXONE
    HW_PIN_SDA=7
    HW_PIN_SCL=8
    CORE_DEBUG_LEVEL=0
    NDEBUG
    WLED_DANGEROUS_OPTIMIZATIONS
    WLED_USE_MY_CONFIG
    # ArduinoJson: disable std::string Reader to avoid ambiguity with our
    # class String : public std::string (both std::string and Arduino String
    # specializations would otherwise match simultaneously).
    ARDUINOJSON_ENABLE_STD_STRING=0
)

# PARLIO_PINS contains commas — must be passed via compile_options to avoid
# CMake treating the comma-separated value as a list.
target_compile_options(${COMPONENT_LIB} PUBLIC
    -DPARLIO_PINS=2,3,4,5,6,20,21,22,23,26,27,32,33,36,47,48
    # memcmp_P: LovyanGFX's lgfx_pngle.c calls memcmp_P which is not in newlib.
    # Pass as a compiler flag so it reaches C files that don't include idf_compat.h.
    -Dmemcmp_P=memcmp
    -Ofast
    -ffast-math
    -mstrict-align
    -Wno-deprecated-declarations
    -Wno-attributes
    -g
)
