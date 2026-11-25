cmake_minimum_required(VERSION 3.20)
project(RAIDEngine VERSION 1.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(GH_BUILD_TESTS "Build the headless test executable" ON)
# raidtool is a Win32 raylib GUI that shells out to raid_engine.exe.
option(GH_BUILD_RADIOTOOL "Build the raidtool GUI (needs raylib, Windows only)" ON)

find_package(Threads REQUIRED)

# Static MSVC runtime: binaries run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# The complete source package ships every library in third_party/: use those
# copies instead of downloading (then the build needs no internet).
foreach(dep IN ITEMS lua miniz raylib)
  string(TOUPPER ${dep} dep_upper)
  if(NOT FETCHCONTENT_SOURCE_DIR_${dep_upper} AND IS_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/${dep})
    set(FETCHCONTENT_SOURCE_DIR_${dep_upper} ${CMAKE_SOURCE_DIR}/third_party/${dep})
    message(STATUS "Using third_party/${dep}")
  endif()
endforeach()

# ---- Lua 5.4 (mod scripts) -----------------------------------------------------
# Compiled as C++ so script errors unwind with exceptions instead of longjmp.
# Set FETCHCONTENT_SOURCE_DIR_LUA to use a local checkout.
FetchContent_Declare(lua
  GIT_REPOSITORY https://github.com/lua/lua.git
  GIT_TAG v5.4.7
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(lua)  # no CMakeLists.txt upstream: this only downloads
set(GH_LUA_SOURCES)
foreach(f lapi lauxlib lbaselib lcode lcorolib lctype ldebug ldo ldump lfunc lgc llex lmathlib lmem
          lobject lopcodes lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio)
  list(APPEND GH_LUA_SOURCES ${lua_SOURCE_DIR}/${f}.c)
endforeach()
set_source_files_properties(${GH_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(gh_lua STATIC ${GH_LUA_SOURCES})
target_include_directories(gh_lua PUBLIC ${lua_SOURCE_DIR})
if(MSVC)
  target_compile_options(gh_lua PRIVATE /TP /W0)
  target_compile_definitions(gh_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(gh_lua PRIVATE -w)
endif()

# ---- miniz (installing mods from .zip) ------------------------------------------
# Set FETCHCONTENT_SOURCE_DIR_MINIZ to use a local checkout.
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.0.2
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-add)  # we compile the four sources ourselves
FetchContent_MakeAvailable(miniz)
add_library(gh_miniz STATIC
  ${miniz_SOURCE_DIR}/miniz.c ${miniz_SOURCE_DIR}/miniz_tdef.c
  ${miniz_SOURCE_DIR}/miniz_tinfl.c ${miniz_SOURCE_DIR}/miniz_zip.c)
target_include_directories(gh_miniz PUBLIC ${miniz_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/cmake)
if(MSVC)
  target_compile_definitions(gh_miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  # miniz_tdef.c relies on a C89-style tentative definition + later
  # definition of s_tdefl_num_probes; strict C99+/C++ rejects it. MSVC
  # accepts it as an extension; GCC/Clang need C89 dialect for this lib.
  target_compile_options(gh_miniz PRIVATE -w -std=gnu89)
endif()

# ---- engine core: pure C++, no window/audio dependency ----------------------
# gh_core is what the game links. It stays STATIC on every platform —
# including Android, where the archives are absorbed into the APK's .so at
# link time (per-ABI builds live under engine/build-android-<abi>).
add_library(gh_core STATIC
  src/json.cpp
  src/chart.cpp
  src/chart_stream.cpp
  src/basegame.cpp
  src/shader_glsl.cpp
  src/effects.cpp
  src/hscript.cpp
  src/hscript_bridge.cpp
  src/session.cpp
  src/synth.cpp
  src/composer.cpp
  src/psych.cpp
  src/scene.cpp
  src/song_scan.cpp
  src/flp_import.cpp
  src/raid.cpp
  src/raid_doc.cpp
  src/modchart.cpp
  src/visuals.cpp
  src/raid_chart.cpp
  src/lua_host.cpp
  src/replay.cpp
  src/rchr.cpp
  src/mod_install.cpp
  src/autochart.cpp
  src/ram.cpp
  src/engine_abi.cpp
  src/rvfx.cpp
)
target_include_directories(gh_core PUBLIC src)
target_link_libraries(gh_core PUBLIC Threads::Threads gh_lua gh_miniz)
if(MSVC)
  target_compile_options(gh_core PRIVATE /W3 /utf-8)
else()
  target_compile_options(gh_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(NOT ANDROID)
  if(GH_BUILD_TESTS)
    add_executable(gh_tests tests/tests.cpp tests/psych_tests.cpp tests/scan_tests.cpp tests/raid_tests.cpp)
    target_link_libraries(gh_tests PRIVATE gh_core)
    if(MSVC)
      target_compile_options(gh_tests PRIVATE /utf-8)
    endif()
    enable_testing()
    add_test(NAME core COMMAND gh_tests)
  endif()

  # Standalone RAID Engine CLI (no raylib): convert/info/verify/window/meta.
  add_executable(raid_engine src/raid_cli.cpp)
  target_link_libraries(raid_engine PRIVATE gh_core)
  if(WIN32)
    target_sources(raid_engine PRIVATE raid_engine.rc)  # embeds RAID_File_Icon.ico
  endif()
endif()

# raidtool: GUI front-end that shells out to raid_engine.exe.
if(GH_BUILD_RADIOTOOL AND WIN32)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raylib)
  add_executable(raidtool WIN32 src/raidtool.cpp src/raidtool_pick.cpp)
  add_dependencies(raidtool raid_engine)
  target_link_libraries(raidtool PRIVATE raylib comdlg32 shell32 ole32)
  target_compile_options(raidtool PRIVATE /W3 /utf-8)
  target_link_options(raidtool PRIVATE /ENTRY:mainCRTStartup)
  if(EXISTS ${CMAKE_SOURCE_DIR}/assets/tool_sfx)
    add_custom_command(TARGET raidtool POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/tool_sfx $<TARGET_FILE_DIR:raidtool>/assets/tool_sfx)
  endif()
  if(EXISTS ${CMAKE_SOURCE_DIR}/tools/discrepancy.py)
    add_custom_command(TARGET raidtool POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/tools/discrepancy.py $<TARGET_FILE_DIR:raidtool>/discrepancy.py)
  endif()
endif()
