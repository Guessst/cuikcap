#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* --- Animation & Border Configuration --- */
#define CONFIG_DASH_LEN 4    /* Length of dashes in pixels */
#define CONFIG_GAP_LEN  4    /* Length of gaps in pixels */
#define CONFIG_ANIM_MS  40   /* Speed of the marching ants (lower = faster) */

#define WM_APP_REBIND (WM_APP + 1)

typedef enum {
    ACTION_CAPTURE,
    ACTION_CANCEL,
    ACTION_REBIND,
    ACTION_QUIT
} ActionType;

typedef struct {
    ActionType action;
    UINT mod;
    UINT vk;
    const char* name;
} KeyBind;

/* --- BSS Segment Globals (Auto-zeroed by OS Loader) --- */
static KeyBind binds[4] = {
    {ACTION_CAPTURE, MOD_CONTROL, VK_SNAPSHOT, "Capture Window"},
    {ACTION_CANCEL, 0, VK_ESCAPE, "Cancel Selection"},
    {ACTION_REBIND, MOD_CONTROL, 'O', "Rebind Capture Key"},
    {ACTION_QUIT, MOD_CONTROL, 'C', "Quit Program"}
};

static HWND hOverlay;
static HBITMAP hFrozenScreen;
static HBRUSH hMarchingBrush;

static BOOL waitingForClick;
static BOOL isClicking;
static int animOffset;

static RECT currentSelection;
static int vScreenLeft;
static int vScreenTop;
static int vScreenWidth;
static int vScreenHeight;

static DWORD mainThreadId;

/* Persistent GDI Handles */
static HDC hScreenDC;
static HDC hMemDC;

/* Static Buffers & Structs */
static char logBufMain[256];
static char logBufConsole[256];
static BYTE brushBits[256];
static char classNameBuf[256];
static char windowNameBuf[256];
static char msgBuf[128];
static char keyNameBuf[32];
static WNDCLASSA wc;
static MSG msg;
static POINT pt;
static INPUT_RECORD ir;
static PAINTSTRUCT ps;
static RECT oldArea;
static RECT newArea;

#if defined(_MSC_VER)
void* memset(void* dest, int c, size_t count)
{
    unsigned char* p = (unsigned char*)dest;
    while (count--) {
        *p++ = (unsigned char)c;
    }
    return dest;
}
#endif

/* --- Helpers --- */
static void FormatKeyBind(UINT mod, UINT vk, char* outBuf) {
    int pos = 0;
    
    if (mod & MOD_CONTROL) {
        const char* s = "CTRL+";
        while (*s) outBuf[pos++] = *s++;
    }
    if (mod & MOD_SHIFT) {
        const char* s = "SHIFT+";
        while (*s) outBuf[pos++] = *s++;
    }
    if (mod & MOD_ALT) {
        const char* s = "ALT+";
        while (*s) outBuf[pos++] = *s++;
    }
    
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        outBuf[pos++] = (char)vk;
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        pos += wsprintfA(outBuf + pos, "F%d", vk - VK_F1 + 1);
    } else if (vk == VK_SNAPSHOT) {
        const char* s = "PRINTSCREEN";
        while (*s) outBuf[pos++] = *s++;
    } else if (vk == VK_ESCAPE) {
        const char* s = "ESC";
        while (*s) outBuf[pos++] = *s++;
    } else if (vk == VK_SPACE) {
        const char* s = "SPACE";
        while (*s) outBuf[pos++] = *s++;
    } else {
        pos += wsprintfA(outBuf + pos, "KEY(0x%02X)", vk);
    }
    outBuf[pos] = '\0';
}

static HBRUSH CreateMarchingBrush(void) {
    int patSize = CONFIG_DASH_LEN + CONFIG_GAP_LEN;
    int bytesPerRow = ((patSize + 15) / 16) * 2;
    HBITMAP hBmp;
    HBRUSH hBrush = NULL;
    int x, y;

    /* Manual zeroing avoids implicit memset */
    for (x = 0; x < 256; x++) {
        brushBits[x] = 0;
    }

    for (y = 0; y < patSize && y < 256 / bytesPerRow; y++) {
        for (x = 0; x < patSize; x++) {
            if ((x + y) % patSize < CONFIG_DASH_LEN) {
                brushBits[y * bytesPerRow + (x / 8)] |= (0x80 >> (x % 8));
            }
        }
    }
    
    hBmp = CreateBitmap(patSize, patSize, 1, 1, brushBits);
    if (hBmp) {
        hBrush = CreatePatternBrush(hBmp);
        DeleteObject(hBmp);
    }
    
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
    char* buffer = (GetCurrentThreadId() == mainThreadId) ? logBufMain : logBufConsole;
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

static void SaveConfig(UINT mod, UINT vk) {
    HANDLE hFile = CreateFileA("cuickcap.config", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE && hFile != NULL) {
        char buf[64];
        DWORD written;
        int len = wsprintfA(buf, "%u %u", mod, vk);
        WriteFile(hFile, buf, (DWORD)len, &written, NULL);
        CloseHandle(hFile);
    }
}

static void LoadConfig(void) {
    HANDLE hFile = CreateFileA("cuickcap.config", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE && hFile != NULL) {
        char buf[64];
        DWORD read = 0;
        int i;
        
        for (i = 0; i < 64; i++) {
            buf[i] = 0;
        }
        
        if (ReadFile(hFile, buf, sizeof(buf) - 1, &read, NULL) && read > 0) {
            UINT m = 0, v = 0;
            char* p = buf;
            while (*p >= '0' && *p <= '9') { m = m * 10 + (*p - '0'); p++; }
            while (*p == ' ') p++;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            if (v != 0) {
                binds[0].mod = m;
                binds[0].vk = v;
            }
        }
        CloseHandle(hFile);
    } else {
        SaveConfig(binds[0].mod, binds[0].vk);
    }
}

static DWORD WINAPI ConsoleInputThread(LPVOID lpParam) {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD read;
    BOOL rebinding = FALSE;
    DWORD mode;

    if (hStdin == INVALID_HANDLE_VALUE || hStdin == NULL) {
        return 0;
    }

    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, mode & ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));

    while (TRUE) {
        DWORD events = 0;
        
        if (GetNumberOfConsoleInputEvents(hStdin, &events) && events > 0) {
            if (ReadConsoleInputA(hStdin, &ir, 1, &read) && read > 0) {
                if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
                    WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
                    DWORD ctrlState = ir.Event.KeyEvent.dwControlKeyState;
                    
                    BOOL isCtrl = (ctrlState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                    BOOL isShift = (ctrlState & SHIFT_PRESSED) != 0;
                    BOOL isAlt = (ctrlState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
                    
                    UINT mod = 0;
                    if (isCtrl) mod |= MOD_CONTROL;
                    if (isShift) mod |= MOD_SHIFT;
                    if (isAlt) mod |= MOD_ALT;

                    if (vk != VK_CONTROL && vk != VK_LCONTROL && vk != VK_RCONTROL &&
                        vk != VK_SHIFT && vk != VK_LSHIFT && vk != VK_RSHIFT &&
                        vk != VK_MENU && vk != VK_LMENU && vk != VK_RMENU) {
                        
                        if (!rebinding) {
                            if (mod == binds[2].mod && vk == binds[2].vk) { 
                                rebinding = TRUE;
                                log_timed("Rebind mode: Press new key combination for capture...");
                            } else if (mod == binds[3].mod && vk == binds[3].vk) { 
                                log_timed("Quit requested. Exiting...");
                                PostThreadMessageA(mainThreadId, WM_QUIT, 0, 0);
                                return 0;
                            }
                        } else {
                            BOOL collision = FALSE;
                            int i;
                            
                            for (i = 1; i < 4; i++) {
                                if (binds[i].mod == mod && binds[i].vk == vk) {
                                    collision = TRUE; break;
                                }
                                if (binds[i].action == ACTION_CANCEL && vk == binds[i].vk) {
                                    collision = TRUE; break;
                                }
                            }
                            
                            if (collision) {
                                log_timed("Clash detected! That key is reserved. Choose another.");
                            } else {
                                binds[0].mod = mod;
                                binds[0].vk = (UINT)vk;
                                SaveConfig(mod, (UINT)vk);
                                PostThreadMessageA(mainThreadId, WM_APP_REBIND, 0, 0);
                                rebinding = FALSE;
                                
                                FormatKeyBind(mod, (UINT)vk, keyNameBuf);
                                wsprintfA(msgBuf, "Keybind updated successfully to: %s", keyNameBuf);
                                log_timed(msgBuf);
                            }
                        }
                    }
                }
            }
        }

        if (rebinding) {
            if (GetAsyncKeyState(VK_SNAPSHOT) & 0x8000) {
                UINT mod = 0;
                BOOL collision = FALSE;
                int i;
                
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mod |= MOD_SHIFT;
                if (GetAsyncKeyState(VK_MENU) & 0x8000) mod |= MOD_ALT;
                
                for (i = 1; i < 4; i++) {
                    if (binds[i].mod == mod && binds[i].vk == VK_SNAPSHOT) {
                        collision = TRUE; break;
                    }
                    if (binds[i].action == ACTION_CANCEL && VK_SNAPSHOT == binds[i].vk) {
                        collision = TRUE; break;
                    }
                }
                
                if (collision) {
                    log_timed("Clash detected! That key is reserved. Choose another.");
                } else {
                    binds[0].mod = mod;
                    binds[0].vk = VK_SNAPSHOT;
                    SaveConfig(mod, VK_SNAPSHOT);
                    PostThreadMessageA(mainThreadId, WM_APP_REBIND, 0, 0);
                    rebinding = FALSE;
                    
                    FormatKeyBind(mod, VK_SNAPSHOT, keyNameBuf);
                    wsprintfA(msgBuf, "Keybind updated successfully to: %s", keyNameBuf);
                    log_timed(msgBuf);
                }
                
                while(GetAsyncKeyState(VK_SNAPSHOT) & 0x8000) Sleep(10);
            }
        }
        
        Sleep(10);
    }
    (void)lpParam;
    return 0;
}

static HWND GetTopLevelWindowFromPoint(POINT p) {
    HWND hwnd = GetTopWindow(NULL);
    while (hwnd) {
        if (hwnd != hOverlay && IsWindowVisible(hwnd)) {
            RECT rect;
            GetWindowRect(hwnd, &rect);
            
            if (p.x >= rect.left && p.x < rect.right && p.y >= rect.top && p.y < rect.bottom) {
                LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
                
                classNameBuf[0] = '\0';
                windowNameBuf[0] = '\0';
                
                GetClassNameA(hwnd, classNameBuf, sizeof(classNameBuf));
                GetWindowTextA(hwnd, windowNameBuf, sizeof(windowNameBuf));

                if (str_contains(classNameBuf, "Magpie") || str_contains(windowNameBuf, "Magpie") ||
                    str_contains(classNameBuf, "Lossless") || str_contains(windowNameBuf, "Lossless")) {
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
    
    return NULL;
}

static RECT GetWindowClientScreenRect(HWND hwnd) {
    static RECT rect;
    rect.left = rect.top = rect.right = rect.bottom = 0;
    
    if (hwnd) {
        HWND rootHwnd = GetAncestor(hwnd, GA_ROOT);
        if (!rootHwnd) rootHwnd = hwnd;
        GetClientRect(rootHwnd, &rect);
        MapWindowPoints(rootHwnd, NULL, (LPPOINT)&rect, 2);
    }
    return rect;
}

static void CaptureFromFrozenScreen(int x, int y, int width, int height) {
    HDC hDestDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBitmap;
    HGDIOBJ hOldDest;
    HGDIOBJ hOldSrc;

    hOldSrc = SelectObject(hMemDC, hFrozenScreen);

    hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    hOldDest = SelectObject(hDestDC, hBitmap);

    BitBlt(hDestDC, 0, 0, width, height, hMemDC, x - vScreenLeft, y - vScreenTop, SRCCOPY);

    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_BITMAP, hBitmap);
        CloseClipboard();
    } else {
        DeleteObject(hBitmap);
    }

    SelectObject(hDestDC, hOldDest);
    SelectObject(hMemDC, hOldSrc);
    DeleteDC(hDestDC);
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

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_TIMER:
            if (wParam == 1) {
                animOffset++;
                if (animOffset >= (CONFIG_DASH_LEN + CONFIG_GAP_LEN)) {
                    animOffset = 0;
                }
                
                if (currentSelection.right > currentSelection.left) {
                    oldArea = currentSelection;
                    oldArea.left   = oldArea.left - vScreenLeft - 2;
                    oldArea.top    = oldArea.top - vScreenTop - 2;
                    oldArea.right  = oldArea.right - vScreenLeft + 2;
                    oldArea.bottom = oldArea.bottom - vScreenTop + 2;
                    InvalidateRect(hwnd, &oldArea, FALSE);
                }
            }
            return 0;

        case WM_PAINT: {
            HDC hdc = BeginPaint(hwnd, &ps);

            if (hFrozenScreen) {
                HGDIOBJ hOld = SelectObject(hMemDC, hFrozenScreen);
                BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, 
                       ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                       hMemDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                SelectObject(hMemDC, hOld);
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
            HWND target;
            GetCursorPos(&pt);
            target = GetTopLevelWindowFromPoint(pt);
            newArea = GetWindowClientScreenRect(target);
            
            if (newArea.left != currentSelection.left || 
                newArea.top != currentSelection.top || 
                newArea.right != currentSelection.right || 
                newArea.bottom != currentSelection.bottom) {
                
                oldArea = currentSelection;
                currentSelection = newArea;
                
                if (hOverlay) {
                    oldArea.left   = oldArea.left - vScreenLeft - 4;
                    oldArea.top    = oldArea.top - vScreenTop - 4;
                    oldArea.right  = oldArea.right - vScreenLeft + 4;
                    oldArea.bottom = oldArea.bottom - vScreenTop + 4;
                    InvalidateRect(hOverlay, &oldArea, FALSE);

                    newArea.left   = newArea.left - vScreenLeft - 4;
                    newArea.top    = newArea.top - vScreenTop - 4;
                    newArea.right  = newArea.right - vScreenLeft + 4;
                    newArea.bottom = newArea.bottom - vScreenTop + 4;
                    InvalidateRect(hOverlay, &newArea, FALSE);
                    
                    UpdateWindow(hOverlay);
                }
            }
            return 0;
        }
        
        case WM_LBUTTONDOWN:
            isClicking = TRUE;
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
            if (wParam == binds[1].vk) { 
                EndCaptureSession();
                log_timed("Capture canceled (Escape).");
            }
            return 0;
    }
    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

void __stdcall mainCRTStartup(void) {
    HGDIOBJ hOld;
    HWND hwnd_init;

    #if defined(_WIN64)
    log_timed("(Windows 64-bit)");
    SetProcessDPIAware();
    #else
    log_timed("(Windows 32-bit)");
    #endif

    mainThreadId = GetCurrentThreadId();
    
    /* Initialize global persistent DCs */
    hScreenDC = GetDC(NULL);
    hMemDC = CreateCompatibleDC(hScreenDC);

    LoadConfig();
    hMarchingBrush = CreateMarchingBrush();

    /* BSS ensures style, cbClsExtra, etc are 0 natively */
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "CuikCapOverlay";
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    RegisterClassA(&wc);

    if (!RegisterHotKey(NULL, 1, binds[0].mod, binds[0].vk)) {
        log_timed("Failed to register initial hotkey. It might be in use.");
        ExitProcess(1);
    }
    
    CreateThread(NULL, 0, ConsoleInputThread, NULL, 0, NULL);

    FormatKeyBind(binds[0].mod, binds[0].vk, keyNameBuf);
    wsprintfA(msgBuf, "Running. Press %s, then click a window.", keyNameBuf);
    log_timed(msgBuf);

    FormatKeyBind(binds[2].mod, binds[2].vk, keyNameBuf);
    wsprintfA(msgBuf, "Press %s in this console to change the capture hotkey.", keyNameBuf);
    log_timed(msgBuf);

    FormatKeyBind(binds[3].mod, binds[3].vk, keyNameBuf);
    wsprintfA(msgBuf, "Press %s in this console to exit.", keyNameBuf);
    log_timed(msgBuf);

    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_APP_REBIND) {
            UnregisterHotKey(NULL, 1);
            if (!RegisterHotKey(NULL, 1, binds[0].mod, binds[0].vk)) {
                log_timed("OS rejected the new hotkey. It may be globally reserved.");
            }
        }
        else if (msg.message == WM_HOTKEY && msg.wParam == 1 && !waitingForClick) {
            waitingForClick = TRUE;
            animOffset = 0;
            
            vScreenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
            vScreenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
            vScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            vScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            
            hFrozenScreen = CreateCompatibleBitmap(hScreenDC, vScreenWidth, vScreenHeight);
            
            hOld = SelectObject(hMemDC, hFrozenScreen);
            BitBlt(hMemDC, 0, 0, vScreenWidth, vScreenHeight, hScreenDC, vScreenLeft, vScreenTop, SRCCOPY | CAPTUREBLT);
            SelectObject(hMemDC, hOld);

            currentSelection.left = 0;
            currentSelection.top = 0;
            currentSelection.right = 0;
            currentSelection.bottom = 0;

            hOverlay = CreateWindowExA(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                "CuikCapOverlay", "",
                WS_POPUP,
                vScreenLeft, vScreenTop, vScreenWidth, vScreenHeight,
                NULL, NULL, GetModuleHandle(NULL), NULL
            );
            
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