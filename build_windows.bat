@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem ============================================================
rem  DS5Dongle BL618 - Windows build script
rem
rem  Usage: build_windows.bat [build|rebuild|clean|flash [COMx]]
rem    build   - incremental build (default)
rem    rebuild - clean + full build
rem    clean   - remove build directory
rem    flash   - flash via serial, e.g. build_windows.bat flash COM5
rem
rem  Environment overrides:
rem    BL_SDK_BASE    path to the DS5Dongle BL618 SDK fork
rem    TOOLCHAIN_PATH path to the T-Head Windows toolchain
rem    BOARD_TYPE     lctech616 (default) | aim61 | m0sdock
rem    USB_SPEED      fs (default) | hs
rem ============================================================

if "%BL_SDK_BASE%"=="" set "BL_SDK_BASE=%~dp0..\bouffalo_sdk"
if "%TOOLCHAIN_PATH%"=="" set "TOOLCHAIN_PATH=%USERPROFILE%\Desktop\toolchain_gcc_t-head_windows"
if "%BOARD_TYPE%"=="" set "BOARD_TYPE=lctech616"
if "%USB_SPEED%"=="" set "USB_SPEED=fs"
if "%NUMBER_OF_PROCESSORS%"=="" set "NUMBER_OF_PROCESSORS=8"

rem ---- sanity checks ----
if not exist "%BL_SDK_BASE%\project.build" (
    echo [ERROR] SDK not found at "%BL_SDK_BASE%"
    echo         Set BL_SDK_BASE to the DS5Dongle BL618 SDK fork ^(see README step 1^).
    exit /b 1
)
if not exist "%TOOLCHAIN_PATH%\bin\riscv64-unknown-elf-gcc.exe" (
    echo [ERROR] RISC-V toolchain not found at "%TOOLCHAIN_PATH%"
    echo         git clone https://gitee.com/bouffalolab/toolchain_gcc_t-head_windows.git
    echo         or set TOOLCHAIN_PATH to override.
    exit /b 1
)

rem ---- board / usb mapping ----
set "BOARD_LCTECH_616="
set "BOARD_M0S_DOCK="
if /i "%BOARD_TYPE%"=="m0sdock" (
    set "BOARD_M0S_DOCK=1"
) else if /i "%BOARD_TYPE%"=="aim61" (
    set "BOARD_LCTECH_616="
    set "BOARD_M0S_DOCK="
) else (
    set "BOARD_TYPE=lctech616"
    set "BOARD_LCTECH_616=1"
)
set "FORCE_FS="
if /i "%USB_SPEED%"=="fs" (
    set "FORCE_FS=1"
) else (
    set "USB_SPEED=hs"
)

rem ---- PATH ----
set "PATH=%TOOLCHAIN_PATH%\bin;%BL_SDK_BASE%\tools\make;%BL_SDK_BASE%\tools\cmake\bin;%BL_SDK_BASE%\tools\ninja;%PATH%"

set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=build"

if /i "%ACTION%"=="clean"  goto :clean
if /i "%ACTION%"=="rebuild" goto :rebuild
if /i "%ACTION%"=="flash"   goto :flash

:build
echo [build] Target: %BOARD_TYPE%  USB: %USB_SPEED%
make -j%NUMBER_OF_PROCESSORS%
if errorlevel 1 exit /b 1
call :copy_output
goto :done

:rebuild
if exist build make clean 2>nul
make -j%NUMBER_OF_PROCESSORS%
if errorlevel 1 exit /b 1
call :copy_output
goto :done

:clean
make clean
goto :done

:flash
set "COMX=%~2"
if "%COMX%"=="" set "COMX=COM5"
make flash COMX=%COMX%
goto :done

:copy_output
rem copy build outputs to firmware/<board>/ like the macOS script
set "OUT_DIR=firmware\%BOARD_TYPE%"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
copy /y "build\build_out\ds5dongle_bl618_bl616.bin" "%OUT_DIR%\ds5dongle-%BOARD_TYPE%.bin" >nul
copy /y "build\build_out\boot2_bl616_isp_release_v8.1.8.bin" "%OUT_DIR%\" >nul
copy /y "build\build_out\partition.bin" "%OUT_DIR%\" >nul
echo [build] Output copied to %OUT_DIR%\
goto :eof

:done
echo [build] Done.
exit /b 0
