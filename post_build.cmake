if(NOT DEFINED DEST)
  message(FATAL_ERROR "post_build.cmake: DEST is not set")
endif()
if(NOT DEFINED BLD)
  message(FATAL_ERROR "post_build.cmake: BLD is not set")
endif()
if(NOT DEFINED APP)
  message(FATAL_ERROR "post_build.cmake: APP is not set")
endif()

file(MAKE_DIRECTORY "${DEST}")

file(GLOB OLD_BINS "${DEST}/*.bin")
if(OLD_BINS)
  file(REMOVE ${OLD_BINS})
endif()

set(SRCS
  "${BLD}/${APP}"
  "${BLD}/bootloader/bootloader.bin"
  "${BLD}/partition_table/partition-table.bin"
)

foreach(F ${SRCS})
  if(EXISTS "${F}")
    file(COPY "${F}" DESTINATION "${DEST}")
  else()
    message(WARNING "post_build.cmake: Missing file: ${F}")
  endif()
endforeach()

set(BOOTLOADER "${BLD}/bootloader/bootloader.bin")
set(PART_TABLE "${BLD}/partition_table/partition-table.bin")
set(APP_BIN "${BLD}/${APP}")
set(FULL_OUT "${DEST}/M5MonsterC5-CardputerADV-full.bin")

if(EXISTS "${BOOTLOADER}" AND EXISTS "${PART_TABLE}" AND EXISTS "${APP_BIN}")
  find_program(PYTHON_EXE NAMES python3 python)
  if(PYTHON_EXE)
    execute_process(
      COMMAND "${PYTHON_EXE}" -m esptool --chip esp32s3 merge_bin -o "${FULL_OUT}"
              --flash_mode dio --flash_freq 80m --flash_size 4MB
              0x0 "${BOOTLOADER}"
              0x8000 "${PART_TABLE}"
              0x10000 "${APP_BIN}"
      RESULT_VARIABLE MERGE_RESULT
      OUTPUT_VARIABLE MERGE_STDOUT
      ERROR_VARIABLE MERGE_STDERR
    )
    if(NOT MERGE_RESULT EQUAL 0)
      message(WARNING "post_build.cmake: esptool merge_bin failed (${MERGE_RESULT}). ${MERGE_STDERR}")
    endif()
  else()
    message(WARNING "post_build.cmake: python not found; skipping full image merge.")
  endif()
else()
  message(WARNING "post_build.cmake: Missing input(s) for full image merge; skipping.")
endif()
