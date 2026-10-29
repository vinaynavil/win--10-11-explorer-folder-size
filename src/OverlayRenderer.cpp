#include "OverlayRenderer.h"

OverlayRenderer::OverlayRenderer() = default;

OverlayRenderer::~OverlayRenderer() {
    Shutdown();
}

bool OverlayRenderer::Initialize(HINSTANCE /*hInstance*/) {
    // Phase 4 will initialize Direct2D/DirectWrite and the WS_EX_LAYERED | WS_EX_TRANSPARENT window
    return true;
}

void OverlayRenderer::Shutdown() {
    if (m_overlayHwnd) {
        DestroyWindow(m_overlayHwnd);
        m_overlayHwnd = nullptr;
    }
}

void OverlayRenderer::UpdateOverlay(HWND /*targetExplorerHwnd*/, const RECT& /*columnClipScreenRect*/, const std::vector<OverlayRowItem>& /*rows*/, bool /*isDarkMode*/) {
    // Phase 4 will render aligned folder sizes
}

void OverlayRenderer::HideOverlay() {
    if (m_overlayHwnd) {
        ShowWindow(m_overlayHwnd, SW_HIDE);
    }
}
