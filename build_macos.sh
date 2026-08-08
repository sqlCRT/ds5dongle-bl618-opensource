#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ── 板型选择 ──
#   lctech616  — LCTech BL616, 默认
#   aim61      — Ai-M61-32S-Kit (BL618)
#   m0sdock    — Sipeed M0S Dock (BL616)
BOARD_TYPE="${BOARD_TYPE:-lctech616}"

# ── USB 速度 ──
#   fs  — Full-Speed 12Mbps（默认，线材兼容性好）
#   hs  — High-Speed 480Mbps（高轮询率，线材兼容性较差）
USB_SPEED="${USB_SPEED:-fs}"

# Clear all board env vars first
unset BOARD_M0S_DOCK 2>/dev/null || true
unset BOARD_LCTECH_616 2>/dev/null || true
unset FORCE_FS 2>/dev/null || true

if [[ "$USB_SPEED" == "fs" ]]; then
    export FORCE_FS=1
fi

case "$BOARD_TYPE" in
    aim61)
        echo "[build] Target: Ai-M61-32S-Kit (BL618)"
        ;;
    m0sdock)
        export BOARD_M0S_DOCK=1
        echo "[build] Target: Sipeed M0S Dock (BL616)"
        ;;
    lctech616|*)
        BOARD_TYPE="lctech616"
        export BOARD_LCTECH_616=1
        echo "[build] Target: LCTech BL616"
        ;;
esac

echo "[build] USB:  ${USB_SPEED}"

# ── 环境变量（仅脚本执行期间生效） ──
export BL_SDK_BASE="${SCRIPT_DIR}/../bouffalo_sdk"
export CHIP=bl616
export BOARD=bl616dk

# ── 工具链查找 ──
TOOLCHAIN_PREFIX=""

find_toolchain() {
    local search_dirs=(
        "$HOME/riscv-toolchain/toolchain/bin"
        "/opt/riscv-toolchain/bin"
        "/opt/riscv-toolchain/xuantie/bin"
        "${BL_SDK_BASE}/tools/toolchain_gcc_t-head_linux/bin"
        "${BL_SDK_BASE}/tools/toolchain/bin"
    )
    local prefixes=("riscv64-unknown-elf-" "riscv64-elf-")

    for dir in "${search_dirs[@]}"; do
        for prefix in "${prefixes[@]}"; do
            if [[ -x "${dir}/${prefix}gcc" ]]; then
                export PATH="${dir}:${PATH}"
                TOOLCHAIN_PREFIX="$prefix"
                echo "[build] Toolchain found: ${dir}"
                return 0
            fi
        done
    done

    for prefix in "${prefixes[@]}"; do
        if command -v "${prefix}gcc" &>/dev/null; then
            TOOLCHAIN_PREFIX="$prefix"
            return 0
        fi
    done

    return 1
}

if ! find_toolchain; then
    echo "============================================"
    echo " ERROR: RISC-V toolchain not found"
    echo ""
    echo " 请安装 riscv64-unknown-elf-gcc 或 riscv64-elf-gcc："
    echo ""
    echo "   macOS (Homebrew):"
    echo "     brew install riscv64-elf-gcc"
    echo ""
    echo "   安装后重新执行: ./build_macos.sh"
    echo "============================================"
    exit 1
fi

export CROSS_COMPILE="$TOOLCHAIN_PREFIX"

# macOS 自带 Make 3.81 不支持 $(file >) 语法，SDK 需要 4.0+
if command -v gmake &>/dev/null; then
    MAKE_CMD="gmake"
elif [[ "$(make --version 2>/dev/null | head -1)" == *"4."* ]]; then
    MAKE_CMD="make"
else
    echo "ERROR: GNU Make >= 4.0 required (macOS default is 3.81)"
    echo "  brew install make   # 安装后可用 gmake 命令"
    exit 1
fi

echo "[build] SDK:       ${BL_SDK_BASE}"
echo "[build] Toolchain: $(which ${TOOLCHAIN_PREFIX}gcc)"
echo "[build] Prefix:    ${CROSS_COMPILE}"
echo "[build] Make:      $(${MAKE_CMD} --version | head -1)"
echo "[build] Chip:      ${CHIP} / ${BOARD}"
echo ""

NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# ── 清理中间文件（保留最终固件） ──
strip_intermediates() {
    echo "[build] Stripping intermediate files..."
    find build/build_out/components build/build_out/drivers \( -name "*.obj" -o -name "*.o" \) -delete 2>/dev/null || true
    rm -rf build/build_out/lib/*.a
    rm -rf build/build_out/components/*/CMakeFiles
    rm -rf build/build_out/drivers/*/CMakeFiles
    rm -rf build/CMakeFiles
    BEFORE_SIZE=$(du -sh build/ 2>/dev/null | cut -f1)
    echo "[build] Build dir: ${BEFORE_SIZE}"
}

# ── 构建 ──
ACTION="${1:-build}"

BIN_PATH="build/build_out/ds5dongle_bl618_bl616.bin"

# Board-type + USB-speed guard: auto-clean when switching config
BUILD_KEY="${BOARD_TYPE}-${USB_SPEED}"
BOARD_STAMP="build/.board_type"
if [[ -f "$BOARD_STAMP" ]]; then
    PREV_KEY=$(cat "$BOARD_STAMP")
    if [[ "$PREV_KEY" != "$BUILD_KEY" ]]; then
        echo "[build] Config changed (${PREV_KEY} → ${BUILD_KEY}), forcing clean..."
        ${MAKE_CMD} clean 2>/dev/null || true
        rm -rf build/build_out/CMakeCache.txt build/build_out/cmake_cache 2>/dev/null || true
    fi
fi

SPEED_SUFFIX=""
if [[ "$USB_SPEED" == "hs" ]]; then
    SPEED_SUFFIX="-hs"
fi
BIN_NAME="ds5dongle-${BOARD_TYPE}${SPEED_SUFFIX}.bin"
OUT_DIR="firmware/${BOARD_TYPE}"

# ── 输出 .bin（线刷用） ──
gen_bin() {
    if [[ ! -f "$BIN_PATH" ]]; then
        echo "[build] ERROR: bin not found at ${BIN_PATH}"
        return 1
    fi

    mkdir -p "${OUT_DIR}"
    cp "$BIN_PATH" "${OUT_DIR}/${BIN_NAME}"

    # Flash package: boot2 + partition + flash config
    cp build/build_out/boot2_*.bin "${OUT_DIR}/"
    cp build/build_out/partition.bin "${OUT_DIR}/"

    cat > "${OUT_DIR}/flash_prog_cfg.ini" <<FLASH_CFG
[cfg]
# 0: no erase, 1:programmed section erase, 2: chip erase
erase = 1
# skip mode set first para is skip addr, second para is skip len, multi-segment region with ; separated
skip_mode = 0x0, 0x0
# 0: not use isp mode, #1: isp mode
boot2_isp_mode = 0

[boot2]
filedir = ./boot2_*.bin
address = 0x000000

[partition]
filedir = ./partition*.bin
address = 0xE000

[FW]
filedir = ./${BIN_NAME}
address = @partition
FLASH_CFG

    echo "[build] .bin → ${OUT_DIR}/${BIN_NAME}"
    echo "[build] Flash package → ${OUT_DIR}/"
}

# ── 执行 ──
do_build() {
    ${MAKE_CMD} CROSS_COMPILE="${CROSS_COMPILE}" -j${NPROC}
    mkdir -p build && echo "$BUILD_KEY" > "$BOARD_STAMP"
}

do_output() {
    gen_bin
}

case "$ACTION" in
    build)
        do_build
        do_output
        echo ""
        echo "[build] Done (${BOARD_TYPE}, usb=${USB_SPEED}). Output: ${OUT_DIR}/"
        ;;
    clean)
        ${MAKE_CMD} clean
        echo "[build] Cleaned."
        ;;
    flash)
        COMX="${2:-/dev/tty.usbserial*}"
        ${MAKE_CMD} flash COMX="$COMX"
        ;;
    rebuild)
        ${MAKE_CMD} clean
        rm -rf build/build_out/CMakeCache.txt build/build_out/cmake_cache 2>/dev/null || true
        do_build
        do_output
        echo ""
        echo "[build] Rebuilt (${BOARD_TYPE}, usb=${USB_SPEED}). Output: ${OUT_DIR}/"
        ;;
    strip)
        strip_intermediates
        ;;
    *)
        echo "Usage: [BOARD_TYPE=...] [USB_SPEED=hs|fs] ./build_macos.sh [build|clean|flash|rebuild|strip]"
        echo ""
        echo "  build   - 编译项目（默认）"
        echo "  clean   - 清除构建产物"
        echo "  flash   - 烧录固件 (可选: ./build_macos.sh flash /dev/tty.usbserial-xxx)"
        echo "  rebuild - 清除后重新编译"
        echo "  strip   - 仅清理中间文件（保留固件）"
        echo ""
        echo "Board types:  lctech616 (默认) | aim61 | m0sdock"
        echo ""
        echo "USB speed:"
        echo "  fs  — Full-Speed 12Mbps（默认，线材兼容性好）"
        echo "  hs  — High-Speed 480Mbps（高轮询率，线材兼容性较差）"
        echo ""
        echo "Examples:"
        echo "  ./build_macos.sh rebuild                                # FS .bin (默认)"
        echo "  USB_SPEED=hs ./build_macos.sh rebuild                   # HS .bin"
        exit 1
        ;;
esac
