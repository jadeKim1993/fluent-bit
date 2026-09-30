#!/usr/bin/perl
# Compares the events sent (one per line in <sent>) with the events found in
# the requests a receiver accepted (answered 200). Prints sent / received / missing / duplicated
# and exits 0 only when every event arrived exactly once.
use strict; use warnings; use JSON::PP;
my ($sent_file, $log) = @ARGV;
my (%sent, %got);
open my $s, "<", $sent_file or die "sent: $!";
while (<$s>) { chomp; $sent{$_} = 1 if length }
if (open my $fh, "<", $log) {
    while (<$fh>) {
        chomp;
        my (undef, undef, undef, $status, $body) = split /\t/, $_, 5;
        next unless $status eq "200";          # rejected requests were not received
        my $d = eval { decode_json($body) } or next;
        my @ev = ref $d eq "HASH" ? @{ $d->{events} } : @$d;
        $got{$_->{ev}}++ for @ev;
    }
}
my @missing = grep { !$got{$_} } sort keys %sent;
my @dups = grep { $got{$_} > 1 } sort keys %got;
my $recv = scalar grep { $sent{$_} } keys %got;
printf "보낸 이벤트 %d건, 받은 이벤트 %d건, 유실 %d건, 중복 %d건%s\n",
       scalar keys %sent, $recv, scalar @missing, scalar @dups,
       @missing ? " (유실 예: " . join(",", @missing[0 .. ($#missing < 4 ? $#missing : 4)]) . ")" : "";
exit(@missing || @dups ? 1 : 0);
