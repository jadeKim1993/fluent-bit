#!/usr/bin/perl
# Prints every request a receiver got (body as received + parsed summary)
# and writes <log>.tsv: time since t0, count, length, events, X-Batch-Id,
# Content-Type
use strict; use warnings; use JSON::PP;
my ($log, $t0, $brief) = @ARGV;
open my $out, ">", "$log.tsv" or die "tsv: $!";
open my $fh, "<", $log or do { print "    (받은 요청 없음)\n"; exit };
my $n = 0;
while (<$fh>) {
    chomp; $n++;
    my ($t, $bid, $ct, $status, $body) = split /\t/, $_, 5;
    my $rel = sprintf "%.1f", $t - $t0;
    printf "    요청 #%d  (+%ss)  응답 %s  X-Batch-Id: %s  Content-Type: %s\n", $n, $rel, $status, $bid, $ct;
    print  "      본문: $body\n" unless $brief;
    my $d = eval { decode_json($body) };
    if (!$d) { print "      → JSON 파싱 실패\n"; next }
    my ($count, @e) = ("-");
    if (ref $d eq "HASH") {
        $count = $d->{count} // "-";
        @e = map { $_->{ev} } @{ $d->{events} };
        printf "      → count=%s, events 배열 길이=%d, 순서=[%s]\n", $count, scalar @e, join(",", @e);
    } else {
        @e = map { $_->{ev} } @$d;
        printf "      → (일반 전송) 배열 길이=%d, 순서=[%s]\n", scalar @e, join(",", @e);
    }
    print $out join("\t", $rel, $count, scalar @e, join(",", @e), $bid, $ct, $status), "\n";
}
print "    (받은 요청 없음)\n" unless $n;
