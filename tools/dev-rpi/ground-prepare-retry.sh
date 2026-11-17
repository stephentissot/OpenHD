#!/usr/bin/env bash
# Preserve completed rollback evidence; never alter the exported baseline backup.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SYSTEMD_PAGER=cat PAGER=cat
test "$(id -u)" = 0
cd /home/openhd/openhd-ground-baseline
exec 9>/run/openhd-ground-deployment.lock
flock -w 10 9
test -f /boot/openhd/ground.txt && test ! -e /boot/openhd/air.txt
test -f rollback-complete && test -f backup-exported
test ! -e deployment-confirmed
test ! -e attempts/first
test "$(dpkg-query -W -f='${Version}' openhd)" = 2.6.4-202505221612-d9ed491
test "$(dpkg-query -W -f='${Version}' openhd-sys-utils)" = 0.4-12-01-2024--13-19-56
test -z "$(dpkg --audit)"
for unit in openhd openhd_sys_utils openhd-web-ui qopenhd h264_decode; do
  test "$(systemctl is-active "$unit.service" || true)" = "$(cat "backup/$unit.active")"
  test "$(systemctl is-enabled "$unit.service" || true)" = "$(cat "backup/$unit.enabled")"
done
test "$(systemctl is-active openhd-ground-rollback.service || true)" = inactive
test "$(systemctl is-active openhd-ground-rollback.timer || true)" = inactive
test "$(systemctl is-enabled openhd-ground-rollback.timer || true)" = disabled
(cd backup; sha256sum --strict -c SHA256SUMS)
tr -d '\r' < ground-bundle.SHA256SUMS | sha256sum --strict -c -
perl -MJSON::PP -e '
  local $/; my $v=decode_json(<>);
  die "Unsafe retained SysUtils config" unless
    exists $v->{firstboot} && JSON::PP::is_bool($v->{firstboot}) && !$v->{firstboot}
    && $v->{run_mode} eq "ground" && !$v->{reset_requested};
' /usr/local/share/OpenHD/SysUtils/config.json
test "$(find /usr/local/share/OpenHD -type f | wc -l)" = 1
umask 077
mkdir -p attempts/first
journalctl --no-pager -b -u openhd.service -u openhd-sys-utils.service \
  -u NetworkManager.service -u openhd-ground-install.service \
  -u openhd-ground-rollback.service > attempts/first/services.log
cp -a /usr/local/share/OpenHD/SysUtils/config.json attempts/first/safety-config.json
# Only the inert safety config and completed transaction units are removed.
# No service is stopped, and the original backup/manifest stay unchanged.
mv /etc/systemd/system/openhd-ground-rollback.service attempts/first/
mv /etc/systemd/system/openhd-ground-rollback.timer attempts/first/
rm /usr/local/share/OpenHD/SysUtils/config.json
rmdir /usr/local/share/OpenHD/SysUtils /usr/local/share/OpenHD
systemctl daemon-reload
bash ./ground-preflight.sh --check-only
for path in deployment-started installation-complete rollback-complete backup/install.log backup/rollback.log; do
  test -f "$path"
  mv "$path" attempts/first/
done
echo '===GROUND_RETRY_PREPARED_BACKUP_UNCHANGED==='