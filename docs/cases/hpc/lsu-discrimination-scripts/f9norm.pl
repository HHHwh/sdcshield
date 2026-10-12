#!/usr/bin/perl
# FAIL9 golden col234: norm structure. Compute z = sum |x|^2 over the GOLDEN
# (expected) column, max element share, and z's exponent field — to (a) exclude
# the dominant-element load-corruption alternative, (b) check whether a -30
# exponent change of z is a clean multi-bit XOR (no carries).
use strict; use warnings;
my ($file)=@ARGV;
sub read_dump { my ($s,$e)=@_; open my $fh,'<',$file or die; my $ln=0; my $buf='';
  while (<$fh>) { $ln++; next if $ln<$s; last if $ln>$e; my @p=/\b([0-9a-f]{2})\b/g; $buf.=join('',@p);} return $buf; }
my $g=read_dump(777542,822541);
my $z=0; my $mx=0; my $n=0;
for my $row (0..299) {
  my $el=234*300+$row;
  for my $h (0,16) {
    my $v=unpack('d<',pack('H*',substr($g,$el*32+$h,16)));
    next if $v==0;
    my $e2=$v*$v; $z+=$e2; $mx=$e2 if $e2>$mx; $n++;
  }
}
printf "golden col234: n=%d z=%.17g max|x|^2=%.17g max_share=%.17g\n",$n,$z,$mx,$mx/$z;
my $zb=unpack('Q<',pack('d<',$z));
printf "z bits=0x%016x exp=0x%03x mant=0x%013x\n",$zb,($zb>>52)&0x7ff,$zb&0xfffffffffffff;
my $exp=($zb>>52)&0x7ff;
printf "exp=%d=0x%03x binary=%011b ; exp-30=0x%03x ; XOR(exp,exp-30)=0x%02x (%d bits)\n",
  $exp,$exp,$exp,$exp-30,$exp^($exp-30),scalar(my @t=($exp^($exp-30))=~/1/g);
printf "=> if z had been deflated by 2^30 (exp-30), required bit flips in exp field: %d\n",
  scalar(($exp^($exp-30))=~/1/g);
