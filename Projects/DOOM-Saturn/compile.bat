@ECHO Off
cd /d "%~dp0"
REM Mirror of the demo-2D-storyboard compile.bat:
REM prepend Compiler\WINDOWS\bin (SH-elf gcc + MinGW DLLs) and
REM Compiler\WINDOWS\Other Utilities (make.exe) to PATH so the
REM toolchain DLLs load in the right order and make is found.
SET COMPILER_DIR=..\..\Compiler
if not exist "%COMPILER_DIR%\WINDOWS\bin\sh-elf-gcc.exe" SET COMPILER_DIR=C:\Users\charl\Desktop\joengine-master\Compiler
SET PATH=%COMPILER_DIR%\WINDOWS\Other Utilities;%COMPILER_DIR%\WINDOWS\bin;%PATH%
make re
