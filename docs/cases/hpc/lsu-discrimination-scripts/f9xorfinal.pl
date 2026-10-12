#!/usr/bin/perl
# FAIL9 col234 (Matrix U): definitive per-element-XOR-vs-arithmetic discrimination.
# Dumps: actual = lines 732541..777540 (run side, CORRECT per golden-side attribution),
#        expected = lines 777542..822541 (init/golden side, CORRUPTED side).
# Little-endian decode: hex-string -> bytes -> u64 LE  (Perl unpack Q< is correct).
use strict; use warnings;
my $file = "/tmp/hpc_log.txt";
sub read_dump { my ($s,$e)=@_; open my $fh,'<',$file or die; my $ln=0; my $buf='';
  while (<$fh>) { $ln++; next if $ln<$s; last if $ln>$e; my @p=/\b([0-9a-f]{2})\b/g; $buf.=join('',@p);} return $buf; }
my $a=read_dump(732541,777540); my $g=read_dump(777542,822541);
my ($col,$dim)=(234,300);
my (%clsA,%clsG); my ($shift_ok,$shift_bad,$xor_ok,$xor_bad,$mant_bad,$sign_bad)=(0,0,0,0,0,0);
my %xorshift;  # predicted eg-ea shift by class of ea
for my $row (0..$dim-1) {
  my $el=$col*$dim+$row;
  for my $h (0,16) {
    my $av=unpack('Q<',pack('H*',substr($a,$el*32+$h,16)));
    my $gv=unpack('Q<',pack('H*',substr($g,$el*32+$h,16)));
    next if $av==0 || $gv==0;
    my $ea=($av>>52)&0x7ff; my $eg=($gv>>52)&0x7ff;
    my $ca=$ea & 0x11; my $cg=$eg & 0x11;
    $clsA{$ca}++; $clsG{$cg}++;
    # observed arithmetic shift eg - ea
    if ($eg-$ea==15) { $shift_ok++; } else { $shift_bad++; }
    # XOR hypothesis: golden = actual ^ 0x0110000000000000  =>  eg == ea ^ 0x11
    if ($eg == ($ea ^ 0x11)) { $xor_ok++; } else { $xor_bad++; }
    $xorshift{$ca} //= (($ea ^ 0x11) - $ea);   # predicted shift for this ea-class
    # mantissa / sign preservation
    $mant_bad++ if (($av & 0x000fffffffffffff) != ($gv & 0x000fffffffffffff));
    $sign_bad++ if (($av>>63) != ($gv>>63));
  }
}
print "col 234 halves checked (nonzero both sides)\n";
print "  arithmetic: eg-ea == +15 for all:  ok=$shift_ok bad=$shift_bad   (golden = actual x 2^15)\n";
print "  mantissa identical: bad=$mant_bad ; sign identical: bad=$sign_bad\n";
print "  actual(correct) exp&0x11 classes: ", join(' ',map{sprintf("0x%02x:%d",$_,$clsA{$_}//0)} (0x00,0x01,0x10,0x11)),"\n";
print "  golden(corrupt) exp&0x11 classes: ", join(' ',map{sprintf("0x%02x:%d",$_,$clsG{$_}//0)} (0x00,0x01,0x10,0x11)),"\n";
print "  fixed-XOR(bits 52/56) predicted shift eg-ea by ACTUAL-exp class:\n";
printf("    0x00 -> %+d   0x01 -> %+d   0x10 -> %+d   0x11 -> %+d\n",
  $xorshift{0x00}//0, $xorshift{0x01}//0, $xorshift{0x10}//0, $xorshift{0x11}//0);
print "  elements CONSISTENT with fixed-XOR (eg == ea^0x11): $xor_ok ; CONTRADICTING: $xor_bad\n";
