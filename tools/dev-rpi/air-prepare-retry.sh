#!/usr/bin/env bash
# Preserve the first attempt and reuse its verified, exported baseline backup.
set -euo pipefail
export SYSTEMD_PAGER=cat PAGER=cat
test "$(id -u)" = 0
cd /home/openhd/openhd-air-baseline
test -f rollback-complete
test ! -e deployment-confirmed
test ! -e attempts/first
test "$(dpkg-query -W -f='${db:Status-Status}:${Version}' openhd)" = installed:2.6.4-202505221612-d9ed491
test "$(dpkg-query -W -f='${db:Status-Status}:${Version}' openhd-sys-utils)" = installed:0.4-12-01-2024--13-19-56
systemctl is-active --quiet openhd.service
if systemctl is-active --quiet openhd-air-rollback.timer; then exit 1; fi
(cd backup; sha256sum -c SHA256SUMS)
umask 077
mkdir -p attempts/first
journalctl --no-pager -b -u openhd.service -u openhd-sys-utils.service > attempts/first/services.log
for path in deployment-started installation-complete rollback-complete backup/install.log backup/rollback.log; do
  test -f "$path"
  mv "$path" attempts/first/
done
echo '===RETRY_PREPARED_OLD_BACKUP_UNCHANGED==='