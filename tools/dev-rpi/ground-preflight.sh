#!/usr/bin/env bash
# Ground Zero 2 only. Service inspection is read-only, including --check-only.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SYSTEMD_PAGER=cat PAGER=cat DEBIAN_FRONTEND=noninteractive
test "$(id -u)" = 0
cd /home/openhd/openhd-ground-baseline
test "$#" = 0 || { test "$#" = 1 && test "$1" = --check-only; }
test "$(dpkg --print-architecture)" = armhf
. /etc/os-release
test "$VERSION_CODENAME" = bullseye
grep -q 'Raspberry Pi Zero 2' /proc/device-tree/model
test -f /boot/openhd/ground.txt
for path in /boot/openhd/air.txt /Config /Video /config /conf \
  /usr/local/share/OpenHD /usr/sbin/policy-rc.d \
  /etc/systemd/system/openhd-sys-utils.service \
  /run/systemd/system/openhd-sys-utils.service \
  /boot/reset.txt /boot/record.txt \
  /opt/space.img.ground-baseline /boot/settings.json /boot/openhd/settings.json \
  /boot/openhd/reset.txt /boot/openhd/record.txt \
  /boot/openhd/scripts/custom_unmanaged_camera.sh \
  /etc/systemd/system/openhd-ground-rollback.service \
  /etc/systemd/system/openhd-ground-rollback.timer \
  /etc/systemd/system/openhd.service.d/ground-baseline-log.conf; do
  test ! -e "$path" && test ! -L "$path"
done
test -f /opt/space.img && test ! -L /opt/space.img
test "$(systemctl show openhd-sys-utils.service -p LoadState --value)" = not-found
if awk '!/^[[:space:]]*#/ && ($2 == "/Config" || $2 == "/Video") { found=1 } END { exit !found }' /etc/fstab; then
  echo 'Unexpected pre-existing compatibility mounts in fstab' >&2; exit 1
fi
test "$(findmnt -rn -M /boot -o LABEL)" = OPENHD
test "$(findmnt -rn -M /Videos -o LABEL)" = RECORDINGS
test "$(dpkg-query -W -f='${db:Status-Status}:${Version}' openhd)" = installed:2.6.4-202505221612-d9ed491
test "$(dpkg-query -W -f='${db:Status-Status}:${Version}' openhd-sys-utils)" = installed:0.4-12-01-2024--13-19-56
test "$(dpkg-query -W -f='${db:Status-Status}:${Version}' libcamera-openhd)" = installed:1.2.7
[[ "$(dpkg-query -W -f='${db:Status-Status}:${Version}' qopenhd)" = installed:2.6.4-* ]]
test "$(find /sys/class/net -maxdepth 1 -name 'wlan*' | wc -l)" = 2
grep -q rtl88x2eu /sys/class/net/wlan0/device/uevent
[[ "$(readlink -f /sys/class/net/wlan0/device)" = */usb* ]]
test "$(basename "$(readlink -f /sys/class/net/wlan1/device/driver)")" = brcmfmac
iw dev wlan0 info | grep -Eq '^[[:space:]]*type monitor$'
ip -4 address show wlan1 | grep -q '192\.168\.3\.1/'
systemctl is-active --quiet NetworkManager.service ssh.service qopenhd.service
test "$(systemctl is-enabled qopenhd.service || true)" = disabled
# Refuse a dependency graph that would implicitly stop the running UI with core.
if systemctl show qopenhd.service -p Requires -p BindsTo -p PartOf | \
  grep -Eq '(^|[= ])(openhd|openhd_sys_utils|openhd-sys-utils)\.service([[:space:]]|$)'; then
  echo 'Unsafe QOpenHD dependency on a service being replaced' >&2; exit 1
fi
for unit in openhd openhd_sys_utils openhd-web-ui qopenhd; do
  active=$(systemctl is-active "$unit.service" || true)
  enabled=$(systemctl is-enabled "$unit.service" || true)
  [[ "$active" = active || "$active" = inactive ]]
  [[ "$enabled" = enabled || "$enabled" = disabled || "$enabled" = static || "$enabled" = indirect ]]
done
# The manifest must cover every artifact AND every Ground tool, exactly once.
perl -e '
  my %wanted = map { $_ => 1 } qw(
    dpkg-repack_1.47_all.deb libdpkg-perl_1.20.13+rpi1_all.deb
    openhd_2.7.0-1794903066-1e84d4b8_armhf.deb
    openhd-sys-utils_1.0.3.20261117093806_armhf.deb
    ground-preflight.sh ground-migrate-settings.pl ground-install.sh
    ground-rollback.sh test-ground-migration.sh);
  while (<>) { s/\r?\n\z//; /\A[0-9a-f]{64}  (\S+)\z/ or die "Bad checksum line";
    delete $wanted{$1} or die "Unexpected/duplicate artifact: $1"; }
  die "Missing checksum entries" if keys %wanted;
' ground-bundle.SHA256SUMS
tr -d '\r' < ground-bundle.SHA256SUMS | sha256sum --strict -c -
for package in openhd_2.7.0-1794903066-1e84d4b8_armhf.deb \
  openhd-sys-utils_1.0.3.20261117093806_armhf.deb; do
  test "$(dpkg-deb -f "$package" Architecture)" = armhf
done
test "$(dpkg-deb -f openhd_2.7.0-1794903066-1e84d4b8_armhf.deb Package)" = openhd
test "$(dpkg-deb -f openhd_2.7.0-1794903066-1e84d4b8_armhf.deb Version)" = 2.7.0-1794903066-1e84d4b8
test "$(dpkg-deb -f openhd-sys-utils_1.0.3.20261117093806_armhf.deb Package)" = openhd-sys-utils
test "$(dpkg-deb -f openhd-sys-utils_1.0.3.20261117093806_armhf.deb Version)" = 1.0.3.20261117093806
test "$#" = 0 || exit 0
for path in backup incoming repack deployment-started deployment-confirmed rollback-complete installation-complete; do
  test ! -e "$path" && test ! -L "$path"
done
test "$(df --output=avail -k / | tail -1)" -gt 350000
umask 077
mkdir -m 700 backup
exec > >(tee backup/preflight.log) 2>&1
dpkg-query -W > backup/packages.tsv
dpkg-query -W -f='${Version}' qopenhd > backup/qopenhd.version
systemctl --no-pager cat openhd.service openhd_sys_utils.service openhd-web-ui.service qopenhd.service > backup/units.txt
for unit in openhd openhd_sys_utils openhd-web-ui qopenhd; do
  systemctl is-enabled "$unit.service" > "backup/$unit.enabled" || true
  systemctl is-active "$unit.service" > "backup/$unit.active" || true
done
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT > backup/partitions.txt
ip -br address > backup/interfaces.txt
# Conditional UI paths avoid fatal missing-path tar errors; preserve whole trees.
paths=(boot/openhd boot/config.txt boot/cmdline.txt etc/fstab etc/modules
  etc/systemd/system etc/NetworkManager/system-connections usr/local/share/openhd)
test -d /usr/local/share/qopenhd
paths+=(usr/local/share/qopenhd)
for path in root/.config/QOpenHD root/.config/QOpenHD.conf \
  home/openhd/.config/QOpenHD home/openhd/.config/QOpenHD.conf \
  root/.config/OpenHD/QOpenHD.conf home/openhd/.config/OpenHD/QOpenHD.conf; do
  if [[ -e "/$path" || -L "/$path" ]]; then paths+=("$path"); fi
done
printf '%s\n' "${paths[@]}" > backup/configuration.paths
tar --exclude='usr/local/share/openhd/web-ui' -czpf backup/configuration.tar.gz -C / "${paths[@]}"
mkdir repack
dpkg-deb -x dpkg-repack_1.47_all.deb repack
dpkg-deb -x libdpkg-perl_1.20.13+rpi1_all.deb repack
(
  cd backup
  export PERL5LIB=/home/openhd/openhd-ground-baseline/repack/usr/share/perl5
  perl ../repack/usr/bin/dpkg-repack --tag=none openhd
  perl ../repack/usr/bin/dpkg-repack --tag=none --generate openhd-sys-utils
  mapfile -t stages < <(find . -mindepth 1 -maxdepth 1 -type d -name 'dpkg-repack.openhd-sys-utils.*')
  test "${#stages[@]}" = 1
  stage=${stages[0]}
  # Old Raspberry Pi postinst removed an X20 conffile. Never recreate it.
  if [[ -f "$stage/DEBIAN/conffiles" ]]; then
    while IFS= read -r path; do
      if [[ -e "$stage$path" || -L "$stage$path" ]]; then printf '%s\n' "$path"; fi
    done < "$stage/DEBIAN/conffiles" > "$stage/DEBIAN/conffiles.present"
    mv "$stage/DEBIAN/conffiles.present" "$stage/DEBIAN/conffiles"
  fi
  dpkg-deb -Zgzip --build "$stage" openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb
  # dpkg-repack can report a per-package failure yet return success.
  for package in openhd_2.6.4-202505221612-d9ed491_armhf.deb \
    openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb; do
    test -s "$package"
    dpkg-deb --info "$package" >/dev/null
    test "$(dpkg-deb -f "$package" Architecture)" = armhf
  done
  test "$(dpkg-deb -f openhd_2.6.4-202505221612-d9ed491_armhf.deb Package)" = openhd
  test "$(dpkg-deb -f openhd_2.6.4-202505221612-d9ed491_armhf.deb Version)" = 2.6.4-202505221612-d9ed491
  test "$(dpkg-deb -f openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb Package)" = openhd-sys-utils
  test "$(dpkg-deb -f openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb Version)" = 0.4-12-01-2024--13-19-56
)
mkdir incoming
dpkg-deb -x openhd_2.7.0-1794903066-1e84d4b8_armhf.deb incoming
dpkg-deb -x openhd-sys-utils_1.0.3.20261117093806_armhf.deb incoming
for binary in openhd openhd_sys_utils; do
  ldd "incoming/usr/local/bin/$binary" > "backup/$binary.ldd"
  cat "backup/$binary.ldd"
  if grep -q 'not found' "backup/$binary.ldd"; then
    echo 'Missing runtime library; no installation attempted' >&2; exit 1
  fi
done
# Never run the new core in preflight: only SysUtils --version is safe here.
incoming/usr/local/bin/openhd_sys_utils --version
apt-get --simulate --no-download install \
  ./openhd_2.7.0-1794903066-1e84d4b8_armhf.deb \
  ./openhd-sys-utils_1.0.3.20261117093806_armhf.deb > backup/apt-simulation.txt
cat backup/apt-simulation.txt
awk '/^(Remv|Purg) / { exit 1 } /^Inst / { if ($2 != "openhd" && $2 != "openhd-sys-utils") exit 1; n++ } END { if (n != 2) exit 1 }' backup/apt-simulation.txt
cat /usr/local/share/openhd/telemetry/ground_settings.json
cat /usr/local/share/openhd/interface/wifibroadcast_settings.json
(
  cd backup
  find . -maxdepth 1 -type f ! -name '*.log' ! -name SHA256SUMS -print0 | \
    sort -z | xargs -0 sha256sum > SHA256SUMS
  sha256sum --strict -c SHA256SUMS
)
echo '===GROUND_BACKUP_AND_PREFLIGHT_COMPLETE_NO_SERVICE_CHANGED==='