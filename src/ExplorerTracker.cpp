#include "ExplorerTracker.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

ExplorerTracker* ExplorerTracker::s_instance = nullptr;

ExplorerTracker::ExplorerTracker(SizeCalculator& calculator, OverlayRenderer& renderer)
    : m_calculator(calculator), m_renderer(renderer) {
    s_instance = this;
    QueryPerformanceFrequency(&m_qpcFreq);
}

ExplorerTracker::~ExplorerTracker() {
    Shutdown();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

bool ExplorerTracker::Initialize(HWND mainMsgHwnd) {
    m_mainMsgHwnd = mainMsgHwnd;

    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia));
    if (FAILED(hr) || !m_uia) {
        return false;
    }

    hr = m_uia->CreateCacheRequest(&m_cacheRequest);
    if (FAILED(hr) || !m_cacheRequest) {
        return false;
    }
    m_cacheRequest->AddProperty(UIA_NamePropertyId);
    m_cacheRequest->AddProperty(UIA_AutomationIdPropertyId);
    m_cacheRequest->AddProperty(UIA_ControlTypePropertyId);
    m_cacheRequest->AddProperty(UIA_BoundingRectanglePropertyId);
    m_cacheRequest->AddProperty(UIA_IsOffscreenPropertyId);

    VARIANT varHeaderItemType{};
    varHeaderItemType.vt = VT_I4;
    varHeaderItemType.lVal = UIA_HeaderItemControlTypeId;

    VARIANT varSplitBtnType{};
    varSplitBtnType.vt = VT_I4;
    varSplitBtnType.lVal = UIA_SplitButtonControlTypeId;

    ComPtr<IUIAutomationCondition> condHeaderItem;
    ComPtr<IUIAutomationCondition> condSplitBtn;
    if (FAILED(m_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, varHeaderItemType, &condHeaderItem)) ||
        FAILED(m_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, varSplitBtnType, &condSplitBtn)) ||
        FAILED(m_uia->CreateOrCondition(condHeaderItem.Get(), condSplitBtn.Get(), &m_headerCond))) {
        return false;
    }

    VARIANT varListItemType{};
    varListItemType.vt = VT_I4;
    varListItemType.lVal = UIA_ListItemControlTypeId;
    if (FAILED(m_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, varListItemType, &m_listItemCond))) {
        return false;
    }

    DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    m_hookForeground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, flags);
    m_hookLocation = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, WinEventProc, 0, 0, flags);
    m_hookScroll = SetWinEventHook(EVENT_SYSTEM_SCROLLINGSTART, EVENT_SYSTEM_SCROLLINGEND, nullptr, WinEventProc, 0, 0, flags);
    m_hookObjectShow = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_REORDER, nullptr, WinEventProc, 0, 0, flags);

    SetTimer(m_mainMsgHwnd, kPeriodicTimerId, 30, nullptr);
    RequestRefresh();
    return true;
}

void ExplorerTracker::HideOverlay() {
    m_lastRenderedClipRect = {};
    m_lastRenderedHeaderRect = {};
    m_hasPendingFolders = false;
    m_renderer.HideOverlay();
}

void ExplorerTracker::Shutdown() {
    if (m_mainMsgHwnd) {
        KillTimer(m_mainMsgHwnd, kRefreshTimerId);
        KillTimer(m_mainMsgHwnd, kPeriodicTimerId);
    }
    m_trailingTimerArmed = false;

    if (m_hookForeground) { UnhookWinEvent(m_hookForeground); m_hookForeground = nullptr; }
    if (m_hookLocation)   { UnhookWinEvent(m_hookLocation);   m_hookLocation = nullptr; }
    if (m_hookScroll)     { UnhookWinEvent(m_hookScroll);     m_hookScroll = nullptr; }
    if (m_hookObjectShow) { UnhookWinEvent(m_hookObjectShow); m_hookObjectShow = nullptr; }

    m_cachedRows.clear();
    m_cachedSizeHeaderEl.Reset();
    m_shellWindows.Reset();
    m_listItemCond.Reset();
    m_headerCond.Reset();
    m_cacheRequest.Reset();
    m_uia.Reset();

    HideOverlay();
}

void ExplorerTracker::SetPaused(bool paused) {
    m_paused = paused;
    if (m_paused) {
        if (m_trailingTimerArmed && m_mainMsgHwnd) {
            KillTimer(m_mainMsgHwnd, kRefreshTimerId);
            m_trailingTimerArmed = false;
        }
        HideOverlay();
    } else {
        RequestRefresh();
    }
}

ULONGLONG ExplorerTracker::GetNowMicroseconds() const {
    if (m_qpcFreq.QuadPart <= 0) {
        return GetTickCount64() * 1000ULL;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<ULONGLONG>((now.QuadPart * 1000000ULL) / m_qpcFreq.QuadPart);
}

ULONGLONG ExplorerTracker::GetTargetFrameIntervalUs(HWND referenceHwnd) {
    HWND target = (referenceHwnd && IsWindow(referenceHwnd)) ? referenceHwnd : GetForegroundWindow();
    HMONITOR hMon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    const ULONGLONG nowTick = GetTickCount64();

    if (hMon && hMon == m_cachedMonitor && (nowTick - m_monitorQueryTick) < 2000ULL) {
        return m_cachedFrameIntervalUs;
    }

    DWORD hz = 60;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);

    const wchar_t* deviceName = (hMon && GetMonitorInfoW(hMon, &mi)) ? mi.szDevice : nullptr;
    if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &dm)) {
        if (dm.dmDisplayFrequency > 1 && dm.dmDisplayFrequency <= 1000) {
            hz = dm.dmDisplayFrequency;
        }
    }

    m_cachedMonitor = hMon;
    m_monitorQueryTick = nowTick;
    m_cachedFrameIntervalUs = std::clamp<ULONGLONG>(1000000ULL / hz, 2000ULL, 16667ULL);
    return m_cachedFrameIntervalUs;
}

void ExplorerTracker::RequestRefresh() {
    if (m_paused || !m_mainMsgHwnd || m_inRefresh) return;

    const ULONGLONG nowUs = GetNowMicroseconds();
    const ULONGLONG intervalUs = GetTargetFrameIntervalUs(m_lastFgHwnd);
    const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    if (leftButtonDown) {
        m_wasLeftButtonDown = true;
    }

    if ((nowUs - m_lastRefreshUs) >= intervalUs) {
        if (m_trailingTimerArmed) {
            KillTimer(m_mainMsgHwnd, kRefreshTimerId);
            m_trailingTimerArmed = false;
        }
        PerformRefresh(leftButtonDown);
        SetTimer(m_mainMsgHwnd, kRefreshTimerId, 16, nullptr);
        m_trailingTimerArmed = true;
    } else if (!m_trailingTimerArmed) {
        const ULONGLONG remainingMs = (intervalUs - (nowUs - m_lastRefreshUs) + 999ULL) / 1000ULL;
        const UINT elapseMs = static_cast<UINT>(std::max<ULONGLONG>(1ULL, remainingMs));
        SetTimer(m_mainMsgHwnd, kRefreshTimerId, elapseMs, nullptr);
        m_trailingTimerArmed = true;
    }
}

void ExplorerTracker::OnTimerTick() {
    if (m_paused || m_inRefresh) return;

    const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    if (m_trailingTimerArmed) {
        if (m_mainMsgHwnd) {
            KillTimer(m_mainMsgHwnd, kRefreshTimerId);
        }
        m_trailingTimerArmed = false;
    }

    HWND fgHwnd = GetForegroundWindow();
    if (!fgHwnd) {
        HideOverlay();
        return;
    }

    wchar_t fgClass[64]{};
    GetClassNameW(fgHwnd, fgClass, 64);
    if (wcscmp(fgClass, L"CabinetWClass") != 0 || IsIconic(fgHwnd)) {
        HideOverlay();
        return;
    }

    // Drag-release edge: mouse button just released after dragging column divider or window
    if (m_wasLeftButtonDown && !leftButtonDown) {
        m_wasLeftButtonDown = false;
        PerformRefresh(true);
        return;
    }

    if (leftButtonDown) {
        m_wasLeftButtonDown = true;
        PerformRefresh(true);
        return;
    }

    // Fast refresh pending sizes ("..." -> size) without full UIA tree walk
    if (m_hasPendingFolders) {
        PerformRefresh(true);
        return;
    }

    // Idle: only trigger full refresh if foreground window, header element, or folder changed
    bool needFullRefresh = false;
    if (fgHwnd != m_lastFgHwnd || !m_cachedSizeHeaderEl || m_cachedRows.empty()) {
        needFullRefresh = true;
    } else {
        HWND activeTabHwnd = nullptr;
        std::wstring currentFolderPath;
        if (ResolveActiveExplorerFolder(fgHwnd, activeTabHwnd, currentFolderPath)) {
            if (activeTabHwnd != m_cachedActiveTabHwnd || currentFolderPath != m_cachedFolderPath) {
                needFullRefresh = true;
            }
        } else {
            needFullRefresh = true;
        }
    }

    if (needFullRefresh) {
        PerformRefresh(false);
    }
}

void CALLBACK ExplorerTracker::WinEventProc(
    HWINEVENTHOOK /*hWinEventHook*/,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG /*idChild*/,
    DWORD /*idEventThread*/,
    DWORD /*dwmsEventTime*/) {
    if (!s_instance || s_instance->m_paused) return;

    if (event == EVENT_OBJECT_LOCATIONCHANGE && idObject == OBJID_CURSOR) {
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 &&
            s_instance->m_lastFgHwnd &&
            GetForegroundWindow() == s_instance->m_lastFgHwnd) {
            s_instance->m_lastDragMoveUs = s_instance->GetNowMicroseconds();
            s_instance->RequestRefresh();
        }
        return;
    }

    if (event == EVENT_SYSTEM_FOREGROUND) {
        s_instance->RequestRefresh();
        return;
    }

    if (!hwnd) return;

    // Ignore tooltips to avoid unrelated hover churn
    wchar_t clsName[32]{};
    if (GetClassNameW(hwnd, clsName, 32) > 0 && wcscmp(clsName, L"tooltips_class32") == 0) {
        return;
    }

    HWND rootHwnd = GetAncestor(hwnd, GA_ROOT);
    if (!rootHwnd) return;

    wchar_t className[64]{};
    if (GetClassNameW(rootHwnd, className, 64) > 0 && wcscmp(className, L"CabinetWClass") == 0) {
        if (event == EVENT_OBJECT_REORDER) {
            s_instance->m_cachedSizeHeaderEl.Reset();
            s_instance->m_lastRenderedClipRect = {};
            s_instance->m_lastRenderedHeaderRect = {};
            s_instance->RequestRefresh();
            return;
        }

        s_instance->RequestRefresh();
    }
}

HWND ExplorerTracker::FindChildByClass(HWND parent, const wchar_t* targetClass, bool requireVisible) {
    if (!parent) return nullptr;

    struct SearchCtx {
        const wchar_t* targetClass;
        bool requireVisible;
        HWND found;
    } ctx{ targetClass, requireVisible, nullptr };

    EnumChildWindows(parent, [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<SearchCtx*>(lParam);
        if (c->requireVisible && !IsWindowVisible(hwnd)) {
            return TRUE;
        }
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, 64) > 0 && wcscmp(cls, c->targetClass) == 0) {
            c->found = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.found;
}

void ExplorerTracker::AdjustViewportForScrollBars(HWND containerHwnd, RECT& inOutViewportRect) {
    if (!containerHwnd) return;

    EnumChildWindows(containerHwnd, [](HWND hwnd, LPARAM lParam) -> BOOL {
        if (!IsWindowVisible(hwnd)) {
            return TRUE;
        }
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, 64) <= 0 || wcscmp(cls, L"ScrollBar") != 0) {
            return TRUE;
        }
        RECT sbRect{};
        if (!GetWindowRect(hwnd, &sbRect)) {
            return TRUE;
        }
        const int sbW = sbRect.right - sbRect.left;
        const int sbH = sbRect.bottom - sbRect.top;
        if (sbW <= 0 || sbH <= 0) {
            return TRUE;
        }

        auto* vp = reinterpret_cast<RECT*>(lParam);
        const int midX = (vp->left + vp->right) / 2;
        const int midY = (vp->top + vp->bottom) / 2;

        if (sbH > sbW) {
            if (sbRect.left >= midX) {
                vp->right = std::min(vp->right, sbRect.left);
            } else if (sbRect.right <= midX) {
                vp->left = std::max(vp->left, sbRect.right);
            }
        } else if (sbW > sbH) {
            if (sbRect.top >= midY) {
                vp->bottom = std::min(vp->bottom, sbRect.top);
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&inOutViewportRect));
}

bool ExplorerTracker::IsSystemDarkMode() {
    HKEY hKey = nullptr;
    DWORD value = 1;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(value);
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(hKey);
    }
    return (value == 0);
}

bool ExplorerTracker::ResolveActiveExplorerFolder(HWND cabinetHwnd, HWND& outActiveTabHwnd, std::wstring& outFolderPath) {
    outActiveTabHwnd = nullptr;
    outFolderPath.clear();

    HWND activeTab = FindChildByClass(cabinetHwnd, L"ShellTabWindowClass", true);
    if (!activeTab) return false;
    outActiveTabHwnd = activeTab;

    long count = 0;
    if (!m_shellWindows || FAILED(m_shellWindows->get_Count(&count))) {
        m_shellWindows.Reset();
        if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_shellWindows))) || !m_shellWindows) {
            return false;
        }
        if (FAILED(m_shellWindows->get_Count(&count))) {
            return false;
        }
    }

    for (long i = 0; i < count; ++i) {
        VARIANT idx{};
        idx.vt = VT_I4;
        idx.lVal = i;

        ComPtr<IDispatch> disp;
        if (FAILED(m_shellWindows->Item(idx, &disp)) || !disp) continue;

        ComPtr<IWebBrowserApp> webBrowser;
        if (FAILED(disp.As(&webBrowser)) || !webBrowser) continue;

        SHANDLE_PTR windowHandle = 0;
        webBrowser->get_HWND(&windowHandle);
        if (reinterpret_cast<HWND>(windowHandle) != cabinetHwnd) continue;

        ComPtr<IServiceProvider> serviceProvider;
        if (FAILED(disp.As(&serviceProvider)) || !serviceProvider) continue;

        ComPtr<IShellBrowser> shellBrowser;
        if (FAILED(serviceProvider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shellBrowser))) || !shellBrowser) continue;

        HWND browserTabHwnd = nullptr;
        if (SUCCEEDED(shellBrowser->GetWindow(&browserTabHwnd))) {
            if (browserTabHwnd != activeTab && !IsChild(activeTab, browserTabHwnd)) {
                continue;
            }
        }

        ComPtr<IShellView> shellView;
        if (FAILED(shellBrowser->QueryActiveShellView(&shellView)) || !shellView) continue;

        ComPtr<IFolderView> folderView;
        if (FAILED(shellView.As(&folderView)) || !folderView) continue;

        ComPtr<IPersistFolder2> persistFolder;
        if (FAILED(folderView->GetFolder(IID_PPV_ARGS(&persistFolder))) || !persistFolder) continue;

        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(persistFolder->GetCurFolder(&pidl)) && pidl) {
            wchar_t pathBuf[MAX_PATH]{};
            bool ok = SHGetPathFromIDListW(pidl, pathBuf) != FALSE;
            CoTaskMemFree(pidl);
            if (ok && pathBuf[0] != L'\0') {
                outFolderPath = pathBuf;
                return true;
            }
        }
    }

    return false;
}

bool ExplorerTracker::TryFastGeometryRefresh(HWND fgHwnd) {
    if (fgHwnd != m_lastFgHwnd ||
        !m_cachedDirectUiHwnd || !IsWindow(m_cachedDirectUiHwnd) || !IsWindowVisible(m_cachedDirectUiHwnd) ||
        !m_cachedActiveTabHwnd || !IsWindow(m_cachedActiveTabHwnd) || !IsWindowVisible(m_cachedActiveTabHwnd) ||
        !m_cachedSizeHeaderEl || m_cachedRows.empty()) {
        return false;
    }

    RECT currentWinRect{};
    if (!GetWindowRect(fgHwnd, &currentWinRect)) {
        return false;
    }

    const int winDx = currentWinRect.left - m_cachedWindowRect.left;
    const int winDy = currentWinRect.top - m_cachedWindowRect.top;
    const int oldW = m_cachedWindowRect.right - m_cachedWindowRect.left;
    const int oldH = m_cachedWindowRect.bottom - m_cachedWindowRect.top;
    const int newW = currentWinRect.right - currentWinRect.left;
    const int newH = currentWinRect.bottom - currentWinRect.top;

    RECT listScreenRect{};
    if (!GetWindowRect(m_cachedDirectUiHwnd, &listScreenRect)) {
        return false;
    }
    RECT tabRect{};
    if (GetWindowRect(m_cachedActiveTabHwnd, &tabRect)) {
        IntersectRect(&listScreenRect, &listScreenRect, &tabRect);
    }
    AdjustViewportForScrollBars(m_cachedDirectUiHwnd, listScreenRect);
    if (listScreenRect.right <= listScreenRect.left || listScreenRect.bottom <= listScreenRect.top) {
        HideOverlay();
        return true;
    }

    const bool isPureWindowMove = (winDx != 0 || winDy != 0) && (oldW == newW) && (oldH == newH);

    RECT sizeHeaderRect{};
    if (isPureWindowMove) {
        sizeHeaderRect = m_cachedSizeHeaderRect;
        OffsetRect(&sizeHeaderRect, winDx, winDy);
    } else {
        HRESULT hrHeader = m_cachedSizeHeaderEl->get_CurrentBoundingRectangle(&sizeHeaderRect);
        if (FAILED(hrHeader) || sizeHeaderRect.right <= sizeHeaderRect.left) {
            m_cachedSizeHeaderEl.Reset();
            const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (leftButtonDown && m_cachedSizeHeaderRect.right > m_cachedSizeHeaderRect.left) {
                // Keep the last valid sizeHeaderRect for this single transitional tick while VK_LBUTTON is still down
                sizeHeaderRect = m_cachedSizeHeaderRect;
                if (winDx != 0 || winDy != 0) {
                    OffsetRect(&sizeHeaderRect, winDx, winDy);
                }
            } else {
                return false;
            }
        }
    }

    m_cachedSizeHeaderRect = sizeHeaderRect;
    m_cachedWindowRect = currentWinRect;
    m_lastListRect = listScreenRect;

    RECT columnClipRect{};
    columnClipRect.left = std::max(sizeHeaderRect.left, listScreenRect.left);
    columnClipRect.right = std::min(sizeHeaderRect.right, listScreenRect.right);
    columnClipRect.top = std::max(sizeHeaderRect.bottom, listScreenRect.top);
    columnClipRect.bottom = listScreenRect.bottom;

    if (columnClipRect.right <= columnClipRect.left || columnClipRect.bottom <= columnClipRect.top) {
        HideOverlay();
        return true;
    }

    if (winDx != 0 || winDy != 0) {
        for (auto& snap : m_cachedRows) {
            OffsetRect(&snap.rowScreenRect, winDx, winDy);
        }
    }

    if (EqualRect(&sizeHeaderRect, &m_lastRenderedHeaderRect) &&
        EqualRect(&columnClipRect, &m_lastRenderedClipRect) &&
        winDx == 0 && winDy == 0 &&
        !m_hasPendingFolders) {
        return true;
    }

    std::vector<OverlayRowItem> overlayRows;
    overlayRows.reserve(m_cachedRows.size());
    bool anyPending = false;

    for (const auto& snap : m_cachedRows) {
        if (snap.rowScreenRect.bottom <= columnClipRect.top || snap.rowScreenRect.top >= columnClipRect.bottom) {
            continue;
        }

        OverlayRowItem item{};
        item.cellScreenRect.left = sizeHeaderRect.left;
        item.cellScreenRect.right = sizeHeaderRect.right;
        item.cellScreenRect.top = snap.rowScreenRect.top;
        item.cellScreenRect.bottom = snap.rowScreenRect.bottom;

        FolderSizeResult sizeRes = m_calculator.RequestFolderSize(snap.fullItemPath);
        item.text = sizeRes.formattedText;
        item.isPending = (sizeRes.status != CalculationStatus::Ready);
        if (item.isPending) {
            anyPending = true;
        }

        overlayRows.push_back(std::move(item));
    }

    if (overlayRows.empty()) {
        HideOverlay();
        return true;
    }

    m_renderer.UpdateOverlay(fgHwnd, columnClipRect, overlayRows, m_cachedDarkMode);
    m_lastRenderedHeaderRect = sizeHeaderRect;
    m_lastRenderedClipRect = columnClipRect;
    m_hasPendingFolders = anyPending;
    return true;
}

void ExplorerTracker::PerformRefresh(bool allowFastGeometryPath) {
    if (!m_uia || !m_cacheRequest || !m_headerCond || !m_listItemCond || m_inRefresh) return;

    struct ReentrancyGuard {
        bool& flag;
        explicit ReentrancyGuard(bool& f) : flag(f) { flag = true; }
        ~ReentrancyGuard() { flag = false; }
    } guard(m_inRefresh);

    const ULONGLONG nowUs = GetNowMicroseconds();
    m_lastRefreshUs = nowUs;

    HWND fgHwnd = GetForegroundWindow();
    if (!fgHwnd) {
        m_lastFgHwnd = nullptr;
        HideOverlay();
        return;
    }

    wchar_t fgClass[64]{};
    GetClassNameW(fgHwnd, fgClass, 64);
    if (wcscmp(fgClass, L"CabinetWClass") != 0 || IsIconic(fgHwnd)) {
        m_lastFgHwnd = nullptr;
        HideOverlay();
        return;
    }

    // ~0.3ms Fast Path during active column resize or window drag
    if (allowFastGeometryPath && TryFastGeometryRefresh(fgHwnd)) {
        return;
    }

    if (!allowFastGeometryPath) {
        m_lastRenderedClipRect = {};
        m_lastRenderedHeaderRect = {};
    }

    m_lastFgHwnd = fgHwnd;
    m_lastFullRefreshUs = nowUs;

    HWND activeTabHwnd = nullptr;
    std::wstring currentFolderPath;
    if (!ResolveActiveExplorerFolder(fgHwnd, activeTabHwnd, currentFolderPath) || currentFolderPath.empty()) {
        m_cachedRows.clear();
        m_cachedSizeHeaderEl.Reset();
        HideOverlay();
        return;
    }

    HWND defViewHwnd = FindChildByClass(activeTabHwnd, L"SHELLDLL_DefView", true);
    HWND directUiHwnd = FindChildByClass(defViewHwnd ? defViewHwnd : activeTabHwnd, L"DirectUIHWND", true);
    if (!directUiHwnd) {
        m_cachedRows.clear();
        m_cachedSizeHeaderEl.Reset();
        HideOverlay();
        return;
    }

    if (directUiHwnd != m_cachedDirectUiHwnd || currentFolderPath != m_cachedFolderPath) {
        m_cachedSizeHeaderEl.Reset();
    }
    m_cachedDirectUiHwnd = directUiHwnd;
    m_cachedActiveTabHwnd = activeTabHwnd;
    m_cachedFolderPath = currentFolderPath;
    GetWindowRect(fgHwnd, &m_cachedWindowRect);

    RECT listScreenRect{};
    if (!GetWindowRect(directUiHwnd, &listScreenRect)) {
        HideOverlay();
        return;
    }

    RECT tabRect{};
    if (GetWindowRect(activeTabHwnd, &tabRect)) {
        IntersectRect(&listScreenRect, &listScreenRect, &tabRect);
    }
    AdjustViewportForScrollBars(directUiHwnd, listScreenRect);
    m_lastListRect = listScreenRect;

    if (listScreenRect.right <= listScreenRect.left || listScreenRect.bottom <= listScreenRect.top) {
        HideOverlay();
        return;
    }

    ComPtr<IUIAutomationElement> rootEl;
    if (FAILED(m_uia->ElementFromHandle(directUiHwnd, &rootEl)) || !rootEl) {
        HideOverlay();
        return;
    }

    RECT sizeHeaderRect{};
    bool foundSizeColumn = false;

    // Try cached Size Header UIA element first (~0.2ms instead of 35ms FindAllBuildCache)
    if (m_cachedSizeHeaderEl) {
        if (SUCCEEDED(m_cachedSizeHeaderEl->get_CurrentBoundingRectangle(&sizeHeaderRect)) &&
            sizeHeaderRect.right > sizeHeaderRect.left) {
            foundSizeColumn = true;
        } else {
            m_cachedSizeHeaderEl.Reset();
        }
    }

    if (!foundSizeColumn) {
        ComPtr<IUIAutomationElementArray> headerItems;
        if (FAILED(rootEl->FindAllBuildCache(TreeScope_Descendants, m_headerCond.Get(), m_cacheRequest.Get(), &headerItems)) || !headerItems) {
            HideOverlay();
            return;
        }

        int headerCount = 0;
        headerItems->get_Length(&headerCount);

        for (int i = 0; i < headerCount; ++i) {
            ComPtr<IUIAutomationElement> item;
            if (FAILED(headerItems->GetElement(i, &item)) || !item) continue;

            BSTR autoId = nullptr;
            BSTR name = nullptr;
            item->get_CachedAutomationId(&autoId);
            item->get_CachedName(&name);

            bool isSizeHeader = false;
            if (autoId && wcscmp(autoId, L"System.Size") == 0) {
                isSizeHeader = true;
            } else if (name && _wcsicmp(name, L"Size") == 0) {
                isSizeHeader = true;
            }

            if (autoId) SysFreeString(autoId);
            if (name) SysFreeString(name);

            if (isSizeHeader) {
                if (SUCCEEDED(item->get_CachedBoundingRectangle(&sizeHeaderRect)) &&
                    sizeHeaderRect.right > sizeHeaderRect.left) {
                    foundSizeColumn = true;
                    m_cachedSizeHeaderEl = item;
                }
                break;
            }
        }
    }

    if (!foundSizeColumn) {
        m_cachedRows.clear();
        HideOverlay();
        return;
    }

    m_cachedSizeHeaderRect = sizeHeaderRect;

    RECT columnClipRect{};
    columnClipRect.left = std::max(sizeHeaderRect.left, listScreenRect.left);
    columnClipRect.right = std::min(sizeHeaderRect.right, listScreenRect.right);
    columnClipRect.top = std::max(sizeHeaderRect.bottom, listScreenRect.top);
    columnClipRect.bottom = listScreenRect.bottom;

    if (columnClipRect.right <= columnClipRect.left || columnClipRect.bottom <= columnClipRect.top) {
        HideOverlay();
        return;
    }

    ComPtr<IUIAutomationElementArray> rowItems;
    if (FAILED(rootEl->FindAllBuildCache(TreeScope_Children, m_listItemCond.Get(), m_cacheRequest.Get(), &rowItems)) || !rowItems) {
        HideOverlay();
        return;
    }

    int rowCount = 0;
    rowItems->get_Length(&rowCount);

    if (rowCount == 0) {
        rootEl->FindAllBuildCache(TreeScope_Descendants, m_listItemCond.Get(), m_cacheRequest.Get(), &rowItems);
        if (rowItems) rowItems->get_Length(&rowCount);
    }

    std::vector<OverlayRowItem> overlayRows;
    overlayRows.reserve(32);
    m_cachedRows.clear();
    m_cachedRows.reserve(32);

    std::wstring baseDir = currentFolderPath;
    if (!baseDir.empty() && baseDir.back() != L'\\') {
        baseDir.push_back(L'\\');
    }

    bool anyPending = false;

    for (int i = 0; i < rowCount; ++i) {
        ComPtr<IUIAutomationElement> rowEl;
        if (FAILED(rowItems->GetElement(i, &rowEl)) || !rowEl) continue;

        RECT rowRect{};
        if (FAILED(rowEl->get_CachedBoundingRectangle(&rowRect))) continue;

        if (rowRect.bottom <= columnClipRect.top || rowRect.top >= columnClipRect.bottom) {
            continue;
        }

        BSTR itemNameBstr = nullptr;
        rowEl->get_CachedName(&itemNameBstr);
        if (!itemNameBstr) continue;

        std::wstring itemName(itemNameBstr);
        SysFreeString(itemNameBstr);
        if (itemName.empty()) continue;

        std::wstring fullItemPath = baseDir + itemName;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(fullItemPath.c_str(), GetFileExInfoStandard, &fad)) {
            continue;
        }

        const bool isDir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!isDir) {
            // Folders only: let Windows Explorer render all file sizes natively
            continue;
        }

        FolderSizeResult sizeRes = m_calculator.RequestFolderSize(fullItemPath);

        OverlayRowItem item{};
        item.cellScreenRect.left = sizeHeaderRect.left;
        item.cellScreenRect.right = sizeHeaderRect.right;
        item.cellScreenRect.top = rowRect.top;
        item.cellScreenRect.bottom = rowRect.bottom;
        item.text = sizeRes.formattedText;
        item.isPending = (sizeRes.status != CalculationStatus::Ready);
        if (item.isPending) {
            anyPending = true;
        }

        CachedRowSnapshot snap{};
        snap.rowScreenRect = rowRect;
        snap.fullItemPath = fullItemPath;

        m_cachedRows.push_back(std::move(snap));
        overlayRows.push_back(std::move(item));
    }

    if (overlayRows.empty()) {
        HideOverlay();
        return;
    }

    m_cachedDarkMode = IsSystemDarkMode();
    m_renderer.UpdateOverlay(fgHwnd, columnClipRect, overlayRows, m_cachedDarkMode);
    m_lastRenderedHeaderRect = sizeHeaderRect;
    m_lastRenderedClipRect = columnClipRect;
    m_hasPendingFolders = anyPending;
}
