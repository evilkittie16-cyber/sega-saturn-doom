param(
    [string] $Set = "chk",
    [string] $Defs = "-DBENCH_HOLD_TURN=7 -DBENCH_SETTLE=0",
    [string] $Proj = "DOOM-Saturn-asm",
    [int] $Frames = 3000,
    [string] $Views = "w:2:-1880:1117:4:204;p2:1:3095:-3319:3:140;m1a:1:3352:-3568:3;p1:1:1056:-3616:0;m8:8:496:2944:3;m5:5:-800:1412:1"
)
# Asm self-check: build each view with -DHW_ASM_CHECK=1 (C reference runs beside
# every asm kernel), run $Frames frames in 5thPlanet, dump every hw_asm_chk_*
# counter array ([0] calls, [1] mismatches, [2..5] first mismatch details).
$ErrorActionPreference = "Continue"
$fp  = "C:\Users\charl\Desktop\5thPlanet-main\5thPlanet-main"
$ds  = "C:\Users\charl\Desktop\joengine-2026.1\Projects\$Proj"
$env:PATH = "C:\Users\charl\Desktop\joengine-2026.1\Compiler\WINDOWS\bin;$env:PATH"
$procs = @()
foreach ($v in $Views.Split(';')) {
    $n, $m, $x, $y, $a, $deg = $v.Split(':'); $dd = ""; if ($deg) { $dd = "-DBENCH_HOLD_DEG=$deg" }
    $out = "$fp\tmp\doomprof\$Set\$n"
    New-Item -ItemType Directory -Force $out | Out-Null
    $env:EXTRA_DEFS = "-DSATURN_BENCH=1 -DBENCH_HOLD_FPS=1 -DBENCH_HOLD_MAP=$m -DBENCH_HOLD_X=$x -DBENCH_HOLD_Y=$y -DBENCH_HOLD_A8=$a $dd -DHW_ASM_CHECK=1 $Defs"
    $log = & cmd.exe /c "`"$ds\compile.bat`"" 2>&1 | Out-String
    Remove-Item Env:EXTRA_DEFS
    $log | Out-File -Encoding utf8 "$out\build.log"
    if (-not (Test-Path "$ds\game.elf")) { "BUILD FAILED $n"; continue }
    & cmd.exe /c "`"$ds\make-iso.bat`"" | Out-Null
    Copy-Item "$ds\game.iso", "$ds\game.cue", "$ds\game.elf" $out -Force
    $syms = & "C:\Users\charl\Desktop\joengine-2026.1\Compiler\WINDOWS\sh-elf\bin\nm.exe" -n "$out\game.elf"
    $syms | Out-File -Encoding ascii "$out\syms.txt"
    $chk = @($syms | Where-Object { $_ -match '_hw_asm_chk_' })
    $chk | Out-File -Encoding ascii "$out\chk_syms.txt"
    $rel = "tmp/doomprof/$Set/$n"
    $s = "frame 5400`nframe $Frames`n"
    foreach ($c in $chk) { $s += "m 0x$($c.Split(' ')[0]) 32`n" }
    $s += "q`n"
    [IO.File]::WriteAllText("$out\cmds.txt", $s, (New-Object Text.ASCIIEncoding))
    $procs += Start-Process -FilePath cmd.exe -WorkingDirectory $fp -NoNewWindow -PassThru -ArgumentList "/c target\release\sdbg.exe `"bios\Sega Saturn BIOS (USA).bin`" $rel\game.cue --cart=ram4m < $rel\cmds.txt > $rel\run.log"
}
$procs | Wait-Process
foreach ($v in $Views.Split(';')) {
    $n = $v.Split(':')[0]; $out = "$fp\tmp\doomprof\$Set\$n"
    "== $n"; Get-Content "$out\chk_syms.txt" -ErrorAction SilentlyContinue
    # sdbg m prints from the exact address; the first row carries the "sdbg> " prompt
    Get-Content "$out\run.log" -ErrorAction SilentlyContinue | Select-String -Pattern '0?6[0-9A-Fa-f]{6}: ' | ForEach-Object { $_.Line -replace '^sdbg> ', '' }
}
