@echo off
chcp 65001 >nul
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem  Scher build script
rem
rem  Usage:
rem    build.bat               build Release
rem    build.bat -p            build Release and package build\scher.zip
rem    build.bat --package     same, long form
rem    build.bat -c            wipe the build directory first
rem    build.bat -p -c         clean rebuild plus package
rem    build.bat -h            help
rem
rem  The package holds scher.exe and its runtime libraries; unzip and run.
rem ===========================================================================

set "PACKAGE=0"
set "CLEAN=0"
set "BUILD_TYPE=Release"
set "PROJECT_DIR=%~dp0"
if "%PROJECT_DIR:~-1%"=="\" set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"
set "BUILD_DIR=%PROJECT_DIR%\build"
rem Override for CI or for trying a clean build without touching build\
if defined SCHER_BUILD_DIR set "BUILD_DIR=%SCHER_BUILD_DIR%"

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="-p"          ( set "PACKAGE=1" & shift & goto parse_args )
if /i "%~1"=="--package"   ( set "PACKAGE=1" & shift & goto parse_args )
if /i "%~1"=="-c"          ( set "CLEAN=1"   & shift & goto parse_args )
if /i "%~1"=="--clean"     ( set "CLEAN=1"   & shift & goto parse_args )
if /i "%~1"=="-d"          ( set "BUILD_TYPE=Debug" & shift & goto parse_args )
if /i "%~1"=="--debug"     ( set "BUILD_TYPE=Debug" & shift & goto parse_args )
if /i "%~1"=="-h"          goto usage
if /i "%~1"=="--help"      goto usage
if /i "%~1"=="/?"          goto usage
echo [ERROR] unknown argument: %~1
echo.
goto usage

:usage
echo Usage: build.bat [-p ^| --package] [-c ^| --clean] [-d ^| --debug]
echo.
echo   -p, --package   also produce build\scher.zip
echo   -c, --clean     delete the build directory before configuring
echo   -d, --debug     build Debug instead of Release
echo   -h, --help      show this help
exit /b 2

:args_done

rem --------------------------------------------------------------------------
rem  Toolchain: environment variables first, then the Qt bundle, then PATH
rem --------------------------------------------------------------------------
if not defined CMAKE_EXE set "CMAKE_EXE=D:\Qt\Tools\CMake_64\bin\cmake.exe"
if not exist "%CMAKE_EXE%" set "CMAKE_EXE=cmake"
if not defined NINJA_EXE set "NINJA_EXE=D:\Qt\Tools\Ninja\ninja.exe"
if not defined MINGW_BIN set "MINGW_BIN=D:\Qt\Tools\mingw1310_64\bin"

set "TOOL_OK=1"
if not exist "%MINGW_BIN%\g++.exe" (
    echo [ERROR] MinGW not found: %MINGW_BIN%
    echo         Set MINGW_BIN to the folder holding g++.exe and retry.
    set "TOOL_OK=0"
)
if not exist "%NINJA_EXE%" (
    echo [ERROR] Ninja not found: %NINJA_EXE%
    echo         Set NINJA_EXE to ninja.exe and retry.
    set "TOOL_OK=0"
)
if "%TOOL_OK%"=="0" exit /b 1

rem Put the toolchain on PATH as well: accepting absolute CC/CXX paths is not
rem enough, because CMake resolves windres/ar/ranlib by name for the resource
rem and static-library steps.
set "PATH=%MINGW_BIN%;%PATH%"

rem cmake may come from PATH, so confirm it actually runs
"%CMAKE_EXE%" --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] cmake not found, set CMAKE_EXE to cmake.exe
    exit /b 1
)

echo ==========================================================
echo   Scher build
echo   source : %PROJECT_DIR%
echo   build  : %BUILD_DIR%
echo   type   : %BUILD_TYPE%
echo   package: %PACKAGE%
echo ==========================================================
echo.

if "%CLEAN%"=="1" (
    if exist "%BUILD_DIR%" (
        echo [1/3] Wiping the build directory...
        rmdir /s /q "%BUILD_DIR%"
        if exist "%BUILD_DIR%" (
            echo [ERROR] cannot delete the build directory, close Scher and retry
            exit /b 1
        )
    )
)
echo [1/3] Configuring...
"%CMAKE_EXE%" -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" ^
    -DCMAKE_C_COMPILER="%MINGW_BIN%\gcc.exe" ^
    -DCMAKE_CXX_COMPILER="%MINGW_BIN%\g++.exe"
if errorlevel 1 (
    echo [ERROR] cmake configure failed
    exit /b 1
)

echo.
echo [2/3] Building scher.exe...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --target tinta
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
if not exist "%BUILD_DIR%\scher.exe" (
    echo [ERROR] the build finished without scher.exe
    exit /b 1
)

if "%PACKAGE%"=="0" (
    echo.
    echo Done: %BUILD_DIR%\scher.exe
    exit /b 0
)

rem --------------------------------------------------------------------------
rem  Package into build\scher.zip.
rem  Only MinGW runtime libraries are collected; Windows supplies the rest.
rem --------------------------------------------------------------------------
echo.
echo [3/3] Packaging...
set "STAGE=%BUILD_DIR%\package\scher"
set "ZIP=%BUILD_DIR%\scher.zip"

if exist "%BUILD_DIR%\package" rmdir /s /q "%BUILD_DIR%\package"
mkdir "%STAGE%" >nul 2>&1

copy /y "%BUILD_DIR%\scher.exe" "%STAGE%\" >nul
if errorlevel 1 (
    echo [ERROR] cannot copy scher.exe
    exit /b 1
)

rem Dependency scan: with -static linking this finds nothing, but the check
rem keeps the package correct if the link settings ever change back. The
rem listing goes through a temp file: a quoted exe inside a for /f command
rem would swallow the pipe and hand it to objdump as a file name.
set "DLL_COUNT=0"
set "DEPS=%TEMP%\scher-deps.txt"
"%MINGW_BIN%\objdump.exe" -p "%BUILD_DIR%\scher.exe" > "%DEPS%" 2>nul
for /f "tokens=2 delims=:" %%D in ('type "%DEPS%" ^| findstr /i "DLL Name"') do (
    set "DLL=%%D"
    set "DLL=!DLL: =!"
    if exist "%MINGW_BIN%\!DLL!" (
        copy /y "%MINGW_BIN%\!DLL!" "%STAGE%\" >nul
        set /a DLL_COUNT+=1
        echo     runtime: !DLL!
    )
)
if "%DLL_COUNT%"=="0" echo     runtime: none needed, linked statically

rem The in-app syntax guide, when the project ships one
if exist "%PROJECT_DIR%\syntax.md" copy /y "%PROJECT_DIR%\syntax.md" "%STAGE%\" >nul

if exist "%ZIP%" del /f /q "%ZIP%"
where tar >nul 2>&1
if errorlevel 1 goto zip_fallback
rem Relative paths: bsdtar reads "E:\..." as a remote host and fails to connect
pushd "%BUILD_DIR%"
tar -a -c -f "scher.zip" -C "package" scher
popd
goto zip_check

:zip_fallback
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%STAGE%' -DestinationPath '%ZIP%' -Force"

:zip_check
if not exist "%ZIP%" (
    echo [ERROR] packaging failed, scher.zip was not created
    exit /b 1
)

echo.
for %%F in ("%ZIP%") do echo Done: %%~fF  ^(%%~zF bytes^)
echo Unzip and run scher.exe.
exit /b 0
