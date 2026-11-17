#!/usr/bin/env bash
# Run as root under Ubuntu WSL, never on a Raspberry Pi.
set -euo pipefail
ROOT=/opt/openhd-dev/armhf-rootfs
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
test "$(id -u)" = 0
test "$(uname -m)" = x86_64
grep -q 'VERSION_CODENAME=bullseye' "$ROOT/etc/os-release"
test "$(chroot "$ROOT" dpkg --print-architecture)" = armhf

# Windows checkouts may use CRLF: normalize the executable shebang on install.
tr -d '\r' < "$HERE/policy-rc.d" > "$ROOT/usr/sbin/policy-rc.d"
chmod 755 "$ROOT/usr/sbin/policy-rc.d"
cp -L /etc/resolv.conf "$ROOT/etc/resolv.conf"
cp /etc/ssl/certs/ca-certificates.crt "$ROOT/etc/ssl/certs/ca-certificates.crt"
for path in dev proc sys; do
  if ! mountpoint -q "$ROOT/$path"; then
    mount --rbind "/$path" "$ROOT/$path"
    mount --make-rslave "$ROOT/$path"
  fi
done
mkdir -p "$ROOT/opt/OpenHD" "$ROOT/opt/OpenHD-SysUtils" "$ROOT/out"
mountpoint -q "$ROOT/opt/OpenHD" || mount --bind /opt/openhd-dev/source "$ROOT/opt/OpenHD"
mountpoint -q "$ROOT/opt/OpenHD-SysUtils" || mount --bind /opt/openhd-dev/sysutils-source "$ROOT/opt/OpenHD-SysUtils"

KEY=/opt/openhd-dev/downloads/openhd-release.key
curl -fL --retry 3 https://dl.cloudsmith.io/public/openhd/release/gpg.8F544FDF656561E4.key -o "$KEY"
gpg --show-keys --with-fingerprint "$KEY"
gpg --batch --yes --dearmor -o "$ROOT/usr/share/keyrings/openhd-release-archive-keyring.gpg" "$KEY"
install -m 644 /opt/openhd-dev/downloads/openhd-release.list "$ROOT/etc/apt/sources.list.d/openhd-release.list"
chroot "$ROOT" /usr/bin/env DEBIAN_FRONTEND=noninteractive dpkg --configure -a
chroot "$ROOT" apt-get update
chroot "$ROOT" apt-cache policy libcamera-openhd gst-openhd-plugins openhd-sys-utils
chroot "$ROOT" /usr/bin/env DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-downgrades --no-install-recommends \
  build-essential cmake ninja-build pkg-config git ca-certificates \
  libpoco-dev libusb-1.0-0-dev libpcap-dev libsodium-dev \
  libnl-3-dev libnl-genl-3-dev libnl-route-3-dev libsdl2-dev \
  libgstreamer-plugins-base1.0-dev libv4l-dev libcamera-openhd=1.2.7 ruby-dev