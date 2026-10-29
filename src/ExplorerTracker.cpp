#include "ExplorerTracker.h"

ExplorerTracker::ExplorerTracker(SizeCalculator& calculator, OverlayRenderer& renderer)
    : m_calculator(calculator), m_renderer(renderer) {}

ExplorerTracker::~ExplorerTracker() {
    Shutdown();
}

bool ExplorerTracker::Initialize(HWND mainMsgHwnd) {
    m_mainMsgHwnd = mainMsgHwnd;
    // Phase 2 will register SetWinEventHook, IShellWindows, and IUIAutomation
    return true;
}

void ExplorerTracker::Shutdown() {
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
    if (m_paused) return;
    // Phase 2 will inspect active CabinetWClass / ShellTabWindowClass and query visible rows
}
