@echo off
REM make-iso.bat — Jo Engine's mkisofs step is broken on modern Cygwin
REM (the stock rule passes -volset "SaturnApp" and the Cygwin mkisofs
REM exits with "Missing pathspec" because it parses that as non-numeric).
REM Replace it with one that uses sane flags.
setlocal
cd /d "%~dp0"
set MKISOFS=..\..\Compiler\WINDOWS\Other Utilities\mkisofs.exe
if not exist "%MKISOFS%" set MKISOFS=C:\Users\charl\Desktop\joengine-master\Compiler\WINDOWS\Other Utilities\mkisofs.exe
set IPBIN=..\..\Compiler\COMMON\IP.BIN
if not exist "%IPBIN%" set IPBIN=C:\Users\charl\Desktop\joengine-master\Compiler\COMMON\IP.BIN
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
echo ISO built: game.iso
endlocal