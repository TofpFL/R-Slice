@echo off
setlocal EnableDelayedExpansion
rem build_engine.bat [windows^|android^|all] [abi ...]
rem   windows  - MSVC x64 libs -> engine\build-msvc\Release\gh_*.lib
rem   android  - NDK libs per ABI -> engine\build-android\<abi>\libgh_*.a
rem   all      - both (default when no args)
rem ABIs: arm64-v8a armeabi-v7a x86 x86_64  (default: arm64-v8a x86_64)

set "WHAT=%~1"
if "%WHAT%"=="" set "WHAT=all"
set "ROOT=%~dp0.."
set "ENGINE=%ROOT%\engine"

where cmake >nul 2>nul
if errorlevel 1 (
  echo [FAIL] cmake not found on PATH.
  exit /b 1
)

if /i "%WHAT%"=="windows" goto :windows
if /i "%WHAT%"=="android" goto :android
if /i "%WHAT%"=="all" goto :windows

echo [FAIL] unknown target "%WHAT%" - use windows ^| android ^| all
exit /b 1

:windows
echo [*] Building engine for Windows ^(MSVC x64^)...
cmake -S "%ENGINE%" -B "%ENGINE%\build-msvc" -G "Visual Studio 17 2022" -A x64 -DGH_BUILD_RADIOTOOL=OFF
if errorlevel 1 exit /b 1
cmake --build "%ENGINE%\build-msvc" --config Release --target gh_core gh_lua gh_miniz
if errorlevel 1 exit /b 1
echo [OK] engine\build-msvc\Release\gh_*.lib
if /i "%WHAT%"=="windows" exit /b 0
echo.

:android
set "NDK=%ANDROID_NDK_ROOT%"
if "%NDK%"=="" set "NDK=%ANDROID_NDK_HOME%"
if "%NDK%"=="" (
  for /d %%d in ("%ANDROID_HOME%\ndk\*") do set "NDK=%%d"
)
if "%NDK%"=="" (
  for /d %%d in ("%LOCALAPPDATA%\Android\Sdk\ndk\*") do set "NDK=%%d"
)
if "%NDK%"=="" (
  echo [FAIL] Android NDK not found. Set ANDROID_NDK_ROOT, ANDROID_HOME,
  echo        or run tools\Androidify.bat --install first.
  exit /b 1
)
echo [*] Using NDK: %NDK%

set "TOOLCHAIN=%NDK:\=/%/build/cmake/android.toolchain.cmake"
set "ABIS="
:collect_abi
if "%~2"=="" goto :abis_done
set "ABIS=!ABIS! %~2"
shift & goto :collect_abi
:abis_done
if "%ABIS%"=="" set "ABIS=arm64-v8a x86_64"

for %%a in (%ABIS%) do (
  echo [*] Building engine for Android %%a...
  cmake -S "%ENGINE%" -B "%ENGINE%\build-android\%%a" -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" ^
    -DANDROID_ABI=%%a -DANDROID_PLATFORM=android-28 ^
    -DCMAKE_BUILD_TYPE=Release -DGH_BUILD_TESTS=OFF -DGH_BUILD_RADIOTOOL=OFF
  if errorlevel 1 exit /b 1
  cmake --build "%ENGINE%\build-android\%%a"
  if errorlevel 1 exit /b 1
)
echo [OK] engine\build-android\^<abi^>\libgh_*.a
exit /b 0
