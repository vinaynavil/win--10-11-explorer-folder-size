#pragma once
#include <windows.h>
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

private:
    SizeCalculator& m_calculator;
    OverlayRenderer& m_renderer;
    HWND m_mainMsgHwnd = nullptr;
    bool m_paused = false;
};
