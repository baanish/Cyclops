// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT

#pragma once

#include "vcam_com.hpp"

struct cyclops_media_stream;

// The virtual camera's media source. Frame Server CoCreates the activator,
// calls ActivateObject, gets this. One video stream; the physical IR camera
// (if resolvable at activation) is handed down to the stream.
struct cyclops_media_source
    : attr_forwarder<IMFAttributes>, IMFMediaSourceEx, IMFGetService, IKsControl,
      IMFSampleAllocatorControl
{
    // IUnknown
    STDMETHOD(QueryInterface)(REFIID iid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IMFMediaSourceEx)
            *ppv = static_cast<IMFMediaSourceEx*>(this);
        else if (iid == IID_IMFMediaSource)
            *ppv = static_cast<IMFMediaSource*>(static_cast<IMFMediaSourceEx*>(this));
        else if (iid == IID_IMFAttributes)
            *ppv = static_cast<IMFAttributes*>(this);
        else if (iid == IID_IMFGetService)
            *ppv = static_cast<IMFGetService*>(this);
        else if (iid == __uuidof(IKsControl))
            *ppv = static_cast<IKsControl*>(this);
        else if (iid == IID_IMFMediaEventGenerator)
            *ppv = static_cast<IMFMediaSourceEx*>(this);
        else if (iid == IID_IMFSampleAllocatorControl)
            *ppv = static_cast<IMFSampleAllocatorControl*>(this);
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

    // IMFMediaEventGenerator
    STDMETHOD(BeginGetEvent)(IMFAsyncCallback* cb, IUnknown* state);
    STDMETHOD(EndGetEvent)(IMFAsyncResult* res, IMFMediaEvent** evt);
    STDMETHOD(GetEvent)(DWORD flags, IMFMediaEvent** evt);
    STDMETHOD(QueueEvent)(MediaEventType met, REFGUID ext, HRESULT st, const PROPVARIANT* v);

    // IMFMediaSource
    STDMETHOD(CreatePresentationDescriptor)(IMFPresentationDescriptor** pd);
    STDMETHOD(GetCharacteristics)(DWORD* chars);
    STDMETHOD(Pause)();
    STDMETHOD(Shutdown)();
    STDMETHOD(Start)(IMFPresentationDescriptor* pd, const GUID* time_format, const PROPVARIANT* pos);
    STDMETHOD(Stop)();

    // IMFMediaSourceEx
    STDMETHOD(GetSourceAttributes)(IMFAttributes** attrs);
    STDMETHOD(GetStreamAttributes)(DWORD stream_id, IMFAttributes** attrs);
    STDMETHOD(SetD3DManager)(IUnknown* manager);

    // IMFGetService
    STDMETHOD(GetService)(REFGUID siid, REFIID iid, LPVOID* obj);

    // IMFSampleAllocatorControl
    STDMETHOD(SetDefaultAllocator)(DWORD stream_id, IUnknown* alloc);
    STDMETHOD(GetAllocatorUsage)(DWORD stream_id, DWORD* input_id, MFSampleAllocatorUsage* usage);

    // IKsControl
    STDMETHOD_(NTSTATUS, KsProperty)(PKSPROPERTY prop, ULONG len, LPVOID data, ULONG data_len, ULONG* ret);
    STDMETHOD_(NTSTATUS, KsMethod)(PKSMETHOD m, ULONG len, LPVOID data, ULONG data_len, ULONG* ret);
    STDMETHOD_(NTSTATUS, KsEvent)(PKSEVENT evt, ULONG len, LPVOID data, ULONG data_len, ULONG* ret);

    ~cyclops_media_source(); // out-of-line: stream_ member is forward-declared here
    HRESULT Initialize(IMFMediaSource* physical, const std::wstring& physical_symlink,
                       IMFAttributes* activator_attrs);
    // DetachObject handoff: moves physical-camera teardown onto the stream.
    // False when the stream can no longer take it (already shut down).
    bool adopt_physical(IMFActivate* act);
    bool is_shutdown()
    {
        winrt::slim_lock_guard g(lock_);
        return !queue_;
    }

private:
    int stream_index_by_id(DWORD id);

    server_lock lock_guard_;
    LONG refs_ = 1;

    winrt::slim_mutex lock_;
    winrt::com_ptr<cyclops_media_stream> stream_;
    winrt::com_ptr<IMFMediaEventQueue> queue_;
    winrt::com_ptr<IMFPresentationDescriptor> descriptor_;
};
