$dir = 'C:\Users\ubuntu\Documents\sdc\sdcshield\docs\cases\sdc1-01-04'
$out = 'C:\Users\ubuntu\Documents\sdc\sdcshield\docs\cases\sdc1-01-04\_decoded'
New-Item -ItemType Directory -Force -Path $out | Out-Null
Get-ChildItem $dir -File | ForEach-Object {
  $b = [System.IO.File]::ReadAllBytes($_.FullName)
  $hex = ($b[0..7] | ForEach-Object { $_.ToString('X2') }) -join ' '
  Write-Host ("{0}  size={1}  head={2}" -f $_.Name, $b.Length, $hex)
  # Detect UTF-16 LE BOM (FF FE)
  if ($b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE) {
    $t = [System.Text.Encoding]::Unicode.GetString($b)
    $dst = Join-Path $out ($_.BaseName + '_utf8' + $_.Extension)
    [System.IO.File]::WriteAllText($dst, $t, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host ("  -> decoded to " + $dst + "  lines=" + ($t -split "`n").Count)
  }
}
