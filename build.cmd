@echo off
rem build.cmd [test|game|all|private] - builds with MSVC x64 (Visual Studio 2022 or later).
rem private: build\private\PreH2.exe with the game files inside (P2_GAMEDIR, default
rem _references\dos\PREH2). That exe contains the game: for your own use only, never publish it.
setlocal
if defined VSCMD_ARG_TGT_ARCH goto have_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%I in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSI=%%I"
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "%VSI%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 2
:have_vs
set "ROOT=%~dp0"
set "B=%ROOT%build"
if not exist "%B%\obj" mkdir "%B%\obj"
set "WHAT=%~1"
if "%WHAT%"=="" set "WHAT=all"
set CFLAGS=/nologo /utf-8 /O2 /W4 /WX /std:c17 /permissive- /D_CRT_SECURE_NO_WARNINGS /wd4996 /wd4310 /Fo"%B%\obj\\" /Fd"%B%\obj\\"
set CORE=%ROOT%src\core\cpu.c %ROOT%src\hal\machine.c %ROOT%src\hal\dos.c %ROOT%src\hal\vga.c %ROOT%src\hal\sb.c %ROOT%src\hal\sqz.c %ROOT%src\hal\pack.c %ROOT%src\host\pngw.c
set "GEN=%ROOT%src\gen\gen_all.c"
if not exist "%GEN%" set "GEN=%ROOT%src\gen\gen_none.c"
if not exist "%ROOT%src\gen\gen_all.c" echo src\gen\gen_all.c not generated: building the interpreter-only version (see README).
set CORE=%CORE% %GEN% %ROOT%src\gen\gen_rt.c
if /i "%WHAT%"=="test" goto test
if /i "%WHAT%"=="all" goto test
if /i "%WHAT%"=="private" goto private
goto game
:test
cl %CFLAGS% %CORE% %ROOT%src\host\headless\main.c /Fe"%B%\p2test.exe" /link /nologo || exit /b 1
if /i "%WHAT%"=="test" exit /b 0
:game
if not exist "%ROOT%src\host\win32\host_win32.c" exit /b 0
rc /nologo /fo "%B%\obj\preh2.res" "%ROOT%src\host\win32\preh2.rc" || exit /b 1
cl %CFLAGS% %CORE% %ROOT%src\host\win32\host_win32.c %ROOT%src\host\win32\audio_win32.c "%B%\obj\preh2.res" /Fe"%B%\PreH2.exe" /link /nologo /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%src\host\win32\preh2.manifest" user32.lib gdi32.lib shell32.lib dwmapi.lib uxtheme.lib advapi32.lib ole32.lib comdlg32.lib winmm.lib || exit /b 1
exit /b 0
:private
if "%P2_GAMEDIR%"=="" set "P2_GAMEDIR=%ROOT%_references\dos\PREH2"
if not exist "%B%\private" mkdir "%B%\private"
python "%ROOT%tools\mkpak.py" "%P2_GAMEDIR%" "%B%\obj\game.pak" || exit /b 1
> "%B%\obj\pack.rc" echo 100 RCDATA "game.pak"
rc /nologo /fo "%B%\obj\pack.res" "%B%\obj\pack.rc" || exit /b 1
rc /nologo /fo "%B%\obj\preh2.res" "%ROOT%src\host\win32\preh2.rc" || exit /b 1
cl %CFLAGS% %CORE% %ROOT%src\host\win32\host_win32.c %ROOT%src\host\win32\audio_win32.c "%B%\obj\preh2.res" "%B%\obj\pack.res" /Fe"%B%\private\PreH2.exe" /link /nologo /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%src\host\win32\preh2.manifest" user32.lib gdi32.lib shell32.lib dwmapi.lib uxtheme.lib advapi32.lib ole32.lib comdlg32.lib winmm.lib || exit /b 1
exit /b 0
