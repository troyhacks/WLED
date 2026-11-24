## wled_platform - the platform layer, on ESP-IDF.
##
## This is the part of the Arduino API that is not about hardware: the clock,
## the IP address class, the filesystem, and the portable C++ the Arduino core
## is made of (String, Print, Stream).  It is here because it is what WLED's
## own code needs, and because none of it has anything to do with a
## peripheral - it is ESP-IDF underneath:
##
##   vendor/   the String / Print / Stream / IPAddress classes, taken from the
##             ESP32 Arduino core (see LICENSE-NOTICE.md for the provenance)
##   include/  the headers this component owns: the clock, the basic types and
##             the core's logging macros
##   src/      the implementation, on top of the ESP-IDF drivers
##
## The names and the semantics match the Arduino core, so nothing above this
## component has to change.  The Arduino layer (components/wled_compat) is
## the other half: the hardware names.

idf_component_register(
    SRCS
        "vendor/WString.cpp"
        "vendor/Print.cpp"
        "vendor/Stream.cpp"
        "vendor/StreamString.cpp"
        "vendor/IPAddress.cpp"
        "vendor/MD5Builder.cpp"
        "vendor/HEXBuilder.cpp"
        "vendor/fs/FS.cpp"
        "vendor/fs/vfs_api.cpp"
        "vendor/fs/LittleFS.cpp"
        "src/wled_time.cpp"
        "src/arduino_stdlib.cpp"
        "src/arduino_log.cpp"
        "libb64/cencode.c"
        "libb64/cdecode.c"
    INCLUDE_DIRS
        "."          # <libb64/cencode.h> - the web server's basic auth
        "include"
        "vendor"
        "vendor/fs"
    REQUIRES
        esp_timer
        esp_rom
        esp_system
        esp_netif
        log
        lwip
        freertos
        vfs
        joltwallet__littlefs
)
