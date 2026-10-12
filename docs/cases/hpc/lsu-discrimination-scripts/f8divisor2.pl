#!/usr/bin/perl
# Discriminate: corrupted divisor = correct XOR small-mask (bit flip in PRF)
#                 vs alien value (load/store path substitution/aliasing).
# Method: for FAIL8 col 38, ratio r = actual/golden is CONSTANT (24145.491339).
#   => corrupted_col = correct_col * k, with k = g_div/a_div single scalar.
# Take two elements x,y of the column. corrupted: x'=k*x, y'=k*y (approx,
# renormalized). The OBSERVED column is a normalized version.
# The bit-level test on the DIVISOR cannot be done directly without knowing
# the divisor value. Instead test the SIGNATURE the report already proved:
#   FAIL9: actual = golden EXACTLY x 2^-15 for all 300 elements -> divisor
#          exponent field flipped (bits of exponent), mantissa IDENTICAL.
#          A load/store substitution would replace the divisor with another
#          in-flight double: mantissa would differ => column would NOT be
#          an exact power-of-two scaling.
# Here we verify FAIL9 exactness rigorously incl. renormalization direction,
# and check FAIL8's ratio is NOT a power of two and column mantissas differ.
use strict; use warnings;
my ($file)=@ARGV;
sub read_dump { my ($s,$e)=@_; open my $fh,'<',$file or die; my $ln=0; my $buf='';
  while (<$fh>) { $ln++; next if $ln<$s; last if $ln>$e; my @p=/\b([0-9a-f]{2})\b/g; $buf.=join('',@p);} return $buf; }
# FAIL9 dumps: find marks. We know from summary: FAIL9 first-err at col234 row0,
# offset 1123200+6 in the V? Actually U. Use fail9 marks from dump_marks.txt.
open(my $mf,'<',"/tmp/dump_marks.txt") or die "no marks: $!";
my @marks; while(<$mf>){chomp; my($l,$t)=split(/:/,$_,2); push @marks,[$l+0,($t=~/actual/)?'A':'E'];}
close $mf;
sub rd { my($st)=@_; open my $fh,'<',$file or die; my $ln=0; my $buf='';
  while(<$fh>){$ln++; next if $ln<=$st; last if $ln>$st+45000; my @p=/\b([0-9a-f]{2})\b/g; $buf.=join('',@p);} return $buf; }
# FAIL9 = 9th failure => marks index 16/17
my $a9=rd($marks[16][0]); my $g9=rd($marks[17][0]);
my $exact=0; my $notexact=0; my %notinfo;
for my $el (0..89999) {
  next unless int($el/300)==234;
  for my $h (0,16) {
    my $as=substr($a9,$el*32+$h,16); my $gs=substr($g9,$el*32+$h,16);
    next if $as eq $gs;
    my $av=unpack('d<',pack('H*',$as)); my $gv=unpack('d<',pack('H*',$gs));
    if ($gv!=0 && $av/$gv==2**-15) { $exact++; }
    elsif ($gv==0 || $av==0) { $notinfo{'zero'}++; }
    else { $notexact++; printf("NOTEXACT el=%d h=%d a=%s g=%s r=%.17g\n",$el,$h/16,$as,$gs,$av/$gv) if $notexact<=5; }
  }
}
print "FAIL9 col234: exact-2^-15 count=$exact, not-exact=$notexact, other=",join(',',map{"$_=$notinfo{$_}"}keys%notinfo),"\n";
