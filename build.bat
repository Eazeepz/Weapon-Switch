@echo off
rem Builds WeaponSwitch.dll, puts it in the WeaponSwitch folder and makes the
rem release zip. Needs CMake and Visual Studio 2022 or newer (with "Desktop
rem development with C++").
setlocal
cd /d "%~dp0"

cmake -S . -B build -A x64 || goto failed
cmake --build build --config Release || goto failed
copy /y build\Release\WeaponSwitch.dll WeaponSwitch\WeaponSwitch.dll >nul || goto failed

rem The zip players download: the WeaponSwitch folder, plus the readme.
for /f "tokens=2 delims== " %%v in ('findstr /b "version" WeaponSwitch\mod.ini') do set VERSION=%%v
set ZIP=WeaponSwitch-%VERSION%.zip
if exist build\package rmdir /s /q build\package
mkdir build\package\WeaponSwitch
copy /y WeaponSwitch\* build\package\WeaponSwitch\ >nul
copy /y README.md build\package\WeaponSwitch\ >nul
powershell -NoProfile -Command "Compress-Archive -Force -Path 'build\package\WeaponSwitch' -DestinationPath '%ZIP%'" || goto failed

echo.
echo Done: WeaponSwitch\WeaponSwitch.dll is built and %ZIP% is ready to share.
exit /b 0

:failed
echo.
echo BUILD FAILED - see the messages above.
exit /b 1
