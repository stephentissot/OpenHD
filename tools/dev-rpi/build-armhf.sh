#!/usr/bin/env bash
# Host-side driver: all compilers and libraries execute inside the ARM chroot.
set -euo pipefail
ROOT=/opt/openhd-dev/armhf-rootfs
test "$(id -u)" = 0
test "$(uname -m)" = x86_64
test "$(chroot "$ROOT" dpkg --print-architecture)" = armhf
# WSL may tear down mounts when no Linux process remains between invocations.
for path in dev proc sys; do
  if ! mountpoint -q "$ROOT/$path"; then
    mount --rbind "/$path" "$ROOT/$path"
    mount --make-rslave "$ROOT/$path"
  fi
done
mountpoint -q "$ROOT/opt/OpenHD" || mount --bind /opt/openhd-dev/source "$ROOT/opt/OpenHD"
mountpoint -q "$ROOT/opt/OpenHD-SysUtils" || mount --bind /opt/openhd-dev/sysutils-source "$ROOT/opt/OpenHD-SysUtils"
mkdir -p /opt/openhd-dev/logs
BUILD_EXTRA=()
if [[ ${OPENHD_CLEAN_BUILD:-0} == 1 ]]; then
  BUILD_EXTRA+=(--clean-first)
fi
set -o pipefail
chroot "$ROOT" cmake -S /opt/OpenHD/OpenHD -B /build/openhd -G Ninja \
  -DCMAKE_BUILD_TYPE=Release 2>&1 | tee /opt/openhd-dev/logs/openhd-configure.log
chroot "$ROOT" cmake --build /build/openhd --parallel 4 "${BUILD_EXTRA[@]}" \
  2>&1 | tee /opt/openhd-dev/logs/openhd-build.log
chroot "$ROOT" cmake -S /opt/OpenHD-SysUtils -B /build/sysutils -G Ninja \
  -DCMAKE_BUILD_TYPE=Release 2>&1 | tee /opt/openhd-dev/logs/sysutils-configure.log
chroot "$ROOT" cmake --build /build/sysutils --parallel 4 "${BUILD_EXTRA[@]}" \
  2>&1 | tee /opt/openhd-dev/logs/sysutils-build.log