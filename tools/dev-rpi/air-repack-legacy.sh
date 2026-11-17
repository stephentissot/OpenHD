#!/usr/bin/env bash
set -euo pipefail
cd /home/openhd/openhd-air-baseline/backup
export PERL5LIB=/home/openhd/openhd-air-baseline/repack/usr/share/perl5
perl ../repack/usr/bin/dpkg-repack --tag=none --generate openhd-sys-utils
stage=$(find . -maxdepth 1 -type d -name 'dpkg-repack.openhd-sys-utils.*' | head -1)
test -n "$stage"
# The old postinst deleted this optional X20 conffile on Raspberry Pi.
# Preserve its absence; omit missing conffiles from the reconstructed package.
while IFS= read -r path; do
  if [[ -e "$stage$path" ]]; then printf '%s\n' "$path"; fi
done < "$stage/DEBIAN/conffiles" > "$stage/DEBIAN/conffiles.present"
mv "$stage/DEBIAN/conffiles.present" "$stage/DEBIAN/conffiles"
dpkg-deb -Zgzip --build "$stage" openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb
sha256sum ./*.deb configuration.tar.gz > SHA256SUMS
sha256sum -c SHA256SUMS
test -s openhd-sys-utils_0.4-12-01-2024--13-19-56_armhf.deb
echo '===BOTH_ROLLBACK_PACKAGES_READY==='