#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <string>
#include <unordered_map>

#include "resource.h"
#include "ShellHook.h"

namespace {
constexpr UINT WM_TRAYICON = WM_APP + 1;

constexpr UINT ID_TRAY_PAUSE = 1001;
constexpr UINT ID_TRAY_CLEAR_CACHE = 1002;
constexpr UINT ID_TRAY_AUTOSTART = 1003;
constexpr UINT ID_TRAY_EXIT = 1004;

constexpr UINT_PTR kIdleTrimTimerId = 3001;
constexpr UINT_PTR kSyncTimerId = 3002;

constexpr wchar_t kMutexName[] = L"Local\\Win11ExplorerFolderSize_SingleInstance_Mutex";
constexpr wchar_t kWndClassName[] = L"Win11ExplorerFolderSizeMsgWnd";
constexpr wchar_t kRunRegKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunAppName[] = L"ExplorerFolderSize";

NOTIFYICONDATAW g_nid{};
UINT g_msgTaskbarCreated = 0;

HANDLE g_hSharedMap = nullptr;
SharedHookControl* g_sharedCtrl = nullptr;

HMODULE g_hHookDll = nullptr;
HOOKPROC g_hookProc = nullptr;
using PFN_NotifyExplorerRefresh = void (CALLBACK*)();
PFN_NotifyExplorerRefresh g_notifyRefresh = nullptr;

std::unordered_map<DWORD, HHOOK> g_threadHooks;
HWINEVENTHOOK g_hWinEventHook = nullptr;

void TrimProcessMemory() {
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
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

void InitSharedControl() {
    g_hSharedMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedHookControl), kSharedHookControlName);
    if (!g_hSharedMap) {
        g_hSharedMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kSharedHookControlName);
    }
    if (g_hSharedMap) {
        g_sharedCtrl = static_cast<SharedHookControl*>(MapViewOfFile(g_hSharedMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedHookControl)));
        if (g_sharedCtrl) {
            InterlockedExchange(&g_sharedCtrl->paused, 0);
            InterlockedExchange(&g_sharedCtrl->clearCacheSeq, 0);
            InterlockedExchange(&g_sharedCtrl->unloadRequested, 0);
        }
    }
}

bool LoadHookDll() {
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
    }
    std::wstring dllPath = std::wstring(exePath) + L"ExplorerFolderSizeHook.dll";
    g_hHookDll = LoadLibraryW(dllPath.c_str());
    if (g_hHookDll) {
        g_hookProc = reinterpret_cast<HOOKPROC>(GetProcAddress(g_hHookDll, "ExplorerCallWndProc"));
        g_notifyRefresh = reinterpret_cast<PFN_NotifyExplorerRefresh>(GetProcAddress(g_hHookDll, "NotifyExplorerRefresh"));
        return (g_hookProc != nullptr);
    }
    return false;
}

void RefreshAllExplorerWindows() {
    if (g_notifyRefresh) {
        g_notifyRefresh();
    } else {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    }

    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"CabinetWClass") == 0) {
            PostMessageW(hwnd, WM_COMMAND, 41504, 0);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return TRUE;
    }, 0);
}

void SyncExplorerHooks() {
    if (!g_hookProc || !g_hHookDll) return;

    // 1. Prune dead thread IDs
    for (auto it = g_threadHooks.begin(); it != g_threadHooks.end(); ) {
        HANDLE hThread = OpenThread(SYNCHRONIZE, FALSE, it->first);
        if (!hThread) {
            UnhookWindowsHookEx(it->second);
            it = g_threadHooks.erase(it);
        } else {
            DWORD waitRes = WaitForSingleObject(hThread, 0);
            CloseHandle(hThread);
            if (waitRes == WAIT_OBJECT_0) {
                UnhookWindowsHookEx(it->second);
                it = g_threadHooks.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 2. Discover and hook active Explorer threads
    auto HookThreadIfNew = [](HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return;
        DWORD pid = 0;
        DWORD threadId = GetWindowThreadProcessId(hwnd, &pid);
        if (threadId != 0 && pid != 0 && pid != GetCurrentProcessId()) {
            if (g_threadHooks.find(threadId) == g_threadHooks.end()) {
                HHOOK hHook = SetWindowsHookExW(WH_CALLWNDPROC, g_hookProc, g_hHookDll, threadId);
                if (hHook) {
                    g_threadHooks[threadId] = hHook;
                    SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 50, nullptr);
                }
            }
        }
    };

    HWND shellHwnd = GetShellWindow();
    if (shellHwnd) {
        HookThreadIfNew(shellHwnd);
    }

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* pHookFunc = reinterpret_cast<decltype(HookThreadIfNew)*>(lParam);
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"CabinetWClass") == 0) {
            (*pHookFunc)(hwnd);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&HookThreadIfNew));
}

void ShutdownHooks() {
    if (g_hWinEventHook) {
        UnhookWinEvent(g_hWinEventHook);
        g_hWinEventHook = nullptr;
    }

    if (g_sharedCtrl) {
        InterlockedExchange(&g_sharedCtrl->unloadRequested, 1);
        InterlockedExchange(&g_sharedCtrl->paused, 1);
    }

    for (auto& pair : g_threadHooks) {
        if (pair.second) {
            UnhookWindowsHookEx(pair.second);
        }
    }
    g_threadHooks.clear();

    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"CabinetWClass") == 0) {
            SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 50, nullptr);
        }
        return TRUE;
    }, 0);

    HWND shellHwnd = GetShellWindow();
    if (shellHwnd) {
        SendMessageTimeoutW(shellHwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 50, nullptr);
    }

    if (g_hHookDll) {
        FreeLibrary(g_hHookDll);
        g_hHookDll = nullptr;
    }
    if (g_sharedCtrl) {
        UnmapViewOfFile(g_sharedCtrl);
        g_sharedCtrl = nullptr;
    }
    if (g_hSharedMap) {
        CloseHandle(g_hSharedMap);
        g_hSharedMap = nullptr;
    }
}

void CALLBACK WinEventProc(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD idEventThread,
    DWORD dwmsEventTime)
{
    UNREFERENCED_PARAMETER(hWinEventHook);
    UNREFERENCED_PARAMETER(event);
    UNREFERENCED_PARAMETER(idObject);
    UNREFERENCED_PARAMETER(idChild);
    UNREFERENCED_PARAMETER(idEventThread);
    UNREFERENCED_PARAMETER(dwmsEventTime);

    if (hwnd) {
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"CabinetWClass") == 0) {
            SyncExplorerHooks();
        }
    }
}

void ShowTrayContextMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    bool isPaused = g_sharedCtrl && (InterlockedCompareExchange(&g_sharedCtrl->paused, 0, 0) != 0);
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
        AddOrRestoreTrayIcon();
        SyncExplorerHooks();
        return 0;
    }

    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayContextMenu(hwnd);
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            if (g_sharedCtrl) {
                bool isPaused = (InterlockedCompareExchange(&g_sharedCtrl->paused, 0, 0) != 0);
                InterlockedExchange(&g_sharedCtrl->paused, isPaused ? 0 : 1);
            }
            RefreshAllExplorerWindows();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_PAUSE:
            if (g_sharedCtrl) {
                bool isPaused = (InterlockedCompareExchange(&g_sharedCtrl->paused, 0, 0) != 0);
                InterlockedExchange(&g_sharedCtrl->paused, isPaused ? 0 : 1);
            }
            RefreshAllExplorerWindows();
            break;
        case ID_TRAY_CLEAR_CACHE:
            if (g_sharedCtrl) {
                InterlockedIncrement(&g_sharedCtrl->clearCacheSeq);
            }
            EnumWindows([](HWND h, LPARAM) -> BOOL {
                wchar_t cls[64]{};
                if (GetClassNameW(h, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"CabinetWClass") == 0) {
                    PostMessageW(h, WM_NULL, 0, 0);
                }
                return TRUE;
            }, 0);
            RefreshAllExplorerWindows();
            TrimProcessMemory();
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
        if (wParam == kSyncTimerId) {
            SyncExplorerHooks();
        } else if (wParam == kIdleTrimTimerId) {
            TrimProcessMemory();
        }
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        SyncExplorerHooks();
        return 0;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            SyncExplorerHooks();
            RefreshAllExplorerWindows();
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
        KillTimer(hwnd, kSyncTimerId);
        KillTimer(hwnd, kIdleTrimTimerId);
        ShutdownHooks();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(pCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

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

    InitSharedControl();
    LoadHookDll();
    SyncExplorerHooks();

    g_hWinEventHook = SetWinEventHook(
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_SHOW,
        nullptr,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    SetTimer(msgHwnd, kSyncTimerId, 2000, nullptr);
    SetTimer(msgHwnd, kIdleTrimTimerId, 30000, nullptr);
    TrimProcessMemory();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hAppIconSmall) DestroyIcon(hAppIconSmall);
    if (hAppIconLarge) DestroyIcon(hAppIconLarge);

    CoUninitialize();
    CloseHandle(hMutex);
    return 0;
}
