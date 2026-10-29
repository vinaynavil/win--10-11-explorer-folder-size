#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct OverlayRowItem {
    RECT cellScreenRect{};
    std::wstring text;
    bool isPending = false;
};

class OverlayRenderer {
public:
    OverlayRenderer();
    ~OverlayRenderer();

    bool Initialize(HINSTANCE hInstance);
    void Shutdown();

    void UpdateOverlay(HWND targetExplorerHwnd, const RECT& columnClipScreenRect, const std::vector<OverlayRowItem>& rows, bool isDarkMode);
    void HideOverlay();

private:
    HWND m_overlayHwnd = nullptr;
};
