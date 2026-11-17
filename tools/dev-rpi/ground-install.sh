#!/usr/bin/env bash
# Launch via a detached systemd service. Never execute as an SSH foreground job.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SYSTEMD_PAGER=cat PAGER=cat DEBIAN_FRONTEND=noninteractive
test "$(id -u)" = 0
cd /home/openhd/openhd-ground-baseline
grep -Eq '/system.slice/[^/]+\.service(/|$)' /proc/self/cgroup
exec 9>/run/openhd-ground-deployment.lock
flock -w 10 9
test -f backup-exported
for path in deployment-started deployment-confirmed installation-complete rollback-complete; do
  test ! -e "$path" && test ! -L "$path"
done
bash ./ground-preflight.sh --check-only
(cd backup; sha256sum --strict -c SHA256SUMS)
test -s backup/openhd_2.6.4-202505221612-d9ed491_armhf.deb
test -s backup/openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb
test "$(dpkg-query -W -f='${Version}' qopenhd)" = "$(cat backup/qopenhd.version)"
# Do not silently deploy against service states different from the exported backup.
for unit in openhd openhd_sys_utils openhd-web-ui qopenhd h264_decode; do
  case "$(cat "backup/$unit.active")" in
    active|inactive) ;; *) echo "Unsupported saved active state: $unit" >&2; exit 1 ;;
  esac
  case "$(cat "backup/$unit.enabled")" in
    enabled|disabled|static|indirect) ;; *) echo "Unsupported saved enable state: $unit" >&2; exit 1 ;;
  esac
  test "$(systemctl is-active "$unit.service" || true)" = "$(cat "backup/$unit.active")"
  test "$(systemctl is-enabled "$unit.service" || true)" = "$(cat "backup/$unit.enabled")"
done
umask 077
exec > >(tee -a backup/install.log) 2>&1
cat > /etc/systemd/system/openhd-ground-rollback.service <<'EOF'
[Unit]
Description=Restore audited OpenHD Ground 2.6.4 baseline
After=local-fs.target
[Service]
Type=oneshot
ExecStart=/bin/bash /home/openhd/openhd-ground-baseline/ground-rollback.sh
TimeoutStartSec=15min
EOF
cat > /etc/systemd/system/openhd-ground-rollback.timer <<'EOF'
[Unit]
Description=Automatic rollback unless OpenHD Ground migration is confirmed
[Timer]
OnActiveSec=15min
OnUnitInactiveSec=2min
AccuracySec=1s
Unit=openhd-ground-rollback.service
[Install]
WantedBy=timers.target
EOF
systemctl daemon-reload
systemctl enable --now openhd-ground-rollback.timer
systemctl is-active --quiet openhd-ground-rollback.timer
trap 'status=$?; trap - ERR; systemctl start --no-block openhd-ground-rollback.service || true; exit "$status"' ERR
touch deployment-started
# All disruptive operations happen after autonomous recovery is armed.
systemctl stop h264_decode.service
systemctl disable h264_decode.service
test "$(systemctl is-active h264_decode.service || true)" = inactive
test "$(systemctl is-enabled h264_decode.service || true)" = disabled
mv /opt/space.img /opt/space.img.ground-baseline
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 755 /usr/sbin/policy-rc.d
ln -s /dev/null /etc/systemd/system/openhd-sys-utils.service
systemctl daemon-reload
systemctl disable --now openhd_sys_utils.service
systemctl stop openhd.service
# QOpenHD stays running and disabled; neither its package nor its config is changed.
systemctl is-active --quiet qopenhd.service
mkdir /Config /Video
mount --bind /boot /Config
mount --bind /Videos /Video
printf '\n# OpenHD Ground 2.7 bind compatibility; no filesystem changes\n/boot /Config none bind 0 0\n/Videos /Video none bind 0 0\n' >> /etc/fstab
mkdir -p /usr/local/share/OpenHD/SysUtils
cat > /usr/local/share/OpenHD/SysUtils/config.json <<'EOF'
{
  "firstboot": false,
  "run_mode": "ground",
  "camera_type": 255,
  "camera2_type": 255,
  "reset_requested": false,
  "set_hostname": false,
  "disable_openhd_service": false
}
EOF
perl ./ground-migrate-settings.pl
dpkg --force-confold -i ./openhd_2.7.0-1794903066-1e84d4b8_armhf.deb \
  ./openhd-sys-utils_1.0.3.20261117093806_armhf.deb
test "$(dpkg-query -W -f='${db:Status-Status}:${Version}' openhd)" = installed:2.7.0-1794903066-1e84d4b8
test "$(dpkg-query -W -f='${db:Status-Status}:${Version}' openhd-sys-utils)" = installed:1.0.3.20261117093806
test "$(dpkg-query -W -f='${Version}' libcamera-openhd)" = 1.2.7
test "$(dpkg-query -W -f='${Version}' qopenhd)" = "$(cat backup/qopenhd.version)"
test -z "$(dpkg --audit)"
rm /usr/sbin/policy-rc.d /etc/systemd/system/openhd-sys-utils.service
mkdir -p /etc/systemd/system/openhd.service.d
printf '[Service]\nStandardOutput=journal\nStandardError=journal\n' > /etc/systemd/system/openhd.service.d/ground-baseline-log.conf
systemctl daemon-reload
systemctl enable openhd-sys-utils.service openhd.service
systemctl start openhd-sys-utils.service
systemctl start openhd.service
test "$(systemctl is-active h264_decode.service || true)" = inactive
test "$(systemctl is-enabled h264_decode.service || true)" = disabled
systemctl is-active --quiet qopenhd.service
test "$(systemctl is-enabled qopenhd.service || true)" = "$(cat backup/qopenhd.enabled)"
touch installation-complete
sync
echo '===GROUND_27_INSTALLED_ROLLBACK_STILL_ARMED==='