#include "ExplorerTracker.h"
#include <exdisp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

ExplorerTracker* ExplorerTracker::s_instance = nullptr;

ExplorerTracker::ExplorerTracker(SizeCalculator& calculator, OverlayRenderer& renderer)
    : m_calculator(calculator), m_renderer(renderer) {
    s_instance = this;
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
    if (SUCCEEDED(hr) && m_cacheRequest) {
        m_cacheRequest->AddProperty(UIA_NamePropertyId);
        m_cacheRequest->AddProperty(UIA_AutomationIdPropertyId);
        m_cacheRequest->AddProperty(UIA_ControlTypePropertyId);
        m_cacheRequest->AddProperty(UIA_BoundingRectanglePropertyId);
        m_cacheRequest->AddProperty(UIA_IsOffscreenPropertyId);
    }

    DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    m_hookForeground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, flags);
    m_hookLocation = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, WinEventProc, 0, 0, flags);
    m_hookScroll = SetWinEventHook(EVENT_SYSTEM_SCROLLINGSTART, EVENT_SYSTEM_SCROLLINGEND, nullptr, WinEventProc, 0, 0, flags);
    m_hookObjectShow = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, WinEventProc, 0, 0, flags);

    // Periodic fallback timer (every 400ms) for smooth tracking during continuous wheel scrolling
    SetTimer(m_mainMsgHwnd, kPeriodicTimerId, 400, nullptr);
    RequestRefresh();
    return true;
}

void ExplorerTracker::Shutdown() {
    if (m_mainMsgHwnd) {
        KillTimer(m_mainMsgHwnd, kRefreshTimerId);
        KillTimer(m_mainMsgHwnd, kPeriodicTimerId);
    }
    if (m_hookForeground) { UnhookWinEvent(m_hookForeground); m_hookForeground = nullptr; }
    if (m_hookLocation) { UnhookWinEvent(m_hookLocation); m_hookLocation = nullptr; }
    if (m_hookScroll) { UnhookWinEvent(m_hookScroll); m_hookScroll = nullptr; }
    if (m_hookObjectShow) { UnhookWinEvent(m_hookObjectShow); m_hookObjectShow = nullptr; }

    if (m_cacheRequest) { m_cacheRequest->Release(); m_cacheRequest = nullptr; }
    if (m_uia) { m_uia->Release(); m_uia = nullptr; }

    m_renderer.HideOverlay();
}

void ExplorerTracker::SetPaused(bool paused) {
    m_paused = paused;
    if (m_paused) {
        m_renderer.HideOverlay();
    } else {
        RequestRefresh();
    }
}

void ExplorerTracker::RequestRefresh() {
    if (m_paused || !m_mainMsgHwnd) return;
    // 16ms (~60fps) coalesce timer to avoid flooding UIA calls during window drag/scroll
    SetTimer(m_mainMsgHwnd, kRefreshTimerId, 16, nullptr);
}

void ExplorerTracker::OnTimerTick() {
    if (m_paused) return;
    PerformRefresh();
}

void CALLBACK ExplorerTracker::WinEventProc(
    HWINEVENTHOOK /*hWinEventHook*/,
    DWORD event,
    HWND hwnd,
    LONG /*idObject*/,
    LONG /*idChild*/,
    DWORD /*idEventThread*/,
    DWORD /*dwmsEventTime*/) {

    if (!s_instance || s_instance->m_paused) return;

    if (event == EVENT_SYSTEM_FOREGROUND) {
        s_instance->RequestRefresh();
        return;
    }

    if (!hwnd) return;
    HWND rootHwnd = GetAncestor(hwnd, GA_ROOT);
    if (!rootHwnd) return;

    wchar_t className[64]{};
    if (GetClassNameW(rootHwnd, className, 64) > 0 && wcscmp(className, L"CabinetWClass") == 0) {
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

bool ExplorerTracker::IsSystemDarkMode() {
    HKEY hKey = nullptr;
    DWORD value = 1; // Default Light
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

    // Find the currently visible ShellTabWindowClass inside CabinetWClass (handles Win11 tabs & Win10 single view)
    HWND activeTab = FindChildByClass(cabinetHwnd, L"ShellTabWindowClass", true);
    if (!activeTab) return false;
    outActiveTabHwnd = activeTab;

    ComPtr<IShellWindows> shellWindows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows))) || !shellWindows) {
        return false;
    }

    long count = 0;
    shellWindows->get_Count(&count);

    for (long i = 0; i < count; ++i) {
        VARIANT idx{};
        idx.vt = VT_I4;
        idx.lVal = i;

        ComPtr<IDispatch> disp;
        if (FAILED(shellWindows->Item(idx, &disp)) || !disp) continue;

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
            // On Win11 with multiple tabs, match the visible ShellTabWindowClass
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

void ExplorerTracker::PerformRefresh() {
    if (!m_uia || !m_cacheRequest) return;

    HWND fgHwnd = GetForegroundWindow();
    if (!fgHwnd) {
        m_renderer.HideOverlay();
        return;
    }

    wchar_t fgClass[64]{};
    GetClassNameW(fgHwnd, fgClass, 64);
    if (wcscmp(fgClass, L"CabinetWClass") != 0) {
        m_renderer.HideOverlay();
        return;
    }

    if (IsIconic(fgHwnd)) {
        m_renderer.HideOverlay();
        return;
    }

    HWND activeTabHwnd = nullptr;
    std::wstring currentFolderPath;
    if (!ResolveActiveExplorerFolder(fgHwnd, activeTabHwnd, currentFolderPath) || currentFolderPath.empty()) {
        m_renderer.HideOverlay();
        return;
    }

    // Locate the SHELLDLL_DefView inside the active tab, then its DirectUIHWND
    HWND defViewHwnd = FindChildByClass(activeTabHwnd, L"SHELLDLL_DefView", true);
    HWND directUiHwnd = FindChildByClass(defViewHwnd ? defViewHwnd : activeTabHwnd, L"DirectUIHWND", true);
    if (!directUiHwnd) {
        m_renderer.HideOverlay();
        return;
    }

    RECT listScreenRect{};
    if (!GetWindowRect(directUiHwnd, &listScreenRect)) {
        m_renderer.HideOverlay();
        return;
    }

    ComPtr<IUIAutomationElement> rootEl;
    if (FAILED(m_uia->ElementFromHandle(directUiHwnd, &rootEl)) || !rootEl) {
        m_renderer.HideOverlay();
        return;
    }

    // Condition 1: Find Header Items (to locate the "Size" / "System.Size" column in Details view)
    VARIANT varHeaderType{};
    varHeaderType.vt = VT_I4;
    varHeaderType.lVal = UIA_HeaderItemControlTypeId;
    ComPtr<IUIAutomationCondition> headerCond;
    m_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, varHeaderType, &headerCond);

    ComPtr<IUIAutomationElementArray> headerItems;
    if (FAILED(rootEl->FindAllBuildCache(TreeScope_Descendants, headerCond.Get(), m_cacheRequest, &headerItems)) || !headerItems) {
        m_renderer.HideOverlay();
        return;
    }

    int headerCount = 0;
    headerItems->get_Length(&headerCount);
    RECT sizeHeaderRect{};
    bool foundSizeColumn = false;

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
            if (SUCCEEDED(item->get_CachedBoundingRectangle(&sizeHeaderRect))) {
                if (sizeHeaderRect.right > sizeHeaderRect.left) {
                    foundSizeColumn = true;
                }
            }
            break;
        }
    }

    if (!foundSizeColumn) {
        // Not in Details View or Size column is hidden
        m_renderer.HideOverlay();
        return;
    }

    // Compute the vertical clip rectangle for the Size column below the header bar
    RECT columnClipRect{};
    columnClipRect.left = std::max(sizeHeaderRect.left, listScreenRect.left);
    columnClipRect.right = std::min(sizeHeaderRect.right, listScreenRect.right);
    columnClipRect.top = std::max(sizeHeaderRect.bottom, listScreenRect.top);
    columnClipRect.bottom = listScreenRect.bottom;

    if (columnClipRect.right <= columnClipRect.left || columnClipRect.bottom <= columnClipRect.top) {
        m_renderer.HideOverlay();
        return;
    }

    // Condition 2: Find visible ListItems (rows) inside DirectUIHWND
    VARIANT varListItemType{};
    varListItemType.vt = VT_I4;
    varListItemType.lVal = UIA_ListItemControlTypeId;
    ComPtr<IUIAutomationCondition> listItemCond;
    m_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, varListItemType, &listItemCond);

    ComPtr<IUIAutomationElementArray> rowItems;
    if (FAILED(rootEl->FindAllBuildCache(TreeScope_Children, listItemCond.Get(), m_cacheRequest, &rowItems)) || !rowItems) {
        m_renderer.HideOverlay();
        return;
    }

    int rowCount = 0;
    rowItems->get_Length(&rowCount);

    // Fallback to TreeScope_Descendants if items are grouped (e.g., Group By view)
    if (rowCount == 0) {
        rootEl->FindAllBuildCache(TreeScope_Descendants, listItemCond.Get(), m_cacheRequest, &rowItems);
        if (rowItems) rowItems->get_Length(&rowCount);
    }

    std::vector<OverlayRowItem> overlayRows;
    overlayRows.reserve(32);

    std::wstring baseDir = currentFolderPath;
    if (!baseDir.empty() && baseDir.back() != L'\\') {
        baseDir.push_back(L'\\');
    }

    for (int i = 0; i < rowCount; ++i) {
        ComPtr<IUIAutomationElement> rowEl;
        if (FAILED(rowItems->GetElement(i, &rowEl)) || !rowEl) continue;

        BOOL isOffscreen = FALSE;
        rowEl->get_CachedIsOffscreen(&isOffscreen);

        RECT rowRect{};
        if (FAILED(rowEl->get_CachedBoundingRectangle(&rowRect))) continue;

        // Skip rows outside the vertical viewport of the column
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
        DWORD attrs = GetFileAttributesW(fullItemPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            // Only calculate and render sizes for folders (files already have native Explorer sizes)
            continue;
        }

        FolderSizeResult sizeRes = m_calculator.RequestFolderSize(fullItemPath);

        OverlayRowItem item{};
        item.cellScreenRect.left = columnClipRect.left;
        item.cellScreenRect.right = columnClipRect.right;
        item.cellScreenRect.top = rowRect.top;
        item.cellScreenRect.bottom = rowRect.bottom;
        item.text = sizeRes.formattedText;
        item.isPending = (sizeRes.status != CalculationStatus::Ready);

        overlayRows.push_back(std::move(item));
    }

    if (overlayRows.empty()) {
        m_renderer.HideOverlay();
        return;
    }

    m_renderer.UpdateOverlay(fgHwnd, columnClipRect, overlayRows, IsSystemDarkMode());
}
