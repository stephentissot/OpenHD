#!/usr/bin/env bash
# Invoked autonomously by a persistent systemd timer or installation error.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SYSTEMD_PAGER=cat PAGER=cat DEBIAN_FRONTEND=noninteractive
cd /home/openhd/openhd-air-baseline
exec 9>/run/openhd-air-deployment.lock
flock -w 120 9
test ! -e deployment-confirmed || exit 0
exec >> backup/rollback.log 2>&1
echo '===ROLLBACK_STARTED==='
(cd backup; sha256sum -c SHA256SUMS)
systemctl stop openhd-sys-utils.service openhd.service || true
if systemctl is-active --quiet openhd-sys-utils.service || systemctl is-active --quiet openhd.service; then
  echo 'Refusing restoration while a new service is still active' >&2
  exit 1
fi
systemctl disable openhd-sys-utils.service || true
ln -sfn /dev/null /etc/systemd/system/openhd-sys-utils.service
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 755 /usr/sbin/policy-rc.d
dpkg --force-confold -i \
  backup/openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb \
  backup/openhd_2.6.4-202505221612-d9ed491_armhf.deb
test "$(dpkg-query -W -f='${db:Status-Status}:${Version}' openhd)" = installed:2.6.4-202505221612-d9ed491
test "$(dpkg-query -W -f='${db:Status-Status}:${Version}' openhd-sys-utils)" = installed:0.4-12-01-2024--13-19-56
# Restore exactly the old settings tree (but keep the untouched web server).
find /usr/local/share/openhd -mindepth 1 -maxdepth 1 ! -name web-ui -exec rm -rf -- {} +
tar -xzpf backup/configuration.tar.gz -C /
# Keep firstboot=false as an inert safeguard even after the old daemon returns.
rm -f /etc/systemd/system/openhd.service.d/air-baseline-log.conf
for path in /config /conf; do
  if [[ -L "$path" && "$(readlink "$path")" = /Config ]]; then rm "$path"; fi
done
mountpoint -q /Config && umount /Config
mountpoint -q /Video && umount /Video
rmdir /Config /Video 2>/dev/null || true
if [[ -f /opt/space.img.air-baseline ]]; then mv /opt/space.img.air-baseline /opt/space.img; fi
rm -f /usr/sbin/policy-rc.d /etc/systemd/system/openhd-sys-utils.service
systemctl daemon-reload
for unit in openhd openhd_sys_utils openhd-web-ui; do
  if grep -qx enabled "backup/$unit.enabled"; then
    systemctl enable "$unit.service"
  else
    systemctl disable "$unit.service" || true
  fi
done
systemctl start NetworkManager.service
nmcli connection reload
nmcli connection up ohd_wifi_hotspot
ip -4 address show wlan0 | grep -q '192.168.3.1/'
systemctl is-active --quiet ssh.service
for unit in openhd_sys_utils openhd openhd-web-ui; do
  if grep -qx active "backup/$unit.active"; then systemctl start "$unit.service"; fi
done
touch rollback-complete
systemctl disable --now openhd-air-rollback.timer
echo '===ROLLBACK_COMPLETE_264_RESTORED==='