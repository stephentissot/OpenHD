#!/usr/bin/env bash
# Autonomous timer recovery; safe to retry after an interrupted installation.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SYSTEMD_PAGER=cat PAGER=cat DEBIAN_FRONTEND=noninteractive
test "$(id -u)" = 0
cd /home/openhd/openhd-ground-baseline
exec 9>/run/openhd-ground-deployment.lock
flock -w 120 9
test ! -e deployment-confirmed || exit 0
exec >> backup/rollback.log 2>&1
echo '===GROUND_ROLLBACK_STARTED==='
(cd backup; sha256sum --strict -c SHA256SUMS)
test -s backup/openhd_2.6.4-202505221612-d9ed491_armhf.deb
test -s backup/openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb
# Stop even originally inactive services before overwriting files and UI configs.
for unit in openhd-sys-utils openhd openhd_sys_utils openhd-web-ui qopenhd; do
  systemctl stop "$unit.service" || true
  state=$(systemctl is-active "$unit.service" || true)
  case "$state" in inactive|failed|unknown) ;; *) echo "Still running: $unit ($state)" >&2; exit 1;; esac
done
systemctl disable openhd-sys-utils.service || true
ln -sfn /dev/null /etc/systemd/system/openhd-sys-utils.service
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 755 /usr/sbin/policy-rc.d
dpkg --force-confold -i \
  backup/openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb \
  backup/openhd_2.6.4-202505221612-d9ed491_armhf.deb
test "$(dpkg-query -W -f='${db:Status-Status}:${Version}' openhd)" = installed:2.6.4-202505221612-d9ed491
test "$(dpkg-query -W -f='${db:Status-Status}:${Version}' openhd-sys-utils)" = installed:0.4-12-01-2024--13-19-56
test "$(dpkg-query -W -f='${Version}' qopenhd)" = "$(cat backup/qopenhd.version)"
# Replace settings exactly, but NEVER delete the untouched web-ui server.
find /usr/local/share/openhd -mindepth 1 -maxdepth 1 ! -name web-ui -exec rm -rf -- {} +
rm -rf -- /usr/local/share/qopenhd
for path in /root/.config/QOpenHD /root/.config/QOpenHD.conf \
  /home/openhd/.config/QOpenHD /home/openhd/.config/QOpenHD.conf \
  /root/.config/OpenHD/QOpenHD.conf /home/openhd/.config/OpenHD/QOpenHD.conf; do
  rm -rf -- "$path"
done
tar -xzpf backup/configuration.tar.gz -C /
# Retain an inert safety config, even if installation failed before creating it.
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
rm -f /etc/systemd/system/openhd.service.d/ground-baseline-log.conf
for path in /config /conf; do
  if [[ -L "$path" && "$(readlink "$path")" = /Config ]]; then rm "$path"; fi
done
# Explicit if avoids set -e aborting on an already-unmounted path during retry.
# Refuse to unmount anything other than our known bind source.
for pair in /Config:/boot /Video:/Videos; do
  target=${pair%%:*}; source=${pair#*:}
  if mountpoint -q "$target"; then
    test "$target" -ef "$source"
    umount "$target"
  fi
  if [[ -d "$target" ]]; then rmdir "$target"; fi
done
if [[ -f /opt/space.img.ground-baseline ]]; then
  test ! -e /opt/space.img && test ! -L /opt/space.img
  mv /opt/space.img.ground-baseline /opt/space.img
fi
rm -f /usr/sbin/policy-rc.d /etc/systemd/system/openhd-sys-utils.service
systemctl daemon-reload
# Enabled and active are independent: Ground QOpenHD is active BUT disabled.
for unit in openhd openhd_sys_utils openhd-web-ui qopenhd; do
  case "$(cat "backup/$unit.enabled")" in
    enabled) systemctl enable "$unit.service" ;;
    disabled) systemctl disable "$unit.service" ;;
    static|indirect) ;; # Original unit files/links restored by the archive.
    *) echo "Unsupported saved enable state: $unit" >&2; exit 1 ;;
  esac
done
# Restore the local UI/core even if hotspot recovery fails afterward.
for unit in openhd_sys_utils openhd openhd-web-ui qopenhd; do
  case "$(cat "backup/$unit.active")" in
    active) systemctl start "$unit.service" ;;
    inactive) systemctl stop "$unit.service" ;;
    *) echo "Unsupported saved active state: $unit" >&2; exit 1 ;;
  esac
done
systemctl start NetworkManager.service
nmcli connection reload
nmcli connection up ohd_wifi_hotspot ifname wlan1
ip -4 address show wlan1 | grep -q '192\.168\.3\.1/'
systemctl is-active --quiet ssh.service
for unit in openhd openhd_sys_utils openhd-web-ui qopenhd; do
  test "$(systemctl is-active "$unit.service" || true)" = "$(cat "backup/$unit.active")"
  test "$(systemctl is-enabled "$unit.service" || true)" = "$(cat "backup/$unit.enabled")"
done
test -z "$(dpkg --audit)"
touch rollback-complete
systemctl disable --now openhd-ground-rollback.timer
echo '===GROUND_ROLLBACK_COMPLETE_264_RESTORED==='