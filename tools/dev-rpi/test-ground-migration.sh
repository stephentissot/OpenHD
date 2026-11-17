#!/usr/bin/env bash
# Local fixtures only: no services, packages, network or target writes.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
base="$root/usr/local/share/openhd"
mkdir -p "$base/telemetry" "$base/interface" "$base/video"
printf '%s\n' '{"enable_rc_over_joystick":true,"rc_over_joystick_update_rate_hz":47,"rc_channel_mapping":"3,2,1,4","gnd_uart_baudrate":57600,"gnd_uart_connection_type":"/dev/ttyUSB0","legacy_extra":6}' > "$base/telemetry/ground_settings.json"
printf '%s\n' '{"wb_frequency":5745,"wb_tx_power_milli_watt":37,"wb_tx_power_milli_watt_armed":81,"wb_rtl8812au_tx_pwr_idx_override":12,"wb_rtl8812au_tx_pwr_idx_override_armed":19,"unknown_radio":false}' > "$base/interface/wifibroadcast_settings.json"
printf '%s\n' '{"camera_must_remain":"byte-identical"}' > "$base/video/air_camera_generic.json"
printf '%s\n' 'opaque camera data' > "$base/video/RPIF_V3_IMX708_0.json"
cp "$base/telemetry/ground_settings.json" "$root/legacy-telemetry.json"
cp "$base/interface/wifibroadcast_settings.json" "$root/legacy-radio.json"
camera_before=$(find "$base/video" -type f -exec sha256sum {} + | sort)
perl -c "$HERE/ground-migrate-settings.pl"
perl "$HERE/ground-migrate-settings.pl" "$root"
perl -MJSON::PP -e '
  sub load { open my $f, "<", $_[0] or die $!; local $/; decode_json(<$f>) }
  my ($base, $root) = @ARGV;
  my $t = load("$base/telemetry/ground_settings.json");
  my $r = load("$base/interface/wifibroadcast_settings.json");
  my $json = JSON::PP->new->canonical;
  for my $pair ([$t, "$root/legacy-telemetry.json"], [$r, "$root/legacy-radio.json"]) {
    my $old = load($pair->[1]);
    for my $key (keys %$old) {
      die "Legacy value changed: $key" unless $json->encode($pair->[0]{$key}) eq $json->encode($old->{$key});
    }
  }
  die "Wrong telemetry defaults" unless $t->{fc_sys_id} == 1 &&
    JSON::PP::is_bool($t->{gnd_uart_flow_control}) && !$t->{gnd_uart_flow_control} &&
    $t->{openhd_uart_telemetry_connection} eq "" && $t->{openhd_uart_telemetry_enabled} &&
    $t->{openhd_uart_telemetry_baudrate} == 115200 && !$t->{openhd_uart_telemetry_flow_control} &&
    $t->{openhd_uart_priority_rc} == 3 && $t->{openhd_uart_priority_openhd} == 2 && $t->{openhd_uart_priority_fc} == 1;
  my %expected = (wb_tx_power_mw_per_card => 37, wb_tx_power_mw_armed_per_card => 81,
    wb_tx_power_idx_per_card => 12, wb_tx_power_idx_armed_per_card => 19);
  for my $key (keys %expected) {
    die "Expected four slots: $key" unless ref($r->{$key}) eq "ARRAY" && @{$r->{$key}} == 4;
    for (@{$r->{$key}}) { die "Slot changed: $key" unless $_ == $expected{$key}; }
  }
' "$base" "$root"
before=$(find "$base" -type f -exec sha256sum {} + | sort)
perl "$HERE/ground-migrate-settings.pl" "$root"
test "$before" = "$(find "$base" -type f -exec sha256sum {} + | sort)"
test "$camera_before" = "$(find "$base/video" -type f -exec sha256sum {} + | sort)"
# Nondefault existing new keys and all four distinct vector slots must survive.
perl -MJSON::PP -e '
  my $base = shift;
  for my $file ("$base/telemetry/ground_settings.json", "$base/interface/wifibroadcast_settings.json") {
    open my $in, "<", $file or die $!; my $v = do { local $/; decode_json(<$in>) }; close $in;
    for my $key (keys %$v) {
      if ($key =~ /_per_card$/) { $v->{$key} = [4,3,2,1]; }
      elsif ($key =~ /^(openhd_uart_|fc_sys_id|gnd_uart_flow_control)/) {
        $v->{$key} = JSON::PP::is_bool($v->{$key}) ? ($v->{$key} ? JSON::PP::false : JSON::PP::true) :
          ($key =~ /connection/ ? "/dev/custom" : 7);
      }
    }
    open my $out, ">", $file or die $!; print {$out} JSON::PP->new->pretty->canonical->encode($v); close $out;
  }
' "$base"
before=$(find "$base" -type f -exec sha256sum {} + | sort)
perl "$HERE/ground-migrate-settings.pl" "$root"
test "$before" = "$(find "$base" -type f -exec sha256sum {} + | sort)"
# An unsafe one-slot existing vector must fail before either file changes.
perl -MJSON::PP -e '
  my $file = shift; open my $in, "<", $file or die $!;
  my $v = do { local $/; decode_json(<$in>) }; close $in;
  $v->{wb_tx_power_mw_per_card} = [1];
  open my $out, ">", $file or die $!; print {$out} encode_json($v); close $out;
' "$base/interface/wifibroadcast_settings.json"
before=$(find "$base" -type f -exec sha256sum {} + | sort)
if perl "$HERE/ground-migrate-settings.pl" "$root" > "$root/rejected.log" 2>&1; then
  echo 'Unsafe vector was accepted' >&2; exit 1
fi
test "$before" = "$(find "$base" -type f -exec sha256sum {} + | sort)"
for file in "$HERE"/ground-*.sh "$HERE/test-ground-migration.sh"; do bash -n "$file"; done
# Exercise the actual preflight/rollback UI path lists without target writes.
ui_paths() {
  perl -0777 -ne '
    /for path in (\/?root\/\.config\/.*?)\; do/s or die "UI path list missing";
    my $paths = $1; $paths =~ s/\\\n/ /g;
    for (split /\s+/, $paths) { s{^/}{}; print "$_\n" if length; }
  ' "$1"
}
test "$(ui_paths "$HERE/ground-preflight.sh")" = "$(ui_paths "$HERE/ground-rollback.sh")"
mapfile -t ui < <(ui_paths "$HERE/ground-preflight.sh")
fixture="$root/ui"
mkdir -p "$fixture"
for path in root/.config/OpenHD/QOpenHD.conf home/openhd/.config/OpenHD/QOpenHD.conf; do
  printf '%s\n' "${ui[@]}" | grep -Fxq "$path"
  mkdir -p "$fixture/$(dirname "$path")"
  printf 'original UI settings\n' > "$fixture/$path"
done
printf 'unrelated settings\n' > "$fixture/root/.config/OpenHD/other.conf"
paths=()
for path in "${ui[@]}"; do
  if [[ -e "$fixture/$path" || -L "$fixture/$path" ]]; then paths+=("$path"); fi
done
tar -czpf "$root/ui.tar.gz" -C "$fixture" "${paths[@]}"
for path in "${ui[@]}"; do rm -rf -- "$fixture/$path"; done
tar -xzpf "$root/ui.tar.gz" -C "$fixture"
for path in "${paths[@]}"; do grep -Fxq 'original UI settings' "$fixture/$path"; done
grep -Fxq 'unrelated settings' "$fixture/root/.config/OpenHD/other.conf"
echo '===GROUND_MIGRATION_FIXTURES_AND_SHELL_SYNTAX_OK==='