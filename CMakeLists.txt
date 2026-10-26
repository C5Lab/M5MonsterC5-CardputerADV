cmake_minimum_required(VERSION 3.16)

# Add linker flag globally (needed for duplicate symbols in ROM)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-zmuldefs")

# Keep the ESP-IDF app descriptor in sync with the version shown by Lab5.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/main/version.h" _lab5_version_line
     REGEX "^#define[ \t]+JANOS_ADV_VERSION[ \t]+\"[^\"]+\"$")
if(NOT _lab5_version_line)
  message(FATAL_ERROR "JANOS_ADV_VERSION missing from main/version.h")
endif()
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" PROJECT_VER "${_lab5_version_line}")

# Bootstrap ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(M5MonsterC5-CardputerADV)

# Workaround for GCC ICE in esp_lcd on some toolchain builds.
idf_component_get_property(ESP_LCD_LIB esp_lcd COMPONENT_LIB)
if(ESP_LCD_LIB)
  target_compile_options(${ESP_LCD_LIB} PRIVATE -O1)
endif()

# Target board selection (adv or k132)
set(BOARD "adv" CACHE STRING "Target board: adv or k132")
string(TOLOWER "${BOARD}" BOARD_LOWER)
if(NOT (BOARD_LOWER STREQUAL "adv" OR BOARD_LOWER STREQUAL "k132"))
  message(FATAL_ERROR "Invalid BOARD: ${BOARD}. Use -DBOARD=adv or -DBOARD=k132.")
endif()
message(STATUS "Target board: ${BOARD_LOWER}")

# ===== POST-BUILD: copy binaries to binaries-esp32s3 =====
set(EXPORT_DIR "${CMAKE_SOURCE_DIR}/binaries-esp32s3/${BOARD_LOWER}")
file(MAKE_DIRECTORY "${EXPORT_DIR}")

set(GEN_APP_BIN_TARGET "gen_${CMAKE_PROJECT_NAME}_binary")
set(_copy_base_dep "")
if(TARGET ${GEN_APP_BIN_TARGET})
  set(_copy_base_dep ${GEN_APP_BIN_TARGET})
elseif(TARGET app)
  set(_copy_base_dep app)
else()
  message(WARNING "copy_firmware: no binary generation target found; disabling auto copy")
endif()

if(_copy_base_dep)
  set(COPY_DEPS ${_copy_base_dep})

  # Export on every build: the IDF binary target is phony, and a stamp can
  # remain newer than a freshly regenerated app image during incremental builds.
  add_custom_target(copy_firmware ALL
    COMMAND ${CMAKE_COMMAND}
            -DDEST="${EXPORT_DIR}"
            -DBLD="${CMAKE_BINARY_DIR}"
            -DAPP="${PROJECT_NAME}.bin"
            -DBOARD="${BOARD_LOWER}"
            -DSRC="${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_SOURCE_DIR}/post_build.cmake"
    DEPENDS ${COPY_DEPS}
    COMMENT "Copying firmware binaries to ${EXPORT_DIR}"
  )
  add_dependencies(copy_firmware ${COPY_DEPS})
endif()
