@echo off
setlocal enabledelayedexpansion

rem Copies the cross-compiled Windows build into the Steam AQtion install.
rem
rem   install-windows.bat [source-dir] [aqtion-dir]
rem
rem source-dir defaults to the folder this script lives in, aqtion-dir to the
rem path below. Both go in under names of their own, beside the stock client
rem and its game library, which are left alone - q2prox.exe looks for
rem gamex86_64x.dll before the stock gamex86_64.dll:
rem   <aqtion-dir>\action\gamex86_64x.dll
rem   <aqtion-dir>\q2prox.exe
rem   <aqtion-dir>\action\fonts\*  (when the build staged a fonts folder)
rem   <aqtion-dir>\action\pics\*   (likewise pics)

set "SRC=%~1"
if "%SRC%"=="" set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"

set "DEST=%~2"
if "%DEST%"=="" set "DEST=X:\SteamLibrary\steamapps\common\AQtion"
if "%DEST:~-1%"=="\" set "DEST=%DEST:~0,-1%"

if not exist "%DEST%\" (
    echo ERROR: AQtion folder not found: %DEST%
    exit /b 1
)
if not exist "%DEST%\action\" (
    echo ERROR: action folder not found: %DEST%\action
    exit /b 1
)

rem Accept whichever name the build produced.
set "DLL="
for %%F in (gamex86_64x.dll gamex86_64.dll gamex64.dll) do (
    if not defined DLL if exist "%SRC%\%%F" set "DLL=%SRC%\%%F"
)
if not defined DLL (
    echo ERROR: no game DLL found in %SRC%
    echo        looked for gamex86_64x.dll, gamex86_64.dll, gamex64.dll
    exit /b 1
)

set "EXE="
for %%F in (q2prox.exe q2pro_x.exe q2pro.exe) do (
    if not defined EXE if exist "%SRC%\%%F" set "EXE=%SRC%\%%F"
)
if not defined EXE (
    echo ERROR: no client executable found in %SRC%
    echo        looked for q2prox.exe, q2pro_x.exe, q2pro.exe
    exit /b 1
)

tasklist /fi "imagename eq q2prox.exe" 2>nul | find /i "q2prox.exe" >nul
if not errorlevel 1 (
    echo ERROR: q2prox.exe is running - close the game first.
    exit /b 1
)

echo Installing into %DEST%

copy /y "%DLL%" "%DEST%\action\gamex86_64x.dll" >nul
if errorlevel 1 (
    echo ERROR: failed to copy %DLL%
    exit /b 1
)
echo   %DLL%  ^-^>  %DEST%\action\gamex86_64x.dll

copy /y "%EXE%" "%DEST%\q2prox.exe" >nul
if errorlevel 1 (
    echo ERROR: failed to copy %EXE%
    exit /b 1
)
echo   %EXE%  ^-^>  %DEST%\q2prox.exe

rem The TrueType text's fonts, if the build staged them
if exist "%SRC%\fonts\" (
    if not exist "%DEST%\action\fonts\" mkdir "%DEST%\action\fonts"
    copy /y "%SRC%\fonts\*" "%DEST%\action\fonts\" >nul
    if errorlevel 1 (
        echo ERROR: failed to copy the fonts
        exit /b 1
    )
    echo   %SRC%\fonts\*  ^-^>  %DEST%\action\fonts\
)

rem jmod's spawnpoint marker, if the build staged it
if exist "%SRC%\pics\" (
    if not exist "%DEST%\action\pics\" mkdir "%DEST%\action\pics"
    copy /y "%SRC%\pics\*" "%DEST%\action\pics\" >nul
    if errorlevel 1 (
        echo ERROR: failed to copy the pics
        exit /b 1
    )
    echo   %SRC%\pics\*  ^-^>  %DEST%\action\pics\
)

echo Done.
exit /b 0
