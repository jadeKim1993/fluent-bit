#!/usr/bin/perl
# Minimal HTTP receiver: logs time, X-Batch-Id, Content-Type, answer and body of every
# request, answers 200 (or the status written in <log>.mode) and closes the
# connection (like a server that drops idle keep-alive connections).
use strict; use warnings;
use IO::Socket::INET; use Time::HiRes qw(time);
my ($port, $log) = @ARGV;
my $srv = IO::Socket::INET->new(LocalAddr => "127.0.0.1", LocalPort => $port,
                                Listen => 16, ReuseAddr => 1) or die "listen: $!";
while (my $c = $srv->accept) {
    my $line = <$c>;
    next unless defined $line;
    my %h;
    while (my $l = <$c>) {
        $l =~ s/\r?\n$//;
        last if $l eq "";
        my ($k, $v) = split /:\s*/, $l, 2;
        $h{lc $k} = $v;
    }
    my ($body, $len) = ("", $h{"content-length"} // 0);
    while (length($body) < $len) {
        my $n = read($c, my $buf, $len - length($body));
        last unless $n;
        $body .= $buf;
    }
    # optional behaviour switch: echo 413 > <log>.mode (401, 503, hang, 200)
    my $mode = "200";
    if (open my $m, "<", "$log.mode") { $mode = <$m> // "200"; chomp $mode; close $m; }
    open my $fh, ">>", $log or die "log: $!";
    printf $fh "%.3f\t%s\t%s\t%s\t%s\n", time, $h{"x-batch-id"} // "-",
           $h{"content-type"} // "-", $mode eq "hang" ? "none" : $mode, $body;
    close $fh;
    if ($mode eq "hang") { sleep 3600; next; }
    my %reason = (200 => "OK", 401 => "Unauthorized", 413 => "Payload Too Large",
                  503 => "Service Unavailable");
    printf $c "HTTP/1.1 %s %s\r\nContent-Length: 0\r\n\r\n", $mode, $reason{$mode} // "Status";
    close $c;
}
