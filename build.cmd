@echo off
rem Builds evo-x2-cool.exe (controller/service) and evo-x2-cool-front.exe (viewer) with the VS Build Tools, x64, size-optimized.
rem Hybrid CRT: the C runtime comes from ucrtbase.dll that ships with Windows 10/11, only the small vcruntime part is linked in,
rem so the files stay small and need no redistributable.
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
set CF=/nologo /O1 /MT /W4 /utf-8 /GL
set LF=/link /LTCG /OPT:REF /OPT:ICF /NODEFAULTLIB:libucrt.lib ucrt.lib
pushd "%~dp0"
rc /nologo evo.rc || goto fail
rc /nologo evo-front.rc || goto fail
cl %CF% /Fe:evo-x2-cool.exe evo-x2-cool.c %LF% evo.res /SUBSYSTEM:CONSOLE || goto fail
cl %CF% /Fe:evo-x2-cool-front.exe evo-x2-cool-front.c %LF% evo-front.res /SUBSYSTEM:WINDOWS || goto fail
del *.obj *.res 2>nul
popd
exit /b 0
:fail
popd
exit /b 1
