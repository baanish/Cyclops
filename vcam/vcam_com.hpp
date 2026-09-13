// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mfcaptureengine.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <propvarutil.h>

#include <winrt/base.h>

#include "ir_capture.hpp"
#include "vcam_ids.hpp"

// Attribute keys not in the 22000 SDK headers (added in build 22621).
// MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES: set by us on the
//   activator, asks Frame Server to hand over the wrapped camera's sources.
// MF_VIRTUALCAMERA_ASSOCIATED_CAMERA_SOURCES: set by Frame Server on the
//   activator, an IMFCollection of IMFActivate for the associated cameras.
// The persisted-symlink attribute itself lives in vcam_ids.hpp
// (cyclops_vcam::symlink_attr) since both sides need it.
constexpr GUID MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES_ =
    { 0xf0273718, 0x4a4d, 0x4ac5, { 0xa1, 0x5d, 0x30, 0x5e, 0xb5, 0xe9, 0x06, 0x67 } };
constexpr GUID MF_VIRTUALCAMERA_ASSOCIATED_CAMERA_SOURCES_ =
    { 0x1bb79e7c, 0x5d83, 0x438c, { 0x94, 0xd8, 0xe5, 0xf0, 0xdf, 0x6d, 0x32, 0x79 } };
constexpr GUID MF_FRAMESERVER_CLIENTCONTEXT_CLIENTPID_ =
    { 0x5f8d322e, 0x0fe4, 0x43e4, { 0x9e, 0x50, 0xd8, 0x3e, 0xcd, 0x9f, 0xc2, 0xb8 } };

#define ENSURE_STORE() \
    if (HRESULT hr_ = ensure_store(); FAILED(hr_)) \
        return hr_

// Object count backing DllCanUnloadNow (replaces winrt::get_module_lock,
// which only tracks winrt::implements objects).
inline LONG& server_locks()
{
    static LONG n = 0;
    return n;
}

struct server_lock
{
    server_lock() { InterlockedIncrement(&server_locks()); }
    ~server_lock() { InterlockedDecrement(&server_locks()); }
};

// IMFAttributes has ~30 methods; implement it once by forwarding to a real
// MF attribute store instead of hand-rolling PROPVARIANT handling. The
// template parameter is the interface this object exposes (which always
// inherits IMFAttributes), so a class deriving attr_forwarder<I> gets a
// concrete I plus all IMFAttributes methods in one inheritance chain.
template<typename I>
struct attr_forwarder : I
{
    // Once created the store lives for the object's lifetime; Shutdown paths
    // must not null it, or an in-flight attribute call could dangle.
    winrt::com_ptr<IMFAttributes> store;
    winrt::slim_mutex store_lock_;

    HRESULT ensure_store()
    {
        winrt::slim_lock_guard g(store_lock_);
        if (store)
            return S_OK;
        return MFCreateAttributes(store.put(), 8);
    }

    STDMETHOD(GetItem)(REFGUID k, PROPVARIANT* v) override { ENSURE_STORE(); return store->GetItem(k, v); }
    STDMETHOD(GetItemType)(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { ENSURE_STORE(); return store->GetItemType(k, t); }
    STDMETHOD(CompareItem)(REFGUID k, REFPROPVARIANT v, BOOL* r) override { ENSURE_STORE(); return store->CompareItem(k, v, r); }
    STDMETHOD(Compare)(IMFAttributes* t, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { ENSURE_STORE(); return store->Compare(t, m, r); }
    STDMETHOD(GetUINT32)(REFGUID k, UINT32* v) override { ENSURE_STORE(); return store->GetUINT32(k, v); }
    STDMETHOD(GetUINT64)(REFGUID k, UINT64* v) override { ENSURE_STORE(); return store->GetUINT64(k, v); }
    STDMETHOD(GetDouble)(REFGUID k, double* v) override { ENSURE_STORE(); return store->GetDouble(k, v); }
    STDMETHOD(GetGUID)(REFGUID k, GUID* v) override { ENSURE_STORE(); return store->GetGUID(k, v); }
    STDMETHOD(GetStringLength)(REFGUID k, UINT32* v) override { ENSURE_STORE(); return store->GetStringLength(k, v); }
    STDMETHOD(GetString)(REFGUID k, LPWSTR v, UINT32 n, UINT32* l) override { ENSURE_STORE(); return store->GetString(k, v, n, l); }
    STDMETHOD(GetAllocatedString)(REFGUID k, LPWSTR* v, UINT32* l) override { ENSURE_STORE(); return store->GetAllocatedString(k, v, l); }
    STDMETHOD(GetBlobSize)(REFGUID k, UINT32* v) override { ENSURE_STORE(); return store->GetBlobSize(k, v); }
    STDMETHOD(GetBlob)(REFGUID k, UINT8* v, UINT32 n, UINT32* l) override { ENSURE_STORE(); return store->GetBlob(k, v, n, l); }
    STDMETHOD(GetAllocatedBlob)(REFGUID k, UINT8** v, UINT32* l) override { ENSURE_STORE(); return store->GetAllocatedBlob(k, v, l); }
    STDMETHOD(GetUnknown)(REFGUID k, REFIID iid, LPVOID* v) override { ENSURE_STORE(); return store->GetUnknown(k, iid, v); }
    STDMETHOD(SetItem)(REFGUID k, REFPROPVARIANT v) override { ENSURE_STORE(); return store->SetItem(k, v); }
    STDMETHOD(DeleteItem)(REFGUID k) override { ENSURE_STORE(); return store->DeleteItem(k); }
    STDMETHOD(DeleteAllItems)() override { ENSURE_STORE(); return store->DeleteAllItems(); }
    STDMETHOD(SetUINT32)(REFGUID k, UINT32 v) override { ENSURE_STORE(); return store->SetUINT32(k, v); }
    STDMETHOD(SetUINT64)(REFGUID k, UINT64 v) override { ENSURE_STORE(); return store->SetUINT64(k, v); }
    STDMETHOD(SetDouble)(REFGUID k, double v) override { ENSURE_STORE(); return store->SetDouble(k, v); }
    STDMETHOD(SetGUID)(REFGUID k, REFGUID v) override { ENSURE_STORE(); return store->SetGUID(k, v); }
    STDMETHOD(SetString)(REFGUID k, LPCWSTR v) override { ENSURE_STORE(); return store->SetString(k, v); }
    STDMETHOD(SetBlob)(REFGUID k, const UINT8* v, UINT32 n) override { ENSURE_STORE(); return store->SetBlob(k, v, n); }
    STDMETHOD(SetUnknown)(REFGUID k, IUnknown* v) override { ENSURE_STORE(); return store->SetUnknown(k, v); }
    STDMETHOD(LockStore)() override { ENSURE_STORE(); return store->LockStore(); }
    STDMETHOD(UnlockStore)() override { ENSURE_STORE(); return store->UnlockStore(); }
    STDMETHOD(GetCount)(UINT32* v) override { ENSURE_STORE(); return store->GetCount(v); }
    STDMETHOD(GetItemByIndex)(UINT32 i, GUID* k, PROPVARIANT* v) override { ENSURE_STORE(); return store->GetItemByIndex(i, k, v); }
    STDMETHOD(CopyAllItems)(IMFAttributes* d) override { ENSURE_STORE(); return store->CopyAllItems(d); }
};

inline std::wstring guid_str(REFGUID g)
{
    wchar_t buf[40];
    return StringFromGUID2(g, buf, _countof(buf)) ? buf : L"?";
}
