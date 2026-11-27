## The WLED application component.
##
## The same sources the Arduino build compiles, as an ESP-IDF component.  The
## Arduino build picks them with `build_src_filter`; under ESP-IDF PlatformIO
## ignores src_filter and takes the list from here, so the two have to be kept
## in step.  The exclusions below are the ones the Arduino build's filter
## makes:
##
##   -<**/dmx*>              dmx_input.cpp, dmx_output.cpp
##   -<**/dependencies/dmx>  the vendored DMX library
##   -<**/DS1307RTC*>        the RTC library
##
## wled00.ino is not here: CMake cannot compile a .ino, and the IDF entry
## point is main/wled_idf.cpp.  The Arduino build still uses the .ino.
##
## log_wrap_shim.c is not here either.  It defines __wrap_esp_log_writev,
## which nothing wraps, and it is known to make every ESP_LOG call hang.
##
## The list is the same for every chip, with one exception: the ESP32-P4's
## own hardware.  The chip is IDF_TARGET - the CMake variable the IDF sets
## from the environment - so the split is made here, where the list is,
## and not in the source files.
##
##   - the PIE vector assembly (particle_simd.S) and the 16-pixel multiply
##     (p4_mul16x16.S) are P4 instructions, and the S31 is a RISC-V chip
##     without them
##   - the HDMI display path is the P4's, and so is the hosted Wi-Fi
##     coprocessor

set(WLED_APP_SRCS
    "wled_idf.cpp"
    "ArtNetReceiver.cpp"
    "FX.cpp"
    "FX_2Dfcn.cpp"
    "FX_fcn.cpp"
    "FXparticleSystem.cpp"
    "FastUDP.cpp"
    "ImageCacheManager.cpp"
    "ar_publish.cpp"
    "bus_colorlight.cpp"
    "bus_manager.cpp"
    "button.cpp"
    "cfg.cpp"
    "colorlight_protocol.cpp"
    "colors.cpp"
    "e131.cpp"
    "event_bus.cpp"
    "event_log.cpp"
    "file.cpp"
    "hub75/Hub75Panel.cpp"
    "improv.cpp"
    "json.cpp"
    "led.cpp"
    "network.cpp"
    "ntp.cpp"
    "ota_littlefs.cpp"
    "overlay.cpp"
    "parlio.cpp"
    "pin_manager.cpp"
    "playlist.cpp"
    "presets.cpp"
    "remote.cpp"
    "set.cpp"
    "src/dependencies/e131/ESPAsyncE131.cpp"
    "src/dependencies/esp_lcd_ili9488/esp_lcd_ili9488.c"
    "src/dependencies/network/Network.cpp"
    "src/dependencies/time/DateStrings.cpp"
    "src/dependencies/time/Time.cpp"
    "src/dependencies/timezone/Timezone.cpp"
    "udp.cpp"
    "um_manager.cpp"
    "usermod.cpp"
    "usermods_list.cpp"
    "util.cpp"
    "wled.cpp"
    "wled_console.cpp"
    "wled_eeprom.cpp"
    "wled_math.cpp"
    "wled_screenshot.cpp"
    "wled_serial.cpp"
    "wled_server.cpp"
    "ws.cpp"
    "xml.cpp"
)

set(WLED_APP_REQUIRES
    wled_compat
    wled_platform
    esp_netif
    esp_eth
    esp_event
    esp_timer
    esp_system
    esp_hw_support
    nvs_flash
    heap
    lwip
    freertos
    vfs
    espressif__mdns
    esp_lcd
    # The USB host driver is not in the IDF any more: from ESP-IDF 6.0
    # the USB component lives in the Component Registry, and the
    # component manager puts it in managed_components/espressif__usb.
    espressif__usb
    esp_wifi
    esp_driver_gpio
    esp_driver_ledc
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_driver_i2c
    esp_driver_spi
    esp_driver_i2s
    esp_driver_parlio
    # The HUB75 panel driver reads the GPIO matrix signals of the two
    # peripherals it drives: the LCD_CAM's i80 bus and the parallel IO
    # transmitter.
    esp_hal_lcd
    esp_hal_parlio
    esp_driver_ppa
    esp_driver_jpeg
    esp_psram
    espressif__esp_codec_dev
    espressif__esp-dsp
    mbedtls
)

# The ESP32-P4's own hardware: the display path, the hosted coprocessor,
# the Ethernet PHY and the USB mass storage host.  The S31 has a radio of
# its own, and none of these components builds there - so they are the P4's
# alone.  (The S31 has a PHY of its own, but a different one: see below.)
#
# esp_hosted 3.x registers itself under this name (not the
# espressif__esp_hosted the managed component would suggest).
if("${IDF_TARGET}" STREQUAL "esp32p4")
    list(APPEND WLED_APP_SRCS
        # The P4's PIE vector assembly and its 16-pixel multiply.
        "particle_simd.S"
        "p4_mul16x16.S"
        # The HDMI display path (the bridge, the panels, the H.264 decoder).
        "wled_hdmi.cpp"
    )
    list(APPEND WLED_APP_REQUIRES
        esp_hosted
        espressif__esp_lcd_lt8912b
        espressif__esp_h264
        # The IP101 PHY left the IDF in 6.0 - it is a registry component
        # now (espressif/ip101).
        espressif__ip101
    )
endif()

# The USB mass storage host.  It is the P4's SD-card-over-USB path, and
# the S3's as well - both of them have the USB OTG and a USB host stack
# that knows their chip.  The component does not know the S31, so the S31
# does not get it.
if("${IDF_TARGET}" STREQUAL "esp32p4" OR "${IDF_TARGET}" STREQUAL "esp32s3")
    list(APPEND WLED_APP_REQUIRES
        espressif__usb_host_msc
    )
endif()

# The ESP32-S31's own hardware: the Ethernet PHY on its board - a YT8531DC,
# a gigabit part, on the chip's RGMII interface.  Its driver is a registry
# component too (espressif/yt8531), for the same reason the IP101 is one.
if("${IDF_TARGET}" STREQUAL "esp32s31")
    list(APPEND WLED_APP_REQUIRES
        espressif__yt8531
    )
endif()

idf_component_register(
    SRCS ${WLED_APP_SRCS}
    INCLUDE_DIRS "." "hub75"
    REQUIRES ${WLED_APP_REQUIRES}
)

# The Arduino build has no such thing, so the sources that have to behave
# differently under IDF (the I2C bus registry, for one) check for this.
target_compile_definitions(${COMPONENT_LIB} PRIVATE WLED_BUILD_IDF=1)

# WLED's own code wants speed, the way the Arduino build has it: -Ofast and
# the rest.  ESP-IDF's own code - and the bootloader with it - is built for
# size, because the bootloader has to fit between the offset it is flashed at
# (0x2000) and the partition table (0x8000).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Ofast
    -funroll-loops
    -fstrict-volatile-bitfields
    -fmerge-all-constants
)
