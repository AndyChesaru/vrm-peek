#include "PreviewHandler.h"

#include <shlwapi.h>
#include <olectl.h>
#include <string>

HINSTANCE g_hInst = nullptr;
std::atomic<long> g_objCount{0};

// {EE2F8D4B-40E1-486F-B8DF-A51B16899142}
const CLSID CLSID_VrmPeek =
    { 0xEE2F8D4B, 0x40E1, 0x486F, { 0xB8, 0xDF, 0xA5, 0x1B, 0x16, 0x89, 0x91, 0x42 } };

namespace {

constexpr wchar_t kClsid[]        = L"{EE2F8D4B-40E1-486F-B8DF-A51B16899142}";
constexpr wchar_t kPreviewIid[]   = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}"; // IPreviewHandler
constexpr wchar_t kSurrogateApp[] = L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}"; // 64-bit prevhost.exe
constexpr wchar_t kFriendly[]     = L"VrmPeek";
constexpr wchar_t kExtension[]    = L".vrm";

LSTATUS SetValue(HKEY root, const std::wstring& subkey, const wchar_t* name, const std::wstring& value)
{
    return RegSetKeyValueW(root, subkey.c_str(), name, REG_SZ,
                           value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

std::wstring ModulePath()
{
    wchar_t buf[MAX_PATH * 2]{};
    DWORD n = GetModuleFileNameW(g_hInst, buf, ARRAYSIZE(buf));
    return (n && n < ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring();
}

HRESULT RegisterHandler(HKEY root)
{
    std::wstring dll = ModulePath();
    if (dll.empty()) return E_FAIL;

    const std::wstring clsKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsid;

    LSTATUS s = SetValue(root, clsKey, nullptr, kFriendly);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    SetValue(root, clsKey, L"AppID", kSurrogateApp);
    SetValue(root, clsKey, L"DisplayName", kFriendly);

    const std::wstring inproc = clsKey + L"\\InprocServer32";
    s = SetValue(root, inproc, nullptr, dll);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    SetValue(root, inproc, L"ThreadingModel", L"Apartment");

    // Associate the handler with .vrm, both directly and through
    // SystemFileAssociations so a third-party ProgID cannot mask it.
    SetValue(root, std::wstring(L"Software\\Classes\\") + kExtension + L"\\ShellEx\\" + kPreviewIid,
             nullptr, kClsid);
    SetValue(root, std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + kExtension +
                   L"\\ShellEx\\" + kPreviewIid,
             nullptr, kClsid);

    // Explorer's list of approved preview handlers.
    SetValue(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", kClsid, kFriendly);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

HRESULT UnregisterHandler(HKEY root)
{
    RegDeleteTreeW(root, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsid).c_str());
    RegDeleteTreeW(root, (std::wstring(L"Software\\Classes\\") + kExtension + L"\\ShellEx\\" + kPreviewIid).c_str());
    RegDeleteTreeW(root, (std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + kExtension +
                          L"\\ShellEx\\" + kPreviewIid).c_str());
    RegDeleteKeyValueW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers", kClsid);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

class CClassFactory final : public IClassFactory
{
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++m_refs); }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        long n = --m_refs;
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;

        auto* handler = new (std::nothrow) CPreviewHandler();
        if (!handler) return E_OUTOFMEMORY;

        HRESULT hr = handler->QueryInterface(riid, ppv);
        handler->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) ++g_objCount; else --g_objCount;
        return S_OK;
    }

    CClassFactory() { ++g_objCount; }

private:
    ~CClassFactory() { --g_objCount; }
    std::atomic<long> m_refs{1};
};

} // namespace

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_VrmPeek) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new (std::nothrow) CClassFactory();
    if (!factory) return E_OUTOFMEMORY;

    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return g_objCount == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    return RegisterHandler(HKEY_CURRENT_USER);
}

STDAPI DllUnregisterServer()
{
    return UnregisterHandler(HKEY_CURRENT_USER);
}

// regsvr32 /n /i:user  -> per-user;  /n /i:machine -> all users (needs elevation)
STDAPI DllInstall(BOOL install, LPCWSTR cmdLine)
{
    HKEY root = HKEY_CURRENT_USER;
    if (cmdLine && _wcsicmp(cmdLine, L"machine") == 0) root = HKEY_LOCAL_MACHINE;
    return install ? RegisterHandler(root) : UnregisterHandler(root);
}
