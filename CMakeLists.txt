cmake_minimum_required(VERSION 3.15)

include(proj.conf)

# FORCE_FS=1 env var → remove CONFIG_USB_HS so SDK sets FORCE_FS bit
if(DEFINED ENV{FORCE_FS})
    unset(CONFIG_USB_HS)
endif()

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_set_main_file(src/main.c)

target_sources(app PRIVATE
    src/bt_hid_host.c
    src/ds5_protocol.c
    src/usb_gamepad.c
    src/ds5_usb_audio.c
    src/audio.c
    src/usb_wake.c
    src/led_status.c
    src/state_mgr.c
    src/config.c
    src/dse.c
    src/remap.c
)

sdk_add_include_directories(src)
sdk_add_include_directories(${BL_SDK_BASE}/components/wireless/bluetooth/blestack/src/include)
sdk_add_include_directories(${BL_SDK_BASE}/components/wireless/bluetooth/btprofile/include)
sdk_add_include_directories(${BL_SDK_BASE}/components/utils/partition)

# Opus codec (fixed-point, encoder and decoder)
include(lib/opus.cmake)
sdk_generate_library(opuscodec)
sdk_library_add_sources(${OPUS_SOURCES})
target_compile_definitions(libopuscodec PRIVATE HAVE_CONFIG_H)
target_compile_options(libopuscodec PRIVATE
    -include${CMAKE_CURRENT_LIST_DIR}/lib/opus_config.h
)
set_source_files_properties(${OPUS_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-O3;-fbuiltin;-fno-lto;-fjump-tables"
)
set_source_files_properties(src/audio.c PROPERTIES
    COMPILE_OPTIONS "-O3"
)

set(OPUS_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/lib/opus/include
    ${CMAKE_CURRENT_LIST_DIR}/lib/opus/celt
    ${CMAKE_CURRENT_LIST_DIR}/lib/opus/silk
    ${CMAKE_CURRENT_LIST_DIR}/lib/opus/silk/fixed
    ${CMAKE_CURRENT_LIST_DIR}/lib/opus
)
target_include_directories(libopuscodec PRIVATE ${OPUS_INCLUDE_DIRS})
target_include_directories(app PRIVATE ${OPUS_INCLUDE_DIRS})

# Keep every Opus instruction in SRAM/TCM. Opus is compiled without LTO so the
# linker can reliably select libopuscodec.a members instead of anonymous LTO
# partitions. The board's original linker template remains the source of truth;
# only the archive selector below is injected into its tcm_text output section.
get_property(OPUS_BASE_LINKER_SCRIPT GLOBAL PROPERTY LINKER_SCRIPT_IN)
if(NOT EXISTS "${OPUS_BASE_LINKER_SCRIPT}")
    message(FATAL_ERROR "Cannot locate the board linker template: ${OPUS_BASE_LINKER_SCRIPT}")
endif()
file(READ "${OPUS_BASE_LINKER_SCRIPT}" OPUS_LINKER_CONTENT)
set(OPUS_TCM_TEXT_ANCHOR "        *(.tcm_code*)")
string(FIND "${OPUS_LINKER_CONTENT}" "${OPUS_TCM_TEXT_ANCHOR}" OPUS_TCM_TEXT_POS)
if(OPUS_TCM_TEXT_POS EQUAL -1)
    message(FATAL_ERROR "Cannot locate tcm_text in ${OPUS_BASE_LINKER_SCRIPT}")
endif()
string(REPLACE
    "${OPUS_TCM_TEXT_ANCHOR}"
    "${OPUS_TCM_TEXT_ANCHOR}\n\n        /* Run the complete Opus codec from SRAM/TCM. */\n        *libopuscodec.a:*.o*(.text*)"
    OPUS_LINKER_CONTENT
    "${OPUS_LINKER_CONTENT}"
)
set(OPUS_TCM_RODATA_ANCHOR "        *(.tcm_const*)")
string(FIND "${OPUS_LINKER_CONTENT}" "${OPUS_TCM_RODATA_ANCHOR}" OPUS_TCM_RODATA_POS)
if(OPUS_TCM_RODATA_POS EQUAL -1)
    message(FATAL_ERROR "Cannot locate tcm_rodata in ${OPUS_BASE_LINKER_SCRIPT}")
endif()
string(REPLACE
    "${OPUS_TCM_RODATA_ANCHOR}"
    "${OPUS_TCM_RODATA_ANCHOR}\n\n        /* Keep hot FFT, MDCT, PVQ and entropy tables off XIP flash. */\n        *libopuscodec.a:*.o*(.rodata*)\n        *libopuscodec.a:*.o*(.srodata*)"
    OPUS_LINKER_CONTENT
    "${OPUS_LINKER_CONTENT}"
)
set(OPUS_RAM_LINKER_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/generated/opus_all_ram.ld.in")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${OPUS_RAM_LINKER_SCRIPT}" "${OPUS_LINKER_CONTENT}")
sdk_set_linker_script_macro("${OPUS_RAM_LINKER_SCRIPT}")
# Board selection via env vars (mutually exclusive)
if(DEFINED ENV{BOARD_LCTECH_616})
    target_compile_definitions(app PRIVATE BOARD_LCTECH_616=1)
    message(STATUS "Target board: LCTech BL616")
elseif(DEFINED ENV{BOARD_M0S_DOCK})
    target_compile_definitions(app PRIVATE BOARD_M0S_DOCK=1)
    message(STATUS "Target board: Sipeed M0S Dock (BL616)")
else()
    message(STATUS "Target board: Ai-M61-32S-Kit (BL618)")
endif()

target_compile_definitions(app PRIVATE HAVE_CONFIG_H)
if(DEFINED ENV{FORCE_FS})
    target_compile_definitions(app PRIVATE FORCE_FS_MODE=1)
    message(STATUS "USB mode: Full-Speed (compatibility variant)")
else()
    message(STATUS "USB mode: High-Speed (default)")
endif()
target_compile_options(app PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_LIST_DIR}/lib/opus_config.h>
)

project(ds5dongle_bl618)
