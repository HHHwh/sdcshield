#!/usr/bin/perl
use strict; use warnings;
my $exp=0x3fc; my $x=$exp^($exp-30); my $pc=0; for my $b (0..10){$pc++ if (($x>>$b)&1);}
printf "exp=0x%03x=%s  exp-30=0x%03x=%s  XOR=0x%03x=%s popcount=%d\n",
 $exp,sprintf("%011b",$exp),$exp-30,sprintf("%011b",$exp-30),$x,sprintf("%011b",$x),$pc;
# Also: dominant-element alternative. max share of z is 7.76% -> removing or
# corrupting ONE element changes z by <= ~8%, i.e. k within [0.96,1.04]-ish,
# NEVER 2^-15. So FAIL9's k=2^-15 cannot come from corrupting one element's
# bits in a register or on a store/load of one element. It must be a scalar
# that multiplies the whole column: the sqrt(z) divisor (or equivalent).
printf "one-element corruption bound: |k-1| <= ~%.3f (max share %.4f)\n", 0.0776, 0.0776;
