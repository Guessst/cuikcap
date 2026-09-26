#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* --- Animation & Border Configuration --- */
#define CONFIG_DASH_LEN 4    /* Length of dashes in pixels */
#define CONFIG_GAP_LEN  4    /* Length of gaps in pixels */
#define CONFIG_ANIM_MS  40   /* Speed of the marching ants (lower = faster) */

static HWND hOverlay = NULL;
static HBITMAP hFrozenScreen = NULL;
static HBRUSH hMarchingBrush = NULL;

static BOOL waitingForClick = FALSE;
static BOOL isClicking = FALSE;
static int animOffset = 0;

static RECT currentSelection;
static int vScreenLeft = 0;
static int vScreenTop = 0;
static int vScreenWidth = 0;
static int vScreenHeight = 0;

/* Generates a seamless diagonal pattern brush of exact user-configured size */
static HBRUSH CreateMarchingBrush(void) {
    int patSize = CONFIG_DASH_LEN + CONFIG_GAP_LEN;
    /* CreateBitmap requires scanlines to be 16-bit (WORD) aligned */
    int bytesPerRow = ((patSize + 15) / 16) * 2;
    int allocSize = bytesPerRow * patSize;
    BYTE* bits;
    HBITMAP hBmp;
    HBRUSH hBrush = NULL;
    int x, y;

    bits = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, allocSize);
    if (!bits) return NULL;

    for (y = 0; y < patSize; y++) {
        for (x = 0; x < patSize; x++) {
            /* Create a diagonal stripe that wraps perfectly */
            if ((x + y) % patSize < CONFIG_DASH_LEN) {
                bits[y * bytesPerRow + (x / 8)] |= (0x80 >> (x % 8));
            }
        }
    }
    
    hBmp = CreateBitmap(patSize, patSize, 1, 1, bits);
    if (hBmp) {
        hBrush = CreatePatternBrush(hBmp);
        DeleteObject(hBmp);
    }
    
    HeapFree(GetProcessHeap(), 0, bits);
    return hBrush;
}

static int str_contains(const char* haystack, const char* needle) {
    const char* h;
    const char* n;
    
    if (!haystack || !needle) return 0;
    if (!*needle) return 1;

    while (*haystack) {
        h = haystack;
        n = needle;
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        if (!*n) {
            return 1;
        }
        haystack++;
    }
    return 0;
}

static void log_timed(const char* message) {
    SYSTEMTIME st;
    char buffer[256];
    HANDLE hConsole;
    DWORD written;
    int len;

    GetLocalTime(&st);
    
    len = wsprintfA(buffer, "%04d-%02d-%02d %02d:%02d:%02d:%03d: %s\r\n",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                    message);

    hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hConsole != INVALID_HANDLE_VALUE && hConsole != NULL) {
        WriteConsoleA(hConsole, buffer, (DWORD)len, &written, NULL);
    }
}

static HWND GetTopLevelWindowFromPoint(POINT pt) {
    HWND hwnd = GetTopWindow(NULL);
    while (hwnd) {
        /* Ignore our overlay window to allow finding the window underneath */
        if (hwnd != hOverlay && IsWindowVisible(hwnd)) {
            RECT rect;
            GetWindowRect(hwnd, &rect);
            
            if (PtInRect(&rect, pt)) {
                LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
                char className[256];
                char windowName[256];
                
                className[0] = '\0';
                windowName[0] = '\0';
                
                GetClassNameA(hwnd, className, sizeof(className));
                GetWindowTextA(hwnd, windowName, sizeof(windowName));

                if (str_contains(className, "Magpie") || str_contains(windowName, "Magpie") ||
                    str_contains(className, "Lossless") || str_contains(windowName, "Lossless")) {
                    return hwnd;
                }

                if ((exStyle & WS_EX_TRANSPARENT) == 0) {
                    LONG style = GetWindowLongA(hwnd, GWL_STYLE);
                    if ((style & WS_DISABLED) == 0) {
                        return hwnd;
                    }
                }
            }
        }
        hwnd = GetWindow(hwnd, GW_HWNDNEXT);
    }
    
    /* Returning NULL instead of WindowFromPoint safely handles desktop background clicks */
    return NULL;
}

static RECT GetWindowClientScreenRect(HWND hwnd) {
    RECT rect;
    
    /* Avoid implicit memset generation */
    rect.left = 0;
    rect.top = 0;
    rect.right = 0;
    rect.bottom = 0;
    
    if (hwnd) {
        HWND rootHwnd = GetAncestor(hwnd, GA_ROOT);
        POINT tl;
        POINT br;
        
        if (!rootHwnd) rootHwnd = hwnd;
        GetClientRect(rootHwnd, &rect);
        
        tl.x = rect.left;
        tl.y = rect.top;
        br.x = rect.right;
        br.y = rect.bottom;
        
        ClientToScreen(rootHwnd, &tl);
        ClientToScreen(rootHwnd, &br);
        
        rect.left = tl.x;
        rect.top = tl.y;
        rect.right = br.x;
        rect.bottom = br.y;
    }
    return rect;
}

static void CaptureFromFrozenScreen(int x, int y, int width, int height) {
    HDC hScreenDC = GetDC(NULL);
    HDC hSrcDC = CreateCompatibleDC(hScreenDC);
    HDC hDestDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBitmap;
    HGDIOBJ hOldDest;

    SelectObject(hSrcDC, hFrozenScreen);

    hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    hOldDest = SelectObject(hDestDC, hBitmap);

    BitBlt(hDestDC, 0, 0, width, height, hSrcDC, x - vScreenLeft, y - vScreenTop, SRCCOPY);

    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_BITMAP, hBitmap);
        CloseClipboard();
    } else {
        DeleteObject(hBitmap);
    }

    SelectObject(hDestDC, hOldDest);
    DeleteDC(hDestDC);
    DeleteDC(hSrcDC);
    ReleaseDC(NULL, hScreenDC);
}

static void EndCaptureSession(void) {
    waitingForClick = FALSE;
    isClicking = FALSE;
    
    if (hOverlay) {
        KillTimer(hOverlay, 1);
        DestroyWindow(hOverlay);
        hOverlay = NULL;
    }
    if (hFrozenScreen) {
        DeleteObject(hFrozenScreen);
        hFrozenScreen = NULL;
    }
    
    currentSelection.left = 0;
    currentSelection.top = 0;
    currentSelection.right = 0;
    currentSelection.bottom = 0;
}

/* 
 * The overlay now naturally handles mouse and keyboard messages,
 * entirely bypassing the need for global low-level hooks.
 */
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            if (wParam == 1) {
                animOffset++;
                if (animOffset >= (CONFIG_DASH_LEN + CONFIG_GAP_LEN)) {
                    animOffset = 0;
                }
                
                if (currentSelection.right > currentSelection.left) {
                    RECT r;
                    r = currentSelection;
                    /* Inflate enough to encompass the 1px external border */
                    InflateRect(&r, 2, 2);
                    OffsetRect(&r, -vScreenLeft, -vScreenTop);
                    InvalidateRect(hwnd, &r, FALSE);
                }
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            if (hFrozenScreen) {
                HDC hMemDC = CreateCompatibleDC(hdc);
                HGDIOBJ hOld = SelectObject(hMemDC, hFrozenScreen);
                BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, 
                       ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                       hMemDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                SelectObject(hMemDC, hOld);
                DeleteDC(hMemDC);
            }

            if (currentSelection.right > currentSelection.left && hMarchingBrush) {
                int x = currentSelection.left - vScreenLeft - 1;
                int y = currentSelection.top - vScreenTop - 1;
                int w = currentSelection.right - currentSelection.left + 2;
                int h = currentSelection.bottom - currentSelection.top + 2;

                HGDIOBJ hOldBrush;
                COLORREF oldBkColor;
                COLORREF oldTextColor;

                oldTextColor = SetTextColor(hdc, RGB(0, 0, 0));
                oldBkColor = SetBkColor(hdc, RGB(255, 255, 255));
                
                SetBrushOrgEx(hdc, animOffset, 0, NULL);
                hOldBrush = SelectObject(hdc, hMarchingBrush);
                PatBlt(hdc, x, y, w, 1, PATCOPY);
                PatBlt(hdc, x + w - 1, y, 1, h, PATCOPY);

                SetBrushOrgEx(hdc, -animOffset, 0, NULL);
                SelectObject(hdc, hMarchingBrush);
                PatBlt(hdc, x, y + h - 1, w, 1, PATCOPY);
                PatBlt(hdc, x, y, 1, h, PATCOPY);

                SelectObject(hdc, hOldBrush);
                SetTextColor(hdc, oldTextColor);
                SetBkColor(hdc, oldBkColor);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
            
        case WM_MOUSEMOVE: {
            POINT pt;
            HWND target;
            RECT newSelection;
            
            GetCursorPos(&pt);
            target = GetTopLevelWindowFromPoint(pt);
            newSelection = GetWindowClientScreenRect(target);
            
            if (newSelection.left != currentSelection.left || 
                newSelection.top != currentSelection.top || 
                newSelection.right != currentSelection.right || 
                newSelection.bottom != currentSelection.bottom) {
                
                RECT oldArea = currentSelection;
                RECT newArea;
                currentSelection = newSelection;
                
                if (hOverlay) {
                    InflateRect(&oldArea, 4, 4);
                    OffsetRect(&oldArea, -vScreenLeft, -vScreenTop);
                    InvalidateRect(hOverlay, &oldArea, FALSE);

                    newArea = currentSelection;
                    InflateRect(&newArea, 4, 4);
                    OffsetRect(&newArea, -vScreenLeft, -vScreenTop);
                    InvalidateRect(hOverlay, &newArea, FALSE);
                    
                    UpdateWindow(hOverlay);
                }
            }
            return 0;
        }
        
        case WM_LBUTTONDOWN:
            isClicking = TRUE;
            /* Captures all mouse input to this window so we reliably receive WM_LBUTTONUP */
            SetCapture(hwnd);
            return 0;
            
        case WM_LBUTTONUP:
            if (isClicking) {
                int width;
                int height;
                
                isClicking = FALSE;
                ReleaseCapture();
                
                width = currentSelection.right - currentSelection.left;
                height = currentSelection.bottom - currentSelection.top;

                if (width > 0 && height > 0) {
                    CaptureFromFrozenScreen(currentSelection.left, currentSelection.top, width, height);
                    log_timed("Captured to clipboard!");
                }
                EndCaptureSession();
            }
            return 0;
            
        case WM_RBUTTONDOWN:
            EndCaptureSession();
            log_timed("Capture canceled (Right Click).");
            return 0;
            
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                EndCaptureSession();
                log_timed("Capture canceled (Escape).");
            }
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void __stdcall mainCRTStartup(void) {
    MSG msg;
    WNDCLASSA wc;
    HDC hScreenDC;
    HDC hMemDC;
    HGDIOBJ hOld;
    POINT pt;
    HWND hwnd_init;

    #if defined(_WIN64)
    log_timed("(Windows 64-bit)");
    SetProcessDPIAware();
    #else
    log_timed("(Windows 32-bit)");
    #endif

    hMarchingBrush = CreateMarchingBrush();

    wc.style = 0;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hIcon = NULL;
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "CuikCapOverlay";
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    RegisterClassA(&wc);

    if (!RegisterHotKey(NULL, 1, MOD_CONTROL, VK_SNAPSHOT)) {
        log_timed("Failed to register hotkey. It might be in use.");
        ExitProcess(1);
    }

    /* Note: Global hooks completely removed */
    log_timed("Running. Press Ctrl + PrintScreen, then click a window.");

    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_HOTKEY && msg.wParam == 1 && !waitingForClick) {
            waitingForClick = TRUE;
            animOffset = 0;
            
            vScreenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
            vScreenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
            vScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            vScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            
            hScreenDC = GetDC(NULL);
            hMemDC = CreateCompatibleDC(hScreenDC);
            hFrozenScreen = CreateCompatibleBitmap(hScreenDC, vScreenWidth, vScreenHeight);
            
            hOld = SelectObject(hMemDC, hFrozenScreen);
            BitBlt(hMemDC, 0, 0, vScreenWidth, vScreenHeight, hScreenDC, vScreenLeft, vScreenTop, SRCCOPY | CAPTUREBLT);
            SelectObject(hMemDC, hOld);
            
            DeleteDC(hMemDC);
            ReleaseDC(NULL, hScreenDC);

            currentSelection.left = 0;
            currentSelection.top = 0;
            currentSelection.right = 0;
            currentSelection.bottom = 0;

            /* WS_EX_TRANSPARENT and WS_EX_LAYERED removed to allow overlay to receive native input */
            hOverlay = CreateWindowExA(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                "CuikCapOverlay", "",
                WS_POPUP,
                vScreenLeft, vScreenTop, vScreenWidth, vScreenHeight,
                NULL, NULL, GetModuleHandle(NULL), NULL
            );
            
            /* Show normally, bring to foreground, and focus to ensure keyboard intercepts work */
            ShowWindow(hOverlay, SW_SHOW);
            SetForegroundWindow(hOverlay);
            SetFocus(hOverlay);
            
            SetTimer(hOverlay, 1, CONFIG_ANIM_MS, NULL);
            
            log_timed("Waiting for window click...");
            
            GetCursorPos(&pt);
            hwnd_init = GetTopLevelWindowFromPoint(pt);
            currentSelection = GetWindowClientScreenRect(hwnd_init);
            InvalidateRect(hOverlay, NULL, FALSE);
            UpdateWindow(hOverlay);
            
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterHotKey(NULL, 1);
    if (hMarchingBrush) DeleteObject(hMarchingBrush);
    ExitProcess(0);
}