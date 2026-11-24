## wled_compat - the Arduino API layer for WLED-MM-P4, backed by ESP-IDF.
##
## There is no Arduino core in this firmware.  This component provides the
## Arduino API that the WLED sources and the Arduino libraries use:
##
##   vendor/   the AsyncUDP and ESPmDNS classes, taken from the ESP32
##             Arduino core (see LICENSE-NOTICE.md for the provenance)
##   include/  the headers the Arduino core used to ship
##   src/      the implementation, on top of the ESP-IDF drivers
##
## What is here is the hardware: the GPIO, LEDC, ADC, UART, I2C, SPI, Ethernet
## and USB-Serial-JTAG names, and the console's Arduino.h.  The parts of the
## Arduino API that are not about hardware - the clock, the IP address class,
## the filesystem, String/Print/Stream - are the platform layer's
## (components/wled_platform).
##
## The names and the semantics match the Arduino core, so nothing above this
## component has to change.

idf_component_register(
    SRCS
        "vendor/AsyncUDP.cpp"
        "vendor/ESPmDNS.cpp"
        "src/arduino_gpio.cpp"
        "src/arduino_pwm.cpp"
        "src/arduino_adc.cpp"
        "src/arduino_pins.cpp"
        "src/arduino_random.cpp"
        "src/arduino_serial.cpp"
        "src/arduino_wire.cpp"
        "src/arduino_spi.cpp"
        "src/arduino_udp.cpp"
        "src/arduino_eth.cpp"
        "src/arduino_misc.cpp"
        "src/arduino_events.cpp"
        "src/arduino_tcp.cpp"
        "src/arduino_update.cpp"
        "src/arduino_cpu.cpp"
    INCLUDE_DIRS
        "."
        "include"
        "vendor"
    REQUIRES
        wled_platform
        esp_timer
        esp_system
        esp_netif
        esp_eth
        heap
        lwip
        freertos
        vfs
        joltwallet__littlefs
        nvs_flash
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_driver_i2c
        esp_driver_spi
        espressif__mdns
        esp_adc
        spi_flash
        app_update
        mbedtls
        esp_hw_support
)
