#include <windows.h>

#include <new>
#include <string>

#include "../CameraIds.h"
#include "MediaSource.h"

EXTERN_C const CLSID CLSID_AwcVirtualCameraSource;   // defined in guid.cpp

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace {

HMODULE g_module = nullptr;
LONG g_objects = 0;

class Factory : public IClassFactory {
public:
    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *object = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return n;
    }

    // IClassFactory
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;

        auto source = Make<awc::VCamSource>();
        if (!source) return E_OUTOFMEMORY;
        const HRESULT hr = source->init();
        if (FAILED(hr)) return hr;
        return source.CopyTo(riid, object);
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        lock ? InterlockedIncrement(&g_objects) : InterlockedDecrement(&g_objects);
        return S_OK;
    }

private:
    LONG refs_ = 1;
};

std::wstring modulePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_module, path, MAX_PATH);
    return path;
}

LSTATUS writeValue(HKEY root, const std::wstring& subkey, const wchar_t* name,
                   const std::wstring& value) {
    HKEY key = nullptr;
    LSTATUS s = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (s != ERROR_SUCCESS) return s;
    s = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return s;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (clsid != CLSID_AwcVirtualCameraSource) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new (std::nothrow) Factory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, object);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_objects == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
    const std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + awc::kSourceClsidString;
    LSTATUS s = writeValue(HKEY_LOCAL_MACHINE, base, nullptr, awc::kSourceDescription);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);

    const std::wstring inproc = base + L"\\InprocServer32";
    s = writeValue(HKEY_LOCAL_MACHINE, inproc, nullptr, modulePath());
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    s = writeValue(HKEY_LOCAL_MACHINE, inproc, L"ThreadingModel", L"Both");
    return s == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(s);
}

STDAPI DllUnregisterServer() {
    const std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + awc::kSourceClsidString;
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, (base + L"\\InprocServer32").c_str(), KEY_WOW64_64KEY, 0);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, base.c_str(), KEY_WOW64_64KEY, 0);
    return S_OK;
}
