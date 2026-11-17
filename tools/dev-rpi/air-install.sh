#!/usr/bin/env bash
# Audited Pi Air only. Must run as a detached systemd service, not an SSH job.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SYSTEMD_PAGER=cat PAGER=cat DEBIAN_FRONTEND=noninteractive
test "$(id -u)" = 0
cd /home/openhd/openhd-air-baseline
umask 077
exec > >(tee -a backup/install.log) 2>&1
test -f backup-exported
test ! -e deployment-started
test "$(dpkg-query -W -f='${Version}' openhd)" = 2.6.4-202505221612-d9ed491
test "$(dpkg-query -W -f='${Version}' libcamera-openhd)" = 1.2.7
test -f /boot/openhd/air.txt
for path in /Config /Video /config /conf /usr/local/share/OpenHD /usr/sbin/policy-rc.d \
  /etc/systemd/system/openhd-sys-utils.service /opt/space.img.air-baseline \
  /boot/settings.json /boot/openhd/settings.json /boot/openhd/reset.txt \
  /boot/openhd/ground.txt /boot/openhd/record.txt \
  /boot/openhd/scripts/custom_unmanaged_camera.sh; do
  test ! -e "$path" && test ! -L "$path"
done
(cd backup; sha256sum -c SHA256SUMS)
test -s backup/openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb
tr -d '\r' < air-bundle.SHA256SUMS | sha256sum -c -
test "$(find /sys/class/net -maxdepth 1 -name 'wlan*' | wc -l)" = 2
grep -q rtl88x2eu /sys/class/net/wlan1/device/uevent
test "$(readlink -f /sys/class/net/wlan0/device/driver | xargs basename)" = brcmfmac
test "$(readlink -f /dev/serial0)" = /dev/ttyS0
cat > /etc/systemd/system/openhd-air-rollback.service <<'EOF'
[Unit]
Description=Restore audited OpenHD Air 2.6.4 baseline
After=local-fs.target
[Service]
Type=oneshot
ExecStart=/bin/bash /home/openhd/openhd-air-baseline/air-rollback.sh
TimeoutStartSec=15min
EOF
cat > /etc/systemd/system/openhd-air-rollback.timer <<'EOF'
[Unit]
Description=Automatic rollback unless OpenHD Air migration is confirmed
[Timer]
OnActiveSec=15min
OnUnitInactiveSec=2min
AccuracySec=1s
Unit=openhd-air-rollback.service
[Install]
WantedBy=timers.target
EOF
systemctl daemon-reload
systemctl enable --now openhd-air-rollback.timer
systemctl is-active --quiet openhd-air-rollback.timer
touch deployment-started
trap 'status=$?; trap - ERR; systemctl start --no-block openhd-air-rollback.service; exit "$status"' ERR
exec 9>/run/openhd-air-deployment.lock
flock -w 10 9
# Protect the original sparse reserve file from SysUtils unconditional deletion.
if [[ -f /opt/space.img ]]; then mv /opt/space.img /opt/space.img.air-baseline; fi
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 755 /usr/sbin/policy-rc.d
ln -s /dev/null /etc/systemd/system/openhd-sys-utils.service
systemctl daemon-reload
systemctl disable --now openhd_sys_utils.service
systemctl stop openhd.service
mkdir /Config /Video
mount --bind /boot /Config
mount --bind /Videos /Video
printf '\n# OpenHD 2.7 path compatibility; no partition or filesystem changes\n/boot /Config none bind 0 0\n/Videos /Video none bind 0 0\n' >> /etc/fstab
mkdir -p /usr/local/share/OpenHD/SysUtils
cat > /usr/local/share/OpenHD/SysUtils/config.json <<'EOF'
{
  "firstboot": false,
  "run_mode": "air",
  "camera_type": 32,
  "camera2_type": 255,
  "reset_requested": false,
  "set_hostname": false,
  "disable_openhd_service": false
}
EOF
perl ./air-migrate-settings.pl
dpkg --force-confold -i ./openhd_2.7.0-1794903066-1e84d4b8_armhf.deb \
  ./openhd-sys-utils_1.0.3.20261117093806_armhf.deb
test "$(dpkg-query -W -f='${Version}' openhd)" = 2.7.0-1794903066-1e84d4b8
dpkg --audit
rm /usr/sbin/policy-rc.d /etc/systemd/system/openhd-sys-utils.service
# Journal output is essential for health verification; functional sources unchanged.
mkdir -p /etc/systemd/system/openhd.service.d
printf '[Service]\nStandardOutput=journal\nStandardError=journal\n' > /etc/systemd/system/openhd.service.d/air-baseline-log.conf
systemctl daemon-reload
systemctl enable openhd-sys-utils.service openhd.service
systemctl start openhd-sys-utils.service
systemctl start openhd.service
touch installation-complete
sync
echo '===AIR_27_INSTALLED_ROLLBACK_STILL_ARMED==='