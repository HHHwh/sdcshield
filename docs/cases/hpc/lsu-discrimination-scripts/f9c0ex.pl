#!/usr/bin/perl
# Extract a real class-0x00 exponent example from FAIL9 col234 ACTUAL (correct) dump,
# and show why +15 (observed) contradicts XOR-0x11 (predicted +17) for that class.
use strict; use warnings;
my $file = "/tmp/hpc_log.txt";
sub read_dump { my ($s,$e)=@_; open my $fh,'<',$file or die; my $ln=0; my $buf='';
  while (<$fh>) { $ln++; next if $ln<$s; last if $ln>$e; my @p=/\b([0-9a-f]{2})\b/g; $buf.=join('',@p);} return $buf; }
my $a=read_dump(732541,777540);
my ($col,$dim)=(234,300);
my $shown=0;
for my $row (0..$dim-1) {
  my $el=$col*$dim+$row;
  for my $h (0,16) {
    my $av=unpack('Q<',pack('H*',substr($a,$el*32+$h,16)));
    next if $av==0;
    my $ea=($av>>52)&0x7ff;
    next unless (($ea & 0x11)==0x00);
    my $eg=$ea+15;
    printf("row=%d h=%d: actual exp ea=0x%03x (class 0x00, bit0=0 bit4=0)\n",$row,$h/16,$ea);
    printf("  observed golden exp eg=0x%03x = ea+15  (binary: %011b + 1111 -> %011b)\n",$eg,$ea,$eg);
    printf("  XOR-0x11 prediction    = 0x%03x = ea+17 (sets bit0+bit4, no carry)  MISMATCH\n",$ea^0x11);
    $shown++;
    last if $shown>=3;
  }
  last if $shown>=3;
}
