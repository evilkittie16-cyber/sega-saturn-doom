@echo off
REM make-iso.bat — Jo Engine's mkisofs step is broken on modern Cygwin
REM (the stock rule passes -volset "SaturnApp" and the Cygwin mkisofs
REM exits with "Missing pathspec" because it parses that as non-numeric).
REM Replace it with one that uses sane flags.
setlocal
cd /d "%~dp0"
set MKISOFS=..\..\Compiler\WINDOWS\Other Utilities\mkisofs.exe
set IPBIN=game.ip.bin
if not exist "%IPBIN%" set IPBIN=..\..\Compiler\COMMON\IP.BIN
powershell -NoProfile -Command "Dismount-DiskImage -ImagePath '%~dp0game.iso' -ErrorAction SilentlyContinue" >nul 2>&1
"%MKISOFS%" ^
  -V "SaturnApp" ^
  -volset 1 ^
  -sectype 2352 ^
  -publisher "SEGA ENTERPRISES, LTD." ^
  -preparer "SEGA ENTERPRISES, LTD." ^
  -A "SaturnApp" ^
  -generic-boot "%IPBIN%" ^
  -full-iso9660-filenames ^
  -o game.iso ^
  cd
if errorlevel 1 (
    echo mkisofs failed
    exit /b 1
)
"..\..\Compiler\WINDOWS\Other Utilities\JoEngineCueMaker.exe"
echo ISO built: game.iso
endlocal
