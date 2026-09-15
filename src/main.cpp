#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include "ai_client.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// IDs for controls
#define ID_COMBO_MODELS   101
#define ID_EDIT_CHAT      103
#define ID_EDIT_INPUT     104
#define ID_BTN_SEND       105
#define ID_STATUS         107

// Custom Messages for async work
#define WM_AI_RESPONSE    (WM_USER + 1)
#define WM_BACKEND_STATUS (WM_USER + 2)

// Popup menu command bases
#define MENU_MODEL_BASE   200

HINSTANCE g_hInst = NULL;
HWND g_hMainWnd = NULL;
HWND g_hComboModels = NULL;
HWND g_hStatus = NULL;
HWND g_hEditChat = NULL;
HWND g_hEditInput = NULL;
HWND g_hBtnSend = NULL;

HBRUSH g_hBrushBg = NULL;
HBRUSH g_hBrushControl = NULL;
HBRUSH g_hBrushDark = NULL;
HFONT g_hFont = NULL;

ULONG_PTR g_gdiplusToken = 0;

AIClient g_aiClient("127.0.0.1");
std::vector<ModelInfo> g_models;
std::wstring g_chatHistory = L"System: Welcome to Local AI Agent!\r\n"
                             L"Connects to local AI models you have already installed (Ollama or LM Studio / OpenAI-compatible).\r\n"
                             L"Choose a model and start typing.\r\n\r\n";
std::mutex g_chatMutex;
bool g_isGenerating = false;
bool g_isRefreshing = false;
bool g_statusConnected = false;
std::wstring g_statusText = L"Searching for a local AI server...";

int g_backendKind = BK_AUTO;
int g_activeBackend = 0;
int g_activePort = 0;
std::string g_backendLabel = "None";

// UI interactive states
bool g_inputFocused = false;
bool g_btnHovered = false;
bool g_btnPressed = false;
bool g_modelBtnHovered = false;
bool g_modelBtnPressed = false;
int g_selectedModelIdx = 0;

// GDI+ rounded rectangle helpers
void DrawRoundedRect(Gdiplus::Graphics& graphics, Gdiplus::Brush& brush, float x, float y, float width, float height, float radius) {
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, radius * 2, radius * 2, 180, 90);
    path.AddArc(x + width - radius * 2, y, radius * 2, radius * 2, 270, 90);
    path.AddArc(x + width - radius * 2, y + height - radius * 2, radius * 2, radius * 2, 0, 90);
    path.AddArc(x, y + height - radius * 2, radius * 2, radius * 2, 90, 90);
    path.CloseFigure();
    graphics.FillPath(&brush, &path);
}

void DrawRoundedRectBorder(Gdiplus::Graphics& graphics, Gdiplus::Pen& pen, float x, float y, float width, float height, float radius) {
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, radius * 2, radius * 2, 180, 90);
    path.AddArc(x + width - radius * 2, y, radius * 2, radius * 2, 270, 90);
    path.AddArc(x + width - radius * 2, y + height - radius * 2, radius * 2, radius * 2, 0, 90);
    path.AddArc(x, y + height - radius * 2, radius * 2, radius * 2, 90, 90);
    path.CloseFigure();
    graphics.DrawPath(&pen, &path);
}

void UpdateChatScrollbar() {
    if (!g_hEditChat || !g_hFont) return;
    int lineCount = (int)SendMessageW(g_hEditChat, EM_GETLINECOUNT, 0, 0);
    HDC hdc = GetDC(g_hEditChat);
    HFONT hOld = (HFONT)SelectObject(hdc, g_hFont);
    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, hOld);
    ReleaseDC(g_hEditChat, hdc);
    RECT rc = {};
    SendMessageW(g_hEditChat, EM_GETRECT, 0, (LPARAM)&rc);
    int lineH = tm.tmHeight > 0 ? tm.tmHeight : 16;
    int visibleLines = (rc.bottom - rc.top) / lineH;
    if (visibleLines < 1) visibleLines = 1;
    ShowScrollBar(g_hEditChat, SB_VERT, lineCount > visibleLines ? TRUE : FALSE);
}

// Hide the caret in the read-only chat edit
LRESULT CALLBACK ChatEditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    UNREFERENCED_PARAMETER(uIdSubclass);
    UNREFERENCED_PARAMETER(dwRefData);
    if (uMsg == WM_SETFOCUS || uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONDBLCLK) {
        LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
        HideCaret(hwnd);
        return res;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK InputSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    UNREFERENCED_PARAMETER(uIdSubclass);
    UNREFERENCED_PARAMETER(dwRefData);
    switch (uMsg) {
    case WM_SETFOCUS: {
        g_inputFocused = true;
        InvalidateRect(GetParent(hwnd), NULL, FALSE);
        break;
    }
    case WM_KILLFOCUS: {
        g_inputFocused = false;
        InvalidateRect(GetParent(hwnd), NULL, FALSE);
        break;
    }
    case WM_KEYDOWN: {
        if (wParam == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) {
            SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(ID_BTN_SEND, BN_CLICKED), 0);
            return 0; // no beep
        }
        break;
    }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Shared hover/press handling for owner-drawn dropdown buttons (model + backend).
// Each button gets its own hover/pressed flags via a small template trick.
template <bool* Hovered, bool* Pressed>
LRESULT CALLBACK DropBtnSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    UNREFERENCED_PARAMETER(uIdSubclass);
    UNREFERENCED_PARAMETER(dwRefData);
    switch (uMsg) {
    case WM_MOUSEMOVE: {
        if (!*Hovered) {
            *Hovered = true;
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        *Hovered = false;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_LBUTTONDOWN: {
        *Pressed = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_LBUTTONUP: {
        if (*Pressed) {
            *Pressed = false;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            RECT rc;
            GetClientRect(hwnd, &rc);
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            if (PtInRect(&rc, pt)) {
                SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM((UINT)(UINT_PTR)dwRefData, BN_CLICKED), (LPARAM)hwnd);
            }
        }
        break;
    }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Wrappers that wire each dropdown button's hover/pressed flags. The control
// id is passed through the subclass dwRefData and forwarded to the postback.
constexpr USHORT kModelBtnSubclass = 3;

LRESULT CALLBACK ModelBtnSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    return DropBtnSubclassProc<&g_modelBtnHovered, &g_modelBtnPressed>(
        hwnd, uMsg, wParam, lParam, uIdSubclass, dwRefData);
}

LRESULT CALLBACK SendBtnSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    UNREFERENCED_PARAMETER(uIdSubclass);
    UNREFERENCED_PARAMETER(dwRefData);
    switch (uMsg) {
    case WM_MOUSEMOVE: {
        if (!g_btnHovered) {
            g_btnHovered = true;
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        g_btnHovered = false;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_LBUTTONDOWN: {
        g_btnPressed = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_LBUTTONUP: {
        if (g_btnPressed) {
            g_btnPressed = false;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            RECT rc;
            GetClientRect(hwnd, &rc);
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            if (PtInRect(&rc, pt)) {
                SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(ID_BTN_SEND, BN_CLICKED), (LPARAM)hwnd);
            }
        }
        break;
    }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

void AppendChat(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(g_chatMutex);
    g_chatHistory += text + L"\r\n\r\n";
    if (g_hEditChat) {
        SetWindowTextW(g_hEditChat, g_chatHistory.c_str());
        SendMessageW(g_hEditChat, EM_SETSEL, (WPARAM)g_chatHistory.size(), (LPARAM)g_chatHistory.size());
        SendMessageW(g_hEditChat, EM_SCROLLCARET, 0, 0);
        HideCaret(g_hEditChat);
        UpdateChatScrollbar();
    }
}

std::wstring UTF8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring out(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz);
    return out;
}

std::string WideToUTF8(const std::wstring& s) {
    if (s.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string out(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz, NULL, NULL);
    return out;
}

void RefreshModels() {
    if (g_isRefreshing) return;
    g_isRefreshing = true;
    g_statusText = L"Detecting local AI backend...";
    InvalidateRect(g_hMainWnd, NULL, FALSE);

    std::thread([]() {
        std::string label, err;
        std::vector<ModelInfo> models = g_aiClient.GetModels(g_backendKind, label, err);

        {
            std::lock_guard<std::mutex> lock(g_chatMutex);
            g_models = models;
            g_activeBackend = g_aiClient.ActiveBackend();
            if (g_activeBackend) {
                g_activePort = AIClient::DetectPort(g_activeBackend);
                g_backendLabel = label;
                std::wstring wlabel = UTF8ToWide(label);
                if (g_models.empty()) {
                    g_statusText = L"\u25CF " + wlabel + L" connected \u00B7 no models installed";
                } else {
                    g_statusText = L"\u25CF " + wlabel + L" \u00B7 " + std::to_wstring(g_models.size()) + L" models";
                }
                g_statusConnected = true;
            } else {
                g_activePort = 0;
                g_backendLabel.clear();
                g_statusText = L"\u25CB No local AI server found \u2014 start Ollama or LM Studio";
                g_statusConnected = false;
            }
        }

        g_isRefreshing = false;
        PostMessageW(g_hMainWnd, WM_BACKEND_STATUS, 0, 0);
    }).detach();
}

void SendPrompt() {
    if (g_isGenerating) return;

    wchar_t inputBuf[4096];
    GetWindowTextW(g_hEditInput, inputBuf, 4096);
    std::wstring promptW(inputBuf);
    if (promptW.empty()) return;

    SetWindowTextW(g_hEditInput, L"");
    AppendChat(L"You: " + promptW);

    std::string modelName = "llama3";
    if (g_selectedModelIdx >= 0 && g_selectedModelIdx < (int)g_models.size()) {
        modelName = g_models[g_selectedModelIdx].name;
    }

    // Snapshot the connected backend. If nothing was detected yet, try
    // Ollama and let the error surface in the chat.
    int backend = (g_activeBackend == BK_OLLAMA || g_activeBackend == BK_OPENAI) ? g_activeBackend : BK_OLLAMA;
    int port = backend == BK_OLLAMA ? AIClient::DetectPort(BK_OLLAMA) : AIClient::DetectPort(BK_OPENAI);

    g_isGenerating = true;
    EnableWindow(g_hBtnSend, FALSE);
    InvalidateRect(g_hBtnSend, NULL, TRUE);

    std::string promptStr = WideToUTF8(promptW);

    std::thread([backend, port, modelName, promptStr]() {
        std::string err;
        std::string resp = g_aiClient.Chat(modelName, promptStr, backend, port, err);

        std::wstring respW = err.empty() ? UTF8ToWide(resp) : (L"Error: " + UTF8ToWide(err));

        wchar_t* heapResp = new wchar_t[respW.size() + 1];
        wcscpy_s(heapResp, respW.size() + 1, respW.c_str());
        PostMessageW(g_hMainWnd, WM_AI_RESPONSE, 0, (LPARAM)heapResp);
    }).detach();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hBrushBg = CreateSolidBrush(RGB(13, 14, 18));      // main background
        g_hBrushControl = CreateSolidBrush(RGB(26, 28, 38)); // control panels
        g_hBrushDark = CreateSolidBrush(RGB(21, 22, 29));    // chat history background

        g_hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        BOOL useDark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDark, sizeof(useDark));

        CreateWindowW(L"STATIC", L"Model:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                      15, 17, 50, 25, hwnd, NULL, g_hInst, NULL); // font set on layout pass

        g_hComboModels = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                       70, 12, 150, 28, hwnd, (HMENU)ID_COMBO_MODELS, g_hInst, NULL);
        SetWindowSubclass(g_hComboModels, ModelBtnSubclassProc, kModelBtnSubclass, (DWORD_PTR)ID_COMBO_MODELS);

        g_hStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                  290, 17, 460, 25, hwnd, (HMENU)ID_STATUS, g_hInst, NULL);

        g_hEditChat = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                    25, 65, 740, 410, hwnd, (HMENU)ID_EDIT_CHAT, g_hInst, NULL);
        SetWindowTheme(g_hEditChat, L"DarkMode_Explorer", NULL);
        SetWindowTextW(g_hEditChat, g_chatHistory.c_str());
        SetWindowSubclass(g_hEditChat, ChatEditSubclassProc, 1, 0);
        ShowScrollBar(g_hEditChat, SB_VERT, FALSE);

        g_hEditInput = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                     25, 509, 615, 25, hwnd, (HMENU)ID_EDIT_INPUT, g_hInst, NULL);
        SetWindowSubclass(g_hEditInput, InputSubclassProc, 2, 0);
        SendMessageW(g_hEditInput, EM_SETCUEBANNER, TRUE, (LPARAM)L"Ask your local AI Agent anything...");

        g_hBtnSend = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                   730, 500, 31, 31, hwnd, (HMENU)ID_BTN_SEND, g_hInst, NULL);
        SetWindowSubclass(g_hBtnSend, SendBtnSubclassProc, 4, 0);

        // Apply font to all child windows
        for (HWND hChild = GetWindow(hwnd, GW_CHILD); hChild; hChild = GetWindow(hChild, GW_HWNDNEXT)) {
            SendMessageW(hChild, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        }

        RefreshModels();
        break;
    }
    case WM_SIZE: {
        int cx = LOWORD(lParam);
        int cy = HIWORD(lParam);
        if (cx > 50 && cy > 100) {
            // Top bar: fixed pickers, right-aligned status
            if (g_hStatus) MoveWindow(g_hStatus, 290, 17, (cx > 310) ? cx - 302 : 460, 25, TRUE);

            MoveWindow(g_hEditChat, 25, 65, cx - 50, cy - 140, TRUE);
            MoveWindow(g_hEditInput, 25, cy - 46, cx - 185, 25, TRUE);
            MoveWindow(g_hBtnSend, cx - 54, cy - 49, 31, 31, TRUE);

            RECT rcChat;
            GetClientRect(g_hEditChat, &rcChat);
            rcChat.left += 8;
            rcChat.right -= 8;
            rcChat.top += 8;
            rcChat.bottom -= 8;
            SendMessageW(g_hEditChat, EM_SETRECT, 0, (LPARAM)&rcChat);

            UpdateChatScrollbar();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cx = rc.right - rc.left;
        int cy = rc.bottom - rc.top;

        HDC hMemDC = CreateCompatibleDC(hdc);
        HBITMAP hMemBmp = CreateCompatibleBitmap(hdc, cx, cy);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, hMemBmp);

        {
            Gdiplus::Graphics graphics(hMemDC);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.Clear(Gdiplus::Color(255, 13, 14, 18));

            Gdiplus::Pen dividerPen(Gdiplus::Color(255, 26, 28, 38), 1.0f);
            graphics.DrawLine(&dividerPen, 0.0f, 48.0f, (float)cx, 48.0f);

            float chatCardX = 15.0f;
            float chatCardY = 55.0f;
            float chatCardW = (float)(cx - 30);
            float chatCardH = (float)(cy - 120);

            Gdiplus::SolidBrush chatBg(Gdiplus::Color(255, 21, 22, 29));
            DrawRoundedRect(graphics, chatBg, chatCardX, chatCardY, chatCardW, chatCardH, 8.0f);

            Gdiplus::Pen chatBorder(Gdiplus::Color(255, 33, 35, 46), 1.0f);
            DrawRoundedRectBorder(graphics, chatBorder, chatCardX, chatCardY, chatCardW, chatCardH, 8.0f);

            float inputCardX = 15.0f;
            float inputCardY = (float)(cy - 55);
            float inputCardW = (float)(cx - 30);
            float inputCardH = 43.0f;

            Gdiplus::SolidBrush inputBg(Gdiplus::Color(255, 26, 28, 38));
            DrawRoundedRect(graphics, inputBg, inputCardX, inputCardY, inputCardW, inputCardH, 8.0f);

            Gdiplus::Color borderCol = g_inputFocused ? Gdiplus::Color(255, 99, 102, 241) : Gdiplus::Color(255, 40, 43, 61);
            float borderSz = g_inputFocused ? 1.5f : 1.0f;
            Gdiplus::Pen inputBorder(borderCol, borderSz);
            DrawRoundedRectBorder(graphics, inputBorder, inputCardX, inputCardY, inputCardW, inputCardH, 8.0f);
        }

        BitBlt(hdc, 0, 0, cx, cy, hMemDC, 0, 0, SRCCOPY);

        SelectObject(hMemDC, hOldBmp);
        DeleteObject(hMemBmp);
        DeleteDC(hMemDC);

        EndPaint(hwnd, &ps);
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        HWND hCtrl = (HWND)lParam;
        if (hCtrl == g_hStatus) {
            SetTextColor(hdc, g_statusConnected ? RGB(134, 222, 160) : RGB(230, 180, 130));
        } else {
            SetTextColor(hdc, RGB(220, 220, 220));
        }
        return (INT_PTR)GetStockObject(NULL_BRUSH);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(220, 220, 220));
        SetBkColor(hdc, RGB(26, 28, 38));
        SetBkMode(hdc, TRANSPARENT);
        return (INT_PTR)g_hBrushControl;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        HWND hCtrl = (HWND)lParam;
        if (hCtrl == g_hEditChat) {
            SetTextColor(hdc, RGB(230, 230, 230));
            SetBkColor(hdc, RGB(21, 22, 29));
            return (INT_PTR)g_hBrushDark;
        } else if (hCtrl == g_hEditInput) {
            SetTextColor(hdc, RGB(240, 240, 240));
            SetBkColor(hdc, RGB(26, 28, 38));
            return (INT_PTR)g_hBrushControl;
        }
        SetTextColor(hdc, RGB(240, 240, 240));
        SetBkColor(hdc, RGB(26, 28, 38));
        return (INT_PTR)g_hBrushControl;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        if (pDIS->CtlID == ID_BTN_SEND) {
            HDC hdc = pDIS->hDC;
            RECT rc = pDIS->rcItem;

            HBRUSH hBg = CreateSolidBrush(RGB(26, 28, 38));
            FillRect(hdc, &rc, hBg);
            DeleteObject(hBg);

            Gdiplus::Graphics graphics(hdc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            BOOL isDisabled = (pDIS->itemState & ODS_DISABLED);
            BOOL isSelected = (pDIS->itemState & ODS_SELECTED);

            Gdiplus::Color btnBgColor;
            if (isDisabled) {
                btnBgColor = Gdiplus::Color(255, 45, 45, 48);
            } else if (isSelected || g_btnPressed) {
                btnBgColor = Gdiplus::Color(255, 67, 56, 202);
            } else if (g_btnHovered) {
                btnBgColor = Gdiplus::Color(255, 79, 70, 229);
            } else {
                btnBgColor = Gdiplus::Color(255, 99, 102, 241);
            }

            Gdiplus::SolidBrush brush(btnBgColor);
            graphics.FillEllipse(&brush, (float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top));

            SetTextColor(hdc, isDisabled ? RGB(140, 140, 140) : RGB(255, 255, 255));
            SetBkMode(hdc, TRANSPARENT);
            SelectObject(hdc, g_hFont);
            DrawTextW(hdc, L"\u27A4", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        } else if (pDIS->CtlID == ID_COMBO_MODELS) {
            HDC hdc = pDIS->hDC;
            RECT rc = pDIS->rcItem;

            HBRUSH hBg = CreateSolidBrush(RGB(13, 14, 18));
            FillRect(hdc, &rc, hBg);
            DeleteObject(hBg);

            Gdiplus::Graphics graphics(hdc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            Gdiplus::Color btnBgColor;
            if (g_modelBtnPressed) btnBgColor = Gdiplus::Color(255, 40, 43, 61);
            else if (g_modelBtnHovered) btnBgColor = Gdiplus::Color(255, 35, 37, 51);
            else btnBgColor = Gdiplus::Color(255, 26, 28, 38);

            Gdiplus::SolidBrush brush(btnBgColor);
            DrawRoundedRect(graphics, brush, (float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), 6.0f);

            Gdiplus::Pen borderPen(Gdiplus::Color(255, 40, 43, 61), 1.0f);
            DrawRoundedRectBorder(graphics, borderPen, (float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), 6.0f);

            std::wstring display;
            if (g_models.empty()) {
                display = g_isRefreshing ? L"Loading Models..." : L"No Models Found";
            } else if (g_selectedModelIdx >= 0 && g_selectedModelIdx < (int)g_models.size()) {
                display = UTF8ToWide(g_models[g_selectedModelIdx].name);
            }

            SetTextColor(hdc, RGB(220, 220, 220));
            SetBkMode(hdc, TRANSPARENT);
            SelectObject(hdc, g_hFont);

            RECT rcText = rc;
            rcText.left += 12;
            rcText.right -= 26;
            DrawTextW(hdc, display.c_str(), -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            RECT rcArrow = rc;
            rcArrow.left = rcArrow.right - 26;
            SetTextColor(hdc, RGB(160, 160, 165));
            DrawTextW(hdc, L"\u25BE", -1, &rcArrow, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);
        if (wmId == ID_BTN_SEND) {
            SendPrompt();
        } else if (wmId == ID_COMBO_MODELS) {
            if (wmEvent == BN_CLICKED) {
                if (g_models.empty()) RefreshModels();

                HMENU hMenu = CreatePopupMenu();
                if (g_models.empty()) {
                    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 199, L"No models found (start Ollama / LM Studio)");
                } else {
                    for (size_t i = 0; i < g_models.size(); ++i) {
                        UINT flags = MF_STRING;
                        if ((int)i == g_selectedModelIdx) flags |= MF_CHECKED;
                        AppendMenuW(hMenu, flags, (UINT_PTR)(MENU_MODEL_BASE + i), UTF8ToWide(g_models[i].name).c_str());
                    }
                }

                RECT rect;
                GetWindowRect(g_hComboModels, &rect);
                int selection = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
                                                rect.left, rect.bottom, 0, hwnd, NULL);
                DestroyMenu(hMenu);

                if (selection >= MENU_MODEL_BASE && selection < MENU_MODEL_BASE + (int)g_models.size()) {
                    g_selectedModelIdx = selection - MENU_MODEL_BASE;
                    InvalidateRect(g_hComboModels, NULL, TRUE);
                }
            } else if (wmEvent == 0 && lParam == 0) {
                if (!g_models.empty() && g_selectedModelIdx >= (int)g_models.size()) {
                    g_selectedModelIdx = 0;
                }
                InvalidateRect(g_hComboModels, NULL, TRUE);
            }
        }
        break;
    }
    case WM_BACKEND_STATUS: {
        if (g_hStatus) SetWindowTextW(g_hStatus, g_statusText.c_str());
        InvalidateRect(g_hComboModels, NULL, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_AI_RESPONSE: {
        wchar_t* respPtr = (wchar_t*)lParam;
        if (respPtr) {
            std::wstring respW(respPtr);
            delete[] respPtr;
            AppendChat(L"AI Agent: " + respW);
        }
        g_isGenerating = false;
        EnableWindow(g_hBtnSend, TRUE);
        InvalidateRect(g_hBtnSend, NULL, TRUE);
        break;
    }
    case WM_DESTROY: {
        if (g_hBrushBg) DeleteObject(g_hBrushBg);
        if (g_hBrushControl) DeleteObject(g_hBrushControl);
        if (g_hBrushDark) DeleteObject(g_hBrushDark);
        if (g_hFont) DeleteObject(g_hFont);
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// --selftest mode: headless verification against an installed local model.
// Usage: LocalAIAgent.exe --selftest [auto|ollama|openai] [modelName]
// Exit code 0 = OK, 1 = failed.
// ---------------------------------------------------------------------------
void WriteStdout(const std::string& s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, s.data(), (DWORD)s.size(), &written, NULL);
}

int RunSelfTest(const wchar_t* backendArg, const wchar_t* modelArg) {
    int kind = BK_AUTO;
    if (backendArg) {
        if (lstrcmpW(backendArg, L"ollama") == 0) kind = BK_OLLAMA;
        else if (lstrcmpW(backendArg, L"openai") == 0) kind = BK_OPENAI;
    }

    AIClient client("127.0.0.1");
    std::string label, err;
    std::vector<ModelInfo> models = client.GetModels(kind, label, err);

    if (models.empty()) {
        WriteStdout("SELFTEST FAIL: no local AI backend detected. " + err + "\n");
        return 1;
    }

    WriteStdout("Backend: " + label + " | installed models: " + std::to_string(models.size()) + "\n");
    for (const auto& m : models) WriteStdout("  - " + m.name + "\n");

    std::string modelName;
    if (modelArg) {
        int sz = WideCharToMultiByte(CP_UTF8, 0, modelArg, -1, NULL, 0, NULL, NULL);
        modelName.assign(sz - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, modelArg, -1, &modelName[0], sz, NULL, NULL);
    } else {
        modelName = models[0].name;
    }

    int port = AIClient::DetectPort(client.ActiveBackend());
    std::string resp = client.Chat(modelName, "Reply with exactly one word: PONG.", client.ActiveBackend(), port, err);
    if (resp.empty()) {
        WriteStdout("SELFTEST FAIL: chat error: " + err + "\n");
        return 1;
    }

    WriteStdout("Reply from '" + modelName + "': " + resp + "\n");
    WriteStdout("SELFTEST PASS\n");
    return 0;
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Headless self-test mode.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        bool selftest = false;
        const wchar_t* backendArg = nullptr;
        const wchar_t* modelArg = nullptr;
        for (int i = 1; i < argc; ++i) {
            if (lstrcmpW(argv[i], L"--selftest") == 0) selftest = true;
            else if (lstrcmpW(argv[i], L"ollama") == 0 || lstrcmpW(argv[i], L"openai") == 0 ||
                     lstrcmpW(argv[i], L"auto") == 0) backendArg = argv[i];
            else modelArg = argv[i];
        }
        LPWSTR backendCopy = nullptr;
        LPWSTR modelCopy = nullptr;
        if (backendArg) { backendCopy = new wchar_t[lstrlenW(backendArg) + 1]; wcscpy_s(backendCopy, lstrlenW(backendArg) + 1, backendArg); }
        if (modelArg) { modelCopy = new wchar_t[lstrlenW(modelArg) + 1]; wcscpy_s(modelCopy, lstrlenW(modelArg) + 1, modelArg); }
        LocalFree(argv);
        if (selftest) {
            int code = RunSelfTest(backendCopy, modelCopy);
            delete[] backendCopy;
            delete[] modelCopy;
            return code;
        }
        delete[] backendCopy;
        delete[] modelCopy;
    }

    g_hInst = hInstance;

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    INITCOMMONCONTROLSEX icx;
    icx.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icx.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icx);

    const wchar_t* CLASS_NAME = L"LocalAIAgentWindow";

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));

    RegisterClassExW(&wc);

    g_hMainWnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"Local AI Agent",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 640,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hMainWnd) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}