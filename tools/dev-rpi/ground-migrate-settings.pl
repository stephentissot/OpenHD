#!/usr/bin/env perl
use strict;
use warnings;
use JSON::PP;

# Only these two files are migrated; in particular, never open camera settings.
my $root = shift // '';
die "Usage: $0 [fixture-root]\n" if @ARGV;
my $base = "$root/usr/local/share/openhd";
my $json = JSON::PP->new->pretty->canonical;
sub load_object {
    my ($path) = @_;
    open my $in, '<', $path or die "$path: $!";
    my $value = do { local $/; $json->decode(<$in>) };
    close $in or die "close $path: $!";
    die "Expected JSON object: $path" unless ref($value) eq 'HASH';
    return $value;
}
sub add_missing {
    my ($value, $defaults) = @_;
    for my $key (keys %$defaults) {
        $value->{$key} = $defaults->{$key} unless exists $value->{$key};
    }
}
sub save_object {
    my ($path, $value) = @_;
    die "Unfinished migration: $path.migration" if -e "$path.migration" || -l "$path.migration";
    open my $out, '>', "$path.migration" or die "$path: $!";
    print {$out} $json->encode($value) or die "write $path: $!";
    close $out or die "close $path: $!";
    my @stat = stat($path);
    chmod($stat[2] & 07777, "$path.migration") or die "chmod $path: $!";
    chown($stat[4], $stat[5], "$path.migration") or die "chown $path: $!";
    rename "$path.migration", $path or die "rename $path: $!";
}
my $telemetry_path = "$base/telemetry/ground_settings.json";
my $radio_path = "$base/interface/wifibroadcast_settings.json";
my $telemetry = load_object($telemetry_path);
my $radio = load_object($radio_path);
add_missing($telemetry, {
    fc_sys_id => 1,
    gnd_uart_flow_control => JSON::PP::false,
    openhd_uart_telemetry_connection => '',
    openhd_uart_telemetry_enabled => JSON::PP::true,
    openhd_uart_telemetry_baudrate => 115200,
    openhd_uart_telemetry_flow_control => JSON::PP::false,
    openhd_uart_priority_rc => 3,
    openhd_uart_priority_openhd => 2,
    openhd_uart_priority_fc => 1,
});
my %vectors = (
    wb_tx_power_mw_per_card => 'wb_tx_power_milli_watt',
    wb_tx_power_mw_armed_per_card => 'wb_tx_power_milli_watt_armed',
    wb_tx_power_idx_per_card => 'wb_rtl8812au_tx_pwr_idx_override',
    wb_tx_power_idx_armed_per_card => 'wb_rtl8812au_tx_pwr_idx_override_armed',
);
for my $key (keys %vectors) {
    if (exists $radio->{$key}) {
        # Preserve existing values, but refuse an unsafe partially migrated vector.
        die "Expected four radio slots: $key" unless ref($radio->{$key}) eq 'ARRAY' && @{$radio->{$key}} == 4;
    } else {
        die "Missing old power setting $vectors{$key}" unless exists $radio->{$vectors{$key}};
        $radio->{$key} = [($radio->{$vectors{$key}}) x 4];
    }
}
# Validate both inputs before writing either one. No legacy/unknown key is removed.
save_object($telemetry_path, $telemetry);
save_object($radio_path, $radio);
print "===GROUND_SETTINGS_MIGRATED_WITH_EXISTING_VALUES_PRESERVED===\n";