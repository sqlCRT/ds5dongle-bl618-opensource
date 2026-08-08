#!/usr/bin/env bash
# Sipeed M0S Dock — 仅编译 .bin（用于 Dev Cube 线刷）
BOARD_TYPE=m0sdock BUILD_MODE=bin exec "$(dirname "$0")/build_macos.sh" "$@"
