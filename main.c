#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HHOOK hMouseHook = NULL;
static BOOL waitingForClick = FALSE;
static BOOL blockNextUp = FALSE;

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

    BitBlt(hMemoryDC, 0, 0, width, height, hScreenDC, topLeft.x, topLeft.y, SRCCOPY);

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
            HWND hwnd = WindowFromPoint(pMouseStruct->pt);
            
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

/* Replaces main() to avoid the CRT startup overhead and __main insertion */
void __stdcall mainCRTStartup(void) {
    /* Do not initialize with = {0} to avoid implicit memset generation by GCC */
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