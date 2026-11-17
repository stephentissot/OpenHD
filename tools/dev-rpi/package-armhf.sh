#!/usr/bin/env bash
# Package the out-of-tree Release build; do not rerun upstream's in-tree make.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
bash "$HERE/build-armhf.sh"
chroot /opt/openhd-dev/armhf-rootfs /bin/bash -s <<'ARMHF'
set -euo pipefail
cd /opt/OpenHD
REV=$(git rev-parse --short=8 HEAD)
VERSION=2.7.0-$(git show -s --format=%ct HEAD)-$REV
STAGE=$(mktemp -d /out/openhd-stage.XXXXXX)
trap 'rm -rf "$STAGE"' EXIT
install -Dm755 /build/openhd/openhd "$STAGE/usr/local/bin/openhd"
install -Dm644 systemd/openhd_rpi.service "$STAGE/etc/systemd/system/openhd.service"
mkdir -p "$STAGE/DEBIAN"
install -m755 before-install.sh "$STAGE/DEBIAN/preinst"
install -m755 after-install.sh "$STAGE/DEBIAN/postinst"
# Same runtime dependency list as upstream package.sh (standard armhf).
cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: openhd
Version: $VERSION
Architecture: armhf
Maintainer: OpenHD Team
Section: net
Priority: optional
Depends: libpoco-dev, libcamera-openhd, gst-openhd-plugins, iw, nmap, aircrack-ng, i2c-tools, libv4l-dev, libusb-1.0-0, libpcap-dev, libnl-3-dev, libnl-genl-3-dev, libsdl2-2.0-0, libsodium-dev, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-plugins-ugly, gstreamer1.0-tools, gstreamer1.0-alsa, gstreamer1.0-pulseaudio
Description: OpenHD 2.7 unmodified functional baseline, local Release build
 Source revision $REV; requires a compatible SysUtils companion.
CONTROL
dpkg-deb --build --root-owner-group "$STAGE" "/out/openhd_${VERSION}_armhf.deb"
cpack --config /build/sysutils/CPackConfig.cmake -B /out
git rev-parse HEAD > /out/openhd-source-revision.txt
git -C /opt/OpenHD-SysUtils rev-parse HEAD > /out/sysutils-source-revision.txt
dpkg-query -W > /out/build-packages.tsv
ldd /build/openhd/openhd > /out/openhd-ldd.txt
ldd /build/sysutils/openhd_sys_utils > /out/sysutils-ldd.txt
cd /out
sha256sum ./*.deb > SHA256SUMS
cat SHA256SUMS
ARMHF