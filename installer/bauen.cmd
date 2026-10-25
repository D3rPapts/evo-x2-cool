@echo off
rem Builds the programs, fetches the PawnIO files and compiles the installer to installer\Output.
pushd "%~dp0"
call ..\build.cmd || goto fail
powershell -NoProfile -ExecutionPolicy Bypass -File holen.ps1 || goto fail
set ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" set ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe
"%ISCC%" /Q evo-x2-cool.iss || goto fail
popd
exit /b 0
:fail
popd
exit /b 1
