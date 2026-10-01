@echo off
rem Builds usbinst.exe, usbeject.exe, usbmon.exe, usbtree.exe and
rem wheeltst.exe with MSVC 4.x. Run it in this directory from an NT4 DDK
rem build window (BASEDIR set, MSVC bin directory on the PATH).

if "%BASEDIR%"=="" goto nobase

set CF=/nologo /c /O1 /W3 /Gs999999 /I%BASEDIR%\src\storage\inc
set LF=/nologo /nodefaultlib /entry:Entry@0 /opt:ref

cl %CF% usbinst.c usbeject.c usbmon.c usbtree.c devmap.c wheeltst.c
if errorlevel 1 goto fail

link %LF% /subsystem:console,3.10 /out:usbinst.exe usbinst.obj kernel32.lib user32.lib advapi32.lib
if errorlevel 1 goto fail
link %LF% /subsystem:console,3.10 /out:usbeject.exe usbeject.obj kernel32.lib user32.lib
if errorlevel 1 goto fail
link %LF% /subsystem:windows,3.10 /out:usbmon.exe usbmon.obj devmap.obj kernel32.lib user32.lib advapi32.lib
if errorlevel 1 goto fail
link %LF% /subsystem:windows,3.10 /out:usbtree.exe usbtree.obj devmap.obj kernel32.lib user32.lib
if errorlevel 1 goto fail
link %LF% /subsystem:windows,3.10 /out:wheeltst.exe wheeltst.obj kernel32.lib user32.lib
if errorlevel 1 goto fail

del *.obj
echo Tools built.
goto end

:nobase
echo Run this from an NT4 DDK build window (BASEDIR is not set).
goto end

:fail
echo Tool build failed.

:end
