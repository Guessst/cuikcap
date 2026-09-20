#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HHOOK hMouseHook = NULL;
static BOOL waitingForClick = FALSE;
static BOOL blockNextUp = FALSE;

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

/* 
 * Replicates WindowFromPoint but explicitly traps upscaler overlays 
 * which would normally be ignored due to WS_EX_TRANSPARENT 
 */
static HWND GetTopLevelWindowFromPoint(POINT pt) {
    HWND hwnd = GetTopWindow(NULL);
    while (hwnd) {
        if (IsWindowVisible(hwnd)) {
            RECT rect;
            GetWindowRect(hwnd, &rect);
            
            if (PtInRect(&rect, pt)) {
                LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
                char className[256];
                char windowName[256];
                
                /* Avoid implicit memset from {0} initialization */
                className[0] = '\0';
                windowName[0] = '\0';
                
                GetClassNameA(hwnd, className, sizeof(className));
                GetWindowTextA(hwnd, windowName, sizeof(windowName));

                /* Priority match: Magpie or Lossless Scaling overlays */
                if (str_contains(className, "Magpie") || str_contains(windowName, "Magpie") ||
                    str_contains(className, "Lossless") || str_contains(windowName, "Lossless")) {
                    return hwnd;
                }

                /* Standard behavior: ignore click-through / disabled windows */
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
    
    /* Fallback if enumeration fails */
    return WindowFromPoint(pt);
}

static void CaptureWindowClientArea(HWND hwnd) {
    HWND rootHwnd;
    RECT clientRect;
    POINT topLeft;
    POINT bottomRight;
    int width;
    int height;
    HDC hScreenDC;
    HDC hMemoryDC;
    HBITMAP hBitmap;
    HBITMAP hOldBitmap;

    if (!hwnd) {
        return;
    }

    rootHwnd = GetAncestor(hwnd, GA_ROOT);
    if (!rootHwnd) {
        rootHwnd = hwnd;
    }

    GetClientRect(rootHwnd, &clientRect);

    topLeft.x = clientRect.left;
    topLeft.y = clientRect.top;
    bottomRight.x = clientRect.right;
    bottomRight.y = clientRect.bottom;

    ClientToScreen(rootHwnd, &topLeft);
    ClientToScreen(rootHwnd, &bottomRight);

    width = bottomRight.x - topLeft.x;
    height = bottomRight.y - topLeft.y;

    if (width <= 0 || height <= 0) {
        return;
    }

    hScreenDC = GetDC(NULL);
    hMemoryDC = CreateCompatibleDC(hScreenDC);
    hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    hOldBitmap = (HBITMAP)SelectObject(hMemoryDC, hBitmap);

    /* CAPTUREBLT is required to copy layered/transparent windows like Magpie overlays */
    BitBlt(hMemoryDC, 0, 0, width, height, hScreenDC, topLeft.x, topLeft.y, SRCCOPY | CAPTUREBLT);

    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_BITMAP, hBitmap);
        CloseClipboard();
    } else {
        DeleteObject(hBitmap);
    }

    SelectObject(hMemoryDC, hOldBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(NULL, hScreenDC);
}

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        if (waitingForClick && wParam == WM_LBUTTONDOWN) {
            MSLLHOOKSTRUCT *pMouseStruct = (MSLLHOOKSTRUCT *)lParam;
            HWND hwnd = GetTopLevelWindowFromPoint(pMouseStruct->pt);
            
            CaptureWindowClientArea(hwnd);
            log_timed("Captured to clipboard!");
            
            waitingForClick = FALSE;
            blockNextUp = TRUE;
            return 1;
        }
        if (blockNextUp && wParam == WM_LBUTTONUP) {
            blockNextUp = FALSE;
            return 1;
        }
    }
    return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
}

void __stdcall mainCRTStartup(void) {
    MSG msg;

    #if defined(_WIN64)
    log_timed("(Windows 64-bit)");
    SetProcessDPIAware();
    #else
    log_timed("(Windows 32-bit)");
    #endif

    if (!RegisterHotKey(NULL, 1, MOD_CONTROL, VK_SNAPSHOT)) {
        log_timed("Failed to register hotkey. It might be in use.");
        ExitProcess(1);
    }

    hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, GetModuleHandle(NULL), 0);
    if (!hMouseHook) {
        log_timed("Failed to install mouse hook.");
        UnregisterHotKey(NULL, 1);
        ExitProcess(1);
    }

    log_timed("Running. Press Ctrl + PrintScreen, then click a window.");

    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_HOTKEY && msg.wParam == 1) {
            waitingForClick = TRUE;
            log_timed("Waiting for window click...");
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(hMouseHook);
    UnregisterHotKey(NULL, 1);
    ExitProcess(0);
}