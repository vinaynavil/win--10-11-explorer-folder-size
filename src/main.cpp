#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cwctype>

#include "resource.h"
#include "ShellHook.h"

namespace {
constexpr UINT WM_TRAYICON = WM_APP + 1;

constexpr UINT ID_TRAY_PAUSE = 1001;
constexpr UINT ID_TRAY_CLEAR_CACHE = 1002;
constexpr UINT ID_TRAY_AUTOSTART = 1003;
constexpr UINT ID_TRAY_EXIT = 1004;
constexpr UINT ID_TRAY_UNINSTALL = 1005;

constexpr UINT_PTR kStartupDelayTimerId = 3003;

constexpr wchar_t kMutexName[] = L"Local\\Win11ExplorerFolderSize_SingleInstance_Mutex";
constexpr wchar_t kWndClassName[] = L"Win11ExplorerFolderSizeMsgWnd";
constexpr wchar_t kRunRegKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupApprovedRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kRunAppName[] = L"Explorer Folder Size";
constexpr wchar_t kLegacyRunAppName[] = L"ExplorerFolderSize";

NOTIFYICONDATAW g_nid{};
UINT g_msgTaskbarCreated = 0;

HANDLE g_hSharedMap = nullptr;
SharedHookControl* g_sharedCtrl = nullptr;

HMODULE g_hHookDll = nullptr;
HOOKPROC g_hookProc = nullptr;
using PFN_NotifyExplorerRefresh = void (CALLBACK*)();
PFN_NotifyExplorerRefresh g_notifyRefresh = nullptr;

std::unordered_map<DWORD, HHOOK> g_threadHooks;
HWINEVENTHOOK g_hWinEventFg = nullptr;
HWINEVENTHOOK g_hWinEventShow = nullptr;

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

void PurgeLegacyAndTelemetryEntries(bool removeAll) {
    HKEY hRunKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_SET_VALUE, &hRunKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hRunKey, kLegacyRunAppName);
        if (removeAll) {
            RegDeleteValueW(hRunKey, kRunAppName);
        }
        RegCloseKey(hRunKey);
    }

    HKEY hStartupKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupApprovedRunKey, 0, KEY_SET_VALUE, &hStartupKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hStartupKey, kLegacyRunAppName);
        RegDeleteValueW(hStartupKey, kRunAppName);
        RegCloseKey(hStartupKey);
    }
}

bool IsAutoStartEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t val[MAX_PATH + 8]{};
    DWORD size = sizeof(val);
    LSTATUS status = RegQueryValueExW(hKey, kRunAppName, nullptr, nullptr, reinterpret_cast<LPBYTE>(val), &size);
    if (status != ERROR_SUCCESS) {
        size = sizeof(val);
        status = RegQueryValueExW(hKey, kLegacyRunAppName, nullptr, nullptr, reinterpret_cast<LPBYTE>(val), &size);
    }
    RegCloseKey(hKey);
    return (status == ERROR_SUCCESS);
}

void SetAutoStartEnabled(bool enable) {
    if (enable) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            const std::wstring cmd = GetQuotedExePath();
            RegSetValueExW(hKey, kRunAppName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(cmd.c_str()),
                static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }
        PurgeLegacyAndTelemetryEntries(false);
    } else {
        PurgeLegacyAndTelemetryEntries(true);
    }
}

void SyncAutoStartPathIfEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_READ | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return;
    }
    wchar_t existingVal[MAX_PATH + 8]{};
    DWORD size = sizeof(existingVal);
    DWORD type = 0;
    bool hasLegacy = (RegQueryValueExW(hKey, kLegacyRunAppName, nullptr, &type, reinterpret_cast<LPBYTE>(existingVal), &size) == ERROR_SUCCESS && type == REG_SZ);
    bool hasModern = false;
    if (!hasLegacy) {
        size = sizeof(existingVal);
        hasModern = (RegQueryValueExW(hKey, kRunAppName, nullptr, &type, reinterpret_cast<LPBYTE>(existingVal), &size) == ERROR_SUCCESS && type == REG_SZ);
    }

    if (hasLegacy || hasModern) {
        const std::wstring expected = GetQuotedExePath();
        if (hasLegacy || _wcsicmp(existingVal, expected.c_str()) != 0) {
            RegSetValueExW(hKey, kRunAppName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(expected.c_str()),
                static_cast<DWORD>((expected.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(hKey);
        PurgeLegacyAndTelemetryEntries(false);
    } else {
        RegCloseKey(hKey);
    }
}

void InitSharedControl() {
    if (g_sharedCtrl) return;
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
    if (g_hHookDll && g_hookProc) return true;
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
    // 1. Discover active CabinetWClass windows
    std::vector<HWND> explorerWindows;
    explorerWindows.reserve(8);
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"CabinetWClass") == 0) {
            auto* pVec = reinterpret_cast<std::vector<HWND>*>(lParam);
            pVec->push_back(hwnd);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&explorerWindows));

    // If no Explorer windows exist, prune dead threads and return early without loading DLL
    if (explorerWindows.empty()) {
        if (g_threadHooks.empty()) {
            return;
        }
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
        return;
    }

    // At least one CabinetWClass window exists: ensure shared control & hook DLL are loaded
    InitSharedControl();
    if (!LoadHookDll() || !g_hookProc || !g_hHookDll) {
        return;
    }

    // 2. Prune dead thread IDs
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

    // 3. Hook newly discovered Explorer window threads
    size_t newThreadsHooked = 0;
    for (HWND hwnd : explorerWindows) {
        DWORD pid = 0;
        DWORD threadId = GetWindowThreadProcessId(hwnd, &pid);
        if (threadId != 0 && pid != 0 && pid != GetCurrentProcessId()) {
            if (g_threadHooks.find(threadId) == g_threadHooks.end()) {
                HHOOK hHook = SetWindowsHookExW(WH_CALLWNDPROC, g_hookProc, g_hHookDll, threadId);
                if (hHook) {
                    g_threadHooks[threadId] = hHook;
                    ++newThreadsHooked;
                    SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 50, nullptr);
                }
            }
        }
    }

    if (newThreadsHooked > 0) {
        TrimProcessMemory();
    }
}

void ShutdownHooks() {
    if (g_hWinEventFg) {
        UnhookWinEvent(g_hWinEventFg);
        g_hWinEventFg = nullptr;
    }
    if (g_hWinEventShow) {
        UnhookWinEvent(g_hWinEventShow);
        g_hWinEventShow = nullptr;
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

    RefreshAllExplorerWindows();

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
    UNREFERENCED_PARAMETER(idEventThread);
    UNREFERENCED_PARAMETER(dwmsEventTime);

    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) {
        return;
    }

    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"CabinetWClass") == 0) {
        SyncExplorerHooks();
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
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_UNINSTALL, L"Uninstall && Clean Up...");
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
        case ID_TRAY_UNINSTALL: {
            int resp = MessageBoxW(
                hwnd,
                L"This will remove Explorer Folder Size from Windows startup, unhook File Explorer, and close the app so you can safely delete its files.\n\nProceed with uninstall cleanup?",
                L"Uninstall Explorer Folder Size",
                MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2 | MB_SETFOREGROUND);
            if (resp == IDYES) {
                PurgeLegacyAndTelemetryEntries(true);
                RefreshAllExplorerWindows();
                DestroyWindow(hwnd);
            }
            break;
        }
        case ID_TRAY_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == kStartupDelayTimerId) {
            KillTimer(hwnd, kStartupDelayTimerId);
            g_hWinEventFg = SetWinEventHook(
                EVENT_SYSTEM_FOREGROUND,
                EVENT_SYSTEM_FOREGROUND,
                nullptr,
                WinEventProc,
                0,
                0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            g_hWinEventShow = SetWinEventHook(
                EVENT_OBJECT_SHOW,
                EVENT_OBJECT_SHOW,
                nullptr,
                WinEventProc,
                0,
                0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            SyncExplorerHooks();
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

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kStartupDelayTimerId);
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
    UNREFERENCED_PARAMETER(nCmdShow);

    if (pCmdLine && *pCmdLine) {
        std::wstring cmd(pCmdLine);
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        if (cmd.find(L"--uninstall") != std::wstring::npos ||
            cmd.find(L"/uninstall") != std::wstring::npos ||
            cmd.find(L"-uninstall") != std::wstring::npos) {
            PurgeLegacyAndTelemetryEntries(true);
            HWND existingHwnd = FindWindowW(kWndClassName, nullptr);
            if (existingHwnd) {
                SendMessageTimeoutW(existingHwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, nullptr);
                Sleep(500);
            }
            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_FLUSHNOWAIT, nullptr, nullptr);
            return 0;
        }
    }

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

    SetTimer(msgHwnd, kStartupDelayTimerId, 3500, nullptr);
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
