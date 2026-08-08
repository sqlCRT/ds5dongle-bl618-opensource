#!/usr/bin/env bash
# LCTech BL616 — 仅编译 .bin（用于 Dev Cube 线刷）
BOARD_TYPE=lctech616 BUILD_MODE=bin exec "$(dirname "$0")/build_macos.sh" "$@"
