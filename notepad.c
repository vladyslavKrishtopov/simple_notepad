#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "resource.h"

#define APP_NAME L"Simple Notepad"
#define ID_EDIT 101

#define IDM_FILE_NEW 1001
#define IDM_FILE_OPEN 1002
#define IDM_FILE_SAVE 1003
#define IDM_FILE_SAVE_AS 1004
#define IDM_FILE_EXIT 1005

#define IDM_EDIT_UNDO 1101
#define IDM_EDIT_CUT 1102
#define IDM_EDIT_COPY 1103
#define IDM_EDIT_PASTE 1104
#define IDM_EDIT_SELECT_ALL 1105

#define IDM_VIEW_TEXT_SIZE_INCREASE 1201
#define IDM_VIEW_TEXT_SIZE_DECREASE 1202

static HWND g_edit = NULL;
static bool g_dirty = false;
static bool g_ignore_change = false;
static wchar_t g_current_path[MAX_PATH] = L"";
static HACCEL g_accel = NULL;
static HICON g_icon_big = NULL;
static HICON g_icon_small = NULL;
static HFONT g_edit_font = NULL;
static WNDPROC g_edit_wnd_proc = NULL;
static int g_font_size_pt = 11;

enum {
    FONT_SIZE_MIN_PT = 8,
    FONT_SIZE_MAX_PT = 48
};

static void ApplyEditFont(HWND hwnd) {
    if (!g_edit) {
        return;
    }

    HDC dc = GetDC(hwnd);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) {
        ReleaseDC(hwnd, dc);
    }

    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    HFONT default_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (default_font) {
        GetObjectW(default_font, sizeof(lf), &lf);
    }

    lf.lfHeight = -MulDiv(g_font_size_pt, dpi, 72);
    HFONT new_font = CreateFontIndirectW(&lf);
    if (!new_font) {
        return;
    }

    SendMessageW(g_edit, WM_SETFONT, (WPARAM)new_font, TRUE);

    if (g_edit_font) {
        DeleteObject(g_edit_font);
    }
    g_edit_font = new_font;
}

static void ChangeTextSize(HWND hwnd, int delta_pt) {
    int next = g_font_size_pt + delta_pt;
    if (next < FONT_SIZE_MIN_PT) {
        next = FONT_SIZE_MIN_PT;
    }
    if (next > FONT_SIZE_MAX_PT) {
        next = FONT_SIZE_MAX_PT;
    }

    if (next == g_font_size_pt) {
        return;
    }

    g_font_size_pt = next;
    ApplyEditFont(hwnd);
}

static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_MOUSEWHEEL) {
        if ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0) {
            int delta = (int)(short)HIWORD(wParam);
            if (delta != 0) {
                HWND parent = GetParent(hwnd);
                int step = delta > 0 ? 1 : -1;
                int count = abs(delta) / WHEEL_DELTA;
                if (count == 0) {
                    count = 1;
                }
                for (int i = 0; i < count; ++i) {
                    ChangeTextSize(parent, step);
                }
            }
            return 0;
        }
    }

    return CallWindowProcW(g_edit_wnd_proc, hwnd, msg, wParam, lParam);
}

static const wchar_t* BaseNameFromPath(const wchar_t* path) {
    const wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        return path;
    }
    return slash + 1;
}

static HICON LoadAppIcon(HINSTANCE instance, int width, int height) {
    return (HICON)LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON,
        width,
        height,
        0
    );
}

static void UpdateWindowTitle(HWND hwnd) {
    wchar_t title[512];
    const wchar_t* name = (g_current_path[0] == L'\0') ? L"Untitled" : BaseNameFromPath(g_current_path);
    swprintf(title, sizeof(title) / sizeof(title[0]), L"%ls%ls - %ls", name, g_dirty ? L"*" : L"", APP_NAME);
    SetWindowTextW(hwnd, title);
}

static bool ReadAllBytes(const wchar_t* path, unsigned char** out_bytes, DWORD* out_size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        CloseHandle(file);
        return false;
    }

    unsigned char* bytes = (unsigned char*)malloc(size + 1);
    if (!bytes) {
        CloseHandle(file);
        return false;
    }

    DWORD read = 0;
    bool ok = ReadFile(file, bytes, size, &read, NULL) && read == size;
    CloseHandle(file);

    if (!ok) {
        free(bytes);
        return false;
    }

    bytes[size] = 0;
    *out_bytes = bytes;
    *out_size = size;
    return true;
}

static bool DecodeTextToWide(const unsigned char* bytes, DWORD size, wchar_t** out_text) {
    UINT code_page = CP_UTF8;
    const unsigned char* start = bytes;
    int start_size = (int)size;

    if (size >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        start = bytes + 3;
        start_size = (int)size - 3;
    } else {
        int test_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)bytes, (int)size, NULL, 0);
        if (test_len == 0) {
            code_page = CP_ACP;
        }
    }

    int wide_len = MultiByteToWideChar(code_page, 0, (const char*)start, start_size, NULL, 0);
    if (wide_len < 0) {
        return false;
    }

    wchar_t* text = (wchar_t*)malloc((size_t)(wide_len + 1) * sizeof(wchar_t));
    if (!text) {
        return false;
    }

    if (wide_len > 0) {
        if (MultiByteToWideChar(code_page, 0, (const char*)start, start_size, text, wide_len) == 0) {
            free(text);
            return false;
        }
    }
    text[wide_len] = L'\0';
    *out_text = text;
    return true;
}

static bool WriteWideAsUtf8(const wchar_t* path, const wchar_t* text) {
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (utf8_len <= 0) {
        return false;
    }

    char* utf8 = (char*)malloc((size_t)utf8_len);
    if (!utf8) {
        return false;
    }

    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, utf8_len, NULL, NULL) == 0) {
        free(utf8);
        return false;
    }

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        free(utf8);
        return false;
    }

    DWORD to_write = (DWORD)(utf8_len - 1);
    DWORD written = 0;
    bool ok = WriteFile(file, utf8, to_write, &written, NULL) && written == to_write;
    CloseHandle(file);
    free(utf8);
    return ok;
}

static bool PromptOpenPath(HWND hwnd, wchar_t* out_path, bool save_mode) {
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    out_path[0] = L'\0';

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = out_path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (save_mode) {
        ofn.Flags |= OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = L"txt";
        return GetSaveFileNameW(&ofn) == TRUE;
    }

    ofn.Flags |= OFN_FILEMUSTEXIST;
    return GetOpenFileNameW(&ofn) == TRUE;
}

static bool LoadDocument(HWND hwnd, const wchar_t* path) {
    unsigned char* bytes = NULL;
    DWORD size = 0;
    if (!ReadAllBytes(path, &bytes, &size)) {
        MessageBoxW(hwnd, L"Failed to open file.", APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }

    wchar_t* text = NULL;
    bool decoded = DecodeTextToWide(bytes, size, &text);
    free(bytes);

    if (!decoded) {
        MessageBoxW(hwnd, L"Failed to decode file text.", APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }

    g_ignore_change = true;
    SetWindowTextW(g_edit, text);
    g_ignore_change = false;
    free(text);

    wcsncpy(g_current_path, path, MAX_PATH - 1);
    g_current_path[MAX_PATH - 1] = L'\0';
    g_dirty = false;
    UpdateWindowTitle(hwnd);
    return true;
}

static bool SaveDocumentToPath(HWND hwnd, const wchar_t* path) {
    int len = GetWindowTextLengthW(g_edit);
    wchar_t* text = (wchar_t*)malloc((size_t)(len + 1) * sizeof(wchar_t));
    if (!text) {
        MessageBoxW(hwnd, L"Out of memory.", APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }

    GetWindowTextW(g_edit, text, len + 1);
    bool ok = WriteWideAsUtf8(path, text);
    free(text);

    if (!ok) {
        MessageBoxW(hwnd, L"Failed to save file.", APP_NAME, MB_OK | MB_ICONERROR);
        return false;
    }

    wcsncpy(g_current_path, path, MAX_PATH - 1);
    g_current_path[MAX_PATH - 1] = L'\0';
    g_dirty = false;
    UpdateWindowTitle(hwnd);
    return true;
}

static bool SaveCurrentDocument(HWND hwnd, bool force_save_as) {
    wchar_t path[MAX_PATH];
    if (!force_save_as && g_current_path[0] != L'\0') {
        wcsncpy(path, g_current_path, MAX_PATH - 1);
        path[MAX_PATH - 1] = L'\0';
    } else {
        if (!PromptOpenPath(hwnd, path, true)) {
            return false;
        }
    }

    return SaveDocumentToPath(hwnd, path);
}

static bool MaybeSaveChanges(HWND hwnd) {
    if (!g_dirty) {
        return true;
    }

    int result = MessageBoxW(
        hwnd,
        L"Do you want to save changes?",
        APP_NAME,
        MB_YESNOCANCEL | MB_ICONQUESTION
    );

    if (result == IDCANCEL) {
        return false;
    }
    if (result == IDYES) {
        return SaveCurrentDocument(hwnd, false);
    }
    return true;
}

static void NewDocument(HWND hwnd) {
    if (!MaybeSaveChanges(hwnd)) {
        return;
    }

    g_ignore_change = true;
    SetWindowTextW(g_edit, L"");
    g_ignore_change = false;

    g_current_path[0] = L'\0';
    g_dirty = false;
    UpdateWindowTitle(hwnd);
}

static void OpenDocument(HWND hwnd) {
    if (!MaybeSaveChanges(hwnd)) {
        return;
    }

    wchar_t path[MAX_PATH];
    if (!PromptOpenPath(hwnd, path, false)) {
        return;
    }

    LoadDocument(hwnd, path);
}

static HMENU BuildMenuBar(void) {
    HMENU menu_bar = CreateMenu();
    HMENU file_menu = CreatePopupMenu();
    HMENU edit_menu = CreatePopupMenu();
    HMENU view_menu = CreatePopupMenu();

    AppendMenuW(file_menu, MF_STRING, IDM_FILE_NEW, L"&New\tCtrl+N");
    AppendMenuW(file_menu, MF_STRING, IDM_FILE_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file_menu, MF_STRING, IDM_FILE_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(file_menu, MF_STRING, IDM_FILE_SAVE_AS, L"Save &As...\tCtrl+Shift+S");
    AppendMenuW(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file_menu, MF_STRING, IDM_FILE_EXIT, L"E&xit");

    AppendMenuW(edit_menu, MF_STRING, IDM_EDIT_UNDO, L"&Undo\tCtrl+Z");
    AppendMenuW(edit_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit_menu, MF_STRING, IDM_EDIT_CUT, L"Cu&t\tCtrl+X");
    AppendMenuW(edit_menu, MF_STRING, IDM_EDIT_COPY, L"&Copy\tCtrl+C");
    AppendMenuW(edit_menu, MF_STRING, IDM_EDIT_PASTE, L"&Paste\tCtrl+V");
    AppendMenuW(edit_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit_menu, MF_STRING, IDM_EDIT_SELECT_ALL, L"Select &All\tCtrl+A");

    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_TEXT_SIZE_INCREASE, L"Increase Text Size\tCtrl++");
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_TEXT_SIZE_DECREASE, L"Decrease Text Size\tCtrl+-");

    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)file_menu, L"&File");
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)edit_menu, L"&Edit");
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)view_menu, L"&View");
    return menu_bar;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_edit = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                L"",
                WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                    ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)ID_EDIT,
                ((LPCREATESTRUCTW)lParam)->hInstance,
                NULL
            );

            g_edit_wnd_proc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);

            ApplyEditFont(hwnd);
            SetMenu(hwnd, BuildMenuBar());
            UpdateWindowTitle(hwnd);
            return 0;
        }

        case WM_SIZE:
            if (g_edit) {
                MoveWindow(g_edit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
            }
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if ((HWND)lParam == g_edit && code == EN_CHANGE && !g_ignore_change) {
                g_dirty = true;
                UpdateWindowTitle(hwnd);
                return 0;
            }

            switch (id) {
                case IDM_FILE_NEW:
                    NewDocument(hwnd);
                    return 0;
                case IDM_FILE_OPEN:
                    OpenDocument(hwnd);
                    return 0;
                case IDM_FILE_SAVE:
                    SaveCurrentDocument(hwnd, false);
                    return 0;
                case IDM_FILE_SAVE_AS:
                    SaveCurrentDocument(hwnd, true);
                    return 0;
                case IDM_FILE_EXIT:
                    SendMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                case IDM_EDIT_UNDO:
                    SendMessageW(g_edit, WM_UNDO, 0, 0);
                    return 0;
                case IDM_EDIT_CUT:
                    SendMessageW(g_edit, WM_CUT, 0, 0);
                    return 0;
                case IDM_EDIT_COPY:
                    SendMessageW(g_edit, WM_COPY, 0, 0);
                    return 0;
                case IDM_EDIT_PASTE:
                    SendMessageW(g_edit, WM_PASTE, 0, 0);
                    return 0;
                case IDM_EDIT_SELECT_ALL:
                    SendMessageW(g_edit, EM_SETSEL, 0, -1);
                    return 0;
                case IDM_VIEW_TEXT_SIZE_INCREASE:
                    ChangeTextSize(hwnd, 1);
                    return 0;
                case IDM_VIEW_TEXT_SIZE_DECREASE:
                    ChangeTextSize(hwnd, -1);
                    return 0;
                default:
                    break;
            }
            break;
        }

        case WM_CLOSE:
            if (MaybeSaveChanges(hwnd)) {
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_DESTROY:
            if (g_edit_font) {
                DeleteObject(g_edit_font);
                g_edit_font = NULL;
            }
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmd_line, int show) {
    (void)prev;

    const wchar_t* class_name = L"SimpleNotepadWindowClass";

    g_icon_small = LoadAppIcon(instance, 16, 16);
    g_icon_big = LoadAppIcon(instance, 32, 32);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = g_icon_big ? g_icon_big : LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = g_icon_small ? g_icon_small : LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Window class registration failed.", APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        class_name,
        APP_NAME,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        650,
        NULL,
        NULL,
        instance,
        NULL
    );

    if (!hwnd) {
        MessageBoxW(NULL, L"Window creation failed.", APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    ACCEL accels[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_FILE_NEW },
        { FVIRTKEY | FCONTROL, 'O', IDM_FILE_OPEN },
        { FVIRTKEY | FCONTROL, 'S', IDM_FILE_SAVE },
        { FVIRTKEY | FCONTROL | FSHIFT, 'S', IDM_FILE_SAVE_AS },
        { FVIRTKEY | FCONTROL, 'A', IDM_EDIT_SELECT_ALL },
        { FVIRTKEY | FCONTROL, VK_OEM_PLUS, IDM_VIEW_TEXT_SIZE_INCREASE },
        { FVIRTKEY | FCONTROL, VK_ADD, IDM_VIEW_TEXT_SIZE_INCREASE },
        { FVIRTKEY | FCONTROL, VK_OEM_MINUS, IDM_VIEW_TEXT_SIZE_DECREASE },
        { FVIRTKEY | FCONTROL, VK_SUBTRACT, IDM_VIEW_TEXT_SIZE_DECREASE },
        { FVIRTKEY | FALT, VK_F4, IDM_FILE_EXIT }
    };
    g_accel = CreateAcceleratorTableW(accels, (int)(sizeof(accels) / sizeof(accels[0])));

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1 && argv[1][0] != L'\0') {
        LoadDocument(hwnd, argv[1]);
    }
    if (argv) {
        LocalFree(argv);
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd, g_accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_accel) {
        DestroyAcceleratorTable(g_accel);
    }
    if (g_icon_big) {
        DestroyIcon(g_icon_big);
    }
    if (g_icon_small) {
        DestroyIcon(g_icon_small);
    }

    return (int)msg.wParam;
}
