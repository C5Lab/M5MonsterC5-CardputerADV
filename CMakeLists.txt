cmake_minimum_required(VERSION 3.16)

# Add linker flag globally (needed for duplicate symbols in ROM)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-zmuldefs")

# Bootstrap ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(M5MonsterC5-CardputerADV)

# ===== POST-BUILD: copy binaries to binaries-esp32s3 =====
set(EXPORT_DIR "${CMAKE_SOURCE_DIR}/binaries-esp32s3")
file(MAKE_DIRECTORY "${EXPORT_DIR}")

set(COPY_STAMP "${CMAKE_BINARY_DIR}/copy_bins.stamp")
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

  add_custom_command(
    OUTPUT "${COPY_STAMP}"
    COMMAND ${CMAKE_COMMAND}
            -DDEST="${EXPORT_DIR}"
            -DBLD="${CMAKE_BINARY_DIR}"
            -DAPP="${PROJECT_NAME}.bin"
            -P "${CMAKE_SOURCE_DIR}/post_build.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${COPY_STAMP}"
    DEPENDS ${COPY_DEPS}
    COMMENT "Copying firmware binaries to ${EXPORT_DIR}"
  )

  add_custom_target(copy_firmware ALL DEPENDS "${COPY_STAMP}")
  add_dependencies(copy_firmware ${COPY_DEPS})
endif()
