// Native dialogs + exe-dir helpers for raidtool. Kept free of raylib.h:
// windows.h and raylib.h declare conflicting symbols (CloseWindow etc.).
#pragma once
#include <string>

namespace raidtool {

std::string pickFile(const char* title, const char* filter);
std::string pickFolder(const std::string& title);
std::string moduleDir();   // directory containing raidtool.exe
bool isDir(const std::string& path);
void installCrashLog();    // writes raidtool_crash.log on unhandled exception

}  // namespace raidtool
