#!/usr/bin/env bash
set -euo pipefail
cd /home/openhd/openhd-air-baseline
# Restore the intact script from the verified upload before retrying.
tar --warning=no-timestamp -xzf ../openhd-air-install-bundle.tar.gz air-preflight.sh
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
tr -d '\r' < air-bundle.SHA256SUMS > "$tmp"
cat "$tmp" > air-bundle.SHA256SUMS
sudo -n bash ./air-preflight.sh