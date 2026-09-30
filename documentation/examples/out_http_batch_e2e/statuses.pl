#!/usr/bin/perl
# Prints the batch status events found in a fluent-bit log (stdout output,
# json_lines) and a status sequence line: "SEQ success,empty,..."
use strict; use warnings; use JSON::PP;
my ($log) = @ARGV;
my @seq;
open my $fh, "<", $log or exit;
while (<$fh>) {
    next unless /^\{.*"batch_id"/;
    my $d = eval { decode_json($_) } or next;
    push @seq, $d->{status};
    printf "    %-20s records=%-4s carried=%-4s attempts=%s http=%-4s error=%s\n",
           $d->{status}, $d->{records}, $d->{carried_over_records}, $d->{attempts},
           $d->{http_status} // "null", $d->{error} // "null";
}
print "SEQ ", join(",", @seq), "\n";
