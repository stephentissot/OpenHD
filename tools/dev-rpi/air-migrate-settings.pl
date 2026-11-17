#!/usr/bin/env perl
use strict;
use warnings;
use JSON::PP;

# Optional root permits fixture tests without touching the actual system.
my $root = shift // '';
my $json = JSON::PP->new->pretty->canonical;
sub migrate {
    my ($path, $defaults) = @_;
    $path = "$root/usr/local/share/openhd/$path";
    open my $in, '<', $path or die "$path: $!";
    local $/;
    my $value = $json->decode(<$in>);
    close $in;
    die "Expected JSON object: $path" unless ref($value) eq 'HASH';
    for my $key (keys %$defaults) {
        $value->{$key} = $defaults->{$key} unless exists $value->{$key};
    }
    open my $out, '>', "$path.migration" or die "$path: $!";
    print {$out} $json->encode($value);
    close $out or die "close $path: $!";
    rename "$path.migration", $path or die "rename $path: $!";
}
migrate('telemetry/air_settings.json', {
    fc_sys_id => 1,
    openhd_uart_telemetry_connection => '',
    openhd_uart_telemetry_enabled => JSON::PP::true,
    openhd_uart_telemetry_baudrate => 115200,
    openhd_uart_telemetry_flow_control => JSON::PP::false,
    openhd_uart_priority_rc => 3,
    openhd_uart_priority_openhd => 2,
    openhd_uart_priority_fc => 1,
});
migrate('video/RPIF_V3_IMX708_0.json', {nxp_enable_aud => JSON::PP::true});
# One USB WB adapter on this audited Air. Preserve its legacy power overrides
# rather than allowing pre-filled per-card defaults to mask the old settings.
my $radio_path = "$root/usr/local/share/openhd/interface/wifibroadcast_settings.json";
open my $radio_in, '<', $radio_path or die $!;
my $radio = do { local $/; $json->decode(<$radio_in>) };
close $radio_in;
my %vectors = (
    wb_tx_power_mw_per_card => 'wb_tx_power_milli_watt',
    wb_tx_power_mw_armed_per_card => 'wb_tx_power_milli_watt_armed',
    wb_tx_power_idx_per_card => 'wb_rtl8812au_tx_pwr_idx_override',
    wb_tx_power_idx_armed_per_card => 'wb_rtl8812au_tx_pwr_idx_override_armed',
);
my %defaults;
for my $key (keys %vectors) {
    die "Missing old power setting $vectors{$key}" unless exists $radio->{$vectors{$key}};
    $defaults{$key} = [$radio->{$vectors{$key}}];
}
migrate('interface/wifibroadcast_settings.json', \%defaults);
print "===SETTINGS_MIGRATED_WITH_EXISTING_VALUES_PRESERVED===\n";