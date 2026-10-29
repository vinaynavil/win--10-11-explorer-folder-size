#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <string>

#include "resource.h"
#include "SizeCalculator.h"
#include "OverlayRenderer.h"
#include "ExplorerTracker.h"

namespace {
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_ASYNC_SIZE_UPDATED = WM_APP + 2;

constexpr UINT ID_TRAY_PAUSE = 1001;
constexpr UINT ID_TRAY_CLEAR_CACHE = 1002;
constexpr UINT ID_TRAY_AUTOSTART = 1003;
constexpr UINT ID_TRAY_EXIT = 1004;

constexpr UINT_PTR kIdleTrimTimerId = 3001;

constexpr wchar_t kMutexName[] = L"Local\\Win11ExplorerFolderSize_SingleInstance_Mutex";
constexpr wchar_t kWndClassName[] = L"Win11ExplorerFolderSizeMsgWnd";
constexpr wchar_t kRunRegKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunAppName[] = L"ExplorerFolderSize";

NOTIFYICONDATAW g_nid{};
UINT g_msgTaskbarCreated = 0;
bool g_wasExplorerActive = true;

SizeCalculator* g_calculator = nullptr;
OverlayRenderer* g_renderer = nullptr;
ExplorerTracker* g_tracker = nullptr;

void TrimProcessMemory() {
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

bool IsExplorerForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg || IsIconic(fg)) {
        return false;
    }
    wchar_t cls[64]{};
    return (GetClassNameW(fg, cls, 64) > 0 && wcscmp(cls, L"CabinetWClass") == 0);
}

void AddOrRestoreTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

std::wstring GetQuotedExePath() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return L"\"" + std::wstring(exePath) + L"\"";
}

bool IsAutoStartEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t val[MAX_PATH + 8]{};
    DWORD size = sizeof(val);
    LSTATUS status = RegQueryValueExW(hKey, kRunAppName, nullptr, nullptr, reinterpret_cast<LPBYTE>(val), &size);
    RegCloseKey(hKey);
    return (status == ERROR_SUCCESS);
}

void SetAutoStartEnabled(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        const std::wstring cmd = GetQuotedExePath();
        RegSetValueExW(hKey, kRunAppName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(cmd.c_str()),
            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hKey, kRunAppName);
    }
    RegCloseKey(hKey);
}

void SyncAutoStartPathIfEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_READ | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return;
    }
    wchar_t existingVal[MAX_PATH + 8]{};
    DWORD size = sizeof(existingVal);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, kRunAppName, nullptr, &type, reinterpret_cast<LPBYTE>(existingVal), &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        const std::wstring expected = GetQuotedExePath();
        if (_wcsicmp(existingVal, expected.c_str()) != 0) {
            RegSetValueExW(hKey, kRunAppName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(expected.c_str()),
                static_cast<DWORD>((expected.size() + 1) * sizeof(wchar_t)));
        }
    }
    RegCloseKey(hKey);
}

void ShowTrayContextMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    bool isPaused = g_tracker && g_tracker->IsPaused();
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_PAUSE, isPaused ? L"Resume Folder Sizes" : L"Pause Folder Sizes");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_CLEAR_CACHE, L"Clear Size Cache");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    UINT autoStartFlags = MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, autoStartFlags, ID_TRAY_AUTOSTART, L"Run at Windows Startup");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_msgTaskbarCreated != 0 && msg == g_msgTaskbarCreated) {
        // Explorer restarted or crashed: restore tray icon and reconnect tracking
        AddOrRestoreTrayIcon();
        if (g_renderer) {
            g_renderer->TrimResources();
        }
        if (g_tracker) {
            g_tracker->RequestRefresh();
        }
        return 0;
    }

    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayContextMenu(hwnd);
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            if (g_tracker) {
                g_tracker->SetPaused(!g_tracker->IsPaused());
            }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_PAUSE:
            if (g_tracker) {
                g_tracker->SetPaused(!g_tracker->IsPaused());
            }
            break;
        case ID_TRAY_CLEAR_CACHE:
            if (g_calculator) {
                g_calculator->ClearCache();
            }
            if (g_renderer) {
                g_renderer->TrimResources();
            }
            TrimProcessMemory();
            if (g_tracker) {
                g_tracker->RequestRefresh();
            }
            break;
        case ID_TRAY_AUTOSTART:
            SetAutoStartEnabled(!IsAutoStartEnabled());
            break;
        case ID_TRAY_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == ExplorerTracker::kRefreshTimerId) {
            KillTimer(hwnd, ExplorerTracker::kRefreshTimerId);
            if (g_tracker) {
                g_tracker->OnTimerTick();
            }
        } else if (wParam == ExplorerTracker::kPeriodicTimerId) {
            if (g_tracker) {
                g_tracker->OnTimerTick();
            }
        } else if (wParam == kIdleTrimTimerId) {
            const bool activeNow = IsExplorerForeground();
            if (!activeNow && g_wasExplorerActive) {
                if (g_renderer) {
                    g_renderer->TrimResources();
                }
                TrimProcessMemory();
            }
            g_wasExplorerActive = activeNow;
        }
        return 0;

    case WM_ASYNC_SIZE_UPDATED:
        if (g_tracker) {
            g_tracker->RequestRefresh();
        }
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (g_renderer) {
            g_renderer->TrimResources();
        }
        if (g_tracker) {
            g_tracker->RequestRefresh();
        }
        return 0;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            if (g_renderer) {
                g_renderer->TrimResources();
            }
            if (g_tracker) {
                g_tracker->RequestRefresh();
            }
        }
        return TRUE;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wParam != FALSE) {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kIdleTrimTimerId);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int /*nCmdShow*/) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        CloseHandle(hMutex);
        return 1;
    }

    SyncAutoStartPathIfEnabled();
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    HICON hAppIconLarge = static_cast<HICON>(LoadImageW(
        hInstance,
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR));
    HICON hAppIconSmall = static_cast<HICON>(LoadImageW(
        hInstance,
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWndClassName;
    wc.hIcon = hAppIconLarge;
    wc.hIconSm = hAppIconSmall;
    RegisterClassExW(&wc);

    // Use a hidden top-level toolwindow (not HWND_MESSAGE) so Windows delivers TaskbarCreated,
    // WM_DISPLAYCHANGE, WM_SETTINGCHANGE, and WM_POWERBROADCAST broadcasts.
    HWND msgHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWndClassName,
        L"ExplorerFolderSizeHost",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr,
        nullptr,
        hInstance,
        nullptr);
    if (!msgHwnd) {
        if (hAppIconSmall) DestroyIcon(hAppIconSmall);
        if (hAppIconLarge) DestroyIcon(hAppIconLarge);
        CoUninitialize();
        CloseHandle(hMutex);
        return 1;
    }

    if (g_msgTaskbarCreated != 0) {
        ChangeWindowMessageFilterEx(msgHwnd, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    }

    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = msgHwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = hAppIconSmall ? hAppIconSmall : LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wcscpy_s(g_nid.szTip, L"Explorer Folder Size (Win10/Win11)");
    AddOrRestoreTrayIcon();

    SizeCalculator calculator([msgHwnd]() {
        PostMessageW(msgHwnd, WM_ASYNC_SIZE_UPDATED, 0, 0);
    });
    OverlayRenderer renderer;
    ExplorerTracker tracker(calculator, renderer);

    g_calculator = &calculator;
    g_renderer = &renderer;
    g_tracker = &tracker;

    renderer.Initialize(hInstance);
    tracker.Initialize(msgHwnd);

    SetTimer(msgHwnd, kIdleTrimTimerId, 10000, nullptr);
    TrimProcessMemory();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    tracker.Shutdown();
    renderer.Shutdown();
    g_tracker = nullptr;
    g_renderer = nullptr;
    g_calculator = nullptr;

    if (hAppIconSmall) DestroyIcon(hAppIconSmall);
    if (hAppIconLarge) DestroyIcon(hAppIconLarge);

    CoUninitialize();
    CloseHandle(hMutex);
    return 0;
}
