// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

#include "vcam_com.hpp"

struct cyclops_media_source;

// Frame Server CoCreates this, fills the attribute store (including
// MF_VIRTUALCAMERA_ASSOCIATED_CAMERA_SOURCES when we asked for it), then calls
// ActivateObject to get the media source.
struct cyclops_activator : attr_forwarder<IMFActivate>
{
    // IUnknown
    STDMETHOD(QueryInterface)(REFIID iid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IMFActivate)
            *ppv = static_cast<IMFActivate*>(this);
        else if (iid == IID_IMFAttributes)
            *ppv = static_cast<IMFAttributes*>(static_cast<IMFActivate*>(this));
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&refs_); }
    STDMETHOD_(ULONG, Release)() override
    {
        ULONG r = InterlockedDecrement(&refs_);
        if (!r)
            delete this;
        return r;
    }

    // IMFActivate
    STDMETHOD(ActivateObject)(REFIID riid, void** ppv) override;
    STDMETHOD(ShutdownObject)() override;
    STDMETHOD(DetachObject)() override;

    ~cyclops_activator(); // out-of-line: source_ member is forward-declared here
    HRESULT Initialize();
    void shutdown_physical();

private:
    server_lock lock_guard_;
    LONG refs_ = 1;
    // The physical IR camera, resolved at activation. Owned here so the right
    // cleanup runs (ShutdownObject for frame-server-provided activates,
    // Shutdown for sources we opened ourselves).
    winrt::com_ptr<IMFMediaSource> physical_;
    winrt::com_ptr<IMFActivate> physical_activate_;
    bool physical_self_opened_ = false;
    std::wstring physical_symlink_;

    winrt::com_ptr<cyclops_media_source> source_;
};
