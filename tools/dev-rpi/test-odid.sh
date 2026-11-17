#!/usr/bin/env bash
# Offline ARMhf verification only. Never starts OpenHD, services, UART or RF.
set -euo pipefail
ROOT=/opt/openhd-dev/armhf-rootfs
SOURCE=$(cd "$(dirname "$0")/../.." && pwd)
test "$(id -u)" = 0
for path in dev proc sys; do
  if ! mountpoint -q "$ROOT/$path"; then
    mount --rbind "/$path" "$ROOT/$path"
    mount --make-rslave "$ROOT/$path"
  fi
done
# ARM 32-bit stat cannot represent some DrvFS inode numbers. Stage only build
# inputs onto native ext4, never modify the workspace/baseline source checkout.
STAGING=/opt/openhd-dev/odid-source
mkdir -p "$STAGING" "$ROOT/opt/odid-work"
rsync -a --checksum --delete --exclude=.git "$SOURCE/OpenHD" "$SOURCE/third_party" "$STAGING/"
if mountpoint -q "$ROOT/opt/odid-work"; then
  umount "$ROOT/opt/odid-work"
fi
mount --bind "$STAGING" "$ROOT/opt/odid-work"
test "$(chroot "$ROOT" dpkg --print-architecture)" = armhf
chroot "$ROOT" cmake -S /opt/odid-work/OpenHD/ohd_telemetry/src/opendroneid \
  -B /build/odid-test -G Ninja -DCMAKE_BUILD_TYPE=Debug
chroot "$ROOT" cmake --build /build/odid-test --parallel 4
chroot "$ROOT" /build/odid-test/openhd_odid_test
if [[ ${1:-} == --ubsan ]]; then
  chroot "$ROOT" cmake -S /opt/odid-work/OpenHD/ohd_telemetry/src/opendroneid \
    -B /build/odid-ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS=-fsanitize=undefined \
    -DCMAKE_CXX_FLAGS=-fsanitize=undefined \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined
  chroot "$ROOT" cmake --build /build/odid-ubsan --parallel 4
  chroot "$ROOT" env UBSAN_OPTIONS=halt_on_error=1 /build/odid-ubsan/openhd_odid_test
fi
if [[ ${1:-} == --full ]]; then
  chroot "$ROOT" cmake -S /opt/odid-work/OpenHD -B /build/odid-full \
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENHD_ENABLE_ODID=ON -DOPENHD_ODID_BLUEZ=ON
  chroot "$ROOT" cmake --build /build/odid-full --target openhd --parallel 4
fi
if [[ ${1:-} == --off ]]; then
  chroot "$ROOT" cmake -S /opt/odid-work/OpenHD -B /build/odid-off \
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENHD_ENABLE_ODID=OFF
  chroot "$ROOT" cmake --build /build/odid-off --target openhd --parallel 4
fi