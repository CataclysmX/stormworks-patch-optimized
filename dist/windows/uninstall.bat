@echo off
rem Restores the original DLLs. Usage: uninstall.bat ["D:\...\Stormworks"]
setlocal EnableExtensions DisableDelayedExpansion
set "SRC=%~dp0"
set "GAME=%~1"
if defined GAME goto have_game
for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| find /i "SteamPath"') do set "GAME=%%b\steamapps\common\Stormworks"
if defined GAME set "GAME=%GAME:/=\%"
if not exist "%GAME%\stormworks64.exe" if exist "%SRC%stormworks64.exe" set "GAME=%SRC:~0,-1%"
:have_game
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"
if not exist "%GAME%\stormworks64.exe" (echo Stormworks not found in "%GAME%". & pause & exit /b 1)

if not exist "%GAME%\OpenAL64_orig.dll" goto openal_done
set "SZ=0"
for %%F in ("%GAME%\OpenAL64_orig.dll") do set "SZ=%%~zF"
if %SZ% LSS 1000000 (echo OpenAL64_orig.dll is not the original: verify the files in Steam. & goto openal_done)
move /y "%GAME%\OpenAL64_orig.dll" "%GAME%\OpenAL64.dll" >nul && echo   OpenAL64.dll restored
:openal_done
if exist "%GAME%\opengl32.dll" del "%GAME%\opengl32.dll" && echo   opengl32.dll removed
if exist "%GAME%\steam_api64_orig.dll" del "%GAME%\steam_api64_orig.dll"
echo Done.
pause
exit /b 0
