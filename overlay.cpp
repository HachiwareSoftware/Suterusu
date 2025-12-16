#include "overlay.h"
#include <windows.h>
#include <thread>
#include <chrono>
#include <cmath>
#include <string>

// Small, subtle circular indicator placed in bottom-right corner.
// while remaining difficult for nearby people to notice. (MOST IMPORTANT FOR CHEETING)

static const char *OVERLAY_CLASS = "SuterusuBottomRightOverlayClass";
static COLORREF g_currentColor = RGB(40, 200, 120); // Default green
static const char *g_overlayText = nullptr;

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1; // Don't erase background
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Create memory DC for double buffering with transparency
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

        // Fill with transparent black (will be keyed out)
        HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
        FillRect(memDC, &rc, blackBrush);

        // Draw a filled circle using the current color
        HBRUSH brush = CreateSolidBrush(g_currentColor);
        HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, brush);
        HPEN pen = CreatePen(PS_SOLID, 1, g_currentColor);
        HPEN oldPen = (HPEN)SelectObject(memDC, pen);

        // Draw circle on the left side if text is present, otherwise center it
        int circleSize = g_overlayText ? 18 : (rc.right - rc.left);
        int circleX = 0;
        Ellipse(memDC, circleX, rc.top, circleX + circleSize, rc.bottom);

        // Draw text to the right of the circle if provided
        if (g_overlayText && g_overlayText[0] != '\0') {
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(255, 255, 255)); // White text
            HFONT hFont = CreateFontA(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
            HFONT hOldFont = (HFONT)SelectObject(memDC, hFont);

            // Position text to the right of the circle
            RECT textRc = rc;
            textRc.left = circleSize + 4; // 4px gap from circle

            char displayChar[2] = {g_overlayText[0], '\0'};
            DrawTextA(memDC, displayChar, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(memDC, hOldFont);
            DeleteObject(hFont);
        }

        SelectObject(memDC, oldPen);
        SelectObject(memDC, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);

        // Copy to screen
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CREATE:
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

void CreateAndShowOverlay(int duration_ms, IndicatorColor color, const char* text)
{
    g_overlayText = text;

    // Set the color based on parameter
    switch (color) {
    case IndicatorColor::Green:
        g_currentColor = RGB(40, 200, 120);
        break;
    case IndicatorColor::Red:
        g_currentColor = RGB(220, 60, 60);
        break;
    }

    HINSTANCE hInst = GetModuleHandleA(NULL);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = OVERLAY_CLASS;
    wc.hbrBackground = NULL;

    RegisterClassA(&wc);

    // Very small size for discreet indicator
    const int circleSize = 18; // compact size
    const int padding = 12; // padding from screen edges

    // If text is present, make window wider to accommodate text beside circle
    int width = text ? (circleSize + 25) : circleSize;  // 25px for character + gap
    int height = circleSize;

    // Position in bottom-right corner
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = screenW - width - padding;
    int y = screenH - height - padding;

    // Create window with extended styles for always-on-top, no taskbar, layered
    HWND hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        OVERLAY_CLASS, NULL, WS_POPUP,
        x, y, width, height,
        NULL, NULL, hInst, NULL
    );

    if (!hwnd)
    {
        UnregisterClassA(OVERLAY_CLASS, hInst);
        return;
    }

    // Make window click-through so it doesn't interfere with mouse input
    LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
    SetWindowLongA(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);

    // Set black as transparent color key and initial alpha
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), (BYTE)180, LWA_COLORKEY | LWA_ALPHA);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    // Pulse effect: fade in and out subtly
    auto startTime = std::chrono::steady_clock::now();
    const int pulseInterval = 50; // ms between alpha updates

    while (true)
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();

        if (elapsed >= duration_ms)
            break;

        // Some AI math idk
        // Subtle sine wave pulsing (alpha between 120 and 220)
        double phase = (elapsed % 1000) / 1000.0; // 1 second pulse cycle
        double alpha = 170.0 + 50.0 * std::sin(phase * 2.0 * 3.14159265);

        SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), (BYTE)alpha, LWA_COLORKEY | LWA_ALPHA);

        std::this_thread::sleep_for(std::chrono::milliseconds(pulseInterval));
    }

    DestroyWindow(hwnd);
    UnregisterClassA(OVERLAY_CLASS, hInst);
}

void ShowOverlayIndicator(int duration_ms, IndicatorColor color, const char* text)
{
    // Run overlay on a detached thread so caller is non-blocking
    std::string copyText(text ? text : ""); // Enforce a deep copy of text to avoid dangling pointer
                                            // I shouldn't use raw pointer in the first place but too late now
    std::thread t([duration_ms, color, copyText]() {
        CreateAndShowOverlay(duration_ms, color, copyText.c_str());
    });
        t.detach();
}
