@echo off
setlocal EnableDelayedExpansion
rem ============================================================
rem  Androidify - build the R-Slice Android APK via Lime
rem
rem  Checks the toolchain (Haxe, haxelibs, JDK 17, Android SDK,
rem  NDK, lime config), fixes what it can, then runs
rem  "lime build android".
rem
rem  Usage:  Androidify.bat [--check] [--install]
rem            --check    verify only, never build or install
rem            --install   download/install missing pieces
rem                       (JDK 17 zip, Android cmdline-tools,
rem                        SDK packages, NDK) before building
rem ============================================================

set "ROOT=%~dp0.."
pushd "%ROOT%"

set CHECK_ONLY=0
set DO_INSTALL=0
for %%a in (%*) do (
  if /i "%%a"=="--check" set CHECK_ONLY=1
  if /i "%%a"=="--install" set DO_INSTALL=1
)

set FAIL=0
set WARN=0

echo ============================================================
echo  Androidify - R-Slice APK builder
echo ============================================================
echo.

rem ---------- 1. Haxe toolchain ----------
echo [1/6] Haxe toolchain
if not defined HAXE_DIR where haxe >nul 2>&1 && set "HAXE_DIR=PATH"
if not defined HAXE_DIR if exist "C:\HaxeToolkit\haxe\haxe.exe" set "HAXE_DIR=C:\HaxeToolkit\haxe"
if not defined HAXE_DIR for /d %%d in ("C:\haxe\haxe_*") do if exist "%%d\haxe.exe" set "HAXE_DIR=%%d"
if defined HAXE_DIR if not "%HAXE_DIR%"=="PATH" set "PATH=%HAXE_DIR%;%PATH%"
if defined HAXE_DIR (
  for /f "delims=" %%v in ('haxe --version 2^>nul') do set "HAXE_VER=%%v"
  echo   [ OK ] haxe !HAXE_VER!
) else (
  echo   [FAIL] haxe.exe not found on PATH, C:\HaxeToolkit\haxe, or C:\haxe\haxe_*
  echo          Install Haxe from https://haxe.org then re-run.
  set FAIL=1
)
where haxelib >nul 2>&1 || (
  echo   [FAIL] haxelib.exe not found - it ships with Haxe
  set FAIL=1
)

rem HAXE_STD_PATH: some installs can't auto-detect std
if not defined HAXE_STD_PATH (
  for /f "delims=" %%i in ('where haxe 2^>nul') do (
    if exist "%%~dpi..\std\Any.hx" set "HAXE_STD_PATH=%%~dpi..\std"
    if exist "%%~dpistd\Any.hx" set "HAXE_STD_PATH=%%~dpistd"
  )
  if not defined HAXE_STD_PATH if exist "C:\HaxeToolkit\haxe\std\Any.hx" set "HAXE_STD_PATH=C:\HaxeToolkit\haxe\std"
  if not defined HAXE_STD_PATH for /d %%d in ("C:\haxe\haxe_*") do if exist "%%d\std\Any.hx" set "HAXE_STD_PATH=%%d\std"
)
if defined HAXE_STD_PATH (
  echo   [ OK ] HAXE_STD_PATH=%HAXE_STD_PATH%
) else (
  echo   [WARN] HAXE_STD_PATH not set and std not auto-found
  set /a WARN+=1
)

rem ---------- 2. Project haxelibs ----------
echo.
echo [2/6] Project haxelibs (.haxelib)
if exist ".haxelib\lime\git\RunScript.hx" (
  echo   [ OK ] lime + dependencies present
) else (
  echo   [FAIL] .haxelib missing - run:  hmm install
  echo          ^(install hmm first: haxelib --global install hmm^)^
  set FAIL=1
)

rem ---------- helpers ----------
rem find javac, install Temurin JDK 17 zip if --install and missing
set "JDK_ROOT="
call :find_jdk
if not defined JDK_ROOT if %DO_INSTALL%==1 call :install_jdk
call :check_jdk

rem find Android SDK, install cmdline-tools + packages if --install
if not defined ANDROID_HOME if defined ANDROID_SDK_ROOT set "ANDROID_HOME=%ANDROID_SDK_ROOT%"
if not defined ANDROID_HOME set "ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk"
set "SDKM="
call :find_sdkmanager
if not defined SDKM if %DO_INSTALL%==1 call :install_sdk_tools
call :find_sdkmanager
if defined SDKM call :install_sdk_packages

rem ---------- 3. JDK 17 ----------
echo.
echo [3/6] JDK 17
if defined JDK_ROOT (
  echo   [ OK ] JDK at !JDK_ROOT! ^(javac !JAVAC_VER!^)
) else (
  echo   [FAIL] javac not found - install JDK 17 ^(Temurin recommended^)
  echo          https://adoptium.net/temurin/releases/?version=17
  echo          or re-run with --install to fetch it automatically
  set FAIL=1
)

rem ---------- 4. Android SDK ----------
echo.
echo [4/6] Android SDK
if exist "%ANDROID_HOME%\platforms\android-35" (
  echo   [ OK ] ANDROID_HOME=%ANDROID_HOME%
) else (
  if %DO_INSTALL%==1 (
    echo   [FAIL] Android SDK still missing after --install
    set FAIL=1
  ) else (
    echo   [FAIL] Android SDK not usable at %ANDROID_HOME%
    echo          Re-run with --install to fetch cmdline-tools + packages,
    echo          or set ANDROID_HOME to an existing SDK.
    set FAIL=1
  )
)

rem ---------- 5. lime config ----------
echo.
echo [5/6] Lime android config
if %FAIL%==0 (
  set "NDK_DIR="
  for /d %%d in ("%ANDROID_HOME%\ndk\*") do set "NDK_DIR=%%d"
  if defined NDK_DIR (
    set "LIME_JH=%JAVA_HOME%"
    if defined JDK_ROOT set "LIME_JH=!JDK_ROOT!"
    haxelib run lime config ANDROID_SDK "%ANDROID_HOME%" >nul 2>&1
    haxelib run lime config ANDROID_NDK_ROOT "%NDK_DIR%" >nul 2>&1
    if defined LIME_JH haxelib run lime config JAVA_HOME "!LIME_JH!" >nul 2>&1
    haxelib run lime config ANDROID_SETUP true >nul 2>&1
    echo   [ OK ] wrote lime config ^(SDK, NDK: !NDK_DIR!, JAVA_HOME^)
  ) else (
    echo   [FAIL] no NDK found under %ANDROID_HOME%\ndk
    if %DO_INSTALL%==1 (
      echo          --install could not provision the NDK - install it manually:
      echo          sdkmanager "ndk;29.0.13113456" --sdk_root="%ANDROID_HOME%"
    ) else (
      echo          Re-run with --install, or:
      echo          sdkmanager "ndk;29.0.13113456" --sdk_root="%ANDROID_HOME%"
    )
    set FAIL=1
  )
)

rem ---------- 6. build ----------
echo.
echo [6/6] Build
if %FAIL%==1 (
  echo.
  echo ============================================================
  echo  NOT BUILDING - fix the [FAIL] items above first.
  echo  Manual fallback:  haxelib run lime setup android
  echo                     haxelib run lime build android
  echo ============================================================
  popd
  exit /b 1
)
if %CHECK_ONLY%==1 (
  echo.
  echo ============================================================
  echo  CHECK PASSED - run without --check to build the APK.
  echo ============================================================
  popd
  exit /b 0
)
echo   Running: haxelib run lime build android
if defined JDK_ROOT set "JAVA_HOME=!JDK_ROOT!" && set "PATH=!JDK_ROOT!\bin;!PATH!"
call haxelib run lime build android
set RC=%ERRORLEVEL%
echo.
if %RC%==0 (
  echo ============================================================
  echo  BUILD OK
  for %%f in ("export\release\android\bin\*.apk" "export\release\android\bin\*.aab") do (
    if exist "%%f" echo   Output: %CD%\%%~nxf
  )
  echo ============================================================
) else (
  echo ============================================================
  echo  BUILD FAILED ^(exit %RC%^) - see output above.
  echo ============================================================
)
popd
exit /b %RC%

rem ============================================================
rem  subroutines
rem ============================================================

:find_jdk
rem Sets JDK_ROOT to a JDK 17 home if javac is usable.
set "JDK_ROOT="
if defined JAVA_HOME if exist "%JAVA_HOME%\bin\javac.exe" set "JDK_ROOT=%JAVA_HOME%"
if not defined JDK_ROOT (
  for /f "delims=" %%v in ('where javac 2^>nul') do (
    for %%i in ("%%v\..\..") do if exist "%%~fi\bin\javac.exe" set "JDK_ROOT=%%~fi"
  )
)
rem project-local JDK from a previous --install run
if not defined JDK_ROOT for /d %%d in ("%ROOT%\tools\jdk-17*") do if exist "%%d\bin\javac.exe" set "JDK_ROOT=%%d"
if defined JDK_ROOT (
  for /f "tokens=3" %%v in ('"%JDK_ROOT%\bin\javac.exe" -version 2^>^&1') do set "JAVAC_VER=%%v"
)
exit /b 0

:install_jdk
rem Download Temurin JDK 17 (zip, no admin needed) into tools\jdk-17*
echo   [ .. ] installing JDK 17 via Adoptium archive...
set "JDK_ZIP=%TEMP%\androidify-jdk17.zip"
set "JDK_DST=%ROOT%\tools"
set "JDK_URL=https://api.adoptium.net/v3/binary/latest/17/ga/windows/x64/jdk/hotspot/normal/archive"
curl -L --retry 3 -o "%JDK_ZIP%" "%JDK_URL%" >nul 2>&1
if errorlevel 1 (
  echo   [FAIL] JDK download failed - get it manually:
  echo          https://adoptium.net/temurin/releases/?version=17
  exit /b 0
)
powershell -NoProfile -Command "Expand-Archive -Path '%JDK_ZIP%' -DestinationPath '%JDK_DST%' -Force" >nul 2>&1
if errorlevel 1 (
  echo   [FAIL] JDK extract failed
  exit /b 0
)
for /d %%d in ("%JDK_DST%\jdk-17*") do set "JDK_ROOT=%%d"
if defined JDK_ROOT (
  echo   [ OK ] JDK 17 installed to !JDK_ROOT!
) else (
  echo   [WARN] JDK extracted but folder not recognized - check %JDK_DST%
)
exit /b 0

:check_jdk
if defined JDK_ROOT (
  if not defined JAVAC_VER for /f "tokens=3" %%v in ('"%JDK_ROOT%\bin\javac.exe" -version 2^>^&1') do set "JAVAC_VER=%%v"
)
exit /b 0

:find_sdkmanager
rem Sets SDKM to sdkmanager.bat if a usable Android SDK exists.
set "SDKM="
if exist "%ANDROID_HOME%\cmdline-tools\latest\bin\sdkmanager.bat" set "SDKM=%ANDROID_HOME%\cmdline-tools\latest\bin\sdkmanager.bat"
if not defined SDKM if exist "%ANDROID_HOME%\tools\bin\sdkmanager.bat" set "SDKM=%ANDROID_HOME%\tools\bin\sdkmanager.bat"
for /d %%d in ("%ANDROID_HOME%\cmdline-tools\*") do (
  if not defined SDKM if exist "%%d\bin\sdkmanager.bat" set "SDKM=%%d\bin\sdkmanager.bat"
)
exit /b 0

:install_sdk_tools
rem Download Android cmdline-tools and unpack into %ANDROID_HOME%\cmdline-tools\latest
echo   [ .. ] installing Android command-line tools...
set "SDK_ZIP=%TEMP%\androidify-cmdtools.zip"
set "OK=0"
for %%u in (
  "https://dl.google.com/android/repository/commandlinetools-win-13114758_latest.zip"
  "https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip"
) do (
  if not defined OK (
    curl -L --retry 3 -o "%SDK_ZIP%" "%%u" >nul 2>&1
    if not errorlevel 1 set "OK=1"
  )
)
if not defined OK (
  echo   [FAIL] cmdline-tools download failed - install Android Studio
  echo          command-line tools manually, or set ANDROID_HOME.
  exit /b 0
)
set "STAGE=%TEMP%\androidify-cmdtools"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
powershell -NoProfile -Command "Expand-Archive -Path '%SDK_ZIP%' -DestinationPath '%STAGE%' -Force" >nul 2>&1
if not exist "%STAGE%\cmdline-tools" (
  echo   [FAIL] cmdline-tools extract failed
  exit /b 0
)
if not exist "%ANDROID_HOME%\cmdline-tools\latest" mkdir "%ANDROID_HOME%\cmdline-tools\latest"
xcopy /e /i /y /q "%STAGE%\cmdline-tools\*" "%ANDROID_HOME%\cmdline-tools\latest\" >nul
rmdir /s /q "%STAGE%"
echo   [ OK ] cmdline-tools installed to %ANDROID_HOME%\cmdline-tools\latest
echo   [ .. ] accepting SDK licenses + installing packages (this takes a while)...
call :accept_licenses
call :install_sdk_packages
exit /b 0

:accept_licenses
rem sdkmanager needs a JDK on PATH/JAVA_HOME
if defined JDK_ROOT (
  set "JAVA_HOME=!JDK_ROOT!"
  set "PATH=!JDK_ROOT!\bin;!PATH!"
)
(
  for /l %%i in (1,1,40) do @echo y
) | "%SDKM%" --sdk_root="%ANDROID_HOME%" --licenses >nul 2>&1
exit /b 0

:install_sdk_packages
if not defined SDKM call :find_sdkmanager
if not defined SDKM exit /b 0
if defined JDK_ROOT (
  set "JAVA_HOME=!JDK_ROOT!"
  set "PATH=!JDK_ROOT!\bin;!PATH!"
)
"%SDKM%" --sdk_root="%ANDROID_HOME%" --install "platforms;android-29" "platforms;android-35" "build-tools;35.0.0" "ndk;29.0.13113456"
exit /b 0
