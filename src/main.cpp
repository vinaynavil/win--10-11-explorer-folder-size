#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <string>
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

    constexpr wchar_t kMutexName[] = L"Local\\Win11ExplorerFolderSize_SingleInstance_Mutex";
    constexpr wchar_t kWndClassName[] = L"Win11ExplorerFolderSizeMsgWnd";
    constexpr wchar_t kRunRegKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunAppName[] = L"ExplorerFolderSize";

    NOTIFYICONDATAW g_nid{};
    SizeCalculator* g_calculator = nullptr;
    OverlayRenderer* g_renderer = nullptr;
    ExplorerTracker* g_tracker = nullptr;

    bool IsAutoStartEnabled() {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
            return false;
        }
        wchar_t val[MAX_PATH]{};
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
            wchar_t exePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(exePath) + L"\"";
            RegSetValueExW(hKey, kRunAppName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(cmd.c_str()),
                static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, kRunAppName);
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
            }
            return 0;

        case WM_ASYNC_SIZE_UPDATED:
            if (g_tracker) {
                g_tracker->RequestRefresh();
            }
            return 0;

        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int /*nCmdShow*/) {
    // Enable Per-Monitor V2 DPI Awareness for exact coordinate alignment on Win10 & Win11
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

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWndClassName;
    RegisterClassExW(&wc);

    HWND msgHwnd = CreateWindowExW(0, kWndClassName, L"ExplorerFolderSizeHost", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!msgHwnd) {
        CoUninitialize();
        CloseHandle(hMutex);
        return 1;
    }

    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = msgHwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Explorer Folder Size (Win10/Win11)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

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

    CoUninitialize();
    CloseHandle(hMutex);
    return 0;
}
