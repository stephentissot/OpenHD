#!/usr/bin/env bash
# Supplement the completed Ground preflight only; never change service state.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SYSTEMD_PAGER=cat PAGER=cat
test "$(id -u)" = 0
test "$#" = 0
cd /home/openhd/openhd-ground-baseline
exec 9>/run/openhd-ground-deployment.lock
flock -w 10 9
test ! -e deployment-started && test ! -L deployment-started
# Verify the existing backup before adding or overwriting any backup content.
(cd backup; sha256sum --strict -c SHA256SUMS)
for path in backup/h264_decode.enabled backup/h264_decode.active \
  backup/legacydecoderunit.txt backup/legacydecoder.tar.gz; do
  test ! -e "$path" && test ! -L "$path"
done
active=$(systemctl is-active h264_decode.service || true)
enabled=$(systemctl is-enabled h264_decode.service || true)
[[ "$active" = active || "$active" = inactive ]]
[[ "$enabled" = enabled || "$enabled" = disabled || "$enabled" = static || "$enabled" = indirect ]]
umask 077
systemctl --no-pager cat h264_decode.service > backup/legacydecoderunit.txt
printf '%s\n' "$enabled" > backup/h264_decode.enabled
printf '%s\n' "$active" > backup/h264_decode.active
if [[ -e /usr/local/bin/h264_decode.sh || -L /usr/local/bin/h264_decode.sh ]]; then
  tar -czpf backup/legacydecoder.tar.gz -C / usr/local/bin/h264_decode.sh
fi
(
  cd backup
  find . -maxdepth 1 -type f ! -name '*.log' ! -name SHA256SUMS -print0 | \
    sort -z | xargs -0 sha256sum > SHA256SUMS
  sha256sum --strict -c SHA256SUMS
)
echo '===GROUND_DECODER_BACKUP_SUPPLEMENT_COMPLETE_NO_SERVICE_CHANGED==='