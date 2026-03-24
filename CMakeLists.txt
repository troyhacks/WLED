cmake_minimum_required(VERSION 3.16.0)

# On Windows with Git Bash, idf_tools.py does not add the RISC-V toolchain to
# PATH (MSYSTEM detection inhibits it). Inject the toolchain bin dir into the
# cmake process PATH so compiler detection succeeds during reconfiguration.
if(WIN32)
  set(_pio_tc "$ENV{USERPROFILE}/.platformio/packages/toolchain-riscv32-esp/bin")
  if(EXISTS "${_pio_tc}")
    set(ENV{PATH} "${_pio_tc};$ENV{PATH}")
  endif()
  unset(_pio_tc)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(WLED)
