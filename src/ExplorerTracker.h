#pragma once
#include <windows.h>
#include <unknwn.h>
#include <uiautomation.h>
#include <exdisp.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include "SizeCalculator.h"
#include "OverlayRenderer.h"

class ExplorerTracker {
public:
    ExplorerTracker(SizeCalculator& calculator, OverlayRenderer& renderer);
    ~ExplorerTracker();

    bool Initialize(HWND mainMsgHwnd);
    void Shutdown();

    void SetPaused(bool paused);
    bool IsPaused() const { return m_paused; }

    void RequestRefresh();
    void OnTimerTick();

    static constexpr UINT_PTR kRefreshTimerId = 2001;
    static constexpr UINT_PTR kPeriodicTimerId = 2002;

private:
    struct CachedRowSnapshot {
        RECT rowScreenRect{};
        std::wstring fullItemPath;
    };

    static void CALLBACK WinEventProc(
        HWINEVENTHOOK hWinEventHook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD idEventThread,
        DWORD dwmsEventTime);

    void PerformRefresh(bool allowFastGeometryPath = false);
    bool TryFastGeometryRefresh(HWND fgHwnd);
    void HideOverlay();
    bool ResolveActiveExplorerFolder(HWND cabinetHwnd, HWND& outActiveTabHwnd, std::wstring& outFolderPath);
    ULONGLONG GetNowMicroseconds() const;
    ULONGLONG GetTargetFrameIntervalUs(HWND referenceHwnd);

    static HWND FindChildByClass(HWND parent, const wchar_t* className, bool requireVisible = true);
    static void AdjustViewportForScrollBars(HWND containerHwnd, RECT& inOutViewportRect);
    static bool IsSystemDarkMode();

    SizeCalculator& m_calculator;
    OverlayRenderer& m_renderer;
    HWND m_mainMsgHwnd = nullptr;

    Microsoft::WRL::ComPtr<IUIAutomation> m_uia;
    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> m_cacheRequest;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> m_headerCond;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> m_listItemCond;
    Microsoft::WRL::ComPtr<IShellWindows> m_shellWindows;
    Microsoft::WRL::ComPtr<IUIAutomationElement> m_cachedSizeHeaderEl;

    HWINEVENTHOOK m_hookForeground = nullptr;
    HWINEVENTHOOK m_hookLocation = nullptr;
    HWINEVENTHOOK m_hookScroll = nullptr;
    HWINEVENTHOOK m_hookObjectShow = nullptr;

    LARGE_INTEGER m_qpcFreq{};
    ULONGLONG m_lastRefreshUs = 0;
    ULONGLONG m_lastFullRefreshUs = 0;

    HMONITOR m_cachedMonitor = nullptr;
    ULONGLONG m_cachedFrameIntervalUs = 16667;
    ULONGLONG m_monitorQueryTick = 0;

    HWND m_lastFgHwnd = nullptr;
    HWND m_cachedDirectUiHwnd = nullptr;
    HWND m_cachedActiveTabHwnd = nullptr;
    std::wstring m_cachedFolderPath;
    RECT m_cachedWindowRect{};
    RECT m_lastListRect{};
    RECT m_cachedSizeHeaderRect{};
    RECT m_lastRenderedClipRect{};
    RECT m_lastRenderedHeaderRect{};
    std::vector<CachedRowSnapshot> m_cachedRows;
    bool m_cachedDarkMode = true;

    bool m_hasPendingFolders = false;
    bool m_wasLeftButtonDown = false;
    ULONGLONG m_lastDragMoveUs = 0;
    bool m_paused = false;
    bool m_trailingTimerArmed = false;
    bool m_inRefresh = false;

    static ExplorerTracker* s_instance;
};
