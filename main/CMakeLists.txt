## The ESP-IDF main component for WLED-MM-P4.
##
## This is where app_main() lives.  The Arduino build gets its app_main() from
## the Arduino core (cores/esp32/main.cpp); without the core we own it.
##
## The WLED application itself is the `wled00` component, which the root
## CMakeLists.txt adds to EXTRA_COMPONENT_DIRS.

idf_component_register(
    SRCS "wled_idf.cpp"
    INCLUDE_DIRS "."
    REQUIRES wled00
)
