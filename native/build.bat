@echo off
setlocal
cd /d "%~dp0"

:: Build ddraw.dll (MPBT DirectDraw shim) — 32-bit x86
:: Output: native\ddraw.dll  (embedded into Rust binary via include_bytes!)

:: Locate vcvarsall.bat via vswhere (works for any VS edition, including CI runners)
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist %VSWHERE% (
    for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set _VS_INSTALL_ROOT=%%i
)

if defined _VS_INSTALL_ROOT (
    set VCVARS="%_VS_INSTALL_ROOT%\VC\Auxiliary\Build\vcvarsall.bat"
) else (
    :: Fallback for VS 2022 Community local installs without vswhere in PATH
    set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
)

if not exist %VCVARS% (
    echo ERROR: vcvarsall.bat not found. Install Visual Studio with the "Desktop development with C++" workload.
    exit /b 1
)

:: Only skip vcvarsall when both cl.exe and the MSVC include/lib environment
:: are already present. Some shells expose cl.exe on PATH without INCLUDE/LIB,
:: which still makes windows.h resolution fail.
where cl.exe >nul 2>&1
if %ERRORLEVEL% neq 0 goto :init_env
if not "%INCLUDE%"=="" if not "%LIB%"=="" goto :build

:init_env
call %VCVARS% x86

:build
cl.exe /nologo /O2 /GS- /EHsc /LD ^
    "%~dp0ddraw.cpp" ^
    /Fe"%~dp0ddraw.dll" ^
    /link /DEF:"%~dp0ddraw.def" ^
    ole32.lib uuid.lib kernel32.lib user32.lib gdi32.lib

if %ERRORLEVEL% neq 0 (
    echo Build FAILED.
    exit /b %ERRORLEVEL%
)

echo Build OK: %~dp0ddraw.dll
