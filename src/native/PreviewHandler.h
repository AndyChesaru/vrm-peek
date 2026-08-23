#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <propsys.h>
#include <wrl/client.h>
#include <string>
#include <atomic>

#include "WebView2.h"

// {EE2F8D4B-40E1-486F-B8DF-A51B16899142}
extern const CLSID CLSID_VrmPeek;

extern HINSTANCE g_hInst;
extern std::atomic<long> g_objCount;

// The preview handler object. Explorer (via prevhost.exe) initialises it with
// the selected .vrm, then asks it to draw into a child of the preview pane.
// We host a WebView2 there and let three.js / three-vrm do the rendering.
class CPreviewHandler final : public IPreviewHandler,
                              public IPreviewHandlerVisuals,
                              public IInitializeWithFile,
                              public IInitializeWithItem,
                              public IInitializeWithStream,
                              public IObjectWithSite,
                              public IOleWindow
{
public:
    CPreviewHandler();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IInitializeWith*
    IFACEMETHODIMP Initialize(LPCWSTR pszFilePath, DWORD grfMode) override;
    IFACEMETHODIMP Initialize(IShellItem* psi, DWORD grfMode) override;
    IFACEMETHODIMP Initialize(IStream* pStream, DWORD grfMode) override;

    // IPreviewHandler
    IFACEMETHODIMP SetWindow(HWND hwnd, const RECT* prc) override;
    IFACEMETHODIMP SetRect(const RECT* prc) override;
    IFACEMETHODIMP DoPreview() override;
    IFACEMETHODIMP Unload() override;
    IFACEMETHODIMP SetFocus() override;
    IFACEMETHODIMP QueryFocus(HWND* phwnd) override;
    IFACEMETHODIMP TranslateAccelerator(MSG* pmsg) override;

    // IPreviewHandlerVisuals
    IFACEMETHODIMP SetBackgroundColor(COLORREF color) override;
    IFACEMETHODIMP SetFont(const LOGFONTW* plf) override;
    IFACEMETHODIMP SetTextColor(COLORREF color) override;

    // IObjectWithSite
    IFACEMETHODIMP SetSite(IUnknown* punkSite) override;
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override;

    // IOleWindow
    IFACEMETHODIMP GetWindow(HWND* phwnd) override;
    IFACEMETHODIMP ContextSensitiveHelp(BOOL fEnterMode) override;

private:
    ~CPreviewHandler();

    static LRESULT CALLBACK HostWndProc(HWND, UINT, WPARAM, LPARAM);
    static ATOM EnsureWindowClass();

    HRESULT CreateHostWindow();
    void    DestroyHostWindow();
    HRESULT StartWebView();
    HRESULT OnEnvironmentReady(ICoreWebView2Environment* env);
    HRESULT OnControllerReady(ICoreWebView2Controller* controller);
    void    WireEvents();
    void    NavigateToViewer();
    void    LayoutWebView();
    void    PushTheme();
    HRESULT OpenModelStream(IStream** ppStream, ULONGLONG* pSize);
    std::wstring ViewerUrl() const;
    bool     IsDark() const;
    COLORREF Background() const;

    std::atomic<long>              m_refs{1};

    // Shell plumbing
    Microsoft::WRL::ComPtr<IUnknown> m_site;
    Microsoft::WRL::ComPtr<IStream>  m_stream;      // set by IInitializeWithStream
    std::wstring                     m_path;        // set by IInitializeWithFile/Item
    std::wstring                     m_displayName;

    HWND     m_hwndParent = nullptr;
    HWND     m_hwndHost   = nullptr;
    RECT     m_rc{};
    COLORREF m_bg = CLR_INVALID;   // host hint, only a fallback for theme detection

    // WebView2
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_env;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>  m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2>            m_webview;
    EventRegistrationToken m_tokResource{};
    EventRegistrationToken m_tokNavStart{};
    EventRegistrationToken m_tokNewWindow{};
    EventRegistrationToken m_tokNavDone{};
    EventRegistrationToken m_tokProcFail{};
    bool m_creating = false;
    bool m_pendingPreview = false;
};
