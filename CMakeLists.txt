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

# Opus codec (fixed-point, encoder only)
include(lib/opus.cmake)
target_sources(app PRIVATE ${OPUS_SOURCES})
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/lib/opus/include
    ${CMAKE_CURRENT_LIST_DIR}/lib/opus/celt
    ${CMAKE_CURRENT_LIST_DIR}/lib/opus/silk
    ${CMAKE_CURRENT_LIST_DIR}/lib/opus/silk/fixed
    ${CMAKE_CURRENT_LIST_DIR}/lib/opus
)
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
