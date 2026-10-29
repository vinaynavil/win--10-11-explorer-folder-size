#pragma once
#include <windows.h>
#include <unknwn.h>
#include <uiautomation.h>
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
    static void CALLBACK WinEventProc(
        HWINEVENTHOOK hWinEventHook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD idEventThread,
        DWORD dwmsEventTime);

    void PerformRefresh();
    bool ResolveActiveExplorerFolder(HWND cabinetHwnd, HWND& outActiveTabHwnd, std::wstring& outFolderPath);
    static HWND FindChildByClass(HWND parent, const wchar_t* className, bool requireVisible = true);
    static bool IsSystemDarkMode();

    SizeCalculator& m_calculator;
    OverlayRenderer& m_renderer;
    HWND m_mainMsgHwnd = nullptr;
    IUIAutomation* m_uia = nullptr;
    IUIAutomationCacheRequest* m_cacheRequest = nullptr;
    HWINEVENTHOOK m_hookForeground = nullptr;
    HWINEVENTHOOK m_hookLocation = nullptr;
    HWINEVENTHOOK m_hookScroll = nullptr;
    HWINEVENTHOOK m_hookObjectShow = nullptr;
    bool m_paused = false;

    static ExplorerTracker* s_instance;
};
