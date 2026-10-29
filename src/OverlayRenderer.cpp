#include "OverlayRenderer.h"
#include <algorithm>
#include <utility>

namespace {
constexpr wchar_t kOverlayClassName[] = L"Win11ExplorerFolderSizeOverlayWnd";

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
} // namespace

OverlayRenderer::OverlayRenderer() = default;

OverlayRenderer::~OverlayRenderer() {
    Shutdown();
}

bool OverlayRenderer::CreateRenderResources() {
    if (!m_d2dFactory) {
        return false;
    }

    m_textBrush.Reset();
    m_dcRenderTarget.Reset();

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,
        96.0f,
        D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT);

    HRESULT hr = m_d2dFactory->CreateDCRenderTarget(&rtProps, &m_dcRenderTarget);
    if (FAILED(hr) || !m_dcRenderTarget) {
        rtProps.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        hr = m_d2dFactory->CreateDCRenderTarget(&rtProps, &m_dcRenderTarget);
        if (FAILED(hr) || !m_dcRenderTarget) {
            return false;
        }
    }

    m_dcRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    if (FAILED(m_dcRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.88f, 0.88f, 0.88f, 0.95f), &m_textBrush)) || !m_textBrush) {
        return false;
    }

    return true;
}

bool OverlayRenderer::Initialize(HINSTANCE hInstance) {
    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory),
        nullptr,
        reinterpret_cast<void**>(m_d2dFactory.GetAddressOf()));
    if (FAILED(hr) || !m_d2dFactory) {
        return false;
    }

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    if (FAILED(hr) || !m_dwriteFactory) {
        return false;
    }

    if (!CreateRenderResources()) {
        return false;
    }

    EnsureTextFormat(96);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kOverlayClassName;
    RegisterClassExW(&wc);

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    m_overlayHwnd = CreateWindowExW(
        exStyle,
        kOverlayClassName,
        L"",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    return (m_overlayHwnd != nullptr);
}

void OverlayRenderer::ReleaseBitmapResources() {
    if (m_memDc) {
        if (m_oldBitmap) {
            SelectObject(m_memDc, m_oldBitmap);
            m_oldBitmap = nullptr;
        }
        if (m_memBitmap) {
            DeleteObject(m_memBitmap);
            m_memBitmap = nullptr;
        }
        DeleteDC(m_memDc);
        m_memDc = nullptr;
    }
    m_bmpWidth = 0;
    m_bmpHeight = 0;
}

void OverlayRenderer::TrimResources() {
    ReleaseBitmapResources();
}

void OverlayRenderer::Shutdown() {
    HideOverlay();
    ReleaseBitmapResources();
    m_textFormat.Reset();
    m_textBrush.Reset();
    m_dcRenderTarget.Reset();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();

    if (m_overlayHwnd) {
        DestroyWindow(m_overlayHwnd);
        m_overlayHwnd = nullptr;
    }
}

bool OverlayRenderer::EnsureTextFormat(UINT dpi) {
    if (dpi == 0) {
        dpi = 96;
    }
    if (m_textFormat && m_currentDpi == dpi) {
        return true;
    }

    m_textFormat.Reset();
    const float fontSizePx = 12.0f * (static_cast<float>(dpi) / 96.0f);

    HRESULT hr = m_dwriteFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSizePx,
        L"en-us",
        &m_textFormat);

    if (SUCCEEDED(hr) && m_textFormat) {
        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsisSign;
        HRESULT hrTrim = m_dwriteFactory->CreateEllipsisTrimmingSign(m_textFormat.Get(), &ellipsisSign);
        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        trimming.delimiter = 0;
        trimming.delimiterCount = 0;
        if (SUCCEEDED(hrTrim) && ellipsisSign) {
            m_textFormat->SetTrimming(&trimming, ellipsisSign.Get());
        }

        m_currentDpi = dpi;
        return true;
    }
    return false;
}

bool OverlayRenderer::EnsureBitmapSize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (m_memDc && m_memBitmap && m_bmpWidth >= width && m_bmpHeight >= height) {
        return true;
    }

    ReleaseBitmapResources();

    const int grownWidth = std::max(width + 256, m_bmpWidth + (m_bmpWidth / 2));
    const int allocWidth = (std::max(512, grownWidth) + 63) & ~63;

    const int grownHeight = std::max(height + 256, m_bmpHeight + (m_bmpHeight / 2));
    const int allocHeight = (std::max(512, grownHeight) + 63) & ~63;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        return false;
    }

    m_memDc = CreateCompatibleDC(screenDc);
    if (!m_memDc) {
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = allocWidth;
    bmi.bmiHeader.biHeight = -allocHeight; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    m_memBitmap = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);

    if (!m_memBitmap) {
        DeleteDC(m_memDc);
        m_memDc = nullptr;
        return false;
    }

    m_oldBitmap = SelectObject(m_memDc, m_memBitmap);
    m_bmpWidth = allocWidth;
    m_bmpHeight = allocHeight;
    return true;
}

void OverlayRenderer::HideOverlay() {
    if (m_overlayHwnd && m_isVisible) {
        ShowWindow(m_overlayHwnd, SW_HIDE);
        m_isVisible = false;
    }
}

void OverlayRenderer::UpdateOverlay(
    HWND targetExplorerHwnd,
    const RECT& columnClipScreenRect,
    const std::vector<OverlayRowItem>& rows,
    bool isDarkMode) {
    if (!m_overlayHwnd || rows.empty()) {
        HideOverlay();
        return;
    }

    if ((!m_dcRenderTarget || !m_textBrush) && !CreateRenderResources()) {
        HideOverlay();
        return;
    }

    const int width = columnClipScreenRect.right - columnClipScreenRect.left;
    const int height = columnClipScreenRect.bottom - columnClipScreenRect.top;
    if (width <= 6 || height <= 8) {
        HideOverlay();
        return;
    }

    UINT dpi = (targetExplorerHwnd && IsWindow(targetExplorerHwnd)) ? GetDpiForWindow(targetExplorerHwnd) : 96;
    if (!EnsureTextFormat(dpi) || !EnsureBitmapSize(width, height)) {
        HideOverlay();
        return;
    }

    RECT subRect{ 0, 0, width, height };
    if (FAILED(m_dcRenderTarget->BindDC(m_memDc, &subRect))) {
        CreateRenderResources();
        HideOverlay();
        return;
    }

    const float scale = static_cast<float>(m_currentDpi) / 96.0f;
    const float rightPadding = 8.0f * scale;
    const float leftPadding = 4.0f * scale;
    const float hLeftInset = 3.0f * scale; // Clear of column divider line

    const D2D1_COLOR_F readyColor = isDarkMode
        ? D2D1::ColorF(0.88f, 0.88f, 0.88f, 0.95f)
        : D2D1::ColorF(0.22f, 0.22f, 0.22f, 0.95f);

    const D2D1_COLOR_F pendingColor = isDarkMode
        ? D2D1::ColorF(0.58f, 0.58f, 0.58f, 0.65f)
        : D2D1::ColorF(0.50f, 0.50f, 0.50f, 0.65f);

    m_dcRenderTarget->BeginDraw();
    m_dcRenderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    for (const auto& item : rows) {
        if (item.text.empty()) {
            continue;
        }

        const float localLeft = std::max(hLeftInset, static_cast<float>(item.cellScreenRect.left - columnClipScreenRect.left) + leftPadding);
        const float localRight = static_cast<float>(item.cellScreenRect.right - columnClipScreenRect.left) - rightPadding;
        const float localTop = static_cast<float>(item.cellScreenRect.top - columnClipScreenRect.top);
        const float localBottom = static_cast<float>(item.cellScreenRect.bottom - columnClipScreenRect.top);

        if (localRight <= localLeft || localBottom <= localTop) {
            continue;
        }

        m_textBrush->SetColor(item.isPending ? pendingColor : readyColor);
        D2D1_RECT_F layoutRect = D2D1::RectF(localLeft, localTop, localRight, localBottom);

        m_dcRenderTarget->DrawText(
            item.text.c_str(),
            static_cast<UINT32>(item.text.size()),
            m_textFormat.Get(),
            layoutRect,
            m_textBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    HRESULT hr = m_dcRenderTarget->EndDraw();
    if (FAILED(hr)) {
        CreateRenderResources();
        HideOverlay();
        return;
    }

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        return;
    }

    POINT wndPos{ columnClipScreenRect.left, columnClipScreenRect.top };
    SIZE wndSize{ width, height };
    POINT srcPt{ 0, 0 };

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(
        m_overlayHwnd,
        screenDc,
        &wndPos,
        &wndSize,
        m_memDc,
        &srcPt,
        0,
        &blend,
        ULW_ALPHA);

    ReleaseDC(nullptr, screenDc);

    if (!m_isVisible) {
        ShowWindow(m_overlayHwnd, SW_SHOWNOACTIVATE);
        m_isVisible = true;
    }
}
