param([string] $Set, [string] $Name, [string] $Views = "w,p2,m1a,m5")
# Read one hw_asm_chk_<Name>[8] counter array from each view of a chk_run set.
foreach ($n in $Views.Split(',')) {
    $d = "tmp\doomprof\$Set\$n"
    if (-not (Test-Path "$d\chk_syms.txt")) { "${n}: no build"; continue }
    $s = Get-Content "$d\chk_syms.txt" | Where-Object { $_ -match "chk_$Name$" }
    if (-not $s) { "${n}: no symbol hw_asm_chk_$Name"; continue }
    $a = [Convert]::ToInt64($s.Split(' ')[0], 16)
    $row = $a -band (-bnot 15)
    $log = Get-Content "$d\run.log"
    $b = @()
    foreach ($k in 0..2) {
        $l = ($log | Where-Object { $_ -match ('{0:X8}: ' -f ($row + 16 * $k)) } | Select-Object -First 1) -replace '^sdbg> ', ''
        if (-not $l) { break }
        $b += @($l.Substring(10).Trim().Split(' ')[0..15])
    }
    $o = $a - $row
    $w = 0..7 | ForEach-Object { if ($b.Count -ge $o + 4 * $_ + 4) { [Convert]::ToUInt32(($b[($o + 4 * $_)..($o + 4 * $_ + 3)] -join ''), 16) } else { -1 } }
    "{0,-4} {1}: calls={2} mism={3} [2]={4} [3]={5} [4]={6} [5]={7} [6]={8} [7]={9}" -f $n, $Name, $w[0], $w[1], $w[2], $w[3], $w[4], $w[5], $w[6], $w[7]
}
