@echo off
rem Usage: scripts\build-installer.bat [ISCC options...]
rem Builds the shareable installer dist\PureSheen-Dynamics-Setup-<version>.exe in one go:
rem   1. configures build-release (Ninja + MSVC 2022 x64 Release, STATIC MSVC runtime so the people you share it with
rem      need no Visual C++ redistributable) and builds PureSheen_VST3, PureSheen_CLAP and PureSheen_Standalone
rem   2. copies them to installer\payload
rem   3. compiles installer\PureSheen.iss with Inno Setup 6 (ISCC.exe; Inno Setup itself installed per-user or for all
rem      users - both locations are found)
rem Extra arguments go to ISCC, e.g.  scripts\build-installer.bat /DTestRoot=C:\Temp\puresheen-test  (test variant).
setlocal
set "ROOT=%~dp0.."
set "BUILD_DIR=build-release"

set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo Inno Setup 6 not found. Install it from https://jrsoftware.org/isdl.php and run this again.
  exit /b 1
)

if not defined VSCMD_VER (
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)

pushd "%ROOT%" || exit /b 1
rem Configure every time (cheap after the first run) so the static runtime is enforced even if build-release was first
rem configured some other way.
cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded >nul || goto fail
rem One format at a time: three link-time-optimised links in parallel can crash the MSVC linker (C1001).
cmake --build "%BUILD_DIR%" --target PureSheen_VST3 || goto fail
cmake --build "%BUILD_DIR%" --target PureSheen_CLAP || goto fail
cmake --build "%BUILD_DIR%" --target PureSheen_Standalone || goto fail

set "ART=%BUILD_DIR%\PureSheen_artefacts\Release"
if exist "installer\payload" rmdir /s /q "installer\payload"
mkdir "installer\payload\VST3" "installer\payload\CLAP" "installer\payload\Standalone" || goto fail
robocopy "%ART%\VST3\PureSheen Dynamics.vst3" "installer\payload\VST3\PureSheen Dynamics.vst3" /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto fail
copy /y "%ART%\CLAP\PureSheen Dynamics.clap" "installer\payload\CLAP\" >nul || goto fail
copy /y "%ART%\Standalone\PureSheen Dynamics.exe" "installer\payload\Standalone\" >nul || goto fail

rem The installer version is the project() version in CMakeLists.txt.
set "APPVER="
for /f "tokens=3" %%v in ('findstr /r /c:"^project(PureSheen VERSION" CMakeLists.txt') do set "APPVER=%%v"
if "%APPVER%"=="" goto fail
"%ISCC%" /Q /DAppVersion=%APPVER% %* "installer\PureSheen.iss" || goto fail
echo.
echo Installer ready in %CD%\dist
popd
exit /b 0

:fail
echo Building the installer FAILED.
popd
exit /b 1
