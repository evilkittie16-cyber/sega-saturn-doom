param(
    [string] $Set = "lodsweep",
    [string] $Proj = "DOOM-Saturn-asm",
    [string] $Views = "w:2:-1880:1117:4:204;p2:1:3095:-3319:3:140;m1a:1:3352:-3568:3",
    [string] $Settings = "",      # file: one "name v0,v1,..." per line (decimal, may be negative)
    [int] $Settle = 30,
    [int] $ProfFrames = 40,
    [switch] $NoBuild,
    [string] $Defs = "",
    [switch] $NoFreeze
)
# Strip-LOD tuning sweep: ONE hold-view build per view, then a single
# 5thPlanet session per view pokes each setting into hw_wall_lod[] and
# captures render (quality), VDP1 dump (quad count) and prof (CPU time).
# Score with lodscore.py <Set>.
$ErrorActionPreference = "Continue"
$fp  = "C:\Users\charl\Desktop\5thPlanet-main\5thPlanet-main"
$ds  = "C:\Users\charl\Desktop\joengine-2026.1\Projects\$Proj"
$env:PATH = "C:\Users\charl\Desktop\joengine-2026.1\Compiler\WINDOWS\bin;$env:PATH"
$lines = @(Get-Content $Settings | Where-Object { $_.Trim() -and -not $_.StartsWith('#') })
$jobs = @()
foreach ($v in $Views.Split(';')) {
    $n, $m, $x, $y, $a, $deg = $v.Split(':'); $dd = ""; if ($deg) { $dd = "-DBENCH_HOLD_DEG=$deg" }
    $out = "$fp\tmp\doomprof\$Set\$n"
    New-Item -ItemType Directory -Force $out | Out-Null
    if (-not $NoBuild) {
        $env:EXTRA_DEFS = "-DSATURN_BENCH=1 -DBENCH_HOLD_FPS=1 -DBENCH_HOLD_MAP=$m -DBENCH_HOLD_X=$x -DBENCH_HOLD_Y=$y -DBENCH_HOLD_A8=$a $dd $Defs"
        $log = & cmd.exe /c "`"$ds\compile.bat`"" 2>&1 | Out-String
        Remove-Item Env:EXTRA_DEFS
        $log | Out-File -Encoding utf8 "$out\build.log"
        & cmd.exe /c "`"$ds\make-iso.bat`"" | Out-Null
        Copy-Item "$ds\game.iso", "$ds\game.cue", "$ds\game.elf" $out -Force
        & "C:\Users\charl\Desktop\joengine-2026.1\Compiler\WINDOWS\sh-elf\bin\nm.exe" -n -S "$out\game.elf" | Out-File -Encoding ascii "$out\syms.txt"
    }
    $sym = Get-Content "$out\syms.txt" | Where-Object { $_ -match ' _hw_wall_lod_in$' }
    $base = [Convert]::ToInt64($sym.Split(' ')[0], 16)
    $rel = "tmp/doomprof/$Set/$n"
    $fz = Get-Content "$out\syms.txt" | Where-Object { $_ -match ' _saturn_freeze_world$' }
    $s = "frame 5400`nframe 1500`n"
    $seq = 0
    if ($fz -and -not $NoFreeze) { $s += ("poke 0x{0} 1 4`nframe 10`n" -f $fz.Split(' ')[0]) }
    foreach ($l in $lines) {
        $name, $vals = $l.Trim().Split(' ', 2)
        $i = 0
        foreach ($val in $vals.Split(',')) {
            # NB: PowerShell's 0xFFFFFFFF literal is Int32 -1; convert explicitly
            $iv = [int64]$val
            if ($iv -lt 0) { $iv += 4294967296 }
            $u = [uint32]$iv
            $s += ("poke 0x{0:X8} 0x{1:X8} 4`n" -f ($base + 4 * $i), $u)
            $i++
        }
        $seq++
        $s += ("poke 0x{0:X8} 0x{1:X8} 4`n" -f ($base + 64), $seq)   # [16] sequence: renderer copies the table
        $s += "frame $Settle`nrender $rel/view_$name.ppm`nv1dump $rel/vram_$name.bin`nprof $ProfFrames $rel/prof_$name.csv`n"
    }
    $s += "q`n"
    [IO.File]::WriteAllText("$out\sweep.txt", $s, (New-Object Text.ASCIIEncoding))
    Copy-Item $Settings "$out\settings.txt" -Force
    $jobs += Start-Process -PassThru -NoNewWindow -WorkingDirectory $fp -FilePath cmd.exe `
        -ArgumentList "/c target\release\sdbg.exe `"bios\Sega Saturn BIOS (USA).bin`" $rel\game.cue --cart=ram4m < $rel\sweep.txt > $rel\sweep.log"
    "started $n"
}
$jobs | Wait-Process
"all done"
