#pragma once
#include <windows.h>
#include <unknwn.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
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

    void UpdateOverlay(
        HWND targetExplorerHwnd,
        const RECT& columnClipScreenRect,
        const std::vector<OverlayRowItem>& rows,
        bool isDarkMode);

    void HideOverlay();
    void TrimResources();

private:
    bool CreateRenderResources();
    bool EnsureBitmapSize(int width, int height);
    bool EnsureTextFormat(UINT dpi);
    void ReleaseBitmapResources();

    HWND m_overlayHwnd = nullptr;
    bool m_isVisible = false;

    HDC m_memDc = nullptr;
    HBITMAP m_memBitmap = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
    int m_bmpWidth = 0;
    int m_bmpHeight = 0;
    UINT m_currentDpi = 0;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_dcRenderTarget;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;

    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
};
