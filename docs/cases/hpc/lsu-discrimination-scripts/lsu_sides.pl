#!/usr/bin/perl
# CORE145 LSU-hypothesis discriminator, part 2:
# Golden-side vs run-side asymmetry: 12/12 failures attributed to golden side
# (test_init period) via -Y -F semantics. If a persistent LSU data-path fault
# existed, it should corrupt BOTH sides symmetrically (same memory traffic).
# Binomial p(12 golden | 12 total, p=0.5) if unbiased.
use strict; use warnings;
my $n=12; my $k=12; my $p=0.5;
my $c=1; for my $i (0..$k-1){ $c = $c*($n-$i)/($i+1); }
my $prob = $c * $p**$k;
printf "P(all %d of %d on golden side | unbiased p=0.5) = %.3g = 1/%.0f\n", $k, $n, $prob, 1/$prob;
# also the crash: loop-count 0, test_init golden computation, time-to-fail 1000.572ms
