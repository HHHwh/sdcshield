$log = 'C:\Users\ubuntu\Documents\sdc\sdcshield\docs\cases\sdc1-01-04\_decoded\9.3.17.13_2026-09-17_19_55_34_utf8.log'
$lines = Get-Content -LiteralPath $log
for ($i = 0; $i -lt $lines.Count; $i++) {
    $ln = $i + 1
    if ($ln -ge 19440 -and $ln -le 19960) {
        if ($lines[$i] -match 'CPU(\d+): Booted secondary processor (\S+)') {
            "{0} CPU{1} {2}" -f $ln, $Matches[1], $Matches[2]
        }
    }
}
