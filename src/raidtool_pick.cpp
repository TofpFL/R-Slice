#include "raidtool_pick.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <cstring>
#endif

namespace raidtool {

#if defined(_WIN32)
std::string pickFile(const char* title, const char* filter) {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    // lpstrFilter is a double-NUL-terminated list of NUL-separated pairs;
    // strlen() would stop at the first embedded NUL.
    size_t flen = 0;
    while (!(filter[flen] == '\0' && filter[flen + 1] == '\0')) ++flen;
    flen += 2;
    std::wstring wf(filter, filter + flen);
    std::wstring wt(title, title + std::strlen(title) + 1);
    ofn.lpstrFilter = wf.c_str();
    ofn.lpstrTitle = wt.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return {};
    char out[MAX_PATH * 4] = {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr);
    std::string s(out, n > 0 ? size_t(n - 1) : 0);
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

std::string pickFolder(const std::string& title) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring wt(title.size() + 1, L'\0');
    wt.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wt.data(), int(wt.size()))));
    BROWSEINFOW bi{};
    bi.lpszTitle = wt.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    std::string out;
    if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
        wchar_t path[MAX_PATH * 4] = {};
        if (SHGetPathFromIDListW(pidl, path)) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                out.resize(size_t(n));
                WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), n, nullptr, nullptr);
                out.resize(size_t(n - 1));
            }
        }
        CoTaskMemFree(pidl);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    for (char& c : out) if (c == '\\') c = '/';
    return out;
}

std::string moduleDir() {
    char exePath[MAX_PATH * 2] = {};
    GetModuleFileNameA(nullptr, exePath, sizeof exePath);
    std::string base = exePath;
    base = base.substr(0, base.find_last_of("/\\"));
    return base;
}

bool isDir(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void installCrashLog() {
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ei) -> LONG {
        if (FILE* f = nullptr; fopen_s(&f, "raidtool_crash.log", "w") == 0 && f) {
            fprintf(f, "code=0x%08lX addr=%p tid=%lu\n",
                    ei->ExceptionRecord->ExceptionCode,
                    ei->ExceptionRecord->ExceptionAddress,
                    GetCurrentThreadId());
            fclose(f);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    });
}
#else
std::string pickFile(const char*, const char*) { return {}; }
std::string pickFolder(const std::string&) { return {}; }
std::string moduleDir() { return "."; }
bool isDir(const std::string&) { return false; }
void installCrashLog() {}
#endif

}  // namespace raidtool
