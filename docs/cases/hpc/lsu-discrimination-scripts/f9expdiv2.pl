#!/usr/bin/perl
# Wait - 307 elements in class 0x10 and 292 in class 0x11, yet ALL 600 scale
# by exactly 2^-15. Bits 52&56 both SET -> exp += +1+16 = +17, not -15.
# Verify the actual exponent fields in the CORRUPTED (actual) dump directly:
# do actual exponents equal golden exponents - 15 for ALL classes?
use strict; use warnings;
my ($file)=@ARGV;
sub read_dump { my ($s,$e)=@_; open my $fh,'<',$file or die; my $ln=0; my $buf='';
  while (<$fh>) { $ln++; next if $ln<$s; last if $ln>$e; my @p=/\b([0-9a-f]{2})\b/g; $buf.=join('',@p);} return $buf; }
my $a=read_dump(732541,777540); my $g=read_dump(777542,822541);
my ($ok,$bad,$classes)=(0,0,'');
my %cls;
for my $row (0..299) {
  my $el=234*300+$row;
  for my $h (0,16) {
    my $as=substr($a,$el*32+$h,16); my $gs=substr($g,$el*32+$h,16);
    my $av=unpack('Q<',pack('H*',$as)); my $gv=unpack('Q<',pack('H*',$gs));
    next if $gv==0;
    my $ea=($av>>52)&0x7ff; my $eg=($gv>>52)&0x7ff;
    if ($ea == $eg-15) { $ok++; } else { $bad++; printf("BAD row=%d h=%d ea=0x%03x eg=0x%03x\n",$row,$h/16,$ea,$eg) if $bad<=3; }
    $cls{($eg & 0x11)}++;
  }
}
print "actual exp == golden exp - 15: ok=$ok bad=$bad\n";
print "golden exp&0x11 classes: ", join(' ',map{sprintf("0x%02x:%d",$_,$cls{$_})} sort keys %cls),"\n";
print "=> corruption applied a UNIFORM -15 exponent shift across elements of\n";
print "   BOTH classes (0x10: 307, 0x11: 292) -> arithmetic scale by 2^-15,\n";
print "   NOT per-element xor of bits 52/56 (that gives +17 for class 0x11).\n";
