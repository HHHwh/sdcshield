#!/usr/bin/perl
# CORE145 LSU-hypothesis discriminator, quantitative part 1:
# If a load-path fault corrupted the divisor double d->d* while it sat in
# a register (post-load), the corrupting event is a BIT FLIP => divisor bits
# = correct bits XOR small mask. If instead the load SUBSTITUTED an alien
# value (store-buffer forwarding / bypass), divisor = unrelated double.
# Observable: column scaling factor k = 1/(corrupt_div/correct_div).
#   FAIL9: k = 2^-15 EXACTLY for 600/600 halves incl. re-rounding => divisor
#          exponent-only corruption, mantissa preserved through recompute.
#          For ANY alien double A, k = correct_div/A exactly a power of 2^-15
#          requires A = correct_div * 2^15 with identical mantissa ->
#          probability of a random in-flight double matching = ~0 unless A
#          literally IS the correct divisor with exponent +15.
#   FAIL8: k = 24145.491339 (not a power of 2). Test: is the implied
#          corrupted divisor = correct * 2^15.56...? We can bound: k constant
#          to 2.7e-10 over 598 components means single-scalar corruption;
#          XOR popcount of column elements is 15-40 bits (typical of two
#          unrelated doubles), BUT the CONSTANCY of ratio across elements
#          means the corruption is upstream of all elements -> single scalar.
# Here: measure FAIL8 ratio constancy precisely + implied corrupted-divisor
# relation, and FAIL8 col11 (which has ~1e-7 spread, secondary effects).
use strict; use warnings;
my ($file)=@ARGV;
sub read_dump { my ($s,$e)=@_; open my $fh,'<',$file or die; my $ln=0; my $buf='';
  while (<$fh>) { $ln++; next if $ln<$s; last if $ln>$e; my @p=/\b([0-9a-f]{2})\b/g; $buf.=join('',@p);} return $buf; }
my $a=read_dump(642514,687513); my $g=read_dump(687515,732514);
for my $col (11,38,39) {
  my @rs; my $n=0;
  for my $row (1..299) {
    my $el=$col*300+$row;
    for my $h (0,16) {
      my $av=unpack('d<',pack('H*',substr($a,$el*32+$h,16))); my $gv=unpack('d<',pack('H*',substr($g,$el*32+$h,16)));
      next if $av==0||$gv==0;
      push @rs,$av/$gv; $n++;
    }
  }
  my $mn=$rs[0]; my $mx=$rs[0]; for(@rs){$mn=$_ if $_<$mn; $mx=$_ if $_>$mx;}
  my $mean=0; $mean+=$_ for @rs; $mean/=$n;
  my $var=0; $var+=($_-$mean)**2 for @rs; my $sd=sqrt($var/$n);
  my $lg=log($mean)/log(2); my $frac=$lg-int($lg);
  printf "col %2d: n=%d ratio mean=%.12g min=%.12g max=%.12g relspread=%.3g sd=%.3g log2(mean)=%.6f (frac=%.6f)\n",
    $col,$n,$mean,$mn,$mx,($mx-$mn)/abs($mean),$sd,$lg,$frac;
}
