#!/usr/bin/perl
# Count GEMM-class runs on the same core in the S3-S4 fail window for the
# LSU-coverage argument. GEMM = load/store-heaviest workload in the suite.
use strict; use warnings;
my $LOG="/tmp/hpc_log.txt";
open(my $fh,'<',$LOG) or die;
my ($t,$ts)=('','');
my %n;
while(<$fh>){
  if (/^ *- test: (\S+)/){ $t=$1; $ts=''; }
  elsif ($t && /time-at-start/ && /(\d{2}:\d{2}:\d{2})Z/){ $ts=$1; }
  elsif ($t && /^ *result: (pass|fail|crash)/){
    my $r=$1;
    if ($ts ge '10:41:00' && $ts le '10:51:00'){
      $n{$t}{$r}++;
    }
    $t='';
  }
}
for my $k (sort keys %n){
  my $line = "$k:";
  for my $r (qw(pass fail crash)){ $line .= " $r=".($n{$k}{$r}//0); }
  print "$line\n";
}
