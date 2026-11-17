#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
base="$root/usr/local/share/openhd"
mkdir -p "$base/telemetry" "$base/video" "$base/interface"
printf '%s\n' '{"fc_uart_connection_type":"DEFAULT","fc_uart_baudrate":115200,"fc_uart_flow_control":false,"fc_battery_n_cells":0}' > "$base/telemetry/air_settings.json"
printf '%s\n' '{"h26x_bitrate_kbits":11000,"rpi_libcamera_ev_value":5,"streamed_video_format":{"videoCodec":"h264","width":1280,"height":720,"framerate":60}}' > "$base/video/RPIF_V3_IMX708_0.json"
printf '%s\n' '{"wb_frequency":5745,"wb_tx_power_milli_watt":25,"wb_tx_power_milli_watt_armed":0,"wb_rtl8812au_tx_pwr_idx_override":10,"wb_rtl8812au_tx_pwr_idx_override_armed":0}' > "$base/interface/wifibroadcast_settings.json"
perl -c "$HERE/air-migrate-settings.pl"
perl "$HERE/air-migrate-settings.pl" "$root"
perl -MJSON::PP -e '
  sub load { open my $f, "<", $_[0] or die $!; local $/; decode_json(<$f>) }
  my $base = shift;
  my $t = load("$base/telemetry/air_settings.json");
  die "UART changed" unless $t->{fc_uart_connection_type} eq "DEFAULT" && $t->{fc_uart_baudrate} == 115200;
  die "Missing telemetry defaults" unless $t->{fc_sys_id} == 1 && $t->{openhd_uart_priority_fc} == 1;
  my $c = load("$base/video/RPIF_V3_IMX708_0.json");
  die "Camera changed" unless $c->{h26x_bitrate_kbits} == 11000 && $c->{rpi_libcamera_ev_value} == 5 && $c->{nxp_enable_aud};
  my $r = load("$base/interface/wifibroadcast_settings.json");
  die "Radio changed" unless $r->{wb_frequency} == 5745 && $r->{wb_tx_power_idx_per_card}[0] == 10 && $r->{wb_tx_power_mw_per_card}[0] == 25;
' "$base"
before=$(find "$base" -type f -exec sha256sum {} + | sort)
perl "$HERE/air-migrate-settings.pl" "$root"
after=$(find "$base" -type f -exec sha256sum {} + | sort)
test "$before" = "$after"
for file in "$HERE"/air-*.sh; do bash -n "$file"; done
echo '===MIGRATION_FIXTURES_AND_SHELL_SYNTAX_OK==='