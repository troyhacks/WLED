# WLED-MM-P4 - pure ESP-IDF project
#
# This is the root project file for the native ESP-IDF build of WLED-MM-P4.
# It is what `idf.py` uses, and it is also what PlatformIO's `framework = espidf`
# builder uses: the builder runs CMake with this file as the project root and
# then compiles the result with SCons.
#
# Layout
#   main/                   the ESP-IDF main component (app_main)
#   wled00/                 the WLED application component (the C++ sources)
#   components/wled_compat/ the Arduino API compatibility layer (IDF backed)
#
# PlatformIO adds `main` (its `src_dir` for the IDF envs) to
# EXTRA_COMPONENT_DIRS on the CMake command line.  We keep whatever it gave us
# and add `wled00`, so the same root file works for both drivers.

cmake_minimum_required(VERSION 3.16.0)

# Preserve anything the driver (PlatformIO) already put in EXTRA_COMPONENT_DIRS,
# then add the WLED application component.
if(DEFINED EXTRA_COMPONENT_DIRS)
    set(WLED_EXTRA_COMPONENT_DIRS "${EXTRA_COMPONENT_DIRS}")
else()
    set(WLED_EXTRA_COMPONENT_DIRS "")
endif()
list(APPEND WLED_EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/wled00")
list(REMOVE_ITEM WLED_EXTRA_COMPONENT_DIRS "")
set(EXTRA_COMPONENT_DIRS "${WLED_EXTRA_COMPONENT_DIRS}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wled_mm_p4)
