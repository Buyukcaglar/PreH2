@echo off
rem package.cmd - builds PreH2.exe and packs dist\PreH2-1.0-win64.zip (no game files).
setlocal
set "ROOT=%~dp0"
call "%ROOT%build.cmd" game || exit /b 1
set "D=%ROOT%dist\PreH2-1.0-win64"
if exist "%D%" rmdir /s /q "%D%"
mkdir "%D%"
copy /y "%ROOT%build\PreH2.exe" "%D%\" >nul || exit /b 1
copy /y "%ROOT%README.md" "%D%\" >nul
copy /y "%ROOT%LICENSE" "%D%\LICENSE.txt" >nul
if exist "%ROOT%dist\PreH2-1.0-win64.zip" del "%ROOT%dist\PreH2-1.0-win64.zip"
tar -a -c -f "%ROOT%dist\PreH2-1.0-win64.zip" -C "%ROOT%dist" PreH2-1.0-win64 || exit /b 1
echo %ROOT%dist\PreH2-1.0-win64.zip
exit /b 0
