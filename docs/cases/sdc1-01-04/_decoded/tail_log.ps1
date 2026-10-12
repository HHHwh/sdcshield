$f = 'C:\Users\ubuntu\Documents\sdc\sdcshield\docs\cases\sdc1-01-04\_decoded\9.3.17.13_2026-09-17_19_55_34_utf8.log'
$lines = Get-Content -LiteralPath $f
"Total lines: $($lines.Count)"
"--- Last 30 lines ---"
$lines[($lines.Count-30)..($lines.Count-1)] | ForEach-Object { $_ }
