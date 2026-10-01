@echo off
set PROCESSOR_ARCHITECTURE=x86
set BASEDIR=C:\DDK
set PATH=C:\MSVC\BIN;C:\DDK\BIN;%PATH%
set INCLUDE=C:\MSVC\INCLUDE
set LIB=C:\DDK\LIB\I386\FREE
set NTMAKEENV=C:\DDK\INC
set BUILD_MAKE_PROGRAM=nmake.exe
set BUILD_DEFAULT=-ei -nmake -i
set BUILD_DEFAULT_TARGETS=-386
set Cpu=i386
set DDKBUILDENV=free
set C_DEFINES=-D_IDWBUILD
set NTDBGFILES=1
set NTDEBUG=
set _OBJ_DIR=obj
set NEW_CRTS=1
set _NTROOT=C:\DDK
cd \USBNT\SRC
build -cZ > C:\BUILD.OUT 2>&1
if exist build.log copy build.log C:\BUILD.LOG
if exist build.err copy build.err C:\BUILD.ERR
if exist build.wrn copy build.wrn C:\BUILD.WRN
dir C:\DDK\lib\i386\free\usbnt.sys >> C:\BUILD.OUT
cd \USBNT\TOOLS
call mktools.bat > C:\TOOLS.OUT 2>&1
mkdir C:\OUT
copy C:\DDK\lib\i386\free\usbnt.sys C:\OUT
copy C:\USBNT\TOOLS\*.exe C:\OUT
echo ALLDONE > C:\DONE.TXT
echo BUILD FINISHED
