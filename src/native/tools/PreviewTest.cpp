// Minimal stand-in for Explorer's preview pane, used to exercise the handler
// outside the shell.
//
//   PreviewTest.exe <file.vrm> [-inproc] [-light] [-w N] [-h N]
//
// By default the handler is created with CLSCTX_LOCAL_SERVER, which routes it
// through prevhost.exe exactly as Explorer does. -inproc loads the DLL into
// this process instead, which is easier to debug.

#include <windows.h>
#include <shobjidl.h>
#include <propsys.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

static const CLSID CLSID_VrmPeek =
    { 0xEE2F8D4B, 0x40E1, 0x486F, { 0xB8, 0xDF, 0xA5, 0x1B, 0x16, 0x89, 0x91, 0x42 } };

static ComPtr<IPreviewHandler> g_handler;

static void Report(const char* what, HRESULT hr)
{
    if (SUCCEEDED(hr)) {
        std::printf("  %-28s ok\n", what);
        return;
    }
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, 0,
                   reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::printf("  %-28s FAILED 0x%08X  %ls", what, static_cast<unsigned>(hr),
                text ? text : L"\n");
    if (text) LocalFree(text);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_handler) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            g_handler->SetRect(&rc);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // keep diagnostics live when redirected

    std::wstring file;
    bool inproc = false, light = false;
    int width = 980, height = 860;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"-inproc")       inproc = true;
        else if (a == L"-light")   light = true;
        else if (a == L"-w" && i + 1 < argc) width = _wtoi(argv[++i]);
        else if (a == L"-h" && i + 1 < argc) height = _wtoi(argv[++i]);
        else if (file.empty())     file = a;
    }

    if (file.empty()) {
        std::printf("usage: PreviewTest.exe <file.vrm> [-inproc] [-light] [-w N] [-h N]\n");
        return 2;
    }

    wchar_t full[MAX_PATH * 2]{};
    if (!GetFullPathNameW(file.c_str(), ARRAYSIZE(full), full, nullptr)) return 2;
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        std::printf("file not found: %ls\n", full);
        return 2;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) { Report("CoInitializeEx", hr); return 1; }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(light ? RGB(0xF4, 0xF4, 0xF6) : RGB(0x17, 0x17, 0x1B));
    wc.lpszClassName = L"VrmPeekTestHost";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VrmPeek Test Host",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                width, height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { std::printf("CreateWindowExW failed\n"); return 1; }

    std::printf("host: %ls\n", inproc ? L"in-process" : L"prevhost.exe surrogate");
    std::printf("file: %ls\n", full);

    hr = CoCreateInstance(CLSID_VrmPeek, nullptr,
                          inproc ? CLSCTX_INPROC_SERVER : CLSCTX_LOCAL_SERVER,
                          IID_PPV_ARGS(&g_handler));
    Report("CoCreateInstance", hr);
    if (FAILED(hr)) return 1;

    ComPtr<IInitializeWithFile> initFile;
    hr = g_handler.As(&initFile);
    Report("QI IInitializeWithFile", hr);
    if (SUCCEEDED(hr)) Report("Initialize(file)", initFile->Initialize(full, STGM_READ));

    ComPtr<IPreviewHandlerVisuals> visuals;
    if (SUCCEEDED(g_handler.As(&visuals))) {
        visuals->SetBackgroundColor(light ? RGB(0xF4, 0xF4, 0xF6) : RGB(0x20, 0x20, 0x20));
        visuals->SetTextColor(light ? RGB(0x1A, 0x1A, 0x1F) : RGB(0xFF, 0xFF, 0xFF));
        Report("IPreviewHandlerVisuals", S_OK);
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    Report("SetWindow", g_handler->SetWindow(hwnd, &rc));
    Report("DoPreview", g_handler->DoPreview());

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    std::printf("\nrunning - close the window to exit\n");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_handler->Unload();
    g_handler.Reset();
    CoUninitialize();
    return 0;
}
