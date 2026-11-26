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

idf_component_register(
    SRCS
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
        "improv.cpp"
        "json.cpp"
        "led.cpp"
        "network.cpp"
        "ntp.cpp"
        "ota_littlefs.cpp"
        "overlay.cpp"
        "parlio.cpp"
        "particle_simd.S"
        "p4_mul16x16.S"
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
        "wled_hdmi.cpp"
        "wled_math.cpp"
        "wled_screenshot.cpp"
        "wled_serial.cpp"
        "wled_server.cpp"
        "ws.cpp"
        "xml.cpp"
    INCLUDE_DIRS "."
    REQUIRES
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
        # The IP101 PHY left the IDF in 6.0 as well - it is a registry
        # component now (espressif/ip101).
        espressif__ip101
        esp_wifi
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_driver_i2c
        esp_driver_spi
        esp_driver_i2s
        esp_driver_parlio
        esp_driver_ppa
        esp_driver_jpeg
        esp_psram
        # esp_hosted 3.x registers itself under this name (not the
        # espressif__esp_hosted the managed component would suggest).
        esp_hosted
        espressif__esp_codec_dev
        espressif__esp-dsp
        espressif__esp_lcd_lt8912b
        espressif__esp_h264
        espressif__usb_host_msc
        mbedtls
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
