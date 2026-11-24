## wled_compat - the Arduino API layer for WLED-MM-P4, backed by ESP-IDF.
##
## There is no Arduino core in this firmware.  This component provides the
## Arduino API that the WLED sources and the Arduino libraries use:
##
##   vendor/   the String / Print / Stream / IPAddress classes, taken from the
##             ESP32 Arduino core (see LICENSE-NOTICE.md for the provenance)
##   include/  the headers the Arduino core used to ship
##   src/      the implementation, on top of the ESP-IDF drivers
##
## The names and the semantics match the Arduino core, so nothing above this
## component has to change.

idf_component_register(
    SRCS
        "vendor/WString.cpp"
        "vendor/Print.cpp"
        "vendor/Stream.cpp"
        "vendor/StreamString.cpp"
        "vendor/IPAddress.cpp"
        "src/arduino_time.cpp"
        "src/arduino_gpio.cpp"
        "src/arduino_pwm.cpp"
        "src/arduino_adc.cpp"
        "src/arduino_random.cpp"
        "src/arduino_serial.cpp"
        "src/arduino_wire.cpp"
        "src/arduino_spi.cpp"
        "src/arduino_fs.cpp"
        "src/arduino_udp.cpp"
        "src/arduino_eth.cpp"
        "src/arduino_mdns.cpp"
        "src/arduino_misc.cpp"
    INCLUDE_DIRS
        "include"
        "vendor"
    REQUIRES
        esp_timer
        esp_system
        esp_netif
        esp_eth
        heap
        lwip
        freertos
        vfs
        esp_littlefs
        nvs_flash
    PRIV_REQUIRES
        driver
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_adc
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_driver_i2c
        esp_driver_spi
        mdns
        esp_adc
)
