#!/bin/bash
#
# build.sh - hydg02 one-command incremental build
#
# Usage:
#   ./build.sh              incremental compile (default)
#   ./build.sh clean        full rebuild
#   ./build.sh install      compile + scp to board
#   ./build.sh run          compile + scp + selftest on board
#   ./build.sh help         show this help
#
# Incremental: only recompiles .c files that changed (or whose
# headers changed).  Typical rebuild after editing one file: ~2s.
#
set -e

BASE="$(cd "$(dirname "$0")" && pwd)"
cd "$BASE"

# ---------- Toolchain detection ----------
TOOLCHAIN_BASE="/home/chh/source_code/mt7628dan_project/openwrt/staging_dir"
TOOLCHAIN_BIN=""

for tc_dir in "$TOOLCHAIN_BASE"/toolchain-mipsel_24kc_gcc-*_musl/bin; do
    if [ -x "$tc_dir/mipsel-openwrt-linux-musl-gcc" ]; then
        TOOLCHAIN_BIN="$tc_dir"
        break
    fi
done

if [ -z "$TOOLCHAIN_BIN" ]; then
    echo "Error: OpenWrt mipsel toolchain not found in:"
    echo "  $TOOLCHAIN_BASE/toolchain-mipsel_24kc_gcc-*_musl/bin"
    echo "Build the firmware first (./build.sh debug in openwrt/) to generate toolchain."
    exit 1
fi

export TOOLCHAIN_BIN
export STAGING_DIR="$(dirname "$TOOLCHAIN_BIN")/../.."
TOOLCHAIN_VER="$(basename "$(dirname "$TOOLCHAIN_BIN")")"

# ---------- Helper ----------
show_help() {
    echo "Usage: ./build.sh [command]"
    echo ""
    echo "Commands:"
    echo "  (no args)    Incremental compile (only changed files)"
    echo "  clean        Full rebuild (remove all .o/.d/binary)"
    echo "  install      Compile + scp to root@\${BOARD_IP:-192.168.50.1}:/tmp/"
    echo "  run          Compile + scp + selftest on board"
    echo "  help         This help"
    echo ""
    echo "Environment:"
    echo "  BOARD_IP     Target board IP (default: 192.168.50.1)"
    echo ""
    echo "Toolchain: $TOOLCHAIN_VER"
}

# ---------- Main ----------
case "${1:-build}" in
    build|"")
        echo "== hydg02 incremental build =="
        echo "== toolchain: $TOOLCHAIN_VER =="
        echo ""
        make -j$(nproc) 2>&1
        BINARY="hydg02"
        if [ -f "$BINARY" ]; then
            SIZE=$(stat -c %s "$BINARY")
            echo ""
            echo "== Done: $BINARY ($SIZE bytes) =="
        fi
        ;;
    clean)
        echo "== hydg02 clean =="
        make clean
        ;;
    install)
        echo "== hydg02 build + install =="
        make -j$(nproc) install
        ;;
    run)
        echo "== hydg02 build + run =="
        make -j$(nproc) run
        ;;
    help|-h|--help)
        show_help
        ;;
    *)
        echo "Unknown command: $1"
        echo ""
        show_help
        exit 1
        ;;
esac
