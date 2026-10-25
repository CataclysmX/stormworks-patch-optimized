@echo off
rem stwpatch installer for Windows.
rem   install.bat                              client, Steam folder found automatically
rem   install.bat "D:\...\Stormworks"          client, explicit game folder
rem   install.bat "D:\...\Stormworks" server   client + dedicated server (server64.exe)
rem The patch files are shipped as stwpatch_*.dll so that extracting the archive anywhere,
rem even into the game folder, never overwrites a game file.
setlocal EnableExtensions DisableDelayedExpansion
set "SRC=%~dp0"
set "GAME=%~1"
set "MODE=%~2"
if defined GAME goto have_game
for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| find /i "SteamPath"') do set "GAME=%%b\steamapps\common\Stormworks"
if defined GAME set "GAME=%GAME:/=\%"
rem run from inside the game folder
if not exist "%GAME%\stormworks64.exe" if exist "%SRC%stormworks64.exe" set "GAME=%SRC:~0,-1%"
:have_game
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"
if not exist "%GAME%\stormworks64.exe" goto not_found
if not exist "%SRC%stwpatch_OpenAL64.dll" goto missing_files

tasklist /fi "imagename eq stormworks64.exe" 2>nul | find /i "stormworks64.exe" >nul && goto game_running
tasklist /fi "imagename eq server64.exe" 2>nul | find /i "server64.exe" >nul && goto game_running

echo Game folder: "%GAME%"
call :steam_api_cleanup

rem ---- client: OpenAL64.dll -> OpenAL64_orig.dll, then the patch as OpenAL64.dll ----
if exist "%GAME%\OpenAL64_orig.dll" goto orig_present
call :size "%GAME%\OpenAL64.dll"
if %SZ% LSS 1000000 goto openal_is_patch
ren "%GAME%\OpenAL64.dll" "OpenAL64_orig.dll"
if errorlevel 1 goto rename_failed
echo   OpenAL64.dll renamed to OpenAL64_orig.dll (original kept)
goto copy_openal
:orig_present
call :size "%GAME%\OpenAL64_orig.dll"
if %SZ% LSS 1000000 goto orig_is_patch
echo   OpenAL64_orig.dll already present (original)
:copy_openal
copy /y "%SRC%stwpatch_OpenAL64.dll" "%GAME%\OpenAL64.dll" >nul
if errorlevel 1 goto copy_failed
echo   OpenAL64.dll = stwpatch

rem ---- dedicated server: opengl32.dll (the game ships none) ----
if /i not "%MODE%"=="server" goto ini
copy /y "%SRC%stwpatch_opengl32.dll" "%GAME%\opengl32.dll" >nul
if errorlevel 1 goto copy_failed
echo   opengl32.dll = stwpatch (for server64.exe)

:ini
if not exist "%GAME%\stwpatch.ini" copy /y "%SRC%stwpatch.ini" "%GAME%\stwpatch.ini" >nul
echo.
echo Installed. Config: stwpatch.ini   Logs: stwpatch.log, stwpatch_server.log
echo If the game does not start: send stwpatch.log and crash.dmp from the game folder.
echo A Steam "verify files" or a game update removes the patch: run install.bat again.
pause
exit /b 0

rem ---------------------------------------------------------------------------------------
:size
set "SZ=%~z1"
if not defined SZ set "SZ=0"
exit /b 0

:steam_api_cleanup
rem steam_api64.dll is checked by the game: an older stwpatch replaced it. Put the original back.
if not exist "%GAME%\steam_api64_orig.dll" exit /b 0
call :size "%GAME%\steam_api64.dll"
if %SZ% GEQ 250000 (
  del "%GAME%\steam_api64_orig.dll"
  echo   removed leftover steam_api64_orig.dll
  exit /b 0
)
move /y "%GAME%\steam_api64_orig.dll" "%GAME%\steam_api64.dll" >nul
echo   steam_api64.dll: original restored
exit /b 0

:not_found
echo Stormworks not found in "%GAME%".
echo Usage: install.bat "C:\Program Files (x86)\Steam\steamapps\common\Stormworks" [server]
pause
exit /b 1
:missing_files
echo stwpatch_OpenAL64.dll not found next to install.bat: extract the whole archive first.
pause
exit /b 1
:game_running
echo Close Stormworks (game and server) first.
pause
exit /b 1
:openal_is_patch
echo ERROR: OpenAL64.dll in the game folder is not the original (too small) and
echo OpenAL64_orig.dll is missing: the original was overwritten.
echo Fix: Steam ^> Stormworks ^> Properties ^> Installed files ^> Verify integrity, then run install.bat again.
pause
exit /b 1
:orig_is_patch
echo ERROR: OpenAL64_orig.dll is not the original OpenAL (too small).
echo Fix: delete OpenAL64_orig.dll, verify the files in Steam, then run install.bat again.
pause
exit /b 1
:rename_failed
echo ERROR: could not rename OpenAL64.dll (file in use or no permission?).
pause
exit /b 1
:copy_failed
echo ERROR: copy failed (file in use or no permission?).
pause
exit /b 1
