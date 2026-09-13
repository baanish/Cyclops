// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#include "vcam_com.hpp"
#include "vcam_activator.hpp"
#include "vcam_source.hpp"

#include <atomic>
#include <new>

extern "C" IMAGE_DOS_HEADER __ImageBase;

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(mod); // nothing runs on thread attach
    return TRUE;
}

struct cyclops_class_factory : IClassFactory
{
    STDMETHOD(QueryInterface)(REFIID iid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IClassFactory)
        {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&refs_); }
    STDMETHOD_(ULONG, Release)() override
    {
        ULONG r = InterlockedDecrement(&refs_);
        if (!r)
            delete this;
        return r;
    }

    STDMETHOD(CreateInstance)(IUnknown* outer, REFIID riid, void** result) override
    {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (outer)
            return CLASS_E_NOAGGREGATION;

        winrt::com_ptr<cyclops_activator> act;
        act.attach(new (std::nothrow) cyclops_activator());
        if (!act)
            return E_OUTOFMEMORY;
        HRESULT hr = act->Initialize();
        if (FAILED(hr))
            return hr;
        return act->QueryInterface(riid, result);
    }

    STDMETHOD(LockServer)(BOOL lock) override
    {
        if (lock)
            InterlockedIncrement(&server_locks());
        else
            InterlockedDecrement(&server_locks());
        return S_OK;
    }

    // Counts toward DllCanUnloadNow: a live factory with zero objects must
    // still pin the module.
    server_lock lock_guard_;
    LONG refs_ = 1;
};

STDAPI DllCanUnloadNow()
{
    // Logged here rather than DllMain: loader lock makes file I/O unsafe there.
    if (server_locks() == 0)
        ir_log(L"vcam dll unloaded");
    return server_locks() ? S_FALSE : S_OK;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (rclsid == cyclops_vcam::clsid)
    {
        static std::atomic_bool logged{ false };
        if (!logged.exchange(true))
            ir_log(L"vcam dll loaded");
        auto* factory = new (std::nothrow) cyclops_class_factory();
        if (!factory)
            return E_OUTOFMEMORY;
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW((HMODULE)&__ImageBase, path, _countof(path));

    const std::wstring key_path = std::wstring(L"Software\\Classes\\CLSID\\") + cyclops_vcam::clsid_str;
    const std::wstring inproc_path = key_path + L"\\InprocServer32";

    // A virtual camera source must live under HKLM: Frame Server (LocalService)
    // loads it, so HKCU registration is invisible to it.
    HKEY key = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, inproc_path.c_str(), 0, nullptr, 0,
                                 KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(st);

    st = RegSetValueExW(key, nullptr, 0, REG_SZ,
                              (const BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
    if (st == ERROR_SUCCESS)
        st = RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ,
                            (const BYTE*)L"Both", sizeof(L"Both"));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(st);
}

STDAPI DllUnregisterServer()
{
    const std::wstring key_path = std::wstring(L"Software\\Classes\\CLSID\\") + cyclops_vcam::clsid_str;
    return HRESULT_FROM_WIN32(RegDeleteTreeW(HKEY_LOCAL_MACHINE, key_path.c_str()));
}
