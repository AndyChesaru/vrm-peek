#include "PreviewHandler.h"

#include <shlwapi.h>
#include <shellapi.h>
#include <pathcch.h>
#include <wrl.h>
#include <wrl/module.h>
#include <string>
#include <vector>
#include <cstdarg>

using namespace Microsoft::WRL;

namespace {

constexpr wchar_t kWindowClass[]  = L"VrmPeekHostWindow";
constexpr wchar_t kAppHost[]      = L"vrmpeek.invalid";     // mapped to the web folder
constexpr wchar_t kModelUrl[]     = L"https://model.vrmpeek.invalid/__model__.vrm";

std::wstring ModuleDirectory()
{
    wchar_t buf[MAX_PATH * 2]{};
    DWORD n = GetModuleFileNameW(g_hInst, buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf)) return {};
    if (FAILED(PathCchRemoveFileSpec(buf, ARRAYSIZE(buf)))) return {};
    return buf;
}

// Explorer hosts preview handlers in a LOW integrity prevhost.exe, which cannot
// write to %LOCALAPPDATA%. LocalLow is the one per-user location a low integrity
// process may write to, so WebView2's user data folder has to live there -
// otherwise the environment fails to create and the pane just stays blank.
std::wstring DataFolder()
{
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, nullptr, &base))) return {};
    std::wstring path = base;
    CoTaskMemFree(base);
    path += L"\\VrmPeek";
    return path;
}

std::wstring UserDataFolder()
{
    std::wstring path = DataFolder();
    if (!path.empty()) path += L"\\WebView2";
    return path;
}

// Diagnostics, silent unless HKCU\Software\VrmPeek\Debug is set to 1. A shell
// extension has nowhere to print to, and the low integrity host makes attaching
// a debugger awkward.
bool DebugEnabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        DWORD value = 0, size = sizeof(value);
        LSTATUS s = RegGetValueW(HKEY_CURRENT_USER, L"Software\\VrmPeek", L"Debug",
                                 RRF_RT_REG_DWORD, nullptr, &value, &size);
        enabled = (s == ERROR_SUCCESS && value != 0) ? 1 : 0;
    }
    return enabled == 1;
}

void Log(const wchar_t* format, ...)
{
    if (!DebugEnabled()) return;

    std::wstring dir = DataFolder();
    if (dir.empty()) return;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    wchar_t line[1024]{};
    SYSTEMTIME t{};
    GetLocalTime(&t);
    int n = swprintf_s(line, L"%02d:%02d:%02d.%03d [%lu] ",
                       t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());

    va_list args;
    va_start(args, format);
    vswprintf_s(line + n, ARRAYSIZE(line) - n - 3, format, args);
    va_end(args);
    wcscat_s(line, L"\r\n");

    std::wstring path = dir + L"\\vrmpeek.log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 1) {
        std::vector<char> utf8(bytes);
        WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(bytes - 1), &written, nullptr);
    }
    CloseHandle(h);
}

// Percent-encode a wide string for use inside a URL query value.
std::wstring UrlEncode(const std::wstring& value)
{
    int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::vector<char> utf8(bytes);
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);

    static const wchar_t* hex = L"0123456789ABCDEF";
    std::wstring out;
    for (int i = 0; i < bytes - 1; ++i) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            out.push_back(L'%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

constexpr COLORREF kDarkBg  = RGB(0x17, 0x17, 0x1B);
constexpr COLORREF kLightBg = RGB(0xF4, 0xF4, 0xF6);

bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix)
{
    size_t n = wcslen(prefix);
    return s.size() >= n && CompareStringOrdinal(s.c_str(), (int)n, prefix, (int)n, TRUE) == CSTR_EQUAL;
}

} // namespace

/* ==================================================================== life */

CPreviewHandler::CPreviewHandler()
{
    ++g_objCount;
}

CPreviewHandler::~CPreviewHandler()
{
    DestroyHostWindow();
    --g_objCount;
}

IFACEMETHODIMP CPreviewHandler::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown)                   *ppv = static_cast<IPreviewHandler*>(this);
    else if (riid == __uuidof(IPreviewHandler))        *ppv = static_cast<IPreviewHandler*>(this);
    else if (riid == __uuidof(IPreviewHandlerVisuals)) *ppv = static_cast<IPreviewHandlerVisuals*>(this);
    else if (riid == __uuidof(IInitializeWithFile))    *ppv = static_cast<IInitializeWithFile*>(this);
    else if (riid == __uuidof(IInitializeWithItem))    *ppv = static_cast<IInitializeWithItem*>(this);
    else if (riid == __uuidof(IInitializeWithStream))  *ppv = static_cast<IInitializeWithStream*>(this);
    else if (riid == __uuidof(IObjectWithSite))        *ppv = static_cast<IObjectWithSite*>(this);
    else if (riid == __uuidof(IOleWindow))             *ppv = static_cast<IOleWindow*>(this);
    else return E_NOINTERFACE;

    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) CPreviewHandler::AddRef()
{
    return static_cast<ULONG>(++m_refs);
}

IFACEMETHODIMP_(ULONG) CPreviewHandler::Release()
{
    long n = --m_refs;
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
}

/* ============================================================ initialize */

IFACEMETHODIMP CPreviewHandler::Initialize(LPCWSTR pszFilePath, DWORD)
{
    if (!pszFilePath) return E_INVALIDARG;
    m_path = pszFilePath;
    m_displayName = PathFindFileNameW(pszFilePath);
    Log(L"Initialize(file) %s", m_path.c_str());
    return S_OK;
}

IFACEMETHODIMP CPreviewHandler::Initialize(IShellItem* psi, DWORD grfMode)
{
    if (!psi) return E_INVALIDARG;

    PWSTR path = nullptr;
    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        HRESULT hr = Initialize(static_cast<LPCWSTR>(path), grfMode);
        CoTaskMemFree(path);
        return hr;
    }

    // Not backed by the file system - fall back to a stream.
    ComPtr<IStream> stream;
    HRESULT hr = psi->BindToHandler(nullptr, BHID_Stream, IID_PPV_ARGS(&stream));
    if (FAILED(hr)) return hr;

    PWSTR name = nullptr;
    if (SUCCEEDED(psi->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name)) && name) {
        m_displayName = name;
        CoTaskMemFree(name);
    }
    m_stream = stream;
    Log(L"Initialize(item) stream for %s", m_displayName.c_str());
    return S_OK;
}

IFACEMETHODIMP CPreviewHandler::Initialize(IStream* pStream, DWORD)
{
    if (!pStream) return E_INVALIDARG;
    m_stream = pStream;

    STATSTG stat{};
    if (SUCCEEDED(pStream->Stat(&stat, STATFLAG_DEFAULT))) {
        if (stat.pwcsName) {
            m_displayName = PathFindFileNameW(stat.pwcsName);
            CoTaskMemFree(stat.pwcsName);
        }
    }
    Log(L"Initialize(stream) %s", m_displayName.c_str());
    return S_OK;
}

/* ================================================================ window */

ATOM CPreviewHandler::EnsureWindowClass()
{
    static std::atomic<ATOM> atom{0};
    if (atom.load()) return atom.load();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = HostWndProc;
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    wc.cbWndExtra    = sizeof(void*);

    ATOM registered = RegisterClassExW(&wc);
    if (!registered && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) registered = 1;
    atom.store(registered);
    return registered;
}

LRESULT CALLBACK CPreviewHandler::HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<CPreviewHandler*>(GetWindowLongPtrW(hwnd, 0));

    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, 0, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        break;
    }
    case WM_SIZE:
        if (self) self->LayoutWebView();
        return 0;

    case WM_ERASEBKGND: {
        COLORREF bg = self ? self->Background() : kDarkBg;
        HDC hdc = reinterpret_cast<HDC>(wp);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(bg);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_SETFOCUS:
        if (self && self->m_controller) self->m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;

    case WM_SETTINGCHANGE:
        if (self) self->PushTheme();
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HRESULT CPreviewHandler::CreateHostWindow()
{
    if (m_hwndHost) return S_OK;
    if (!m_hwndParent) { Log(L"CreateHostWindow: no parent"); return E_UNEXPECTED; }
    Log(L"CreateHostWindow parent=%p valid=%d", m_hwndParent, IsWindow(m_hwndParent) ? 1 : 0);
    if (!EnsureWindowClass()) return HRESULT_FROM_WIN32(GetLastError());

    m_hwndHost = CreateWindowExW(
        0, kWindowClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        m_rc.left, m_rc.top, m_rc.right - m_rc.left, m_rc.bottom - m_rc.top,
        m_hwndParent, nullptr, g_hInst, this);

    Log(L"host window %p in parent %p rect %d,%d %dx%d err=%lu",
        m_hwndHost, m_hwndParent, m_rc.left, m_rc.top,
        m_rc.right - m_rc.left, m_rc.bottom - m_rc.top,
        m_hwndHost ? 0UL : GetLastError());

    return m_hwndHost ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

void CPreviewHandler::DestroyHostWindow()
{
    if (m_controller) {
        m_controller->Close();
        m_controller.Reset();
        m_webview.Reset();
    }
    m_env.Reset();
    if (m_hwndHost) {
        SetWindowLongPtrW(m_hwndHost, 0, 0);
        DestroyWindow(m_hwndHost);
        m_hwndHost = nullptr;
    }
}

void CPreviewHandler::LayoutWebView()
{
    if (!m_controller || !m_hwndHost) return;
    RECT rc{};
    GetClientRect(m_hwndHost, &rc);
    m_controller->put_Bounds(rc);
}

/* ====================================================== IPreviewHandler */

IFACEMETHODIMP CPreviewHandler::SetWindow(HWND hwnd, const RECT* prc)
{
    Log(L"SetWindow hwnd=%p valid=%d rect=%s %d,%d %dx%d", hwnd, IsWindow(hwnd) ? 1 : 0,
        prc ? L"yes" : L"null",
        prc ? prc->left : 0, prc ? prc->top : 0,
        prc ? prc->right - prc->left : 0, prc ? prc->bottom - prc->top : 0);
    if (prc) m_rc = *prc;

    if (hwnd != m_hwndParent) {
        DestroyHostWindow();
        m_hwndParent = hwnd;
    } else if (m_hwndHost) {
        SetWindowPos(m_hwndHost, nullptr, m_rc.left, m_rc.top,
                     m_rc.right - m_rc.left, m_rc.bottom - m_rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return S_OK;
}

IFACEMETHODIMP CPreviewHandler::SetRect(const RECT* prc)
{
    Log(L"SetRect %d,%d %dx%d host=%p", prc ? prc->left : 0, prc ? prc->top : 0,
        prc ? prc->right - prc->left : 0, prc ? prc->bottom - prc->top : 0, m_hwndHost);
    if (!prc) return E_INVALIDARG;
    m_rc = *prc;
    if (m_hwndHost) {
        SetWindowPos(m_hwndHost, nullptr, m_rc.left, m_rc.top,
                     m_rc.right - m_rc.left, m_rc.bottom - m_rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return S_OK;
}

IFACEMETHODIMP CPreviewHandler::DoPreview()
{
    Log(L"DoPreview path='%s' stream=%d webview=%d",
        m_path.c_str(), m_stream ? 1 : 0, m_webview ? 1 : 0);
    if (m_path.empty() && !m_stream) return E_UNEXPECTED;

    HRESULT hr = CreateHostWindow();
    if (FAILED(hr)) return hr;

    if (m_webview) {
        NavigateToViewer();
        return S_OK;
    }

    m_pendingPreview = true;
    return StartWebView();
}

IFACEMETHODIMP CPreviewHandler::Unload()
{
    Log(L"Unload");
    m_pendingPreview = false;
    if (m_webview) m_webview->Navigate(L"about:blank");
    m_stream.Reset();
    m_path.clear();
    m_displayName.clear();
    return S_OK;
}

IFACEMETHODIMP CPreviewHandler::SetFocus()
{
    if (m_controller) {
        m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return S_OK;
    }
    if (m_hwndHost) {
        ::SetFocus(m_hwndHost);
        return S_OK;
    }
    return S_FALSE;
}

IFACEMETHODIMP CPreviewHandler::QueryFocus(HWND* phwnd)
{
    if (!phwnd) return E_INVALIDARG;
    *phwnd = ::GetFocus();
    return *phwnd ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

IFACEMETHODIMP CPreviewHandler::TranslateAccelerator(MSG* pmsg)
{
    if (!m_site) return S_FALSE;
    ComPtr<IPreviewHandlerFrame> frame;
    if (SUCCEEDED(m_site.As(&frame))) return frame->TranslateAccelerator(pmsg);
    return S_FALSE;
}

/* =============================================== IPreviewHandlerVisuals */

IFACEMETHODIMP CPreviewHandler::SetBackgroundColor(COLORREF color)
{
    m_bg = color;
    PushTheme();
    return S_OK;
}

IFACEMETHODIMP CPreviewHandler::SetTextColor(COLORREF)
{
    // The viewer derives its own foreground from the theme it picked; see IsDark.
    return S_OK;
}

IFACEMETHODIMP CPreviewHandler::SetFont(const LOGFONTW*)
{
    return S_OK; // The viewer uses its own type scale.
}

/* ======================================================== IObjectWithSite */

IFACEMETHODIMP CPreviewHandler::SetSite(IUnknown* punkSite)
{
    m_site = punkSite;
    return S_OK;
}

IFACEMETHODIMP CPreviewHandler::GetSite(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!m_site) return E_FAIL;
    return m_site.CopyTo(riid, ppv);
}

/* ============================================================ IOleWindow */

IFACEMETHODIMP CPreviewHandler::GetWindow(HWND* phwnd)
{
    if (!phwnd) return E_INVALIDARG;
    *phwnd = m_hwndHost;
    return m_hwndHost ? S_OK : E_FAIL;
}

IFACEMETHODIMP CPreviewHandler::ContextSensitiveHelp(BOOL)
{
    return E_NOTIMPL;
}

/* ============================================================== WebView2 */

bool CPreviewHandler::IsDark() const
{
    // Windows 11 reports a white background to preview handlers even when
    // Explorer itself is dark, so following IPreviewHandlerVisuals here would
    // paint a white slab inside dark chrome. The app theme is the signal that
    // actually matches the surrounding pane; the host colour is only a fallback
    // for hosts that do not follow the OS theme at all.
    DWORD value = 0, size = sizeof(value);
    LSTATUS s = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (s == ERROR_SUCCESS) return value == 0;

    if (m_bg != CLR_INVALID) {
        int luminance = (GetRValue(m_bg) * 299 + GetGValue(m_bg) * 587 + GetBValue(m_bg) * 114) / 1000;
        return luminance < 128;
    }
    return true;
}

COLORREF CPreviewHandler::Background() const
{
    return IsDark() ? kDarkBg : kLightBg;
}

std::wstring CPreviewHandler::ViewerUrl() const
{
    std::wstring url = L"https://";
    url += kAppHost;
    url += L"/viewer.html?theme=";
    url += IsDark() ? L"dark" : L"light";
    if (!m_displayName.empty()) { url += L"&name=" + UrlEncode(m_displayName); }
    return url;
}

HRESULT CPreviewHandler::StartWebView()
{
    Log(L"StartWebView creating=%d", m_creating ? 1 : 0);
    if (m_creating) return S_OK;
    m_creating = true;

    std::wstring userData = UserDataFolder();
    if (userData.empty()) {
        Log(L"user data folder unavailable");
        m_creating = false;
        return E_FAIL;
    }
    int mk = SHCreateDirectoryExW(nullptr, userData.c_str(), nullptr);
    Log(L"user data folder '%s' (create=%d)", userData.c_str(), mk);

    ComPtr<CPreviewHandler> self(this);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [self](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                Log(L"environment callback hr=0x%08X", result);
                if (FAILED(result) || !env) { self->m_creating = false; return result; }
                return self->OnEnvironmentReady(env);
            }).Get());

    Log(L"CreateCoreWebView2EnvironmentWithOptions hr=0x%08X", hr);
    if (FAILED(hr)) m_creating = false;
    return hr;
}

HRESULT CPreviewHandler::OnEnvironmentReady(ICoreWebView2Environment* env)
{
    Log(L"environment ready, host=%p", m_hwndHost);
    if (!m_hwndHost) { m_creating = false; return S_OK; }
    m_env = env;

    ComPtr<CPreviewHandler> self(this);
    return env->CreateCoreWebView2Controller(
        m_hwndHost,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [self](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                Log(L"controller callback hr=0x%08X", result);
                self->m_creating = false;
                if (FAILED(result) || !controller) return result;
                return self->OnControllerReady(controller);
            }).Get());
}

HRESULT CPreviewHandler::OnControllerReady(ICoreWebView2Controller* controller)
{
    Log(L"controller ready, host=%p pending=%d", m_hwndHost, m_pendingPreview ? 1 : 0);
    if (!m_hwndHost) { controller->Close(); return S_OK; }

    m_controller = controller;
    m_controller->get_CoreWebView2(&m_webview);
    if (!m_webview) return E_FAIL;

    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(m_controller.As(&controller2))) {
        COLORREF bg = Background();
        COREWEBVIEW2_COLOR c{ 255, GetRValue(bg), GetGValue(bg), GetBValue(bg) };
        controller2->put_DefaultBackgroundColor(c);
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(m_webview->get_Settings(&settings))) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsBuiltInErrorPageEnabled(FALSE);

        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);

        ComPtr<ICoreWebView2Settings6> settings6;
        if (SUCCEEDED(settings.As(&settings6))) settings6->put_IsSwipeNavigationEnabled(FALSE);
    }

    ComPtr<ICoreWebView2_3> webview3;
    if (SUCCEEDED(m_webview.As(&webview3))) {
        std::wstring webFolder = ModuleDirectory() + L"\\web";
        webview3->SetVirtualHostNameToFolderMapping(
            kAppHost, webFolder.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    }

    WireEvents();
    LayoutWebView();

    if (m_pendingPreview) NavigateToViewer();
    return S_OK;
}

void CPreviewHandler::WireEvents()
{
    ComPtr<CPreviewHandler> self(this);

    // Serve the selected .vrm from memory / disk instead of the network.
    m_webview->AddWebResourceRequestedFilter(kModelUrl, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    m_webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [self](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                ComPtr<IStream> stream;
                ULONGLONG size = 0;
                ComPtr<ICoreWebView2WebResourceResponse> response;

                HRESULT open = self->OpenModelStream(&stream, &size);
                Log(L"model requested hr=0x%08X size=%llu", open, size);
                if (FAILED(open) || !self->m_env) {
                    if (self->m_env) {
                        self->m_env->CreateWebResourceResponse(
                            nullptr, 404, L"Not Found",
                            L"Access-Control-Allow-Origin: https://vrmpeek.invalid", &response);
                        args->put_Response(response.Get());
                    }
                    return S_OK;
                }

                wchar_t headers[512]{};
                swprintf_s(headers,
                    L"Content-Type: model/gltf-binary\r\n"
                    L"Content-Length: %llu\r\n"
                    L"Cache-Control: no-store\r\n"
                    L"Access-Control-Allow-Origin: https://vrmpeek.invalid",
                    size);

                if (SUCCEEDED(self->m_env->CreateWebResourceResponse(
                        stream.Get(), 200, L"OK", headers, &response))) {
                    args->put_Response(response.Get());
                }
                return S_OK;
            }).Get(), &m_tokResource);

    m_webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL ok = FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                args->get_IsSuccess(&ok);
                args->get_WebErrorStatus(&status);
                Log(L"navigation completed success=%d status=%d", ok, static_cast<int>(status));
                return S_OK;
            }).Get(), &m_tokNavDone);

    m_webview->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                args->get_ProcessFailedKind(&kind);
                Log(L"WebView2 process failed, kind=%d", static_cast<int>(kind));
                return S_OK;
            }).Get(), &m_tokProcFail);

    // The page must never navigate away from the bundled viewer.
    m_webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr;
                if (FAILED(args->get_Uri(&uri)) || !uri) return S_OK;
                std::wstring u = uri;
                CoTaskMemFree(uri);
                if (!StartsWithNoCase(u, L"https://vrmpeek.invalid/") && !StartsWithNoCase(u, L"about:blank")) {
                    args->put_Cancel(TRUE);
                }
                return S_OK;
            }).Get(), &m_tokNavStart);

    // Links in the model's metadata open in the user's browser, never in the pane.
    m_webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE);
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    std::wstring u = uri;
                    CoTaskMemFree(uri);
                    if (StartsWithNoCase(u, L"https://") || StartsWithNoCase(u, L"http://")) {
                        ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
                return S_OK;
            }).Get(), &m_tokNewWindow);
}

void CPreviewHandler::NavigateToViewer()
{
    if (!m_webview) return;
    m_pendingPreview = false;
    std::wstring url = ViewerUrl();
    Log(L"navigating to %s", url.c_str());
    m_webview->Navigate(url.c_str());
}

void CPreviewHandler::PushTheme()
{
    if (!m_webview) return;

    wchar_t json[128]{};
    swprintf_s(json, L"{\"type\":\"theme\",\"dark\":%s}", IsDark() ? L"true" : L"false");
    m_webview->PostWebMessageAsJson(json);

    if (m_controller) {
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(m_controller.As(&controller2))) {
            COLORREF bg = Background();
            COREWEBVIEW2_COLOR c{ 255, GetRValue(bg), GetGValue(bg), GetBValue(bg) };
            controller2->put_DefaultBackgroundColor(c);
        }
    }
}

HRESULT CPreviewHandler::OpenModelStream(IStream** ppStream, ULONGLONG* pSize)
{
    if (!ppStream || !pSize) return E_POINTER;
    *ppStream = nullptr;
    *pSize = 0;

    ComPtr<IStream> result;

    if (!m_path.empty()) {
        HRESULT hr = SHCreateStreamOnFileEx(
            m_path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &result);
        if (FAILED(hr)) return hr;
    } else if (m_stream) {
        // Explorer's stream is bound to this apartment; hand WebView2 a private
        // in-memory copy that is safe to read from its own thread.
        LARGE_INTEGER zero{};
        m_stream->Seek(zero, STREAM_SEEK_SET, nullptr);

        ULONGLONG expected = 0;
        STATSTG srcStat{};
        if (SUCCEEDED(m_stream->Stat(&srcStat, STATFLAG_NONAME))) expected = srcStat.cbSize.QuadPart;

        ComPtr<IStream> mem;
        mem.Attach(SHCreateMemStream(nullptr, 0));
        if (!mem) return E_OUTOFMEMORY;

        // Reserve up front so a large avatar does not reallocate its way
        // through memory one chunk at a time.
        if (expected) {
            ULARGE_INTEGER reserve{};
            reserve.QuadPart = expected;
            mem->SetSize(reserve);
        }

        // IStream::CopyTo is optional and the shell's stream returns E_NOTIMPL,
        // so copy by hand.
        std::vector<BYTE> chunk(64 * 1024);
        ULONGLONG total = 0;
        for (;;) {
            ULONG read = 0;
            HRESULT hr = m_stream->Read(chunk.data(), static_cast<ULONG>(chunk.size()), &read);
            if (FAILED(hr)) return hr;
            if (read == 0) break;

            ULONG wrote = 0;
            hr = mem->Write(chunk.data(), read, &wrote);
            if (FAILED(hr)) return hr;
            if (wrote != read) return E_FAIL;
            total += wrote;
        }

        // Trim the reservation back to what actually arrived.
        ULARGE_INTEGER actual{};
        actual.QuadPart = total;
        mem->SetSize(actual);

        mem->Seek(zero, STREAM_SEEK_SET, nullptr);
        result = mem;
    } else {
        return E_UNEXPECTED;
    }

    STATSTG stat{};
    if (SUCCEEDED(result->Stat(&stat, STATFLAG_NONAME))) *pSize = stat.cbSize.QuadPart;

    *ppStream = result.Detach();
    return S_OK;
}
