#!/usr/bin/env bash
# Run inside the uploaded bundle on the Air Pi; does not stop/start services.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SYSTEMD_PAGER=cat PAGER=cat
test "$(id -u)" = 0
test "$(dpkg --print-architecture)" = armhf
grep -q 'Raspberry Pi Zero 2' /proc/device-tree/model
test -f /boot/openhd/air.txt
test ! -f /boot/openhd/ground.txt
test "$(dpkg-query -W -f='${Version}' openhd)" = 2.6.4-202505221612-d9ed491
test "$(dpkg-query -W -f='${Version}' libcamera-openhd)" = 1.2.7
test -c /dev/serial0
test ! -e /usr/local/share/OpenHD
test ! -e /run/openhd-air-baseline
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$HERE"
tr -d '\r' < air-bundle.SHA256SUMS | sha256sum -c -
test "$(df --output=avail -k / | tail -1)" -gt 350000
test ! -e backup
mkdir -m 700 backup
umask 077
exec > >(tee backup/preflight.log) 2>&1
dpkg-query -W > backup/packages.tsv
systemctl --no-pager cat openhd.service openhd_sys_utils.service > backup/units.txt
for unit in openhd openhd_sys_utils openhd-web-ui; do
  systemctl is-enabled "$unit.service" > "backup/$unit.enabled" || true
  systemctl is-active "$unit.service" > "backup/$unit.active" || true
done
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT > backup/partitions.txt
ip -br address > backup/interfaces.txt
tar --exclude='usr/local/share/openhd/web-ui' -czpf backup/configuration.tar.gz \
  -C / boot/openhd boot/config.txt boot/cmdline.txt etc/fstab etc/modules \
  etc/systemd/system etc/NetworkManager/system-connections usr/local/share/openhd
dpkg-deb -x dpkg-repack_1.47_all.deb repack
dpkg-deb -x libdpkg-perl_1.20.13+rpi1_all.deb repack
(
  cd backup
  PERL5LIB="$HERE/repack/usr/share/perl5" perl "$HERE/repack/usr/bin/dpkg-repack" \
    --tag=none openhd openhd-sys-utils
  # dpkg-repack can report a per-package failure yet return success.
  test -s openhd_2.6.4-202505221612-d9ed491_armhf.deb
  test -s openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb
  sha256sum ./*.deb configuration.tar.gz > SHA256SUMS
  sha256sum -c SHA256SUMS
)
mkdir incoming
dpkg-deb -x openhd_2.7.0-1794903066-1e84d4b8_armhf.deb incoming
dpkg-deb -x openhd-sys-utils_1.0.3.20261117093806_armhf.deb incoming
for binary in openhd openhd_sys_utils; do
  ldd "incoming/usr/local/bin/$binary" > "backup/$binary.ldd"
  cat "backup/$binary.ldd"
  if grep -q 'not found' "backup/$binary.ldd"; then
    echo "Missing runtime library: aborting before any installation" >&2
    exit 1
  fi
done
incoming/usr/local/bin/openhd_sys_utils --version
apt-get --simulate --no-download install \
  ./openhd_2.7.0-1794903066-1e84d4b8_armhf.deb \
  ./openhd-sys-utils_1.0.3.20261117093806_armhf.deb > backup/apt-simulation.txt
cat backup/apt-simulation.txt
echo '===SETTINGS_FOR_MIGRATION==='
cat /usr/local/share/openhd/telemetry/air_settings.json
cat /usr/local/share/openhd/video/air_camera_generic.json
cat /usr/local/share/openhd/interface/wifibroadcast_settings.json
cat /usr/local/share/openhd/video/RPIF_V3_IMX708_0.json
echo '===BACKUP_AND_PREFLIGHT_COMPLETE_NO_SERVICE_CHANGED==='