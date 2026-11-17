#!/usr/bin/env bash
# Offline ARMhf-only compile/test. No services, radios, OpenHD or sysfs mounts.
set -euo pipefail
ROOT=/opt/openhd-dev/armhf-rootfs
SOURCE=$(cd "$(dirname "$0")" && pwd)
test "$(id -u)" = 0
STAGE=$(mktemp -d "$ROOT/tmp/odid-bluez.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
cp "$SOURCE/BluezBackend.h" "$SOURCE/BluezBackend.cpp" \
   "$SOURCE/BluezBackend_test.cpp" "$STAGE/"
INNER=${STAGE#"$ROOT"}
chroot "$ROOT" /bin/bash -s -- "$INNER" <<'BUILD'
set -euo pipefail
cd "$1"
pkg-config --modversion gio-2.0
# Production translation unit: validate headers/API without enabling test seams.
g++ -std=c++17 -Wall -Wextra -Werror -pthread $(pkg-config --cflags gio-2.0) \
    -fsyntax-only BluezBackend.cpp
g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -fsanitize=undefined \
    BluezBackend_test.cpp $(pkg-config --cflags --libs gio-2.0 gio-unix-2.0) -o bluez-test
env UBSAN_OPTIONS=halt_on_error=1 G_DEBUG=fatal-warnings ./bluez-test
BUILD