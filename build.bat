@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

cd /d "%~dp0"

echo === dysonbehind.dll build ===

rem ---- 1. CMake ----
where cmake >nul 2>&1
if errorlevel 1 (
    echo [!] cmake not found in PATH
    echo     install CMake or add it to PATH
    pause
    exit /b 1
)

rem ---- 2. Detect Visual Studio via vswhere ----
set "VSVER="
set "VSPATH="

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property catalog_productLineVersion`) do set "VSVER=%%i"
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)

rem ---- 3. Fallback: scan VS year folders ----
if not defined VSVER (
    set "YEARS=18 17 16 15"
    for %%y in (!YEARS!) do (
        if not defined VSVER (
            if exist "%ProgramFiles%\Microsoft Visual Studio\%%y" (
                for /f "tokens=*" %%d in ('dir /b /ad "%ProgramFiles%\Microsoft Visual Studio\%%y" 2^>nul') do (
                    if exist "%ProgramFiles%\Microsoft Visual Studio\%%y\%%d\MSBuild\Current\Bin\MSBuild.exe" (
                        set "VSVER=%%y"
                        set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\%%y\%%d"
                    )
                )
            )
            if not defined VSVER (
                if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%y" (
                    for /f "tokens=*" %%d in ('dir /b /ad "%ProgramFiles(x86)%\Microsoft Visual Studio\%%y" 2^>nul') do (
                        if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%y\%%d\MSBuild\Current\Bin\MSBuild.exe" (
                            set "VSVER=%%y"
                            set "VSPATH=%ProgramFiles(x86)%\Microsoft Visual Studio\%%y\%%d"
                        )
                    )
                )
            )
        )
    )
)

rem ---- 4. Map year -> CMake generator ----
set "GEN="
if defined VSVER (
    if "!VSVER!"=="18" set "GEN=Visual Studio 18 2026"
    if "!VSVER!"=="17" set "GEN=Visual Studio 17 2022"
    if "!VSVER!"=="16" set "GEN=Visual Studio 16 2019"
    if "!VSVER!"=="15" set "GEN=Visual Studio 15 2017"
)

if not defined GEN (
    echo [!] Visual Studio with C++ tools not found
    echo     install VS 2017/2019/2022/2026 with "Desktop development with C++"
    pause
    exit /b 1
)

echo [*] Visual Studio !VSVER! detected: !VSPATH!
echo [*] generator: !GEN!

rem ---- 5. Configure (clean stale cache if generator changed) ----
if exist build\CMakeCache.txt (
    findstr /c:"!GEN!" build\CMakeCache.txt >nul 2>&1
    if errorlevel 1 (
        echo [*] stale cache, reconfiguring...
        rmdir /s /q build
    )
)

if not exist build (
    echo [*] configuring...
    cmake -B build -G "!GEN!" -A x64
    if errorlevel 1 (
        echo [!] cmake configure failed
        pause
        exit /b 1
    )
)

rem ---- 6. Build ----
echo [*] building Release...
cmake --build build --config Release
if errorlevel 1 (
    echo [!] build failed
    pause
    exit /b 1
)

echo.
echo [OK] %~dp0build\Release\dysonbehind.dll
echo [OK] %~dp0build\Release\Injector.exe
echo     1. run Injector.exe (as admin) - it injects the DLL
echo     2. INSERT toggles menu in game
echo.
pause
