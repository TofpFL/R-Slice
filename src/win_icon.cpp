// Forces the running window's icon (title bar + taskbar) from the exe's
// embedded resources. GLFW sets the class icon from the "GLFW_ICON" resource
// at registration, but a runtime WM_SETICON makes the result independent of
// resource-name lookup quirks. Windows-only; empty elsewhere.
#include "platform.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

extern "C" void* GetWindowHandle(void);  // raylib

namespace gh::platform {

void setWindowIcon() {
    HWND hwnd = static_cast<HWND>(GetWindowHandle());
    if (!hwnd) return;
    HMODULE exe = GetModuleHandleW(nullptr);
    HICON icon = static_cast<HICON>(LoadImageW(exe, L"GLFW_ICON", IMAGE_ICON, 0, 0,
                                               LR_DEFAULTSIZE | LR_SHARED));
    if (!icon)
        icon = static_cast<HICON>(LoadImageW(exe, L"IDI_ICON1", IMAGE_ICON, 0, 0,
                                               LR_DEFAULTSIZE | LR_SHARED));
    if (!icon) return;
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
}

}  // namespace gh::platform
#else
namespace gh::platform {
void setWindowIcon() {}
}  // namespace gh::platform
#endif
